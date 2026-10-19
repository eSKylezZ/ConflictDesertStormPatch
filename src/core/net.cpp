// Online connection without a server of ours - see net.h. Pieces:
//  - one UDP port (IPv4 + IPv6 socket on the same number), worker thread with select(), everything under g_mx
//  - gathering: own LAN address (route to the internet), IPv6 global addresses, public IPv4 from two STUN servers (sent
//    from the game port, so that mapping is the one hole punching uses; different answers = "strict" NAT), router port
//    opened by PCP (RFC 6887) / NAT-PMP (RFC 6886) / UPnP IGD (SSDP + SOAP AddPortMapping) on a helper thread
//  - codes: Crockford base32 of {kind, flags, session, [joiner nonce], candidates {type, address, port}, CRC-8}
//  - packets: "DSF" + type + session + joiner nonce (12 bytes) + payload. HELLO joiner -> host (every candidate, 4/s),
//    WELCOME host -> joiner, PUNCH host -> joiner's reply-code candidates (the joiner answers with HELLO), PING / PONG
//    every second (round trip, keeps NAT mappings open, 15 s silence = gone), SAY / ACK text (resent until acked), BYE.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#include <windows.h>

#include "core/net.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "core/log.h"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace {
constexpr uint16_t kBasePort = 27616;
constexpr int kPortTries = 16, kMaxJoiners = 3;
constexpr uint32_t kStunCookie = 0x2112A442;
constexpr DWORD kGatherMaxMs = 9000, kHelloForMs = 120000, kPunchForMs = 60000, kTimeoutMs = 15000;
const char* const kStunServers[][2] = {{"stun.l.google.com", "19302"}, {"stun.cloudflare.com", "3478"}};
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";   // Crockford base32

enum PType : uint8_t { HELLO = 1, WELCOME, PUNCH, PING, PONG, SAY, ACK, BYE, RAW };   // RAW: unreliable game data
enum CandType : uint8_t { kLan = 1, kMapped = 2, kStun = 3, kV6 = 4 };
enum CodeKind : uint8_t { kHostCode = 0xD1, kReplyCode = 0xD2 };
enum Flags : uint8_t { kFlagPcp = 1, kFlagNatPmp = 2, kFlagUpnp = 4, kFlagStrict = 8 };

// ---- addresses ----
struct Addr {
    int fam = 0;   // AF_INET / AF_INET6, 0 = none
    uint8_t ip[16]{};
    uint16_t port = 0;
    bool operator==(const Addr& o) const {
        return fam == o.fam && port == o.port && memcmp(ip, o.ip, fam == AF_INET ? 4 : 16) == 0;
    }
};

Addr FromSockaddr(const sockaddr* sa) {
    Addr a;
    if (sa->sa_family == AF_INET) {
        const auto* s = reinterpret_cast<const sockaddr_in*>(sa);
        a.fam = AF_INET;
        memcpy(a.ip, &s->sin_addr, 4);
        a.port = ntohs(s->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const auto* s = reinterpret_cast<const sockaddr_in6*>(sa);
        a.fam = AF_INET6;
        memcpy(a.ip, &s->sin6_addr, 16);
        a.port = ntohs(s->sin6_port);
    }
    return a;
}

int ToSockaddr(const Addr& a, sockaddr_storage& ss) {
    memset(&ss, 0, sizeof ss);
    if (a.fam == AF_INET) {
        auto* s = reinterpret_cast<sockaddr_in*>(&ss);
        s->sin_family = AF_INET;
        memcpy(&s->sin_addr, a.ip, 4);
        s->sin_port = htons(a.port);
        return sizeof(sockaddr_in);
    }
    auto* s = reinterpret_cast<sockaddr_in6*>(&ss);
    s->sin6_family = AF_INET6;
    memcpy(&s->sin6_addr, a.ip, 16);
    s->sin6_port = htons(a.port);
    return sizeof(sockaddr_in6);
}

std::string Str(const Addr& a) {
    if (!a.fam) return "-";
    char buf[INET6_ADDRSTRLEN] = {};
    inet_ntop(a.fam, a.ip, buf, sizeof buf);
    char out[80];
    snprintf(out, sizeof out, a.fam == AF_INET6 ? "[%s]:%u" : "%s:%u", buf, a.port);
    return out;
}

bool PrivateV4(const uint8_t* ip) {
    return ip[0] == 10 || ip[0] == 127 || (ip[0] == 172 && (ip[1] & 0xF0) == 16) || (ip[0] == 192 && ip[1] == 168) ||
           (ip[0] == 100 && (ip[1] & 0xC0) == 64) || (ip[0] == 169 && ip[1] == 254);   // + carrier NAT, link-local
}

void Put16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
void Put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }
uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

uint32_t Random32() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    static uint32_t s = GetCurrentProcessId() * 2654435761u ^ t.LowPart;
    s ^= t.LowPart + 0x9E3779B9u + (s << 6) + (s >> 2);
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    return s ? s : 1;
}

// ---- codes ----
struct Cand {
    uint8_t type;
    Addr a;
};

uint8_t Crc8(const uint8_t* p, size_t n) {
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; ++i) c = uint8_t(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

std::string Encode(std::vector<uint8_t> data) {
    data.push_back(Crc8(data.data(), data.size()));
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (uint8_t b : data) {
        acc = acc << 8 | b, bits += 8;
        while (bits >= 5) out += kAlphabet[(acc >> (bits -= 5)) & 31];
    }
    if (bits) out += kAlphabet[(acc << (5 - bits)) & 31];
    std::string grouped;
    for (size_t i = 0; i < out.size(); ++i) grouped += (i && i % 5 == 0 ? "-" : "") + std::string(1, out[i]);
    return grouped;
}

bool Decode(const std::string& code, std::vector<uint8_t>& data) {
    data.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : code) {
        if (c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        c = char(toupper(static_cast<unsigned char>(c)));
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        const char* p = strchr(kAlphabet, c);
        if (!p || !c) return false;
        acc = acc << 5 | uint32_t(p - kAlphabet), bits += 5;
        if (bits >= 8) data.push_back(uint8_t(acc >> (bits -= 8)));
    }
    if (data.size() < 2 || Crc8(data.data(), data.size() - 1) != data.back()) return false;
    data.pop_back();
    return true;
}

void PutCands(std::vector<uint8_t>& d, const std::vector<Cand>& cands) {
    for (const Cand& c : cands) {
        d.push_back(c.type);
        d.insert(d.end(), c.a.ip, c.a.ip + (c.a.fam == AF_INET ? 4 : 16));
        d.push_back(uint8_t(c.a.port >> 8)), d.push_back(uint8_t(c.a.port));
    }
}

bool GetCands(const std::vector<uint8_t>& d, size_t i, std::vector<Cand>& out) {
    while (i < d.size()) {
        Cand c{d[i++], {}};
        const size_t n = c.type == kV6 ? 16 : 4;
        if (c.type < kLan || c.type > kV6 || i + n + 2 > d.size()) return false;
        c.a.fam = c.type == kV6 ? AF_INET6 : AF_INET;
        memcpy(c.a.ip, &d[i], n), i += n;
        c.a.port = Get16(&d[i]), i += 2;
        out.push_back(c);
    }
    return true;
}

const char* CandName(uint8_t t) {
    return t == kLan ? "LAN" : t == kMapped ? "router port" : t == kStun ? "public (STUN)" : "IPv6";
}

bool PathAllows(net::Paths p, uint8_t type) {
    return p == net::Paths::All || (p == net::Paths::Lan && type == kLan) ||
           (p == net::Paths::Public && (type == kMapped || type == kStun)) || (p == net::Paths::V6 && type == kV6);
}

const char* MapName(net::Map m) {
    return m == net::Map::Pcp ? "PCP" : m == net::Map::NatPmp ? "NAT-PMP" : m == net::Map::Upnp ? "UPnP" : "none";
}

// ---- router port mapping (helper thread; blocking sockets with timeouts) ----
struct MapResult {
    net::Map how = net::Map::None;
    Addr ext;
    uint32_t life = 0;
    uint8_t pcpNonce[12]{};
    std::string control, service;   // UPnP
    std::vector<std::string> log;
};

void Note(MapResult& r, const char* fmt, ...) {
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    r.log.emplace_back(buf);
}

// one request to the gateway's port 5351, answer of at least `minLen` bytes (tries 250 / 500 / 1000 ms)
int GatewayAsk(const Addr& gw, const uint8_t* req, int len, uint8_t* resp, int cap, int minLen) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    Addr to = gw;
    to.port = 5351;
    sockaddr_storage ss;
    const int sl = ToSockaddr(to, ss);
    int got = 0;
    for (int wait : {250, 500, 1000}) {
        sendto(s, reinterpret_cast<const char*>(req), len, 0, reinterpret_cast<sockaddr*>(&ss), sl);
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(s, &fs);
        timeval tv{0, wait * 1000};
        if (select(0, &fs, nullptr, nullptr, &tv) > 0) {
            got = recv(s, reinterpret_cast<char*>(resp), cap, 0);
            if (got >= minLen || (got >= 4 && resp[0] == 0 && minLen == 60)) break;   // (PCP asked, NAT-PMP answered)
            got = 0;
        }
    }
    closesocket(s);
    return got > 0 ? got : 0;
}

bool TryPcp(const Addr& lan, const Addr& gw, uint16_t port, uint32_t life, MapResult& r, bool remove = false) {
    uint8_t q[60] = {}, a[1100];
    q[0] = 2, q[1] = 1;   // version 2, MAP request
    Put32(q + 4, life);
    q[18] = q[19] = 0xFF;   // client IP as IPv4-mapped IPv6
    memcpy(q + 20, lan.ip, 4);
    if (!remove)
        for (int i = 0; i < 12; i += 4) Put32(r.pcpNonce + i, Random32());
    memcpy(q + 24, r.pcpNonce, 12);
    q[36] = 17;   // UDP
    Put16(q + 40, port), Put16(q + 42, remove ? 0 : port);
    q[54] = q[55] = 0xFF;   // suggested external address ::ffff:0.0.0.0 = any
    const int n = GatewayAsk(gw, q, sizeof q, a, sizeof a, 60);
    if (remove) return n >= 60 && a[3] == 0;
    if (!n) return Note(r, "PCP: no answer from %s", Str(gw).c_str()), false;
    if (a[0] != 2) return Note(r, "PCP: not supported (router speaks version %u)", a[0]), false;
    if (n < 60 || a[1] != 0x81 || memcmp(a + 24, r.pcpNonce, 12) != 0) return Note(r, "PCP: bad answer"), false;
    if (a[3]) return Note(r, "PCP: refused (result %u)", a[3]), false;
    r.how = net::Map::Pcp;
    r.life = Get32(a + 4);
    r.ext.fam = AF_INET;
    memcpy(r.ext.ip, a + 56, 4);
    r.ext.port = Get16(a + 42);
    return true;
}

bool TryNatPmp(const Addr& gw, uint16_t port, uint32_t life, MapResult& r, bool remove = false) {
    uint8_t q[12] = {}, a[64];
    q[1] = 1;   // map UDP
    Put16(q + 4, port), Put16(q + 6, remove ? 0 : port);
    Put32(q + 8, life);
    int n = GatewayAsk(gw, q, sizeof q, a, sizeof a, 16);
    if (remove) return n >= 16 && Get16(a + 2) == 0;
    if (!n) return Note(r, "NAT-PMP: no answer from %s", Str(gw).c_str()), false;
    if (a[0] != 0 || a[1] != 129 || Get16(a + 2)) return Note(r, "NAT-PMP: refused (result %u)", Get16(a + 2)), false;
    r.ext.fam = AF_INET;
    r.ext.port = Get16(a + 10);
    r.life = Get32(a + 12);
    const uint8_t qa[2] = {0, 0};   // external address
    n = GatewayAsk(gw, qa, 2, a, sizeof a, 12);
    if (n < 12 || a[1] != 128 || Get16(a + 2)) return Note(r, "NAT-PMP: mapped, but no external address"), false;
    memcpy(r.ext.ip, a + 8, 4);
    r.how = net::Map::NatPmp;
    return true;
}

// -- UPnP: minimal HTTP over a TCP socket (Connection: close) --
bool ParseUrl(const std::string& url, Addr& host, std::string& path) {
    if (url.compare(0, 7, "http://") != 0) return false;
    const size_t slash = url.find('/', 7);
    std::string hp = url.substr(7, slash == std::string::npos ? std::string::npos : slash - 7);
    path = slash == std::string::npos ? "/" : url.substr(slash);
    uint16_t port = 80;
    if (const size_t c = hp.find(':'); c != std::string::npos) port = uint16_t(atoi(hp.c_str() + c + 1)), hp.resize(c);
    host = {};
    host.fam = AF_INET;
    host.port = port;
    return inet_pton(AF_INET, hp.c_str(), host.ip) == 1;
}

bool Http(const std::string& url, const char* method, const std::string& extra, const std::string& body, int& status,
          std::string& out) {
    Addr host;
    std::string path;
    if (!ParseUrl(url, host, path)) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sockaddr_storage ss;
    const int sl = ToSockaddr(host, ss);
    connect(s, reinterpret_cast<sockaddr*>(&ss), sl);
    fd_set fs;
    FD_ZERO(&fs);
    FD_SET(s, &fs);
    timeval tv{2, 0};
    if (select(0, nullptr, &fs, nullptr, &tv) <= 0) return closesocket(s), false;
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    DWORD to = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
    char hostHdr[64];
    snprintf(hostHdr, sizeof hostHdr, "%u.%u.%u.%u:%u", host.ip[0], host.ip[1], host.ip[2], host.ip[3], host.port);
    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: " + hostHdr + "\r\nConnection: close\r\n" + extra;
    if (!body.empty())
        req += "Content-Type: text/xml; charset=\"utf-8\"\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    send(s, req.data(), int(req.size()), 0);
    std::string resp;
    char buf[4096];
    for (int n; (n = recv(s, buf, sizeof buf, 0)) > 0;) resp.append(buf, n);
    closesocket(s);
    const size_t hdrEnd = resp.find("\r\n\r\n");
    if (resp.compare(0, 5, "HTTP/") != 0 || hdrEnd == std::string::npos) return false;
    status = atoi(resp.c_str() + resp.find(' ') + 1);
    std::string headers = resp.substr(0, hdrEnd);
    for (char& c : headers) c = char(tolower(static_cast<unsigned char>(c)));
    out = resp.substr(hdrEnd + 4);
    if (headers.find("transfer-encoding: chunked") != std::string::npos) {
        std::string plain;
        size_t i = 0;
        while (i < out.size()) {
            const size_t eol = out.find("\r\n", i);
            if (eol == std::string::npos) break;
            const size_t n = strtoul(out.c_str() + i, nullptr, 16);
            if (!n) break;
            plain.append(out, eol + 2, n);
            i = eol + 2 + n + 2;
        }
        out = plain;
    }
    return true;
}

std::string Tag(const std::string& xml, const char* tag, size_t from = 0, size_t to = std::string::npos) {
    const std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
    const size_t a = xml.find(open, from);
    if (a == std::string::npos || (to != std::string::npos && a >= to)) return {};
    const size_t b = xml.find(close, a);
    return b == std::string::npos ? std::string() : xml.substr(a + open.size(), b - a - open.size());
}

bool Soap(const MapResult& r, const char* action, const std::string& args, int& status, std::string& out) {
    const std::string body =
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + std::string(action) +
        " xmlns:u=\"" + r.service + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>";
    return Http(r.control, "POST", "SOAPAction: \"" + r.service + "#" + action + "\"\r\n", body, status, out);
}

bool TryUpnp(const Addr& lan, uint16_t port, uint32_t life, MapResult& r) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    Addr local = lan;
    local.port = 0;
    sockaddr_storage ss;
    bind(s, reinterpret_cast<sockaddr*>(&ss), ToSockaddr(local, ss));
    Addr mc{};
    mc.fam = AF_INET, mc.port = 1900;
    const uint8_t mcip[4] = {239, 255, 255, 250};
    memcpy(mc.ip, mcip, 4);
    const int ml = ToSockaddr(mc, ss);
    for (const char* st : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                           "urn:schemas-upnp-org:device:InternetGatewayDevice:2"}) {
        const std::string m = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") +
                              st + "\r\n\r\n";
        sendto(s, m.data(), int(m.size()), 0, reinterpret_cast<sockaddr*>(&ss), ml);
    }
    std::string location;
    const DWORD until = GetTickCount() + 2500;
    while (location.empty() && GetTickCount() < until) {
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(s, &fs);
        timeval tv{0, 250000};
        if (select(0, &fs, nullptr, nullptr, &tv) <= 0) continue;
        char buf[2048];
        const int n = recv(s, buf, sizeof buf - 1, 0);
        if (n <= 0) continue;
        buf[n] = 0;
        std::string reply(buf), lower(buf);
        for (char& c : lower) c = char(tolower(static_cast<unsigned char>(c)));
        const size_t l = lower.find("\nlocation:");
        if (l == std::string::npos) continue;
        size_t a = l + 10;
        while (a < reply.size() && reply[a] == ' ') ++a;
        location = reply.substr(a, reply.find("\r\n", a) - a);
    }
    closesocket(s);
    if (location.empty()) return Note(r, "UPnP: no router answered"), false;
    int status = 0;
    std::string desc;
    if (!Http(location, "GET", "", "", status, desc) || status != 200)
        return Note(r, "UPnP: can't read %s (HTTP %d)", location.c_str(), status), false;
    // the WAN connection service: WANIPConnection preferred, else WANPPPConnection
    for (const char* want : {"WANIPConnection", "WANPPPConnection"}) {
        for (size_t at = desc.find("<service>"); at != std::string::npos && r.control.empty(); at = desc.find("<service>", at + 9)) {
            const size_t end = desc.find("</service>", at);
            const std::string type = Tag(desc, "serviceType", at, end);
            if (type.find(want) == std::string::npos) continue;
            std::string ctl = Tag(desc, "controlURL", at, end);
            if (ctl.compare(0, 7, "http://") != 0) {
                std::string base = Tag(desc, "URLBase");
                if (base.empty()) base = location.substr(0, location.find('/', 7));
                while (!base.empty() && base.back() == '/') base.pop_back();
                ctl = base + (ctl.empty() || ctl[0] != '/' ? "/" : "") + ctl;
            }
            r.control = ctl, r.service = type;
        }
    }
    if (r.control.empty()) return Note(r, "UPnP: router has no WAN connection service"), false;
    char lanStr[16];
    snprintf(lanStr, sizeof lanStr, "%u.%u.%u.%u", lan.ip[0], lan.ip[1], lan.ip[2], lan.ip[3]);
    std::string out;
    for (uint32_t lease : {life, 0u}) {   // 725 OnlyPermanentLeasesSupported -> permanent (deleted on Stop)
        const std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
                                 "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + std::to_string(port) +
                                 "</NewInternalPort><NewInternalClient>" + lanStr +
                                 "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>Conflict Desert "
                                 "Storm co-op</NewPortMappingDescription><NewLeaseDuration>" + std::to_string(lease) +
                                 "</NewLeaseDuration>";
        if (!Soap(r, "AddPortMapping", args, status, out)) return Note(r, "UPnP: no answer from %s", r.control.c_str()), false;
        if (status == 200) {
            r.life = lease;
            break;
        }
        const std::string code = Tag(out, "errorCode");
        if (code != "725" || !lease) return Note(r, "UPnP: AddPortMapping refused (HTTP %d, error %s)", status, code.c_str()), false;
    }
    if (!Soap(r, "GetExternalIPAddress", "", status, out) || status != 200)
        return Note(r, "UPnP: mapped, but no external address"), false;
    const std::string ip = Tag(out, "NewExternalIPAddress");
    r.ext.fam = AF_INET;
    r.ext.port = port;
    if (inet_pton(AF_INET, ip.c_str(), r.ext.ip) != 1) return Note(r, "UPnP: bad external address \"%s\"", ip.c_str()), false;
    r.how = net::Map::Upnp;
    return true;
}

void UpnpRemove(const MapResult& r, uint16_t port) {
    int status = 0;
    std::string out;
    Soap(r, "DeletePortMapping",
         "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) + "</NewExternalPort><NewProtocol>UDP</NewProtocol>",
         status, out);
}

MapResult DoMap(net::Map mode, const Addr& lan, const Addr& gw, uint16_t port) {
    MapResult r;
    constexpr uint32_t kLife = 7200;
    if (!lan.fam) return Note(r, "no IPv4 route to the internet - no router port"), r;
    if ((mode == net::Map::Auto || mode == net::Map::Pcp) && gw.fam && TryPcp(lan, gw, port, kLife, r)) {
    } else if ((mode == net::Map::Auto || mode == net::Map::NatPmp) && gw.fam && TryNatPmp(gw, port, kLife, r)) {
    } else if ((mode == net::Map::Auto || mode == net::Map::Upnp) && TryUpnp(lan, port, kLife, r)) {
    }
    if (r.how != net::Map::None && PrivateV4(r.ext.ip)) {
        Note(r, "%s: the router's own address %s is private - it sits behind another router or carrier NAT, its port "
                "doesn't help", MapName(r.how), Str(r.ext).c_str());
        r.how = net::Map::None;
    }
    return r;
}

void DoUnmap(const MapResult& r, const Addr& lan, const Addr& gw, uint16_t port) {
    MapResult tmp = r;
    if (r.how == net::Map::Pcp) TryPcp(lan, gw, port, 0, tmp, true);
    else if (r.how == net::Map::NatPmp) TryNatPmp(gw, port, 0, tmp, true);
    else if (r.how == net::Map::Upnp) UpnpRemove(r, port);
}

// ---- session state (all under g_mx) ----
struct Peer {
    uint32_t nonce = 0;
    std::vector<Cand> cands;   // from a reply code (host side) / the join code (joiner side)
    bool confirmed = false, viaReply = false;
    Addr addr;
    std::string path;
    DWORD lastHeard = 0, lastPing = 0, punchUntil = 0, lastPunch = 0;
    uint32_t pingSeq = 0, sendSeq = 1, recvSeq = 0;
    int rtt = -1;
    struct Out {
        uint32_t seq;
        std::string text;
        DWORD last;
        int tries;
    };
    std::deque<Out> out;
};

struct Stun {
    Addr server;
    uint8_t tid[12];
    int tries = 0;
    DWORD last = 0;
    bool done = false;
    Addr mapped;
};

enum class Role { None, Host, Join };

std::mutex g_mx;
std::thread g_worker, g_helper;
std::atomic<bool> g_run{false}, g_helperBusy{false};
bool g_wsa = false;
Role g_role = Role::None;
SOCKET g_s4 = INVALID_SOCKET, g_s6 = INVALID_SOCKET;
uint16_t g_port = 0;
uint32_t g_session = 0, g_nonce = 0;
net::Map g_mapMode = net::Map::Auto;
net::Paths g_paths = net::Paths::All;
DWORD g_start = 0;
bool g_gatherDone = false, g_resolved = false;
Addr g_lan, g_gw;
std::vector<Addr> g_v6;
std::vector<Stun> g_stun;
MapResult g_map;
bool g_mapDone = false;
DWORD g_mapAt = 0;
std::vector<Peer> g_peers;   // host: joiners; joiner: [0] = the host
uint8_t g_hostFlags = 0;     // joiner: the host's code flags
DWORD g_lastHello = 0;
bool g_hello = true;         // joiner: sends hellos (off: only answers punches - tests)
bool g_gaveUp = false;
std::string g_code;
std::vector<net::Event> g_events;
std::deque<std::string> g_recent;   // last info lines for the menus
std::vector<net::Command> g_commands;
std::vector<net::Raw> g_raw;   // received RAW payloads (lockstep inputs ...)

void Emit(net::Event::Kind k, const char* fmt, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    dslog::Write("Net: %s", buf);
    g_events.push_back({k, buf});
    if (k == net::Event::Info || k == net::Event::Message) {
        g_recent.emplace_back(buf);
        while (g_recent.size() > 6) g_recent.pop_front();
    }
}

void SendTo(const Addr& to, const uint8_t* p, int n) {
    const SOCKET s = to.fam == AF_INET6 ? g_s6 : g_s4;
    if (s == INVALID_SOCKET) return;
    sockaddr_storage ss;
    const int sl = ToSockaddr(to, ss);
    sendto(s, reinterpret_cast<const char*>(p), n, 0, reinterpret_cast<sockaddr*>(&ss), sl);
}

void SendPacket(const Addr& to, PType t, uint32_t nonce, const void* payload = nullptr, int len = 0) {
    uint8_t p[1400] = {'D', 'S', 'F', t};
    Put32(p + 4, g_session), Put32(p + 8, nonce);
    if (len > int(sizeof p) - 12) len = int(sizeof p) - 12;
    if (len) memcpy(p + 12, payload, len);
    SendTo(to, p, 12 + len);
}

std::vector<Cand> OwnCands() {
    std::vector<Cand> c;
    auto add = [&](uint8_t type, const Addr& a) {
        if (!a.fam || !PathAllows(g_paths, type)) return;
        for (const Cand& x : c)
            if (x.a == a) return;
        c.push_back({type, a});
    };
    if (g_lan.fam) {
        Addr a = g_lan;
        a.port = g_port;
        add(kLan, a);
    }
    if (g_map.how != net::Map::None) add(kMapped, g_map.ext);
    for (const Stun& s : g_stun)
        if (s.done) add(kStun, s.mapped);
    for (Addr a : g_v6) {
        a.port = g_port;
        add(kV6, a);
        break;   // one is enough (codes stay short)
    }
    return c;
}

uint8_t OwnFlags() {
    uint8_t f = g_map.how == net::Map::Pcp ? kFlagPcp : g_map.how == net::Map::NatPmp ? kFlagNatPmp : g_map.how == net::Map::Upnp ? kFlagUpnp : 0;
    Addr first;
    for (const Stun& s : g_stun)
        if (s.done) {
            if (!first.fam) first = s.mapped;
            else if (!(first == s.mapped)) f |= kFlagStrict;
        }
    return f;
}

std::string MakeCode(bool reply) {
    std::vector<uint8_t> d = {uint8_t(reply ? kReplyCode : kHostCode), OwnFlags(), 0, 0, 0, 0};
    Put32(&d[2], g_session);
    if (reply) d.resize(10), Put32(&d[6], g_nonce);
    PutCands(d, OwnCands());
    return Encode(d);
}

std::string Describe(const std::vector<Cand>& cands) {
    std::string s;
    for (const Cand& c : cands) s += (s.empty() ? "" : ", ") + std::string(CandName(c.type)) + " " + Str(c.a);
    return s.empty() ? "no addresses" : s;
}

// what kind of route a confirmed peer's address is
std::string PathOf(const Addr& a, const Peer& p, bool host) {
    if (a.fam == AF_INET6) return "IPv6 (direct, no NAT)";
    if (a.ip[0] == 127) return "this PC (loopback)";
    if (PrivateV4(a.ip)) return "same network (LAN)";
    const uint8_t flags = host ? OwnFlags() : g_hostFlags;
    const char* how = flags & kFlagPcp ? "PCP" : flags & kFlagNatPmp ? "NAT-PMP" : flags & kFlagUpnp ? "UPnP" : nullptr;
    if (how && !(host && p.viaReply && g_map.how == net::Map::None))
        return std::string("internet - host's router port opened by ") + how;
    return "internet - hole punched";
}

void Confirm(Peer& p, const Addr& from, bool host) {
    p.confirmed = true;
    p.addr = from;
    p.lastHeard = p.lastPing = GetTickCount();
    p.path = PathOf(from, p, host);
    const int idx = int(&p - g_peers.data()) + 2;
    if (host) Emit(net::Event::Info, "player %d connected from %s - %s", idx, Str(from).c_str(), p.path.c_str());
    else Emit(net::Event::Info, "connected to the host at %s - %s", Str(from).c_str(), p.path.c_str());
}

Peer* PeerByNonce(uint32_t n) {
    for (Peer& p : g_peers)
        if (p.nonce == n) return &p;
    return nullptr;
}

void OnStun(const uint8_t* d, int n, const Addr& from) {
    for (Stun& s : g_stun) {
        if (s.done || !(s.server == from) || memcmp(d + 8, s.tid, 12) != 0) continue;
        for (int i = 20; i + 4 <= n;) {
            const uint16_t t = Get16(d + i), l = Get16(d + i + 2);
            const uint8_t* v = d + i + 4;
            if (i + 4 + l > n) break;
            if ((t == 0x0020 || t == 0x0001) && l >= 8 && v[1] == 1) {
                s.mapped.fam = AF_INET;
                s.mapped.port = Get16(v + 2);
                memcpy(s.mapped.ip, v + 4, 4);
                if (t == 0x0020) {
                    s.mapped.port ^= kStunCookie >> 16;
                    uint8_t c[4];
                    Put32(c, kStunCookie);
                    for (int k = 0; k < 4; ++k) s.mapped.ip[k] ^= c[k];
                }
                s.done = true;
                dslog::Write("Net: STUN %s says our public address is %s", Str(s.server).c_str(), Str(s.mapped).c_str());
                if (t == 0x0020) break;
            }
            i += 4 + ((l + 3) & ~3);
        }
    }
}

void OnPacket(const uint8_t* d, int n, const Addr& from) {
    if (n >= 20 && Get16(d) == 0x0101 && Get32(d + 4) == kStunCookie) return OnStun(d, n, from);
    if (n < 12 || d[0] != 'D' || d[1] != 'S' || d[2] != 'F' || Get32(d + 4) != g_session) return;
    const PType t = PType(d[3]);
    const uint32_t nonce = Get32(d + 8);
    const uint8_t* pl = d + 12;
    const int pn = n - 12;
    const DWORD now = GetTickCount();
    Peer* p = nullptr;
    if (g_role == Role::Host) {
        p = PeerByNonce(nonce);
        if (t == HELLO) {
            if (!p) {
                if (int(g_peers.size()) >= kMaxJoiners) return;
                g_peers.push_back({});
                p = &g_peers.back();
                p->nonce = nonce;
            }
            if (!p->confirmed) Confirm(*p, from, true);
            SendPacket(from, WELCOME, nonce);
        }
    } else if (g_role == Role::Join) {
        if (nonce != g_nonce || g_peers.empty()) return;
        p = &g_peers[0];
        if (t == WELCOME && !p->confirmed) Confirm(*p, from, false);
        if (t == PUNCH) {
            dslog::Write("Net: punch from %s - answering", Str(from).c_str());
            SendPacket(from, HELLO, g_nonce);
        }
    }
    if (!p || !p->confirmed) return;
    p->lastHeard = now;
    switch (t) {
        case PING:
            if (pn >= 8) SendPacket(from, PONG, nonce, pl, 8);
            break;
        case PONG:
            if (pn >= 8 && Get32(pl) == p->pingSeq) p->rtt = int(now - Get32(pl + 4));
            break;
        case SAY:
            if (pn >= 4) {
                // in order only: the next one is delivered, older ones (resent) acknowledged again, newer ones left
                // unacknowledged - they come again after the missing one
                const uint32_t seq = Get32(pl);
                if (seq > p->recvSeq + 1) break;
                uint8_t ack[4];
                memcpy(ack, pl, 4);
                SendPacket(from, ACK, nonce, ack, 4);
                if (seq == p->recvSeq + 1) {
                    p->recvSeq = seq;
                    const std::string text(reinterpret_cast<const char*>(pl + 4), pn - 4);
                    const int who = g_role == Role::Host ? int(p - g_peers.data()) + 2 : 1;
                    if (!text.empty() && text[0] == '\x01') {   // game command (net::SendCommand)
                        g_commands.push_back({who, text.substr(1)});
                        dslog::Write("Net: command from player %d: %s", who, text.c_str() + 1);
                    } else Emit(net::Event::Message, "%s: %s", who == 1 ? "host" : ("player " + std::to_string(who)).c_str(), text.c_str());
                }
            }
            break;
        case ACK:
            if (pn >= 4)
                for (auto it = p->out.begin(); it != p->out.end(); ++it)
                    if (it->seq == Get32(pl)) {
                        dslog::Write("Net: delivered \"%s\" (%d tries)", it->text.c_str(), it->tries);
                        p->out.erase(it);
                        break;
                    }
            break;
        case RAW:
            if (g_raw.size() < 4096)
                g_raw.push_back({g_role == Role::Host ? int(p - g_peers.data()) + 2 : 1, std::vector<uint8_t>(pl, pl + pn)});
            break;
        case BYE:
            Emit(net::Event::Info, g_role == Role::Host ? "player %d left" : "the host ended the session",
                  int(p - g_peers.data()) + 2);
            p->confirmed = false;
            p->nonce = 0;
            break;
        default: break;
    }
}

void StartHelper() {   // DNS for the STUN servers + router port (blocking work)
    if (g_helper.joinable()) g_helper.join();
    g_helperBusy = true;
    const Addr lan = g_lan, gw = g_gw;
    const uint16_t port = g_port;
    const net::Map mode = g_mapMode;
    const bool resolve = !g_resolved;
    g_helper = std::thread([=] {
        std::vector<Stun> stun;
        if (resolve)
            for (const auto& s : kStunServers) {
                addrinfo hints{}, *res = nullptr;
                hints.ai_family = AF_INET, hints.ai_socktype = SOCK_DGRAM;
                if (getaddrinfo(s[0], s[1], &hints, &res) == 0 && res) {
                    Stun st;
                    st.server = FromSockaddr(res->ai_addr);
                    for (int i = 0; i < 12; i += 4) Put32(st.tid + i, Random32());
                    stun.push_back(st);
                    freeaddrinfo(res);
                } else dslog::Write("Net: can't resolve STUN server %s", s[0]);
            }
        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (resolve) g_stun = stun, g_resolved = true;
        }
        MapResult r = mode == net::Map::None ? MapResult{} : DoMap(mode, lan, gw, port);
        if (mode == net::Map::None) Note(r, "router port: off (none / noport)");
        std::lock_guard<std::mutex> lk(g_mx);
        for (const std::string& l : r.log) dslog::Write("Net: %s", l.c_str());
        if (g_map.how != net::Map::None && r.how == net::Map::None && g_mapDone) {
            Emit(net::Event::Info, "router port renewal failed - keeping the old one");
        } else {
            g_map = r;
            if (g_mapDone && r.how != net::Map::None) dslog::Write("Net: router port renewed (%s)", MapName(r.how));
        }
        if (!g_mapDone)
            for (const std::string& l : r.log) g_events.push_back({net::Event::Info, l});
        g_mapDone = true;
        g_mapAt = GetTickCount();
        g_helperBusy = false;
    });
}

void Tick() {
    const DWORD now = GetTickCount();
    // STUN requests (4 tries, 500 ms apart)
    for (Stun& s : g_stun)
        if (!s.done && s.tries < 4 && now - s.last >= 500) {
            uint8_t q[20];
            Put16(q, 1), Put16(q + 2, 0), Put32(q + 4, kStunCookie);
            memcpy(q + 8, s.tid, 12);
            SendTo(s.server, q, 20);
            s.last = now, ++s.tries;
        }
    if (!g_gatherDone) {
        bool stunDone = g_resolved;
        for (const Stun& s : g_stun) stunDone &= s.done || (s.tries >= 4 && now - s.last >= 500);
        if ((stunDone && g_mapDone) || now - g_start > kGatherMaxMs) {
            g_gatherDone = true;
            const uint8_t f = OwnFlags();
            int stunOk = 0;
            for (const Stun& s : g_stun) stunOk += s.done;
            if (!stunOk) Emit(net::Event::Info, "no answer from the STUN servers - public address unknown (offline?)");
            if (f & kFlagStrict)
                Emit(net::Event::Info, "your router uses a different public port per destination (strict NAT): hole "
                                        "punching may fail, a router port or IPv6 is needed");
            if (g_map.how != net::Map::None)
                Emit(net::Event::Info, "router port opened by %s: %s (%u s)", MapName(g_map.how), Str(g_map.ext).c_str(), g_map.life);
            else Emit(net::Event::Info, "router port: not opened");
            if (g_role == Role::Host) {
                g_code = MakeCode(false);
                Emit(net::Event::Info, "your addresses: %s", Describe(OwnCands()).c_str());
                Emit(net::Event::Code, "%s", g_code.c_str());
            } else {
                g_code = MakeCode(true);
                Emit(net::Event::Info, "your addresses: %s", Describe(OwnCands()).c_str());
                if (g_peers.empty() || !g_peers[0].confirmed) Emit(net::Event::ReplyCode, "%s", g_code.c_str());
            }
        }
    }
    // router port renewal at half its lifetime (0 = permanent)
    if (g_mapDone && !g_helperBusy && g_map.how != net::Map::None && g_map.life && now - g_mapAt > g_map.life * 500)
        StartHelper();
    if (g_role == Role::Join && !g_peers.empty()) {
        Peer& h = g_peers[0];
        if (!h.confirmed && !g_gaveUp) {
            if (now - g_start > kHelloForMs) {
                g_gaveUp = true;
                Emit(net::Event::Info, "could not reach the host - 'net stop', then try again (or a virtual LAN / Steam)");
            } else if (g_hello && now - g_lastHello >= 250) {
                g_lastHello = now;
                for (const Cand& c : h.cands)
                    if (PathAllows(g_paths, c.type)) SendPacket(c.a, HELLO, g_nonce);
            }
        }
    }
    for (Peer& p : g_peers) {
        if (!p.nonce) continue;
        if (!p.confirmed) {
            if (g_role == Role::Host && p.punchUntil && int(p.punchUntil - now) > 0 && now - p.lastPunch >= 250) {
                p.lastPunch = now;
                for (const Cand& c : p.cands) SendPacket(c.a, PUNCH, p.nonce);
            }
            continue;
        }
        if (now - p.lastHeard > kTimeoutMs) {
            Emit(net::Event::Info, g_role == Role::Host ? "player %d: no answer for 15 s - dropped" : "the host stopped answering",
                  int(&p - g_peers.data()) + 2);
            p.confirmed = false;
            p.nonce = 0;
            continue;
        }
        if (now - p.lastPing >= 1000) {
            p.lastPing = now;
            uint8_t ping[8];
            Put32(ping, ++p.pingSeq), Put32(ping + 4, now);
            SendPacket(p.addr, PING, p.nonce, ping, 8);
        }
        for (auto it = p.out.begin(); it != p.out.end();) {
            if (now - it->last < 300) {
                ++it;
                continue;
            }
            if (++it->tries > 20) {
                Emit(net::Event::Info, "message not delivered: %s", it->text.c_str());
                it = p.out.erase(it);
                continue;
            }
            it->last = now;
            uint8_t buf[260];
            Put32(buf, it->seq);
            const int n = int(std::min<size_t>(it->text.size(), 250));
            memcpy(buf + 4, it->text.data(), n);
            SendPacket(p.addr, SAY, p.nonce, buf, 4 + n);
            ++it;
        }
    }
    g_peers.erase(std::remove_if(g_peers.begin(), g_peers.end(), [](const Peer& p) { return !p.nonce; }), g_peers.end());
    if (g_role == Role::Join && g_peers.empty()) {
        g_role = Role::None;   // the worker ends; Stop() tidies up
        Emit(net::Event::Info, "session over - 'net stop' closes the router port");
    }
}

void Worker() {
    while (g_run) {
        fd_set fs;
        FD_ZERO(&fs);
        if (g_s4 != INVALID_SOCKET) FD_SET(g_s4, &fs);
        if (g_s6 != INVALID_SOCKET) FD_SET(g_s6, &fs);
        timeval tv{0, 20000};
        const int r = select(0, &fs, nullptr, nullptr, &tv);
        std::lock_guard<std::mutex> lk(g_mx);
        if (r > 0)
            for (SOCKET s : {g_s4, g_s6}) {
                if (s == INVALID_SOCKET || !FD_ISSET(s, &fs)) continue;
                for (int k = 0; k < 64; ++k) {
                    uint8_t buf[1500];
                    sockaddr_storage from;
                    int fl = sizeof from;
                    const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
                    if (n <= 0) break;
                    Addr a = FromSockaddr(reinterpret_cast<sockaddr*>(&from));
                    OnPacket(buf, n, a);
                }
            }
        if (g_role != Role::None) Tick();
    }
}

// own addresses: the LAN address of the route to the internet, its gateway, global IPv6 addresses
void FindAddresses() {
    g_lan = g_gw = {};
    g_v6.clear();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        Addr pub{};
        pub.fam = AF_INET, pub.port = 53;
        const uint8_t ip[4] = {8, 8, 8, 8};
        memcpy(pub.ip, ip, 4);
        sockaddr_storage ss;
        if (connect(s, reinterpret_cast<sockaddr*>(&ss), ToSockaddr(pub, ss)) == 0) {
            sockaddr_storage me;
            int ml = sizeof me;
            if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &ml) == 0) g_lan = FromSockaddr(reinterpret_cast<sockaddr*>(&me));
            g_lan.port = 0;
        }
        closesocket(s);
    }
    ULONG size = 32768;
    std::vector<uint8_t> buf(size);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST, nullptr, aa,
                             &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST, nullptr,
                                 aa, &size) != NO_ERROR)
            aa = nullptr;
    }
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || !a->FirstGatewayAddress) continue;
        bool hasLan = false;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const Addr x = FromSockaddr(u->Address.lpSockaddr);
            if (x.fam == AF_INET && g_lan.fam && memcmp(x.ip, g_lan.ip, 4) == 0) hasLan = true;
            if (x.fam == AF_INET6 && (x.ip[0] & 0xE0) == 0x20 && u->DadState == IpDadStatePreferred &&
                u->SuffixOrigin != IpSuffixOriginRandom)
                g_v6.push_back(x);
        }
        if (hasLan)
            for (auto* g = a->FirstGatewayAddress; g; g = g->Next)
                if (g->Address.lpSockaddr->sa_family == AF_INET) {
                    g_gw = FromSockaddr(g->Address.lpSockaddr);
                    break;
                }
    }
}

SOCKET OpenUdp(int fam, uint16_t port) {
    SOCKET s = socket(fam, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    if (fam == AF_INET6) {
        DWORD v6only = 1;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof v6only);
    }
    BOOL excl = TRUE;   // a second copy of the game must not share the port
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
    Addr any{};
    any.fam = fam, any.port = port;
    sockaddr_storage ss;
    if (bind(s, reinterpret_cast<sockaddr*>(&ss), ToSockaddr(any, ss)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    BOOL noReset = FALSE;   // an ICMP "port unreachable" would otherwise fail the next recvfrom (WSAECONNRESET)
    DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &noReset, sizeof noReset, nullptr, 0, &ret, nullptr, nullptr);
    return s;
}

// caller holds g_mx; opens the port and starts gathering + the worker
bool Begin(Role role) {
    if (!g_wsa) {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return Emit(net::Event::Info, "Winsock didn't start"), false;
        g_wsa = true;
    }
    for (int i = 0; i < kPortTries && g_s4 == INVALID_SOCKET; ++i) {
        g_s4 = OpenUdp(AF_INET, uint16_t(kBasePort + i));
        if (g_s4 != INVALID_SOCKET) g_port = uint16_t(kBasePort + i);
    }
    if (g_s4 == INVALID_SOCKET) return Emit(net::Event::Info, "no free UDP port %u-%u", kBasePort, kBasePort + kPortTries - 1), false;
    g_s6 = OpenUdp(AF_INET6, g_port);
    FindAddresses();
    g_role = role;
    g_start = GetTickCount();
    g_gatherDone = g_mapDone = g_resolved = g_gaveUp = false;
    g_map = {};
    g_stun.clear();
    g_code.clear();
    g_lastHello = 0;
    Emit(net::Event::Info, "UDP port %u (IPv6 %s), LAN %s, router %s", g_port, g_s6 != INVALID_SOCKET ? "too" : "off",
          g_lan.fam ? Str(g_lan).substr(0, Str(g_lan).find(':')).c_str() : "-", g_gw.fam ? Str(g_gw).substr(0, Str(g_gw).find(':')).c_str() : "-");
    StartHelper();
    g_run = true;
    g_worker = std::thread(Worker);
    return true;
}
}  // namespace

bool net::Host(Map map) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_role != Role::None || g_run) return Emit(net::Event::Info, "already in a session - 'net stop' first"), false;
    g_mapMode = map;
    g_paths = Paths::All;
    g_session = Random32();
    g_nonce = 0;
    g_peers.clear();
    if (!Begin(Role::Host)) return false;
    Emit(net::Event::Info, "hosting - finding your addresses%s ...", map == Map::Auto ? " and opening a router port" : "");
    return true;
}

bool net::Join(const std::string& code, Paths paths, bool openPort, bool hello) {
    std::vector<uint8_t> d;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!Decode(code, d)) return Emit(net::Event::Info, "that code isn't valid (typo?)"), false;
    if (d[0] == kReplyCode) return Emit(net::Event::Info, "that is a reply code - the host enters it with 'net reply'"), false;
    std::vector<Cand> cands;
    if (d.size() < 6 || d[0] != kHostCode || !GetCands(d, 6, cands)) return Emit(net::Event::Info, "that code isn't a join code"), false;
    if (g_role != Role::None || g_run) return Emit(net::Event::Info, "already in a session - 'net stop' first"), false;
    int usable = 0;
    for (const Cand& c : cands) usable += PathAllows(paths, c.type);
    if (!usable) return Emit(net::Event::Info, "the code has no address of that kind (%s)", Describe(cands).c_str()), false;
    g_session = Get32(&d[2]);
    g_hostFlags = d[1];
    g_nonce = Random32();
    g_paths = paths;
    g_mapMode = openPort ? Map::Auto : Map::None;
    g_hello = hello;
    g_peers.assign(1, {});
    g_peers[0].nonce = g_nonce;
    g_peers[0].cands = cands;
    if (!Begin(Role::Join)) return g_peers.clear(), false;
    Emit(net::Event::Info, "joining - host's addresses: %s%s", Describe(cands).c_str(),
          paths == Paths::All ? "" : paths == Paths::Lan ? " (LAN only)" : paths == Paths::Public ? " (public only)" : " (IPv6 only)");
    return true;
}

bool net::AddReplyCode(const std::string& code) {
    std::vector<uint8_t> d;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!Decode(code, d)) return Emit(net::Event::Info, "that code isn't valid (typo?)"), false;
    std::vector<Cand> cands;
    if (g_role != Role::Host) return Emit(net::Event::Info, "only the host enters reply codes ('net host' first)"), false;
    if (d.size() < 10 || d[0] != kReplyCode || !GetCands(d, 10, cands))
        return Emit(net::Event::Info, "that isn't a reply code"), false;
    if (Get32(&d[2]) != g_session) return Emit(net::Event::Info, "that reply code belongs to another session"), false;
    const uint32_t nonce = Get32(&d[6]);
    Peer* p = PeerByNonce(nonce);
    if (p && p->confirmed) return Emit(net::Event::Info, "that player is already connected"), true;
    if (!p) {
        if (int(g_peers.size()) >= kMaxJoiners) return Emit(net::Event::Info, "the session is full"), false;
        g_peers.push_back({});
        p = &g_peers.back();
        p->nonce = nonce;
    }
    p->cands = cands;
    p->viaReply = true;
    p->punchUntil = GetTickCount() + kPunchForMs;
    Emit(net::Event::Info, "punching towards %s ...", Describe(cands).c_str());
    return true;
}

int net::SendRaw(const void* data, int len) {
    std::lock_guard<std::mutex> lk(g_mx);
    int n = 0;
    for (const Peer& p : g_peers)
        if (p.confirmed) SendPacket(p.addr, RAW, p.nonce, data, len), ++n;
    return n;
}

std::vector<net::Raw> net::TakeRaw() {
    std::lock_guard<std::mutex> lk(g_mx);
    std::vector<Raw> r;
    r.swap(g_raw);
    return r;
}

int net::LocalPlayer() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_role == Role::Host ? 1 : g_role == Role::Join ? 2 : 0;
}

bool net::SendCommand(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mx);
    int n = 0;
    for (Peer& p : g_peers)
        if (p.confirmed) p.out.push_back({p.sendSeq++, "\x01" + text.substr(0, 249), 0, 0}), ++n;
    dslog::Write("Net: command to %d player(s): %s", n, text.c_str());
    return n > 0;
}

namespace {
Peer* PeerOfPlayer(int player) {   // under g_mx
    if (g_role == Role::Join) return player == 1 && !g_peers.empty() ? &g_peers[0] : nullptr;
    if (g_role == Role::Host && player >= 2 && player - 2 < int(g_peers.size())) return &g_peers[player - 2];
    return nullptr;
}
}  // namespace

bool net::SendCommandTo(int player, const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mx);
    Peer* p = PeerOfPlayer(player);
    if (!p || !p->confirmed) return false;
    p->out.push_back({p->sendSeq++, "\x01" + text.substr(0, 249), 0, 0});
    dslog::Write("Net: command to player %d: %s", player, text.c_str());
    return true;
}

bool net::Drop(int player) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_role != Role::Host) return false;
    Peer* p = PeerOfPlayer(player);
    if (!p || !p->nonce) return false;
    if (p->confirmed)
        for (int k = 0; k < 2; ++k) SendPacket(p->addr, BYE, p->nonce);
    Emit(net::Event::Info, "player %d dropped", player);
    p->confirmed = false;
    p->nonce = 0;   // the worker removes it
    return true;
}

int net::Unacked(int player) {
    std::lock_guard<std::mutex> lk(g_mx);
    Peer* p = PeerOfPlayer(player);
    return p ? int(p->out.size()) : 0;
}

std::vector<net::Command> net::TakeCommands() {
    std::lock_guard<std::mutex> lk(g_mx);
    std::vector<Command> c;
    c.swap(g_commands);
    return c;
}

bool net::Say(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mx);
    int n = 0;
    for (Peer& p : g_peers)
        if (p.confirmed) p.out.push_back({p.sendSeq++, text.substr(0, 250), 0, 0}), ++n;
    if (!n) Emit(net::Event::Info, "nobody connected");
    return n > 0;
}

void net::Stop() {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_run && g_role == Role::None && g_s4 == INVALID_SOCKET) return;
        for (int k = 0; k < 2; ++k)
            for (const Peer& p : g_peers)
                if (p.confirmed) SendPacket(p.addr, BYE, p.nonce);
        g_run = false;
    }
    if (g_worker.joinable()) g_worker.join();
    if (g_helper.joinable()) g_helper.join();
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_map.how != Map::None) {
        DoUnmap(g_map, g_lan, g_gw, g_port);
        dslog::Write("Net: router port closed (%s)", MapName(g_map.how));
    }
    g_map = {};
    for (SOCKET* s : {&g_s4, &g_s6})
        if (*s != INVALID_SOCKET) closesocket(*s), *s = INVALID_SOCKET;
    g_role = Role::None;
    g_peers.clear();
    g_code.clear();
    g_commands.clear();
    g_raw.clear();
    Emit(net::Event::Info, "stopped");
    g_recent.clear();
}

bool net::Active() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_run;
}

std::vector<std::string> net::Status() {
    std::lock_guard<std::mutex> lk(g_mx);
    std::vector<std::string> out;
    if (!g_run) return {"not in a session - 'net host' or 'net join <code>'"};
    char buf[400];
    snprintf(buf, sizeof buf, "%s, UDP port %u, %s", g_role == Role::Host ? "hosting" : g_role == Role::Join ? "joined" : "ended",
             g_port, g_gatherDone ? "addresses found" : "still finding addresses");
    out.push_back(buf);
    out.push_back("your addresses: " + Describe(OwnCands()));
    out.push_back(std::string("router port: ") + (g_map.how != Map::None ? MapName(g_map.how) + std::string(" ") + Str(g_map.ext) : "none") +
                  ((OwnFlags() & kFlagStrict) ? ", strict NAT" : ""));
    if (!g_code.empty()) out.push_back((g_role == Role::Host ? "join code: " : "reply code: ") + g_code);
    for (const Peer& p : g_peers) {
        const std::string who = g_role == Role::Host ? "player " + std::to_string(int(&p - g_peers.data()) + 2) : "host";
        if (p.confirmed)
            snprintf(buf, sizeof buf, "%s: %s - %s, ping %d ms", who.c_str(), Str(p.addr).c_str(), p.path.c_str(), p.rtt);
        else snprintf(buf, sizeof buf, "%s: not connected yet%s", who.c_str(), p.punchUntil ? " (punching)" : "");
        out.push_back(buf);
    }
    return out;
}

std::vector<net::Event> net::TakeEvents() {
    std::lock_guard<std::mutex> lk(g_mx);
    std::vector<Event> e;
    e.swap(g_events);
    return e;
}

net::Snapshot net::Snap() {
    std::lock_guard<std::mutex> lk(g_mx);
    Snapshot s;
    if (!g_run) return s;
    s.role = g_role == Role::Host ? Snapshot::Role::Host : g_role == Role::Join ? Snapshot::Role::Join : Snapshot::Role::None;
    s.gathered = g_gatherDone;
    s.code = g_code;
    if (g_map.how != Map::None) s.routerPort = std::string(MapName(g_map.how)) + " " + Str(g_map.ext);
    s.strictNat = (OwnFlags() & kFlagStrict) != 0;
    for (const Peer& p : g_peers) s.peers.push_back({p.nonce, p.confirmed, p.path, p.rtt, p.punchUntil && !p.confirmed});
    s.recent.assign(g_recent.begin(), g_recent.end());
    return s;
}

bool net::IsJoinCode(const std::string& text) {
    std::vector<uint8_t> d;
    std::vector<Cand> cands;
    return Decode(text, d) && d.size() >= 6 && d[0] == kHostCode && GetCands(d, 6, cands) && !cands.empty();
}
