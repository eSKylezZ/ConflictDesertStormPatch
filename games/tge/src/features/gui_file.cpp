// TGE menu files as an editable model (see gui_file.h).
#include "features/gui_file.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace gui {
uint32_t NameHash(const std::string& name) {
    uint32_t h = 1;
    for (char ch : name) {
        const uint8_t c = static_cast<uint8_t>(tolower(static_cast<uint8_t>(ch)));
        for (int i = 0; i < 8; ++i) h = h << 1 | (((c >> i) ^ h ^ (h >> 1) ^ (h >> 21) ^ (h >> 31)) & 1);
    }
    return h;
}

namespace {
constexpr uint32_t kCustomTextHash = 0x27FDF0BA;   // the only class with its own loader (0x4C11C0): read as text

struct Reader {
    const uint8_t* d;
    size_t n, o = 0;
    void Need(size_t k) {
        if (o + k > n) throw std::runtime_error("cut off");
    }
    uint8_t U8() { Need(1); return d[o++]; }
    uint16_t U16() { Need(2); uint16_t v; memcpy(&v, d + o, 2); o += 2; return v; }
    uint32_t U32() { Need(4); uint32_t v; memcpy(&v, d + o, 4); o += 4; return v; }
    float F() { Need(4); float v; memcpy(&v, d + o, 4); o += 4; return v; }
    std::string Str() {
        const uint32_t k = U32();
        if (k > 0x10000) throw std::runtime_error("bad string length");
        Need(k);
        std::string s(reinterpret_cast<const char*>(d + o), k);
        o += k;
        return s;
    }
};
struct Writer {
    std::vector<uint8_t> b;
    void U8(uint8_t v) { b.push_back(v); }
    void U16(uint16_t v) { b.insert(b.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 2); }
    void U32(uint32_t v) { b.insert(b.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); }
    void F(float v) { b.insert(b.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); }
    void Str(const std::string& s) {
        U32(static_cast<uint32_t>(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    }
};

uint16_t StateFlags(const Control& c, size_t i) { return i < c.states.size() ? c.states[i].flags : 0; }

std::unique_ptr<Control> ReadControl(Reader& r);

void ReadBase(Reader& r, Control& c) {
    c.name = r.Str();
    c.flags = r.U16();
    for (float& f : c.rect) f = r.F();
    const uint32_t n = r.U32();
    if (n > 16) throw std::runtime_error("too many states");
    c.states.resize(n);
    for (State& s : c.states) {
        s.flags = r.U16();
        for (float& f : s.v) f = r.F();
        s.argb = r.U32();
    }
}
void ReadSprites(Reader& r, Control& c) {
    const uint32_t n = r.U32();
    if (n > 16) throw std::runtime_error("too many faces");
    c.sprites.resize(n);
    for (size_t i = 0; i < n; ++i) {
        SpriteFace& f = c.sprites[i];
        f.texture = (StateFlags(c, i) & 0x20) != 0;
        f.sheet = r.Str();
        if (!f.texture) {
            const uint32_t len = r.U32();
            if (len) f.index = r.U32();
            r.Need(len);
            f.image.assign(reinterpret_cast<const char*>(r.d + r.o), len);
            r.o += len;
        }
        f.b = r.U8();
    }
}
void ReadTexts(Reader& r, Control& c) {
    const uint32_t n = r.U32();
    if (n > 16) throw std::runtime_error("too many texts");
    c.texts.resize(n);
    for (size_t i = 0; i < n; ++i) {
        TextFace& t = c.texts[i];
        t.font = r.Str();
        t.keyed = (StateFlags(c, i) & 0x10) != 0;
        t.hashOrSize = r.U32();
        t.text = r.Str();
        t.align = r.U8();
        t.b = r.U8();
    }
}
void ReadChildren(Reader& r, Control& c) {
    const uint32_t n = r.U32();
    if (n > 256) throw std::runtime_error("too many children");
    for (uint32_t i = 0; i < n; ++i) c.children.push_back(ReadControl(r));
}
void ReadItems(Reader& r, Control& c) {
    ReadChildren(r, c);
    const uint32_t n = r.U32();
    if (n > 256) throw std::runtime_error("too many items");
    c.items.resize(n);
    for (uint32_t& h : c.items) h = r.U32();
}

std::unique_ptr<Control> ReadControl(Reader& r) {
    auto c = std::make_unique<Control>();
    c->type = r.U32();
    c->hash = r.U32();
    ReadBase(r, *c);
    switch (c->hash == kCustomTextHash ? 3 : c->type) {
        case 2: ReadSprites(r, *c); break;
        case 3: ReadTexts(r, *c); break;
        case 4: case 10: ReadChildren(r, *c); break;
        case 5: {
            ReadSprites(r, *c);
            const uint32_t n = r.U32();
            if (n > 16) throw std::runtime_error("too many frame parts");
            c->frame.resize(n, std::vector<float>(4));
            for (auto& v : c->frame)
                for (float& f : v) f = r.F();
            break;
        }
        case 6: break;
        case 7: ReadItems(r, *c); for (float& f : c->slider) f = r.F(); break;
        case 8: ReadItems(r, *c); break;
        case 9: ReadItems(r, *c); for (uint32_t& v : c->list) v = r.U32(); break;
        default: throw std::runtime_error("unknown control type");
    }
    return c;
}

void WriteControl(Writer& w, const Control& c) {
    w.U32(c.type);
    w.U32(c.hash);
    w.Str(c.name);
    w.U16(c.flags);
    for (float f : c.rect) w.F(f);
    w.U32(static_cast<uint32_t>(c.states.size()));
    for (const State& s : c.states) {
        w.U16(s.flags);
        for (float f : s.v) w.F(f);
        w.U32(s.argb);
    }
    auto sprites = [&] {
        w.U32(static_cast<uint32_t>(c.sprites.size()));
        for (const SpriteFace& f : c.sprites) {
            w.Str(f.sheet);
            if (!f.texture) {
                w.U32(static_cast<uint32_t>(f.image.size()));
                if (!f.image.empty()) w.U32(f.index);
                w.b.insert(w.b.end(), f.image.begin(), f.image.end());
            }
            w.U8(f.b);
        }
    };
    auto children = [&] {
        w.U32(static_cast<uint32_t>(c.children.size()));
        for (const auto& ch : c.children) WriteControl(w, *ch);
    };
    auto items = [&] {
        children();
        w.U32(static_cast<uint32_t>(c.items.size()));
        for (uint32_t h : c.items) w.U32(h);
    };
    switch (c.hash == kCustomTextHash ? 3 : c.type) {
        case 2: sprites(); break;
        case 3:
            w.U32(static_cast<uint32_t>(c.texts.size()));
            for (const TextFace& t : c.texts) {
                w.Str(t.font);
                w.U32(t.hashOrSize);
                w.Str(t.text);
                w.U8(t.align);
                w.U8(t.b);
            }
            break;
        case 4: case 10: children(); break;
        case 5:
            sprites();
            w.U32(static_cast<uint32_t>(c.frame.size()));
            for (const auto& v : c.frame)
                for (float f : v) w.F(f);
            break;
        case 7: items(); for (float f : c.slider) w.F(f); break;
        case 8: items(); break;
        case 9: items(); for (uint32_t v : c.list) w.U32(v); break;
        default: break;
    }
}

std::vector<Link> ReadTable(Reader& r) {
    const uint32_t n = r.U32();
    if (n > 256) throw std::runtime_error("bad nav table");
    std::vector<Link> t(n);
    for (Link& l : t) l = {r.U32(), r.U32(), r.U32(), r.U32()};
    return t;
}
void WriteTable(Writer& w, const std::vector<Link>& t) {
    w.U32(static_cast<uint32_t>(t.size()));
    for (const Link& l : t) {
        w.U32(l.key);
        w.U32(l.type);
        w.U32(l.target);
        w.U32(l.source);
    }
}

Control* FindIn(std::vector<std::unique_ptr<Control>>& list, uint32_t hash) {
    for (auto& c : list) {
        if (c->hash == hash) return c.get();
        if (Control* f = FindIn(c->children, hash)) return f;
    }
    return nullptr;
}

void RenameAll(Control& c, const std::string& tag, int& n, std::vector<std::pair<uint32_t, uint32_t>>& map) {
    char name[32];
    snprintf(name, sizeof name, "~%s%d", tag.c_str(), ++n);
    c.name = name;
    const uint32_t h = NameHash(name);
    map.push_back({c.hash, h});
    c.hash = h;
    for (auto& ch : c.children) RenameAll(*ch, tag, n, map);
}
uint32_t Mapped(const std::vector<std::pair<uint32_t, uint32_t>>& map, uint32_t h) {
    for (const auto& [from, to] : map)
        if (from == h) return to;
    return h;
}
}  // namespace

std::vector<std::pair<uint32_t, uint32_t>> RenameTree(Control& c, const std::string& tag, int& n) {
    std::vector<std::pair<uint32_t, uint32_t>> map;
    RenameAll(c, tag, n, map);
    auto fixItems = [&](Control& x, auto&& self) -> void {
        for (uint32_t& h : x.items) h = Mapped(map, h);
        for (auto& ch : x.children) self(*ch, self);
    };
    fixItems(c, fixItems);
    return map;
}

bool Remove(Screen& s, uint32_t hash) {
    for (size_t i = 0; i < s.controls.size(); ++i)
        if (s.controls[i]->hash == hash) {
            s.controls.erase(s.controls.begin() + i);
            if (i < s.controlNav.size()) s.controlNav.erase(s.controlNav.begin() + i);
            return true;
        }
    return false;
}

std::unique_ptr<Control> Control::Clone() const {
    auto c = std::make_unique<Control>();
    c->type = type, c->hash = hash, c->name = name, c->flags = flags;
    memcpy(c->rect, rect, sizeof rect);
    c->states = states, c->sprites = sprites, c->frame = frame, c->texts = texts, c->items = items;
    memcpy(c->slider, slider, sizeof slider);
    memcpy(c->list, list, sizeof list);
    for (const auto& ch : children) c->children.push_back(ch->Clone());
    return c;
}

std::unique_ptr<Screen> Screen::Clone() const {
    auto s = std::make_unique<Screen>();
    s->hash = hash, s->name = name, s->flags = flags, s->background = background, s->focus = focus;
    s->nav = nav, s->controlNav = controlNav;
    for (const auto& c : controls) s->controls.push_back(c->Clone());
    return s;
}

bool File::Parse(const uint8_t* data, size_t size, std::string* error) {
    try {
        Reader r{data, size};
        version = r.U16();
        if (version != 0x1D) throw std::runtime_error("not version 0x1D");
        design[0] = r.U32(), design[1] = r.U32();
        for (uint32_t& v : safe) v = r.U32();
        auto list = [&](std::vector<std::string>& out) {
            const uint32_t n = r.U32();
            if (n > 1024) throw std::runtime_error("bad list");
            for (uint32_t i = 0; i < n; ++i) out.push_back(r.Str());
        };
        const uint32_t nl = r.U32();
        if (nl > 64) throw std::runtime_error("bad .loc list");
        for (uint32_t i = 0; i < nl; ++i) {
            std::string s = r.Str();
            locs.push_back({s, r.U32()});
        }
        list(textures), list(fonts), list(sheets), list(bitmaps);
        const uint32_t ns = r.U32();
        if (ns > 256) throw std::runtime_error("bad screen count");
        for (uint32_t i = 0; i < ns; ++i) {
            auto s = std::make_unique<Screen>();
            s->hash = r.U32();
            s->name = r.Str();
            s->flags = r.U8();
            s->background = r.Str();
            const uint32_t nc = r.U32();
            if (nc > 1024) throw std::runtime_error("bad control count");
            for (uint32_t k = 0; k < nc; ++k) s->controls.push_back(ReadControl(r));
            s->focus = r.U32();
            screens.push_back(std::move(s));
        }
        if (r.U32() != ns) throw std::runtime_error("navigation screen count");
        for (auto& s : screens) {
            s->nav = ReadTable(r);
            const uint32_t nc = r.U32();
            if (nc != s->controls.size()) throw std::runtime_error("navigation control count");
            for (uint32_t k = 0; k < nc; ++k) s->controlNav.push_back(ReadTable(r));
        }
        if (r.o != size) throw std::runtime_error("bytes after the navigation");
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

std::vector<uint8_t> File::Write() const {
    Writer w;
    w.U16(version);
    w.U32(design[0]), w.U32(design[1]);
    for (uint32_t v : safe) w.U32(v);
    w.U32(static_cast<uint32_t>(locs.size()));
    for (const auto& [s, p] : locs) w.Str(s), w.U32(p);
    for (const auto* l : {&textures, &fonts, &sheets, &bitmaps}) {
        w.U32(static_cast<uint32_t>(l->size()));
        for (const std::string& s : *l) w.Str(s);
    }
    w.U32(static_cast<uint32_t>(screens.size()));
    for (const auto& s : screens) {
        w.U32(s->hash);
        w.Str(s->name);
        w.U8(s->flags);
        w.Str(s->background);
        w.U32(static_cast<uint32_t>(s->controls.size()));
        for (const auto& c : s->controls) WriteControl(w, *c);
        w.U32(s->focus);
    }
    w.U32(static_cast<uint32_t>(screens.size()));
    for (const auto& s : screens) {
        WriteTable(w, s->nav);
        w.U32(static_cast<uint32_t>(s->controlNav.size()));
        for (const auto& t : s->controlNav) WriteTable(w, t);
    }
    return w.b;
}

Screen* File::FindScreen(uint32_t hash) {
    for (auto& s : screens)
        if (s->hash == hash) return s.get();
    return nullptr;
}

Screen* File::AddScreen(std::unique_ptr<Screen> s, uint32_t after) {
    Screen* p = s.get();
    auto at = screens.end();
    for (auto it = screens.begin(); it != screens.end(); ++it)
        if ((*it)->hash == after) at = it + 1;
    screens.insert(at, std::move(s));
    return p;
}

Control* Find(Screen& s, uint32_t hash, size_t* top) {
    for (size_t i = 0; i < s.controls.size(); ++i) {
        Control* c = s.controls[i].get();
        if (c->hash == hash || FindIn(c->children, hash)) {
            if (top) *top = i;
            return c->hash == hash ? c : FindIn(c->children, hash);
        }
    }
    return nullptr;
}

std::vector<Link>* NavOf(Screen& s, uint32_t hash) {
    for (size_t i = 0; i < s.controls.size() && i < s.controlNav.size(); ++i)
        if (s.controls[i]->hash == hash) return &s.controlNav[i];
    return nullptr;
}

std::vector<std::pair<uint32_t, uint32_t>> Rename(Screen& s, const std::string& tag) {
    std::vector<std::pair<uint32_t, uint32_t>> map;
    int n = 0;
    const std::string screenName = "~" + tag;
    map.push_back({s.hash, NameHash(screenName)});
    s.name = screenName;
    s.hash = map.back().second;
    for (auto& c : s.controls) RenameAll(*c, tag, n, map);
    std::vector<std::unique_ptr<Control>*> stack;
    auto fixItems = [&](Control& c, auto&& self) -> void {
        for (uint32_t& h : c.items) h = Mapped(map, h);
        for (auto& ch : c.children) self(*ch, self);
    };
    for (auto& c : s.controls) fixItems(*c, fixItems);
    s.focus = Mapped(map, s.focus);
    auto fix = [&](std::vector<Link>& t) {
        for (Link& l : t) {
            if (l.type != 0) l.target = Mapped(map, l.target);   // type 0 = another screen (kept)
            l.source = Mapped(map, l.source);
        }
    };
    fix(s.nav);
    for (auto& t : s.controlNav) fix(t);
    return map;
}

uint32_t SetKey(Control& c, size_t face, const std::string& key) {
    if (face >= c.texts.size()) return 0;
    TextFace& t = c.texts[face];
    t.keyed = true;
    t.text = key;
    t.hashOrSize = NameHash(key);
    if (face < c.states.size()) c.states[face].flags |= 0x10;
    return t.hashOrSize;
}

Control* Insert(Screen& s, size_t at, std::unique_ptr<Control> c, std::vector<Link> nav) {
    Control* p = c.get();
    if (at > s.controls.size()) at = s.controls.size();
    s.controls.insert(s.controls.begin() + at, std::move(c));
    s.controlNav.insert(s.controlNav.begin() + at, std::move(nav));
    return p;
}

void MoveY(Control& c, float dy) { c.rect[1] += dy; }   // children are placed relative to their parent
}  // namespace gui
