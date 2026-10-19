// DesertStormFix Patcher: turns the Large Address Aware flag of DesertStorm.exe on or off. With it, the 32-bit game
// gets 4 GB of address space on 64-bit Windows instead of 2 GB. Windows reads the flag from the exe header when it
// creates the process, so the plugin (dinput8.dll, loaded later) can't set it - this tool changes that one bit in the
// file on disk, nothing else, after checking the exe is the known Steam / GOG build. A backup of the original is kept
// next to it (DesertStorm.exe.original) and turning it off restores the original bytes exactly.
//
// Without arguments: a window listing every install found (own folder, game registry key, GOG, Steam libraries).
// Command line (for scripts, and the elevated helper run): /status | /enable | /disable [/force] [folder...]
//   exit codes 0 done / nothing to do, 1 no install found, 2 failed, 3 exe in use (game running),
//   4 access denied, 5 unknown exe version (needs /force).
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// SHA-256 of the original DesertStorm.exe (Steam and GOG ship the same file).
constexpr char kOriginalSha256[] = "d712f5ec82c14da66b098332b3cc9c99689091f46317112ea35dfd393dc3e164";
constexpr wchar_t kExeName[] = L"DesertStorm.exe";
constexpr wchar_t kBackupSuffix[] = L".original";

enum class State { Missing, Original, Patched, Unknown, Unreadable };

enum class Result { Done = 0, NotFound = 1, Failed = 2, InUse = 3, AccessDenied = 4, UnknownVersion = 5 };

struct Exe {
    State state = State::Missing;
    bool laa = false;
    DWORD flagOffset = 0;  // file offset of IMAGE_FILE_HEADER.Characteristics
    DWORD error = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// File checks

std::string Sha256Hex(const BYTE* data, size_t size) {
    std::string out;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return out;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BYTE digest[32];
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        if (BCryptHashData(hash, const_cast<BYTE*>(data), static_cast<ULONG>(size), 0) == 0 &&
            BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            char hex[3];
            for (BYTE b : digest) {
                sprintf_s(hex, "%02x", b);
                out += hex;
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

// Offset of the file header's Characteristics field of a 32-bit x86 PE, 0 if the data isn't one.
DWORD FlagOffset(const std::vector<BYTE>& d) {
    if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z') return 0;
    DWORD pe = *reinterpret_cast<const DWORD*>(&d[0x3C]);
    if (pe + 4 + sizeof(IMAGE_FILE_HEADER) > d.size() || std::memcmp(&d[pe], "PE\0\0", 4) != 0) return 0;
    auto fh = reinterpret_cast<const IMAGE_FILE_HEADER*>(&d[pe + 4]);
    if (fh->Machine != IMAGE_FILE_MACHINE_I386) return 0;
    return pe + 4 + offsetof(IMAGE_FILE_HEADER, Characteristics);
}

WORD Flags(const std::vector<BYTE>& d, DWORD off) { return *reinterpret_cast<const WORD*>(&d[off]); }

// The exe's state from its bytes: known original (flag off / on) or not ours.
Exe Classify(const std::vector<BYTE>& d) {
    Exe e;
    e.flagOffset = FlagOffset(d);
    if (!e.flagOffset) {
        e.state = State::Unknown;
        return e;
    }
    e.laa = (Flags(d, e.flagOffset) & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
    std::vector<BYTE> plain = d;
    *reinterpret_cast<WORD*>(&plain[e.flagOffset]) &= ~IMAGE_FILE_LARGE_ADDRESS_AWARE;
    bool known = Sha256Hex(plain.data(), plain.size()) == kOriginalSha256;
    e.state = !known ? State::Unknown : e.laa ? State::Patched : State::Original;
    return e;
}

bool ReadHandle(HANDLE h, std::vector<BYTE>& out) {
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024) return false;
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    return ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
}

std::wstring ExePath(const std::wstring& folder) { return folder + L"\\" + kExeName; }

Exe Inspect(const std::wstring& folder) {
    Exe e;
    HANDLE h = CreateFileW(ExePath(folder).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        e.error = GetLastError();
        e.state = (e.error == ERROR_FILE_NOT_FOUND || e.error == ERROR_PATH_NOT_FOUND) ? State::Missing
                                                                                        : State::Unreadable;
        return e;
    }
    std::vector<BYTE> d;
    bool ok = ReadHandle(h, d);
    CloseHandle(h);
    if (!ok) {
        e.state = State::Unreadable;
        return e;
    }
    return Classify(d);
}

// Writes a copy of the original exe next to it, once (an existing backup is never overwritten).
bool WriteBackup(const std::wstring& folder, const std::vector<BYTE>& original) {
    std::wstring path = ExePath(folder) + kBackupSuffix;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, original.data(), static_cast<DWORD>(original.size()), &put, nullptr) &&
              put == original.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// Sets (enable) or clears the flag. The file is opened exclusively for writing: while the game runs, Windows holds
// the exe as an image and the open fails with a sharing violation.
Result Apply(const std::wstring& folder, bool enable, bool force) {
    HANDLE h = CreateFileW(ExePath(folder).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION || err == ERROR_USER_MAPPED_FILE)
            return Result::InUse;
        if (err == ERROR_ACCESS_DENIED) {
            // Read-only attribute, or a folder that needs administrator rights.
            DWORD attr = GetFileAttributesW(ExePath(folder).c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY)) return Result::Failed;
            return Result::AccessDenied;
        }
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return Result::NotFound;
        return Result::Failed;
    }
    Result r = Result::Failed;
    std::vector<BYTE> d;
    if (ReadHandle(h, d)) {
        Exe e = Classify(d);
        if (!e.flagOffset) {
            r = Result::UnknownVersion;
        } else if (e.state == State::Unknown && !force) {
            r = Result::UnknownVersion;
        } else if (e.laa == enable) {
            r = Result::Done;
        } else {
            std::vector<BYTE> original = d;
            if (e.state != State::Unknown)
                *reinterpret_cast<WORD*>(&original[e.flagOffset]) &= ~IMAGE_FILE_LARGE_ADDRESS_AWARE;
            if (WriteBackup(folder, original)) {
                FILETIME created, accessed, written;
                bool times = GetFileTime(h, &created, &accessed, &written) != 0;
                WORD flags = Flags(d, e.flagOffset);
                flags = enable ? (flags | IMAGE_FILE_LARGE_ADDRESS_AWARE) : (flags & ~IMAGE_FILE_LARGE_ADDRESS_AWARE);
                DWORD put = 0;
                if (SetFilePointer(h, static_cast<LONG>(e.flagOffset), nullptr, FILE_BEGIN) == e.flagOffset &&
                    WriteFile(h, &flags, sizeof(flags), &put, nullptr) && put == sizeof(flags) &&
                    FlushFileBuffers(h)) {
                    if (times) SetFileTime(h, &created, &accessed, &written);
                    std::vector<BYTE> check;
                    r = (ReadHandle(h, check) && Classify(check).laa == enable) ? Result::Done : Result::Failed;
                }
            }
        }
    }
    CloseHandle(h);
    return r;
}

// ---------------------------------------------------------------------------------------------------------------
// Finding installs

std::wstring Lower(std::wstring s) {
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

void AddFolder(std::vector<std::wstring>& out, std::wstring dir) {
    for (auto& c : dir)
        if (c == L'/') c = L'\\';
    while (dir.size() > 3 && dir.back() == L'\\') dir.pop_back();
    if (dir.empty()) return;
    wchar_t full[MAX_PATH];
    if (!GetFullPathNameW(dir.c_str(), MAX_PATH, full, nullptr)) return;
    dir = full;
    DWORD attr = GetFileAttributesW(ExePath(dir).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return;
    for (auto& f : out)
        if (Lower(f) == Lower(dir)) return;
    out.push_back(dir);
}

// A registry value that names the exe or its folder.
void AddFromRegistry(std::vector<std::wstring>& out, HKEY root, const wchar_t* key, const wchar_t* value,
                     REGSAM view) {
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return;
    wchar_t buf[MAX_PATH];
    DWORD size = sizeof(buf), type = 0;
    if (RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        buf[MAX_PATH - 1] = 0;
        std::wstring p = buf;
        for (auto& c : p)
            if (c == L'/') c = L'\\';
        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) p = p.substr(0, p.find_last_of(L'\\'));
        AddFolder(out, p);
    }
    RegCloseKey(k);
}

void AddGog(std::vector<std::wstring>& out) {
    HKEY games;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games", 0, KEY_READ | KEY_WOW64_32KEY, &games) !=
        ERROR_SUCCESS)
        return;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(games, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring key = std::wstring(L"SOFTWARE\\GOG.com\\Games\\") + name;
        AddFromRegistry(out, HKEY_LOCAL_MACHINE, key.c_str(), L"path", KEY_WOW64_32KEY);  // only kept if it has the exe
    }
    RegCloseKey(games);
}

void AddSteam(std::vector<std::wstring>& out) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    wchar_t buf[MAX_PATH];
    DWORD size = sizeof(buf), type = 0;
    LSTATUS st = RegQueryValueExW(k, L"SteamPath", nullptr, &type, reinterpret_cast<BYTE*>(buf), &size);
    RegCloseKey(k);
    if (st != ERROR_SUCCESS || type != REG_SZ) return;
    buf[MAX_PATH - 1] = 0;
    std::vector<std::wstring> libs = {buf};

    // libraryfolders.vdf: "path"   "H:\\SteamLibrary"
    std::wstring vdf = std::wstring(buf) + L"\\steamapps\\libraryfolders.vdf";
    HANDLE h = CreateFileW(vdf.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        std::vector<BYTE> d;
        if (ReadHandle(h, d)) {
            std::string text(d.begin(), d.end());
            for (size_t pos = 0; (pos = text.find("\"path\"", pos)) != std::string::npos;) {
                size_t a = text.find('"', pos + 6);
                size_t b = a == std::string::npos ? a : text.find('"', a + 1);
                if (b == std::string::npos) break;
                std::string p = text.substr(a + 1, b - a - 1);
                for (size_t q; (q = p.find("\\\\")) != std::string::npos;) p.erase(q, 1);
                wchar_t w[MAX_PATH];
                if (MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, w, MAX_PATH)) libs.push_back(w);
                pos = b + 1;
            }
        }
        CloseHandle(h);
    }
    for (auto& lib : libs) AddFolder(out, lib + L"\\steamapps\\common\\conflict_desert_storm");
}

std::vector<std::wstring> FindInstalls() {
    std::vector<std::wstring> out;
    wchar_t self[MAX_PATH];
    if (GetModuleFileNameW(nullptr, self, MAX_PATH)) {
        std::wstring dir = self;
        AddFolder(out, dir.substr(0, dir.find_last_of(L'\\')));
    }
    const wchar_t* key = L"SOFTWARE\\Pivotal Games\\Conflict Desert Storm";
    AddFromRegistry(out, HKEY_LOCAL_MACHINE, key, L"ExePath", KEY_WOW64_32KEY);
    AddFromRegistry(out, HKEY_CURRENT_USER,
                    L"Software\\Classes\\VirtualStore\\MACHINE\\SOFTWARE\\WOW6432Node\\Pivotal Games\\Conflict Desert Storm",
                    L"ExePath", 0);
    AddGog(out);
    AddSteam(out);
    wchar_t drives[256];
    DWORD n = GetLogicalDriveStringsW(255, drives);
    for (const wchar_t* d = drives; n && *d; d += wcslen(d) + 1) {
        if (GetDriveTypeW(d) != DRIVE_FIXED) continue;
        for (const wchar_t* rel : {L"GOG Games\\Conflict - Desert Storm", L"GOG\\Games\\Conflict - Desert Storm",
                                   L"Program Files (x86)\\GOG Galaxy\\Games\\Conflict - Desert Storm",
                                   L"Program Files (x86)\\SCi Games\\Conflict Desert Storm",
                                   L"Games\\Conflict - Desert Storm", L"Games\\Conflict Desert Storm"})
            AddFolder(out, std::wstring(d) + rel);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Texts

const wchar_t* StateText(State s, bool laa) {
    switch (s) {
        case State::Original: return L"Original (2 GB)";
        case State::Patched: return L"4 GB on";
        case State::Unknown: return laa ? L"Other version (4 GB on)" : L"Other version (2 GB)";
        case State::Unreadable: return L"Can't read the exe";
        default: return L"No DesertStorm.exe";
    }
}

const wchar_t* ResultText(Result r) {
    switch (r) {
        case Result::Done: return L"Done.";
        case Result::NotFound: return L"DesertStorm.exe wasn't found in that folder.";
        case Result::InUse: return L"DesertStorm.exe is in use. Close the game (and its launcher) and try again.";
        case Result::AccessDenied: return L"Windows refused to change DesertStorm.exe (administrator rights needed).";
        case Result::UnknownVersion:
            return L"This DesertStorm.exe isn't the Steam / GOG version DesertStormFix knows, so it was left alone.";
        default: return L"Changing DesertStorm.exe failed. Nothing was changed, or the change was undone.";
    }
}

bool Is64BitWindows() {
    BOOL wow = FALSE;
    return IsWow64Process(GetCurrentProcess(), &wow) && wow;
}

// ---------------------------------------------------------------------------------------------------------------
// Command line

struct Args {
    enum { None, Status, Enable, Disable } action = None;
    bool force = false;
    std::vector<std::wstring> folders;
};

Args ParseArgs() {
    Args a;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring s = Lower(argv[i]);
        if (s == L"/status" || s == L"--status") a.action = Args::Status;
        else if (s == L"/enable" || s == L"--enable") a.action = Args::Enable;
        else if (s == L"/disable" || s == L"--disable") a.action = Args::Disable;
        else if (s == L"/force" || s == L"--force") a.force = true;
        else a.folders.push_back(argv[i]);
    }
    LocalFree(argv);
    return a;
}

void Print(const std::wstring& s) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    std::wstring line = s + L"\r\n";
    DWORD mode, n;
    if (GetConsoleMode(out, &mode)) {
        WriteConsoleW(out, line.c_str(), static_cast<DWORD>(line.size()), &n, nullptr);
    } else {  // redirected: UTF-8
        int len = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string u(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, u.data(), len, nullptr, nullptr);
        WriteFile(out, u.data(), static_cast<DWORD>(len - 1), &n, nullptr);
    }
}

int RunCommandLine(const Args& a) {
    // Output goes to the console we were started from (this is a GUI exe), or a redirected stdout.
    if (!GetStdHandle(STD_OUTPUT_HANDLE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE con = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        SetStdHandle(STD_OUTPUT_HANDLE, con);
    }
    std::vector<std::wstring> folders;
    for (auto& f : a.folders) AddFolder(folders, f);
    if (a.folders.empty()) folders = FindInstalls();
    if (folders.empty()) {
        Print(L"No Conflict: Desert Storm install found.");
        return static_cast<int>(Result::NotFound);
    }
    int worst = 0;
    for (auto& f : folders) {
        if (a.action == Args::Status) {
            Exe e = Inspect(f);
            Print(f + L": " + StateText(e.state, e.laa));
            continue;
        }
        Result r = Apply(f, a.action == Args::Enable, a.force);
        Print(f + L": " + ResultText(r));
        if (static_cast<int>(r) > worst) worst = static_cast<int>(r);
    }
    return worst;
}

// Runs this exe elevated for one folder and returns its result.
Result RunElevated(HWND owner, const std::wstring& folder, bool enable, bool force) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring params = enable ? L"/enable" : L"/disable";
    if (force) params += L" /force";
    params += L" \"" + folder + L"\"";
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return Result::AccessDenied;
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = static_cast<DWORD>(Result::Failed);
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code <= 5 ? static_cast<Result>(code) : Result::Failed;
}

// ---------------------------------------------------------------------------------------------------------------
// Window

enum : int { IdList = 100, IdEnable, IdDisable, IdAdd, IdClose, IdInfo, IdStatus };

struct Ui {
    HWND wnd = nullptr, list = nullptr, info = nullptr, status = nullptr;
    HWND enable = nullptr, disable = nullptr, add = nullptr, close = nullptr;
    HFONT font = nullptr;
    int dpi = 96;
    std::vector<std::wstring> folders;
    std::vector<Exe> exes;
} g;

int S(int v) { return MulDiv(v, g.dpi, 96); }

int Selected() { return ListView_GetNextItem(g.list, -1, LVNI_SELECTED); }

void UpdateButtons() {
    int i = Selected();
    bool any = i >= 0 && i < static_cast<int>(g.exes.size());
    State s = any ? g.exes[i].state : State::Missing;
    bool usable = s == State::Original || s == State::Patched || s == State::Unknown;
    EnableWindow(g.enable, usable && !g.exes[i].laa);
    EnableWindow(g.disable, usable && g.exes[i].laa);
}

void Refresh(int keep = -1) {
    ListView_DeleteAllItems(g.list);
    g.exes.clear();
    for (size_t i = 0; i < g.folders.size(); ++i) {
        g.exes.push_back(Inspect(g.folders[i]));
        LVITEMW it = {LVIF_TEXT};
        it.iItem = static_cast<int>(i);
        it.pszText = const_cast<wchar_t*>(g.folders[i].c_str());
        ListView_InsertItem(g.list, &it);
        ListView_SetItemText(g.list, static_cast<int>(i), 1,
                             const_cast<wchar_t*>(StateText(g.exes[i].state, g.exes[i].laa)));
    }
    int sel = keep >= 0 ? keep : 0;
    if (!g.folders.empty()) ListView_SetItemState(g.list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    if (g.folders.empty())
        SetWindowTextW(g.status, L"No install found. Use \"Add folder...\" to pick the folder with DesertStorm.exe.");
    UpdateButtons();
}

void Change(bool enable) {
    int i = Selected();
    if (i < 0) return;
    const std::wstring& folder = g.folders[i];
    bool force = false;
    Result r = Apply(folder, enable, force);
    if (r == Result::UnknownVersion && g.exes[i].flagOffset) {
        int answer = MessageBoxW(g.wnd,
                                 L"This DesertStorm.exe isn't the Steam / GOG version DesertStormFix was made for.\n\n"
                                 L"Change the 4 GB flag anyway? A backup is kept as DesertStorm.exe.original.",
                                 L"DesertStormFix Patcher", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (answer != IDYES) return;
        force = true;
        r = Apply(folder, enable, force);
    }
    if (r == Result::AccessDenied) r = RunElevated(g.wnd, folder, enable, force);
    std::wstring msg = ResultText(r);
    if (r == Result::Done)
        msg = enable ? L"4 GB is on. Start the game the usual way (Steam, GOG or the exe) - nothing else to do. "
                       L"A backup of the original is DesertStorm.exe.original."
                     : L"4 GB is off: DesertStorm.exe is the original again.";
    SetWindowTextW(g.status, msg.c_str());
    Refresh(i);
}

void AddFolderDialog() {
    BROWSEINFOW bi = {};
    bi.hwndOwner = g.wnd;
    bi.lpszTitle = L"Pick the Conflict: Desert Storm folder (the one with DesertStorm.exe)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH];
    bool ok = SHGetPathFromIDListW(pidl, path) != 0;
    CoTaskMemFree(pidl);
    if (!ok) return;
    size_t before = g.folders.size();
    AddFolder(g.folders, path);
    if (g.folders.size() == before) {
        bool known = false;
        for (auto& f : g.folders) known |= Lower(f) == Lower(path);
        SetWindowTextW(g.status, known ? L"That folder is already in the list." : L"No DesertStorm.exe in that folder.");
        return;
    }
    Refresh(static_cast<int>(g.folders.size()) - 1);
}

void Layout() {
    RECT rc;
    GetClientRect(g.wnd, &rc);
    int w = rc.right, h = rc.bottom, m = S(12), bw = S(120), bh = S(28);
    int infoH = S(66), statusH = S(40);
    MoveWindow(g.info, m, m, w - 2 * m, infoH, TRUE);
    int listTop = m + infoH + S(6);
    int buttonsTop = h - m - bh;
    int statusTop = buttonsTop - S(8) - statusH;
    MoveWindow(g.list, m, listTop, w - 2 * m, statusTop - S(8) - listTop, TRUE);
    MoveWindow(g.status, m, statusTop, w - 2 * m, statusH, TRUE);
    MoveWindow(g.enable, m, buttonsTop, bw, bh, TRUE);
    MoveWindow(g.disable, m + bw + S(8), buttonsTop, bw, bh, TRUE);
    MoveWindow(g.add, m + 2 * (bw + S(8)), buttonsTop, bw, bh, TRUE);
    MoveWindow(g.close, w - m - bw, buttonsTop, bw, bh, TRUE);
    int statusCol = S(170);
    ListView_SetColumnWidth(g.list, 1, statusCol);
    RECT lr;
    GetClientRect(g.list, &lr);
    ListView_SetColumnWidth(g.list, 0, lr.right - statusCol);
}

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g.wnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return c;
}

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g.wnd = wnd;
            HDC dc = GetDC(wnd);
            g.dpi = GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(wnd, dc);
            NONCLIENTMETRICSW ncm = {sizeof(ncm)};
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
            g.font = CreateFontIndirectW(&ncm.lfMessageFont);
            std::wstring info =
                L"Lets Conflict: Desert Storm use 4 GB of memory instead of 2 GB. This sets one flag in "
                L"DesertStorm.exe (Large Address Aware); a backup is kept as DesertStorm.exe.original. "
                L"Steam's \"Verify integrity\" or a GOG update puts the original back - just run this again.";
            if (!Is64BitWindows()) info += L"\r\nThis is 32-bit Windows: the flag makes no difference here.";
            g.info = Make(L"STATIC", info.c_str(), 0, IdInfo);
            g.list = Make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, IdList,
                          WS_EX_CLIENTEDGE);
            ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            LVCOLUMNW col = {LVCF_TEXT | LVCF_WIDTH};
            col.pszText = const_cast<wchar_t*>(L"Install");
            col.cx = S(300);
            ListView_InsertColumn(g.list, 0, &col);
            col.pszText = const_cast<wchar_t*>(L"Status");
            col.cx = S(150);
            ListView_InsertColumn(g.list, 1, &col);
            g.status = Make(L"STATIC", L"", 0, IdStatus);
            g.enable = Make(L"BUTTON", L"Turn 4 GB on", BS_DEFPUSHBUTTON | WS_TABSTOP, IdEnable);
            g.disable = Make(L"BUTTON", L"Turn 4 GB off", BS_PUSHBUTTON | WS_TABSTOP, IdDisable);
            g.add = Make(L"BUTTON", L"Add folder...", BS_PUSHBUTTON | WS_TABSTOP, IdAdd);
            g.close = Make(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, IdClose);
            g.folders = FindInstalls();
            Layout();
            Refresh();
            return 0;
        }
        case WM_SIZE: Layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {S(600), S(320)};
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_NOTIFY: {
            auto hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->idFrom == IdList && hdr->code == LVN_ITEMCHANGED) UpdateButtons();
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IdEnable: Change(true); return 0;
                case IdDisable: Change(false); return 0;
                case IdAdd: AddFolderDialog(); return 0;
                case IdClose:
                case IDCANCEL: DestroyWindow(wnd); return 0;
            }
            break;
        case WM_DESTROY:
            if (g.font) DeleteObject(g.font);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

int RunWindow(HINSTANCE inst, int show) {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"DesertStormFixPatcher";
    RegisterClassExW(&wc);
    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"DesertStormFix Patcher - 4 GB memory", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(720, dpi, 96), MulDiv(400, dpi, 96), nullptr,
                               nullptr, inst, nullptr);
    if (!wnd) return 2;
    ShowWindow(wnd, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    Args a = ParseArgs();
    if (a.action != Args::None) return RunCommandLine(a);
    return RunWindow(inst, show);
}
