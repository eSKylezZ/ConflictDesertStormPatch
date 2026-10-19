// pse.settings: declared, typed values in HKCU\<registryRoot>\Plugins\<plugin>. REG_DWORD for bool / int / choice /
// colour, REG_SZ for strings. A stored value of the wrong type or out of range is never trusted as it is.
#include <windows.h>

#include <memory>
#include <mutex>

#include "internal.h"

namespace pse {
namespace {

struct Setting {
    PSEPluginHandle owner = 0;
    PSESettingDesc desc{};   // our copy; its strings point into the members below
    std::string key, defaultString, label, help, group;
    std::vector<std::string> choiceText;
    std::vector<const char*> choicePtrs;   // NULL-terminated
    int32_t value = 0;
    std::string text;
};

struct ChangeListener {
    PSEPluginHandle owner;
    PSESettingChanged cb;
    void* ctx;
};

std::recursive_mutex g_mutex;
std::vector<std::unique_ptr<Setting>> g_settings;   // declaration order
std::vector<ChangeListener> g_listeners;

bool ValidKey(const char* key) {
    if (!key || !*key) return false;
    for (const char* p = key; *p; ++p) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

std::wstring KeyPath(PSEPluginHandle plugin) {
    return detail::Widen(detail::Cfg().registryRoot + "\\Plugins\\" + PluginName(plugin));
}

bool ReadDword(PSEPluginHandle plugin, const std::string& name, int32_t* out) {
    DWORD v = 0, size = sizeof v;
    LSTATUS s = RegGetValueW(HKEY_CURRENT_USER, KeyPath(plugin).c_str(), detail::Widen(name).c_str(), RRF_RT_REG_DWORD,
                             nullptr, &v, &size);
    if (s != ERROR_SUCCESS) return false;
    *out = static_cast<int32_t>(v);
    return true;
}

bool ReadString(PSEPluginHandle plugin, const std::string& name, std::string* out) {
    std::wstring path = KeyPath(plugin), value = detail::Widen(name);
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, path.c_str(), value.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &size) !=
        ERROR_SUCCESS)
        return false;
    std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
    size = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, path.c_str(), value.c_str(), RRF_RT_REG_SZ, nullptr, buf.data(), &size) !=
        ERROR_SUCCESS)
        return false;
    buf.resize(wcsnlen(buf.c_str(), buf.size()));
    *out = detail::Narrow(buf);
    return true;
}

bool WriteValue(PSEPluginHandle plugin, const std::string& name, DWORD type, const void* data, DWORD len) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, KeyPath(plugin).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return false;
    LSTATUS s = RegSetValueExW(key, detail::Widen(name).c_str(), 0, type, static_cast<const BYTE*>(data), len);
    RegCloseKey(key);
    return s == ERROR_SUCCESS;
}

int32_t Clamp(const Setting& s, int32_t v) {
    switch (s.desc.type) {
        case PSE_SETTING_BOOL: return v ? 1 : 0;
        case PSE_SETTING_INT: return v < s.desc.minInt ? s.desc.minInt : v > s.desc.maxInt ? s.desc.maxInt : v;
        case PSE_SETTING_CHOICE: {
            int32_t last = static_cast<int32_t>(s.choiceText.size()) - 1;
            return v < 0 ? 0 : v > last ? last : v;
        }
        case PSE_SETTING_COLOUR: return v & 0xFFFFFF;
        default: return v;
    }
}

Setting* Find(PSEPluginHandle plugin, const char* key) {
    if (!key) return nullptr;
    for (auto& s : g_settings)
        if (s->owner == plugin && s->key == key) return s.get();
    return nullptr;
}

void FireChanged(PSEPluginHandle plugin, const std::string& key) {
    std::vector<ChangeListener> snapshot;
    {
        std::lock_guard lock(g_mutex);
        for (const ChangeListener& l : g_listeners)
            if (l.owner == plugin) snapshot.push_back(l);
    }
    for (const ChangeListener& l : snapshot) detail::CallSettingChanged(l.owner, l.cb, key.c_str(), l.ctx);  // guard.cpp
}

const char* Keep(std::string& store, const char* src) {
    if (!src) return nullptr;
    store = src;
    return store.c_str();
}

PSEResult PSE_CALL Declare(PSEPluginHandle plugin, const PSESettingDesc* desc) {
    if (!detail::ValidPlugin(plugin) || !desc || desc->size < sizeof(PSESettingDesc) || !ValidKey(desc->key) ||
        desc->type < PSE_SETTING_BOOL || desc->type > PSE_SETTING_STRING)
        return PSE_E_INVALID_ARG;
    if (desc->type == PSE_SETTING_INT && desc->minInt > desc->maxInt) return PSE_E_INVALID_ARG;
    if (desc->type == PSE_SETTING_CHOICE && (!desc->choices || !desc->choices[0])) return PSE_E_INVALID_ARG;
    std::lock_guard lock(g_mutex);
    if (Find(plugin, desc->key)) return PSE_E_EXISTS;

    auto s = std::make_unique<Setting>();
    s->owner = plugin;
    s->desc = *desc;
    s->desc.size = sizeof(PSESettingDesc);
    s->desc.key = Keep(s->key, desc->key);
    s->desc.defaultString = Keep(s->defaultString, desc->defaultString);
    s->desc.label = Keep(s->label, desc->label);
    s->desc.help = Keep(s->help, desc->help);
    s->desc.group = Keep(s->group, desc->group);
    if (desc->type == PSE_SETTING_CHOICE) {
        for (const char* const* c = desc->choices; *c; ++c) s->choiceText.emplace_back(*c);
        for (const std::string& c : s->choiceText) s->choicePtrs.push_back(c.c_str());
        s->choicePtrs.push_back(nullptr);
        s->desc.choices = s->choicePtrs.data();
    } else {
        s->desc.choices = nullptr;
    }
    s->desc.defaultInt = Clamp(*s, desc->defaultInt);

    if (desc->type == PSE_SETTING_STRING) {
        if (!ReadString(plugin, s->key, &s->text)) s->text = s->defaultString;
    } else {
        int32_t stored;
        s->value = ReadDword(plugin, s->key, &stored) ? Clamp(*s, stored) : s->desc.defaultInt;
    }
    g_settings.push_back(std::move(s));
    return PSE_OK;
}

int32_t PSE_CALL GetInt(PSEPluginHandle plugin, const char* key) {
    std::lock_guard lock(g_mutex);
    Setting* s = Find(plugin, key);
    return s && s->desc.type != PSE_SETTING_STRING ? s->value : 0;
}

PSEResult PSE_CALL SetInt(PSEPluginHandle plugin, const char* key, int32_t value) {
    std::string name;
    {
        std::lock_guard lock(g_mutex);
        Setting* s = Find(plugin, key);
        if (!s) return PSE_E_NOT_FOUND;
        if (s->desc.type == PSE_SETTING_STRING) return PSE_E_INVALID_ARG;
        int32_t v = Clamp(*s, value);
        if (v == s->value) return PSE_OK;
        DWORD dw = static_cast<DWORD>(v);
        if (!WriteValue(plugin, s->key, REG_DWORD, &dw, sizeof dw)) return PSE_E_ACCESS;
        s->value = v;
        name = s->key;
    }
    FireChanged(plugin, name);
    return PSE_OK;
}

uint32_t PSE_CALL GetString(PSEPluginHandle plugin, const char* key, char* buf, uint32_t bufLen) {
    std::lock_guard lock(g_mutex);
    Setting* s = Find(plugin, key);
    const std::string empty;
    const std::string& text = s && s->desc.type == PSE_SETTING_STRING ? s->text : empty;
    if (buf && bufLen) {
        size_t n = text.size() < bufLen - 1 ? text.size() : bufLen - 1;
        memcpy(buf, text.data(), n);
        buf[n] = 0;
    }
    return static_cast<uint32_t>(text.size());
}

PSEResult PSE_CALL SetString(PSEPluginHandle plugin, const char* key, const char* value) {
    if (!value) return PSE_E_INVALID_ARG;
    std::string name;
    {
        std::lock_guard lock(g_mutex);
        Setting* s = Find(plugin, key);
        if (!s) return PSE_E_NOT_FOUND;
        if (s->desc.type != PSE_SETTING_STRING) return PSE_E_INVALID_ARG;
        if (s->text == value) return PSE_OK;
        std::wstring w = detail::Widen(value);
        if (!WriteValue(plugin, s->key, REG_SZ, w.c_str(), static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t))))
            return PSE_E_ACCESS;
        s->text = value;
        name = s->key;
    }
    FireChanged(plugin, name);
    return PSE_OK;
}

PSEResult PSE_CALL OnChange(PSEPluginHandle plugin, PSESettingChanged cb, void* ctx) {
    if (!detail::ValidPlugin(plugin) || !cb) return PSE_E_INVALID_ARG;
    std::lock_guard lock(g_mutex);
    g_listeners.push_back({plugin, cb, ctx});
    return PSE_OK;
}

PSEResult PSE_CALL Describe(PSEPluginHandle owner, uint32_t index, const PSESettingDesc** out,
                            PSEPluginHandle* outOwner) {
    if (!out) return PSE_E_INVALID_ARG;
    std::lock_guard lock(g_mutex);
    uint32_t i = 0;
    for (auto& s : g_settings) {
        if (owner != 0 && s->owner != owner) continue;
        if (i++ != index) continue;
        *out = &s->desc;
        if (outOwner) *outOwner = s->owner;
        return PSE_OK;
    }
    *out = nullptr;
    return PSE_E_NOT_FOUND;
}

const PSESettingsInterface kTable = {sizeof(PSESettingsInterface), 1, &Declare,  &GetInt,  &SetInt,
                                     &GetString, &SetString, &OnChange, &Describe};

}  // namespace

namespace detail {
void ResetSettings() {
    std::lock_guard lock(g_mutex);
    g_settings.clear();
    g_listeners.clear();
}
// A failed / unloaded external plugin: its change callbacks point into a freed DLL, and its rows must not show up in
// menus (Describe). Stored values stay in the registry for the next start.
void ForgetSettings(PSEPluginHandle plugin) {
    std::lock_guard lock(g_mutex);
    std::erase_if(g_listeners, [plugin](const ChangeListener& l) { return l.owner == plugin; });
    std::erase_if(g_settings, [plugin](const std::unique_ptr<Setting>& s) { return s->owner == plugin; });
}
}  // namespace detail

const PSESettingsInterface* SettingsInterface() { return &kTable; }

std::string PluginSettingsKey(PSEPluginHandle plugin) {
    if (!detail::ValidPlugin(plugin)) return {};
    return detail::Cfg().registryRoot + "\\Plugins\\" + PluginName(plugin);
}

}  // namespace pse
