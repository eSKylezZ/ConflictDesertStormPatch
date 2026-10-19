// pse.serialization + pse::cosave: the "<save>.pse" co-save next to each game save.
//
// File: "PSE\x01", u32 chunk count, chunks {u8 name length, name (plugin, no NUL), u32 tag, u32 version, u32 length,
// data}; little endian. Chunks of plugins that aren't loaded are carried over on rewrite - from the save that was
// loaded this session (even if its file is gone by now), else from the file being overwritten.
#include <windows.h>

#include <cstring>
#include <map>
#include <set>

#include "internal.h"

struct CoChunk {
    std::string plugin;
    uint32_t tag = 0, version = 0;
    std::vector<uint8_t> data;
};

struct PSESaveWriter {
    PSEPluginHandle plugin;
    std::vector<CoChunk>* out;
};

struct PSESaveReader {
    std::vector<const CoChunk*> chunks;
    size_t next = 0;
    const CoChunk* current = nullptr;
    size_t pos = 0;
};

namespace pse {
namespace {

struct Callbacks {
    PSESaveCallback save = nullptr;
    PSELoadCallback load = nullptr;
    PSERevertCallback revert = nullptr;
    void* ctx = nullptr;
};

std::map<PSEPluginHandle, Callbacks> g_callbacks;
std::vector<CoChunk> g_unknown;   // chunks of plugins not loaded, from the last Load / Save of this session

constexpr uint8_t kMagic[4] = {'P', 'S', 'E', 1};

void Warn(const std::string& text) { LogLine(detail::LoaderHandle(), PSE_LOG_WARN, text.c_str()); }

enum class ReadResult { Ok, Missing, Bad };

ReadResult ReadFileBytes(const char* path, std::vector<uint8_t>* out) {
    HANDLE h = CreateFileW(detail::Widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ReadResult::Missing : ReadResult::Bad;
    }
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < 0x40000000;   // 1 GB: no sane co-save is near that
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out->empty() || (ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) &&
                              got == out->size());
    }
    CloseHandle(h);
    return ok ? ReadResult::Ok : ReadResult::Bad;
}

// Strict: any inconsistency (truncation, trailing bytes, counts) = the whole file is rejected.
bool Parse(const std::vector<uint8_t>& b, std::vector<CoChunk>* out) {
    size_t p = 0;
    auto u32 = [&](uint32_t* v) {
        if (b.size() - p < 4) return false;
        std::memcpy(v, &b[p], 4);
        p += 4;
        return true;
    };
    if (b.size() < 8 || std::memcmp(b.data(), kMagic, 4) != 0) return false;
    p = 4;
    uint32_t count;
    u32(&count);
    for (uint32_t i = 0; i < count; ++i) {
        if (p >= b.size()) return false;
        uint8_t nameLen = b[p++];
        if (b.size() - p < nameLen) return false;
        CoChunk c;
        c.plugin.assign(reinterpret_cast<const char*>(&b[p]), nameLen);
        p += nameLen;
        uint32_t len;
        if (!u32(&c.tag) || !u32(&c.version) || !u32(&len) || b.size() - p < len) return false;
        c.data.assign(b.begin() + static_cast<ptrdiff_t>(p), b.begin() + static_cast<ptrdiff_t>(p + len));
        p += len;
        out->push_back(std::move(c));
    }
    return p == b.size();
}

void Put32(std::vector<uint8_t>& b, uint32_t v) {
    uint8_t x[4];
    std::memcpy(x, &v, 4);
    b.insert(b.end(), x, x + 4);
}

bool WriteAtomic(const char* path, const std::vector<CoChunk>& chunks) {
    std::vector<uint8_t> b(kMagic, kMagic + 4);
    Put32(b, static_cast<uint32_t>(chunks.size()));
    for (const CoChunk& c : chunks) {
        b.push_back(static_cast<uint8_t>(c.plugin.size()));
        b.insert(b.end(), c.plugin.begin(), c.plugin.end());
        Put32(b, c.tag);
        Put32(b, c.version);
        Put32(b, static_cast<uint32_t>(c.data.size()));
        b.insert(b.end(), c.data.begin(), c.data.end());
    }
    std::wstring target = detail::Widen(path), temp = target + L".tmp";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &put, nullptr) && put == b.size();
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    // Replace in one step: a crash mid-save leaves the old co-save (or a stray .tmp), never half a file.
    if (ok) ok = MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}

// Every plugin that set callbacks, in plugin order.
std::vector<std::pair<PSEPluginHandle, Callbacks>> Participants() {
    std::vector<std::pair<PSEPluginHandle, Callbacks>> out;
    for (PSEPluginHandle h : detail::PluginsInOrder()) {
        auto it = g_callbacks.find(h);
        if (it != g_callbacks.end()) out.emplace_back(h, it->second);
    }
    return out;
}

void RevertAll() {
    for (auto& [h, cb] : Participants())
        if (cb.revert) detail::CallRevert(h, cb.revert, cb.ctx);   // guard.cpp: external plugins under SEH
}

PSEResult PSE_CALL SetCallbacks(PSEPluginHandle plugin, PSESaveCallback save, PSELoadCallback load,
                                PSERevertCallback revert, void* ctx) {
    if (!detail::ValidPlugin(plugin)) return PSE_E_INVALID_ARG;
    g_callbacks[plugin] = {save, load, revert, ctx};
    return PSE_OK;
}

PSEResult PSE_CALL WriteChunk(PSESaveWriter* w, uint32_t tag, uint32_t version, const void* data, uint32_t len) {
    if (!w || !w->out || (len && !data)) return PSE_E_INVALID_ARG;
    CoChunk c;
    c.plugin = PluginName(w->plugin);
    c.tag = tag;
    c.version = version;
    if (len) c.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);
    w->out->push_back(std::move(c));
    return PSE_OK;
}

int PSE_CALL NextChunk(PSESaveReader* r, uint32_t* tag, uint32_t* version, uint32_t* len) {
    if (!r || r->next >= r->chunks.size()) {
        if (r) r->current = nullptr;
        return 0;
    }
    r->current = r->chunks[r->next++];
    r->pos = 0;
    if (tag) *tag = r->current->tag;
    if (version) *version = r->current->version;
    if (len) *len = static_cast<uint32_t>(r->current->data.size());
    return 1;
}

uint32_t PSE_CALL Read(PSESaveReader* r, void* buf, uint32_t len) {
    if (!r || !r->current || (!buf && len)) return 0;
    size_t left = r->current->data.size() - r->pos;
    size_t n = len < left ? len : left;
    if (n) std::memcpy(buf, r->current->data.data() + r->pos, n);
    r->pos += n;
    return static_cast<uint32_t>(n);
}

const PSESerializationInterface kTable = {sizeof(PSESerializationInterface), 1, &SetCallbacks, &WriteChunk,
                                          &NextChunk, &Read};

}  // namespace

namespace detail {
void ForgetCosave(PSEPluginHandle plugin) { g_callbacks.erase(plugin); }

void ResetCosave() {
    g_callbacks.clear();
    g_unknown.clear();
}
}  // namespace detail

namespace cosave {

bool Save(const char* coSavePath) {
    if (!coSavePath || !*coSavePath) return false;
    // Unknown plugins' chunks: this session's (from the loaded save) win per plugin, else the overwritten file's.
    std::vector<CoChunk> keep;
    std::set<std::string> fromSession;
    for (const CoChunk& c : g_unknown) {
        if (FindPlugin(c.plugin.c_str())) continue;   // loaded since: it writes its own now
        fromSession.insert(c.plugin);
        keep.push_back(c);
    }
    std::vector<uint8_t> bytes;
    ReadResult rr = ReadFileBytes(coSavePath, &bytes);
    if (rr == ReadResult::Ok) {
        std::vector<CoChunk> old;
        if (Parse(bytes, &old)) {
            for (CoChunk& c : old)
                if (!FindPlugin(c.plugin.c_str()) && !fromSession.count(c.plugin)) keep.push_back(std::move(c));
        } else {
            Warn(std::string("co-save ") + coSavePath + " is damaged - overwriting it");
        }
    } else if (rr == ReadResult::Bad) {
        Warn(std::string("co-save ") + coSavePath + " couldn't be read - overwriting it");
    }

    std::vector<CoChunk> out;
    for (auto& [h, cb] : Participants()) {
        if (!cb.save) continue;
        PSESaveWriter w{h, &out};
        detail::CallSave(h, cb.save, &w, cb.ctx);
    }
    out.insert(out.end(), keep.begin(), keep.end());
    if (!WriteAtomic(coSavePath, out)) {
        Warn(std::string("couldn't write co-save ") + coSavePath);
        return false;
    }
    g_unknown = std::move(keep);
    return true;
}

namespace {
// Hands each installed plugin its chunks (revert for those without any); keeps the rest as "not loaded" chunks.
void Distribute(const std::vector<CoChunk>& chunks) {
    g_unknown.clear();
    for (const CoChunk& c : chunks)
        if (!FindPlugin(c.plugin.c_str())) g_unknown.push_back(c);

    for (auto& [h, cb] : Participants()) {
        PSESaveReader r;
        std::string name = PluginName(h);
        for (const CoChunk& c : chunks)
            if (c.plugin == name) r.chunks.push_back(&c);
        if (r.chunks.empty() || !cb.load) {
            if (cb.revert) detail::CallRevert(h, cb.revert, cb.ctx);
        } else {
            detail::CallLoad(h, cb.load, &r, cb.ctx);
        }
    }
}
}  // namespace

bool Load(const char* coSavePath) {
    std::vector<CoChunk> chunks;
    bool loaded = false;
    std::vector<uint8_t> bytes;
    ReadResult rr = coSavePath && *coSavePath ? ReadFileBytes(coSavePath, &bytes) : ReadResult::Missing;
    if (rr == ReadResult::Ok) {
        loaded = Parse(bytes, &chunks);
        if (!loaded) {
            Warn(std::string("co-save ") + coSavePath + " is damaged - ignored (plugins start fresh)");
            chunks.clear();
        }
    } else if (rr == ReadResult::Bad) {
        Warn(std::string("co-save ") + coSavePath + " couldn't be read - ignored (plugins start fresh)");
    }
    Distribute(chunks);
    return loaded;
}

bool LoadBytes(const void* data, size_t len, const char* label) {
    std::vector<CoChunk> chunks;
    std::vector<uint8_t> bytes;
    if (data && len) bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);
    const bool loaded = Parse(bytes, &chunks);
    if (!loaded) {
        Warn(std::string("co-save image ") + (label ? label : "(memory)") + " is damaged - ignored (plugins start fresh)");
        chunks.clear();
    }
    Distribute(chunks);
    return loaded;
}

void NewGame() {
    g_unknown.clear();
    RevertAll();
}

}  // namespace cosave

const PSESerializationInterface* SerializationInterface() { return &kTable; }

}  // namespace pse
