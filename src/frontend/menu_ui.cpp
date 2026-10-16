#include "menu_ui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "RmlUi_Platform_SDL.h"
#include "dream/render/vk/window.h"
#include "dreamcomp/port.h"
#include "dreamcomp/settings.h"

namespace dreamcomp::frontend {

namespace {

constexpr const char* kTabs[][2] = {{"general", "General"},
                                    {"controls", "Controls"},
                                    {"graphics", "Graphics"},
                                    {"sound", "Sound"},
                                    {"mods", "Mods"}};
constexpr int kTabCount = 5;

// PromptFont code points, the same table RecompFrontend uses (recompinput/src/input_types.cpp):
// Xbox-style face buttons, whatever the pad.
constexpr const char* kGlyphA = "\xE2\x86\xA7";      // U+21A7
constexpr const char* kGlyphB = "\xE2\x86\xA6";      // U+21A6
constexpr const char* kGlyphLB = "\xE2\x86\x98";     // U+2198
constexpr const char* kGlyphRB = "\xE2\x86\x99";     // U+2199
// PlayStation's (dreamcomp): Cross, Circle, L1, R1
constexpr const char* kGlyphCross = "\xE2\x87\xA3";   // U+21E3
constexpr const char* kGlyphCircle = "\xE2\x87\xA2";  // U+21E2
constexpr const char* kGlyphL1 = "\xE2\x86\xB0";      // U+21B0
constexpr const char* kGlyphR1 = "\xE2\x86\xB1";      // U+21B1
constexpr const char* kGlyphEnter = "\xE2\x90\xAE";  // U+242E
constexpr const char* kGlyphEsc = "\xE2\x90\xAF";    // U+242F
constexpr const char* kCheck = "\xE2\x9C\x93";
constexpr const char* kCross = "\xE2\x9C\x97";
constexpr const char* kEllipsis = "\xE2\x80\xA6";
constexpr const char* kDot = "  \xC2\xB7  ";

std::string esc(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string id_safe(const std::string& s) {
    std::string out;
    for (unsigned char c : s) out += std::isalnum(c) ? static_cast<char>(c) : '_';
    return out;
}

std::string u8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::string url_path(const std::filesystem::path& p) {
    const auto s = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::string file_url(const std::filesystem::path& p) {
    std::string s = url_path(p);
    std::string out = "file:///";
    if (!s.empty() && s[0] == '/')
        s.erase(0, 1);
    for (unsigned char c : s) {
        if (std::isalnum(c) || std::strchr("/-_.~:", c))
            out += static_cast<char>(c);
        else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string utf8(char32_t c) {
    std::string o;
    if (c < 0x80) {
        o += static_cast<char>(c);
    } else if (c < 0x800) {
        o += static_cast<char>(0xC0 | (c >> 6));
        o += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        o += static_cast<char>(0xE0 | (c >> 12));
        o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        o += static_cast<char>(0xF0 | (c >> 18));
        o += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (c & 0x3F));
    }
    return o;
}

constexpr char32_t kDeviceGamepad = 0x243C;   // PF_DEVICE_GAMEPAD
constexpr char32_t kDeviceKeyboard = 0x243D;  // PF_DEVICE_KEYBOARD
constexpr char32_t kIconCross = 0x2717;       // PF_ICON_CROSS

// A binding as PromptFont glyphs (RecompFrontend's table), or "" when PromptFont has no glyph
// for it; the caller then shows the name as text.
std::string binding_glyph(const dream::render::Binding& b, bool ps = false,
                          bool back_is_touchpad = false) {
    using dream::render::BindSource;
    const std::string& n = b.code;
    if (b.source == BindSource::Key) {
        if (n.size() == 1) {
            const char c = n[0];
            if (c >= 'A' && c <= 'Z')
                return utf8(0xFF21 + static_cast<char32_t>(c - 'A'));
            if (c >= 'a' && c <= 'z')
                return utf8(0xFF21 + static_cast<char32_t>(c - 'a'));
            if (c >= '0' && c <= '9')
                return utf8(0xFF10 + static_cast<char32_t>(c - '0'));
        }
        if (n.size() >= 2 && n.size() <= 3 && n[0] == 'F' &&
            std::all_of(n.begin() + 1, n.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            const int k = std::atoi(n.c_str() + 1);
            if (k >= 1 && k <= 12)
                return utf8(0x2460 + static_cast<char32_t>(k - 1));
        }
        // PromptFont's up and right keyboard arrows are swapped (RecompFrontend's note).
        static const std::pair<const char*, char32_t> keys[] = {
            {"Left", 0x23F4},      {"Up", 0x23F6},         {"Right", 0x23F5},
            {"Down", 0x23F7},      {"Escape", 0x242F},     {"PrintScreen", 0x2430},
            {"ScrollLock", 0x2431}, {"Pause", 0x2432},     {"Insert", 0x2434},
            {"Home", 0x2435},      {"PageUp", 0x2436},     {"Delete", 0x2437},
            {"End", 0x2438},       {"PageDown", 0x2439},   {"Space", 0x243A},
            {"Backspace", 0x242D}, {"Tab", 0x242B},        {"Return", 0x242E},
            {"CapsLock", 0x242C},  {"Numlock", 0x2433}};
        for (const auto& [name, cp] : keys)
            if (n == name)
                return utf8(cp);
        if (n == "Left Shift")
            return "L" + utf8(0x2429);
        if (n == "Right Shift")
            return "R" + utf8(0x2429);
        return {};
    }
    if (b.source == BindSource::Button && ps) {
        // PlayStation glyphs (dreamcomp): Cross, Circle, Square, Triangle, L1, R1, Options; Back
        // is Create, or the touch pad where the platform reports its click as Back (the PS5's).
        static const std::pair<const char*, char32_t> ps_buttons[] = {
            {"a", 0x21E3}, {"b", 0x21E2}, {"x", 0x21E0}, {"y", 0x21E1},
            {"leftshoulder", 0x21B0}, {"rightshoulder", 0x21B1}, {"start", 0x21E8}};
        for (const auto& [name, cp] : ps_buttons)
            if (n == name)
                return utf8(cp);
        if (n == "back")
            return utf8(back_is_touchpad ? 0x21E7 : 0x21E6);
    }
    if (b.source == BindSource::Axis && ps && (n == "lefttrigger" || n == "righttrigger") &&
        b.sign >= 0)
        return utf8(n == "lefttrigger" ? 0x21B2 : 0x21B3);  // L2, R2
    if (b.source == BindSource::Button) {
        static const std::pair<const char*, char32_t> buttons[] = {
            {"a", 0x21A7},           {"b", 0x21A6},          {"x", 0x21A4},
            {"y", 0x21A5},           {"back", 0x21FA},       {"guide", 0x21F9},
            {"start", 0x21FB},       {"leftstick", 0x21BA},  {"rightstick", 0x21BB},
            {"leftshoulder", 0x2198}, {"rightshoulder", 0x2199}, {"dpup", 0x219F},
            {"dpdown", 0x21A1},      {"dpleft", 0x219E},     {"dpright", 0x21A0},
            {"misc1", 0x21E5},       {"paddle1", 0x2276},    {"paddle2", 0x2277},
            {"paddle3", 0x2278},     {"paddle4", 0x2279},    {"touchpad", 0x21E7}};
        for (const auto& [name, cp] : buttons)
            if (n == name)
                return utf8(cp);
        return {};
    }
    if (b.source == BindSource::Axis) {
        const bool pos = b.sign >= 0;
        static const struct {
            const char* name;
            char32_t plus, minus;
        } axes[] = {{"leftx", 0x21C0, 0x21BC},       {"lefty", 0x21C2, 0x21BE},
                    {"rightx", 0x21C1, 0x21BD},      {"righty", 0x21C3, 0x21BF},
                    {"lefttrigger", 0x2196, 0x21DC}, {"righttrigger", 0x2197, 0x21DD}};
        for (const auto& a : axes)
            if (n == a.name)
                return utf8(pos ? a.plus : a.minus);
    }
    return {};
}

// The Dreamcast pad's inputs, in the order the Controls tab lists them.
struct CtlRow {
    dream::render::PadControl c;
    const char* label;
};
constexpr CtlRow kCtlRows[] = {
    {dream::render::PadControl::StickUp, "Analog Up"},
    {dream::render::PadControl::StickDown, "Analog Down"},
    {dream::render::PadControl::StickLeft, "Analog Left"},
    {dream::render::PadControl::StickRight, "Analog Right"},
    {dream::render::PadControl::A, "A"},
    {dream::render::PadControl::B, "B"},
    {dream::render::PadControl::X, "X"},
    {dream::render::PadControl::Y, "Y"},
    {dream::render::PadControl::LeftTrigger, "L Trigger"},
    {dream::render::PadControl::RightTrigger, "R Trigger"},
    {dream::render::PadControl::Start, "Start"},
    {dream::render::PadControl::Up, "D-Pad Up"},
    {dream::render::PadControl::Down, "D-Pad Down"},
    {dream::render::PadControl::Left, "D-Pad Left"},
    {dream::render::PadControl::Right, "D-Pad Right"},
};

const char* ctl_label(unsigned control) {
    for (const auto& r : kCtlRows)
        if (static_cast<unsigned>(r.c) == control)
            return r.label;
    return "";
}

std::string glyph(const char* g) { return std::string("<span class=\"glyph\">") + g + "</span>"; }
std::string keycap(const char* k) { return std::string("<span class=\"key\">") + k + "</span>"; }
std::string hint(const std::string& what) { return "<span class=\"hint-text\">" + what + "</span>"; }

// ---- navigation helpers ----
Rml::Element* region_of(Rml::Element* e) {
    for (; e; e = e->GetParentNode()) {
        const auto& id = e->GetId();
        if (id == "lmenu" || id == "ptabs" || id == "prows" || id == "pfoot" || id == "modal")
            return e;
    }
    return nullptr;
}

Rml::Rectanglef box_of(Rml::Element* e) {
    const Rml::Vector2f pos = e->GetAbsoluteOffset(Rml::BoxArea::Border);
    return Rml::Rectanglef::FromPositionSize(pos, e->GetBox().GetSize(Rml::BoxArea::Border));
}

long nav_score(const Rml::Rectanglef& from, const Rml::Rectanglef& to,
               Rml::Input::KeyIdentifier key) {
    // Distance along the direction, plus a heavy penalty for lying outside the strip the source
    // projects in that direction (the heuristic RmlUi's own `nav: auto` uses).
    const bool vertical = key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN;
    const int ax = vertical ? 1 : 0, cx = vertical ? 0 : 1;
    const float main = (key == Rml::Input::KI_DOWN || key == Rml::Input::KI_RIGHT)
                           ? to.p0[ax] - from.p1[ax]
                           : from.p0[ax] - to.p1[ax];
    if (main < -1.0f)
        return -1;
    const float cross =
        std::max(0.0f, to.p0[cx] - from.p1[cx]) + std::max(0.0f, from.p0[cx] - to.p1[cx]);
    return static_cast<long>(std::max(main, 0.0f)) + 10000L * static_cast<long>(cross);
}

Rml::Element* nav_search(Rml::Element* region, Rml::Element* from, Rml::Input::KeyIdentifier key) {
    if (!region)
        return nullptr;
    Rml::ElementList list;
    region->QuerySelectorAll(list, ".focusable");
    const Rml::Rectanglef src = box_of(from);
    Rml::Element* best = nullptr;
    long best_score = -1;
    for (Rml::Element* e : list) {
        if (e == from)
            continue;
        const long sc = nav_score(src, box_of(e), key);
        if (sc >= 0 && (!best || sc < best_score)) {
            best = e;
            best_score = sc;
        }
    }
    return best;
}

const char* kAboutText =
    "A native PC port built with dreamcomp, a framework for Dreamcast recompilation ports: the "
    "game's SH-4 code is translated ahead of time into C++, and the console's graphics and sound "
    "hardware are emulated by the dream-recomp engine.<br/><br/><b>You need your own disc.</b> No "
    "game data is included; the port reads your disc image and checks it is the exact release it "
    "was built from.<br/><br/>dreamcomp by Daniel Gomes Vieira. dream-recomp engine by phobos665, "
    "with portions of Flycast (flyinghead and contributors). Licensed under the GNU General Public "
    "License v2.0.<br/><br/>Interface: RmlUi (MIT), FreeType (FTL / GPL-2.0), SDL3 (zlib). Fonts: "
    "Inter and PromptFont (SIL Open Font License 1.1).<br/><br/>Press Select to open the read-me.";

}  // namespace

// ---- construction -------------------------------------------------------------------------------

MenuUi::MenuUi(const UiConfig& cfg, Mode mode, UiHost host)
    : cfg_(cfg), mode_(mode), host_(std::move(host)), draft_(*cfg.settings) {
    config_dir_ = cfg.settings->file().parent_path();
    if (!cfg.config.empty()) {
        std::string err;
        game_ok_ = dream::translator::load_game_config(cfg.config, game_, err);
        if (!game_ok_)
            game_error_ = err;
    } else {
        game_error_ = "the game's configuration file was not found next to the executable";
    }
    panel_ = mode_ == Mode::InGame;
}

MenuUi::~MenuUi() {
    if (verifying_ && verify_.valid())
        verify_.wait();
    // The document outlives this listener (RmlUi frees it at Shutdown): detach first.
    if (doc_) {
        doc_->RemoveEventListener(Rml::EventId::Click, this);
        doc_->RemoveEventListener(Rml::EventId::Keydown, this);
        doc_->RemoveEventListener(Rml::EventId::Focus, this, true);
        doc_->RemoveEventListener(Rml::EventId::Mouseover, this, true);
    }
}

std::string MenuUi::title() const {
    if (cfg_.port && cfg_.port->title)
        return cfg_.port->title;
    if (game_ok_ && !game_.title.empty())
        return game_.title;
    return "dreamcomp";
}

bool MenuUi::load(Rml::Context* ctx, std::string* error) {
    ctx_ = ctx;
    const auto fe = cfg_.exe_dir / "frontend";
    std::string rml;
    {
        std::ifstream in(fe / "launcher.rml", std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        rml = ss.str();
    }
    if (rml.empty()) {
        *error = "cannot read " + u8(fe / "launcher.rml");
        return false;
    }
    std::error_code ec;
    if (std::filesystem::exists(fe / "port.rcss", ec)) {
        const auto at = rml.find("</head>");
        if (at != std::string::npos)
            rml.insert(at, "<link type=\"text/rcss\" href=\"port.rcss\"/>\n");
    }
    // RmlUi parses a document's source as a URL, where "C:" would be a protocol; it spells drive
    // letters "C|" itself and turns them back when resolving relative links.
    std::string source = url_path(fe / "launcher.rml");
    std::replace(source.begin(), source.end(), ':', '|');
    doc_ = ctx_->LoadDocumentFromMemory(rml, source);
    if (!doc_) {
        *error = "cannot load " + u8(fe / "launcher.rml");
        return false;
    }
    doc_->SetClass("ingame", mode_ == Mode::InGame);
    doc_->AddEventListener(Rml::EventId::Click, this);
    doc_->AddEventListener(Rml::EventId::Keydown, this);
    doc_->AddEventListener(Rml::EventId::Focus, this, true);
    doc_->AddEventListener(Rml::EventId::Mouseover, this, true);
    doc_->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    if (mode_ == Mode::Pregame)
        start_verify(false);
    refresh_pads();
    dirty_ = true;
    return true;
}

std::vector<std::string> MenuUi::all_screens() {
    std::vector<std::string> out{"launcher"};
    for (const auto& t : kTabs) out.push_back(std::string("settings/") + t[0]);
    return out;
}

std::string MenuUi::screen_name() const {
    return panel_ ? std::string("settings/") + kTabs[tab_][0] : std::string("launcher");
}

void MenuUi::show(const std::string& screen) {
    modal_open_ = false;
    if (screen.rfind("settings", 0) == 0) {
        const auto slash = screen.find('/');
        open_panel(slash == std::string::npos ? std::string("general") : screen.substr(slash + 1));
    } else if (mode_ == Mode::Pregame) {
        panel_ = false;
        focus_first_ = true;
        dirty_ = true;
    }
}

void MenuUi::open_panel(const std::string& tab) {
    for (int i = 0; i < kTabCount; ++i)
        if (tab == kTabs[i][0])
            tab_ = i;
    if (!panel_ && mode_ == Mode::Pregame)
        panel_from_ = focus_id();
    if (!panel_ || mode_ == Mode::InGame) {
        ctl_view_ = 0;
        bindings_loaded_ = false;
        capture_.active = false;
    }
    panel_ = true;
    focus_first_ = true;
    dirty_ = true;
}

void MenuUi::close_panel() {
    if (modal_open_ && modal_.confirm_quit) {
        modal_open_ = false;
        dirty_ = true;
    }
    if (mode_ == Mode::InGame) {
        if (host_.close_panel)
            host_.close_panel();
        return;
    }
    panel_ = false;
    want_focus_ = panel_from_.empty() ? std::string("l-start") : panel_from_;
    dirty_ = true;
}

// ---- disc -----------------------------------------------------------------------------------------

void MenuUi::start_verify(bool sync) {
    const std::string path = cfg_.settings->get("disc");
    if (path.empty()) {
        disc_ = DiscCheck{};
        verifying_ = false;
        return;
    }
    const std::string expected = game_ok_ ? game_.sha1_1st_read : std::string();
    const auto p = std::filesystem::u8path(path);
    if (sync) {
        disc_ = verify_disc(p, expected);
        verifying_ = false;
        return;
    }
    verifying_ = true;
    verify_ = std::async(std::launch::async, [p, expected] { return verify_disc(p, expected); });
}

void MenuUi::verify_now() {
    if (verifying_ && verify_.valid())
        verify_.wait();
    verifying_ = false;
    start_verify(true);
    dirty_ = true;
}

void MenuUi::disc_picked(const std::string& path) {
    dialog_open_ = false;
    dirty_ = true;
    if (path.empty())
        return;
    // Loading a disc is an action, not a setting: it is checked at once and, when it is the right
    // one, remembered at once. A wrong one is explained in a message and not kept.
    const std::string expected = game_ok_ ? game_.sha1_1st_read : std::string();
    const DiscCheck c = verify_disc(std::filesystem::u8path(path), expected);
    const std::string file = u8(std::filesystem::u8path(path).filename());
    if (disc_playable(c)) {
        disc_ = c;
        cfg_.settings->set("disc", path);
        if (!cfg_.settings->save())
            std::fprintf(stderr, "launcher: could not write %s\n",
                         u8(cfg_.settings->file()).c_str());
        std::printf("launcher: disc loaded: %s\n", path.c_str());
        want_focus_ = "l-start";
        return;
    }
    modal_open_ = true;
    if (c.status == DiscStatus::WrongRevision) {
        modal_ = {"Wrong release or revision",
                  file + " is a different release or revision of " + title() +
                      ". This port is built for one exact version and cannot run another.",
                  "disc&#160;&#160;" + esc(c.found_sha1) + "<br/>expected&#160;&#160;" +
                      esc(c.expected_sha1)};
    } else {
        modal_ = {"Cannot read this image",
                  file + ": " + (c.detail.empty() ? std::string("not a readable disc image") : c.detail) +
                      ". Choose a .cue, .gdi or .chd dump of your own disc.",
                  ""};
    }
    want_focus_ = "modal-ok";
    std::printf("launcher: disc refused: %s\n", modal_.title.c_str());
}

bool MenuUi::playable() const { return !verifying_ && disc_playable(disc_) && game_ok_; }

// ---- rows -------------------------------------------------------------------------------------------

std::string MenuUi::name_html(const std::string& key, const std::string& name, bool live) const {
    std::string s = esc(name);
    if (mode_ == Mode::InGame && !live)
        s += "<span class=\"badge\">Next start</span>";
    (void)key;
    return s;
}

std::string MenuUi::row_cycle(const std::string& key, const std::string& name,
                              const std::string& value, const std::string& help, bool live) {
    const std::string id = "r-" + key;
    descs_[id] = {name, help, live};
    return "<div id=\"" + id + "\" class=\"prow focusable" + (draft_.pending(key) ? " pending" : "") +
           "\" act=\"cycle:" + key + "\"><div class=\"plabel\">" + name_html(key, name, live) +
           "</div><div class=\"pvalue\"><span class=\"chev\" act=\"prev:" + key +
           "\">\xE2\x80\xB9</span><span class=\"val\">" + esc(value) +
           "</span><span class=\"chev\" act=\"next:" + key +
           "\">\xE2\x80\xBA</span></div><span class=\"pend\"></span></div>";
}

std::string MenuUi::row_toggle(const std::string& key, const std::string& name, bool on,
                               const std::string& help, bool live) {
    const std::string id = "r-" + key;
    descs_[id] = {name, help, live};
    return "<div id=\"" + id + "\" class=\"prow focusable" + (draft_.pending(key) ? " pending" : "") +
           "\" act=\"toggle:" + key + "\"><div class=\"plabel\">" + name_html(key, name, live) +
           "</div><div class=\"pvalue\"><span class=\"switch-label\">" + (on ? "On" : "Off") +
           "</span><span class=\"switch" + (on ? " on" : "") +
           "\"><span class=\"knob\"></span></span></div><span class=\"pend\"></span></div>";
}

std::string MenuUi::row_radio(const std::string& key, const std::string& name,
                              const std::vector<Choice>& choices, const std::string& current,
                              const std::string& help, bool live,
                              const std::vector<bool>& enabled) {
    const std::string id = "r-" + key;
    descs_[id] = {name, help, live};
    std::string r = "<div id=\"" + id + "\" class=\"orow" +
                    (draft_.pending(key) ? " pending" : "") + "\"><div class=\"oname\">" +
                    name_html(key, name, live) + "<span class=\"pend\"></span></div><div "
                    "class=\"oopts\">";
    for (std::size_t i = 0; i < choices.size(); ++i) {
        const bool on = i >= enabled.size() || enabled[i];
        const bool sel = choices[i].value == current;
        r += "<div id=\"o-" + key + "-" + id_safe(choices[i].value) + "\" class=\"opt" +
             (sel ? " sel" : "") + (on ? " focusable" : " off") + "\" act=\"" +
             (on ? "set:" + key + "=" + choices[i].value : std::string()) + "\">" +
             esc(choices[i].label) + "</div>";
    }
    return r + "</div></div>";
}

std::string MenuUi::row_onoff(const std::string& key, const std::string& name, bool on,
                              const std::string& help, bool live) {
    return row_radio(key, name, {{"false", "Off"}, {"true", "On"}}, on ? "true" : "false", help,
                     live);
}

void MenuUi::select(const std::string& key, const std::string& value) {
    if (key == "texture_pack")
        toggle(key, value == "true" ? 1 : -1);
    else
        draft_.set(key, value);
    want_focus_ = "o-" + key + "-" + id_safe(value);
    dirty_ = true;
}

std::string MenuUi::row_slider(const std::string& key, const std::string& name, int percent,
                               const std::string& help, bool live) {
    const std::string id = "r-" + key;
    descs_[id] = {name, help, live};
    return "<div id=\"" + id + "\" class=\"prow focusable" + (draft_.pending(key) ? " pending" : "") +
           "\" act=\"slider:" + key + "\"><div class=\"plabel\">" + name_html(key, name, live) +
           "</div><div class=\"pvalue\"><div class=\"slider\" id=\"" + key +
           "-slider\"><div class=\"track\"><div class=\"fill\" style=\"width: " +
           std::to_string(percent) + "%;\"></div></div></div><span class=\"slider-val\">" +
           (percent == 0 ? std::string("Off") : std::to_string(percent) + " %") +
           "</span></div><span class=\"pend\"></span></div>";
}

std::string MenuUi::row_button(const std::string& id, const std::string& act,
                               const std::string& name, const std::string& button,
                               const std::string& help, bool enabled) {
    descs_[id] = {name, help, true};
    std::string r = "<div id=\"" + id + "\" class=\"prow" + (enabled ? " focusable" : " dim") +
                    "\" act=\"" + (enabled ? act : std::string()) + "\"><div class=\"plabel\">" +
                    esc(name) + "</div>";
    if (!button.empty())
        r += "<div class=\"pvalue\"><span class=\"inline-button\">" + esc(button) + "</span></div>";
    r += "<span class=\"pend\"></span></div>";
    return r;
}

bool MenuUi::draft_expanded() const {
    if (!cfg_.port || !cfg_.port->widescreen)
        return false;
    const std::string a = draft_.get("aspect", "expanded");
    return a != "original" && a != "4:3";
}

float MenuUi::draft_aspect() const {
    // Expanded follows the window; 16:9 stands for it in the resolution's description.
    return draft_expanded() ? std::min(16.0f / 9.0f, cfg_.port->max_aspect) : 4.0f / 3.0f;
}

bool MenuUi::tab_has_settings(int tab) const { return tab != 1; }

// ---- pages ------------------------------------------------------------------------------------------

std::string MenuUi::page_launcher() const {
    std::string s = "<div id=\"backdrop\"><div class=\"band b0\"></div><div class=\"band "
                    "b1\"></div><div class=\"band b2\"></div><div class=\"band b3\"></div><div "
                    "class=\"band b4\"></div><div class=\"band b5\"></div><div class=\"band "
                    "b6\"></div><div class=\"band b7\"></div></div>";
    std::error_code ec;
    if (std::filesystem::exists(cfg_.exe_dir / "frontend" / "art.png", ec))
        s += "<img id=\"art\" src=\"art.png\"/>";
    std::string sub;
    if (game_ok_) {
        sub = game_.product;
        if (!game_.region.empty())
            sub += (sub.empty() ? "" : kDot) + game_.region;
    }
    s += "<div id=\"launcher\"><div id=\"l-title\">" + esc(title()) + "</div><div id=\"l-sub\">" +
         esc(sub) + "</div><div id=\"lmenu\">";
    const bool ready = playable();
    s += std::string("<div id=\"l-start\" class=\"litem focusable\" act=\"") +
         (ready ? "start" : "load") + "\">" + (ready ? "Start Game" : "Load Game") + "</div>";
    s += "<div id=\"l-controls\" class=\"litem focusable\" act=\"panel:controls\">Controls</div>";
    s += "<div id=\"l-settings\" class=\"litem focusable\" act=\"panel:general\">Settings</div>";
    s += "<div id=\"l-mods\" class=\"litem focusable\" act=\"panel:mods\">Mods</div>";
    s += "<div id=\"l-quit\" class=\"litem focusable\" act=\"exit\">Quit</div></div>";

    // The disc, discreetly, under the list.
    std::string cls, text;
    const std::string shown = disc_.path.empty() ? cfg_.settings->get("disc") : disc_.path;
    const std::string file =
        shown.empty() ? std::string() : u8(std::filesystem::u8path(shown).filename());
    if (!game_ok_) {
        cls = "bad";
        text = std::string(kCross) + "  This build cannot start: " + game_error_;
    } else if (verifying_) {
        text = "Checking disc" + std::string(kEllipsis);
    } else if (dialog_open_) {
        text = "Choose your disc image" + std::string(kEllipsis);
    } else if (disc_playable(disc_)) {
        cls = "ok";
        text = std::string(kCheck) + "  " + file + "  \xE2\x80\x94  verified";
    } else if (disc_.status == DiscStatus::None) {
        text = "Load Game: choose an image of your own disc (.cue, .gdi or .chd)";
    } else {
        cls = "bad";
        text = std::string(kCross) + "  " + file + "  \xE2\x80\x94  " +
               (disc_.status == DiscStatus::WrongRevision ? "wrong release or revision"
                                                          : "cannot be read");
    }
    s += "<div id=\"l-disc\" class=\"" + cls + "\">" + esc(text) + "</div></div>";
    if (ready)
        s += "<div id=\"l-change\" class=\"focusable\" act=\"load\">Change disc" +
             std::string(kEllipsis) + "</div>";
    s += "<div id=\"version\">" + esc(cfg_.version) + "</div>";
    s += "<div id=\"l-hints\">";
    if (has_pad())
        s += glyph(glyph_a()) + hint("Select");
    else
        s += glyph(kGlyphEnter) + hint("Select");
    s += "</div>";
    return s;
}

std::string MenuUi::tab_general() {
    std::string s;
    s += row_slider("rumble", "Rumble strength", std::clamp(draft_.get_int("rumble", 100), 0, 100),
                    "How strongly controllers with motors vibrate when the game asks for it. 0 % "
                    "turns rumble off.",
                    true);
    s += row_radio("fps", "Frame rate", {{"auto", "Auto"}, {"120", "120"}, {"60", "60"}},
                   draft_.get("fps", "auto"),
                   "<b>Auto</b> blends an in-between frame between the game's frames on displays "
                   "of 120 Hz or more.<br/><b>120</b>: always blend.<br/><b>60</b>: only the "
                   "game's own frames.<br/><br/>The game itself always runs at 60.",
                   false);
    s += row_radio("frame_timing", "Frame timing",
                   {{"display", "Match display"}, {"exact", "Exact 59.94 Hz"}},
                   draft_.get("frame_timing", "display") == "exact" ? "exact" : "display",
                   "<b>Match display</b> runs the game at your display's rate when it is within "
                   "half a percent of the Dreamcast's 59.94 Hz (a 60 Hz screen: 0.1 % faster, "
                   "which nobody can hear or see), so every refresh gets a new frame.<br/><br/>"
                   "<b>Exact 59.94 Hz</b> keeps the console's timing; a 60 Hz screen then shows "
                   "one frame twice about every 17 seconds.",
                   false);
    if (cfg_.can_open_external)
        s += row_button("r-about", "readme", "About", "Read me", kAboutText);
    return s;
}

std::string MenuUi::tab_controls() {
    if (!bindings_loaded_)
        load_bindings();
    return ctl_view_ == 0 ? controls_cards() : controls_mappings();
}

std::string MenuUi::controls_cards() {
    const unsigned players =
        std::clamp(cfg_.port ? cfg_.port->players : 4u, 1u, 4u);
    std::string s = "<div id=\"ccards\">";
    for (unsigned p = 0; p < players; ++p) {
        // A console has no keyboard: player 1 is its controller, connected or not yet.
        const bool pad = p < pads_.size() || (p == 0 && !cfg_.keyboard);
        const bool usable = p == 0 || pad;
        std::string icon = utf8(pad || p > 0 ? kDeviceGamepad : kDeviceKeyboard);
        std::string dev = p < pads_.size() ? pads_[p]
                          : pad          ? std::string("Controller")
                          : (p == 0 ? std::string("Keyboard") : "No controller");
        std::string dev2 = !cfg_.keyboard ? ""
                           : p == 0 && pad ? "+ Keyboard"
                                           : (p == 0 ? "Connect a controller at any time" : "");
        s += "<div class=\"ccard-wrap\"><div class=\"ccard-label\">Player " + std::to_string(p + 1) +
             "</div><div class=\"ccard" + std::string(usable ? "" : " empty") +
             "\"><div class=\"cicon\">" + icon + "</div><div class=\"cdev\">" + esc(dev) +
             "</div><div class=\"cdev2\">" + esc(dev2) + "</div></div>";
        const std::string id = "c-edit-" + std::to_string(p);
        s += "<div id=\"" + id + "\" class=\"button secondary" +
             std::string(usable ? " focusable" : " off") + "\" act=\"" +
             (usable ? "ctl-edit:" + std::to_string(p) : std::string()) +
             "\">Edit Profile</div></div>";
    }
    s += cfg_.keyboard
             ? "</div><div class=\"cnote\">Controllers play in the order they are connected; the "
               "keyboard plays as player 1. Every controller uses the controller profile.</div>"
             : "</div><div class=\"cnote\">Controllers play in the order they are connected. "
               "Every controller uses the same profile.</div>";
    return s;
}

std::string MenuUi::controls_mappings() {
    const bool kb = ctl_device_ == 0;
    std::string s = "<div class=\"chead\">";
    if (capture_.active)
        s += std::string("<div class=\"ctitle rec\">") +
             (kb ? "Press a key for " : "Press a button or move a stick for ") +
             ctl_label(capture_.control) + "</div><div class=\"pspacer\"></div><div "
             "class=\"chint\">" + (kb ? "Esc cancels" : cfg_.keyboard ? "Select or Esc cancels" : "The touch pad cancels") + "</div>";
    else
        s += std::string("<div class=\"ctitle\">Editing: ") +
             (kb ? "Keyboard profile" : "Controller profile") +
             "</div><div class=\"pspacer\"></div><div id=\"c-back\" class=\"button "
             "focusable\" act=\"ctl-back\">Go back</div>";
    s += "</div>";
    const auto& dev = editing();
    const std::string how =
        !cfg_.keyboard
            ? "Press " + std::string(glyph(glyph_a())) +
                  " on a box, then press the button or move the stick to use. The touch pad "
                  "cancels."
        : kb ? "Click a box, or press Enter on it, then press the key to use. Escape cancels."
           : "Click a box, or press " + glyph(glyph_a()) +
                 " on it, then press the button or move the stick to use. Select or Escape "
                 "cancels.";
    const std::string who = kb ? "The keyboard plays as player 1, together with the first "
                                 "controller."
                               : "Every controller uses this profile; controller 1 plays as "
                                 "player 1, controller 2 as player 2.";
    for (const auto& r : kCtlRows) {
        const unsigned i = static_cast<unsigned>(r.c);
        const std::string key = id_safe(dream::render::pad_control_key(r.c));
        const std::string row = "b-" + key;
        descs_[row] = {r.label,
                       std::string("Two boxes: either binding works.<br/><br/>") + how +
                           "<br/><br/>" + glyph(utf8(kIconCross).c_str()) +
                           " clears both.<br/><br/>" + who,
                       true};
        s += "<div id=\"" + row + "\" class=\"crow\"><div class=\"clabel\">" + r.label +
             "</div><div class=\"cslots\">";
        for (unsigned slot = 0; slot < dream::render::kBindingSlots; ++slot) {
            const auto& b = dev.slot(r.c, slot);
            const bool rec = capture_.active && capture_.control == i && capture_.slot == slot;
            std::string inner;
            if (rec)
                inner = "<span class=\"brec\"></span>";
            else if (b.bound()) {
                const std::string g = binding_glyph(b, ps_glyphs(), !cfg_.keyboard);
                inner = g.empty() ? "<span class=\"btext\">" + esc(b.label()) + "</span>"
                                  : "<span class=\"bglyph\">" + g + "</span>";
            }
            s += "<div id=\"s-" + key + "-" + std::to_string(slot) + "\" class=\"cslot focusable" +
                 (rec ? " rec" : "") + "\" act=\"bind:" + std::to_string(i) + ":" +
                 std::to_string(slot) + "\">" + inner + "</div>";
        }
        s += "<div id=\"t-" + key + "\" class=\"ctrash focusable\" act=\"ctl-clear:" +
             std::to_string(i) + "\">" + utf8(kIconCross) + "</div></div></div>";
    }
    return s;
}

std::string MenuUi::controls_footer() {
    const bool kb = ctl_device_ == 0;
    descs_["c-dev"] = {"Controller / keyboard",
                       "Which profile you are editing. Both are live at once: the keyboard "
                       "still works with a controller plugged in.",
                       true};
    descs_["c-reset"] = {"Reset to defaults",
                         std::string("Puts every binding in the ") +
                             (kb ? "keyboard" : "controller") +
                             " profile back to the original layout.",
                         true};
    const std::string reset =
        "<div id=\"c-reset\" class=\"button warning focusable\" act=\"ctl-reset\">Reset to "
        "defaults</div>";
    if (!cfg_.keyboard)
        return reset;  // a console: the controller profile only
    return std::string("<div id=\"c-dev\" class=\"ctoggle focusable\" act=\"ctl-device\"><span "
                       "class=\"tg") +
           (kb ? "" : " on") + "\">" + utf8(kDeviceGamepad) + "</span><span class=\"tg" +
           (kb ? " on" : "") + "\">" + utf8(kDeviceKeyboard) + "</span></div>" + reset;
}

dream::render::DeviceBindings& MenuUi::editing() {
    return ctl_device_ == 0 ? bindings_.keyboard : bindings_.gamepad;
}

void MenuUi::load_bindings() {
    std::string path = host_.bindings_path ? host_.bindings_path() : std::string();
    if (path.empty())
        path = dream::render::vk::default_bindings_path();
    bindings_ = dream::render::Bindings::defaults();
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (in) {
        std::stringstream ss;
        ss << in.rdbuf();
        bindings_ = dream::render::Bindings::from_text(ss.str());
    }
    bindings_loaded_ = true;
}

void MenuUi::save_bindings() {
    std::string path = host_.bindings_path ? host_.bindings_path() : std::string();
    if (path.empty())
        path = dream::render::vk::default_bindings_path();
    const std::string text = bindings_.to_text();
    std::ofstream out(std::filesystem::u8path(path), std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        std::fprintf(stderr, "controls: cannot write %s\n", path.c_str());
        return;
    }
    out.close();
    std::printf("controls: saved %s\n", path.c_str());
    if (host_.bindings_changed)
        host_.bindings_changed();
}

void MenuUi::finish_capture(const dream::render::Binding* b) {
    const unsigned control = capture_.control, slot = capture_.slot;
    capture_.active = false;
    dirty_ = true;
    want_focus_ = "s-" + id_safe(dream::render::pad_control_key(
                             static_cast<dream::render::PadControl>(control))) +
                  "-" + std::to_string(slot);
    if (!b) {
        std::printf("controls: capture cancelled\n");
        return;
    }
    editing().slot(static_cast<dream::render::PadControl>(control), slot) = *b;
    std::printf("controls: %s %s slot %u = %s\n", ctl_device_ == 0 ? "keyboard" : "controller",
                ctl_label(control), slot + 1, b->text().c_str());
    save_bindings();
}

// While a slot waits for input: the next key (keyboard profile) or button / stick direction
// (controller profile) becomes the binding. Escape, or a pad's Select, cancels. True: consumed.
bool MenuUi::capture_event(const SDL_Event& ev) {
    using dream::render::BindSource;
    const bool kb = ctl_device_ == 0;
    switch (ev.type) {
    case SDL_EVENT_KEY_DOWN: {
        if (ev.key.repeat)
            return true;
        if (ev.key.key == SDLK_ESCAPE) {
            finish_capture(nullptr);
            return true;
        }
        if (!kb)
            return true;
        const char* n = SDL_GetScancodeName(ev.key.scancode);
        if (!n || !*n)
            return true;
        const dream::render::Binding b{BindSource::Key, n, 1};
        finish_capture(&b);
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
            finish_capture(nullptr);
            return true;
        }
        if (kb)
            return true;
        const char* n =
            SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(ev.gbutton.button));
        if (!n || !*n)
            return true;
        const dream::render::Binding b{BindSource::Button, n, 1};
        finish_capture(&b);
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (kb || std::abs(static_cast<int>(ev.gaxis.value)) < 16384)
            return true;
        const char* n = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(ev.gaxis.axis));
        if (!n || !*n)
            return true;
        const dream::render::Binding b{BindSource::Axis, n, ev.gaxis.value < 0 ? -1 : 1};
        finish_capture(&b);
        return true;
    }
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        // A click elsewhere cancels; on another box it then starts that one.
        finish_capture(nullptr);
        return false;
    default:
        return false;
    }
}

std::string MenuUi::tab_graphics() {
    const bool ws = cfg_.port && cfg_.port->widescreen;
    const bool anam = cfg_.port && cfg_.port->widescreen_anamorphic;
    std::string s;
    const int scale = std::clamp(draft_.get_int("scale", 2), 1, 8);
    std::vector<Choice> scales;
    for (int i = 1; i <= 8; ++i)
        scales.push_back({std::to_string(i), i == 1 ? std::string("Original") : std::to_string(i) + "x"});
    s += row_radio("scale", "Resolution", scales, std::to_string(scale),
                   "Draws the 3D scene at a multiple of the console's 640\xC3\x97" "480. Higher is "
                   "sharper and needs a faster graphics card.<br/><br/>Now: " +
                       esc(scale_label(scale, draft_aspect(), anam)) +
                       (draft_expanded() ? " at 16:9; the width follows the window" : ""),
                   true);  // live: the render target is rebuilt at the next vblank
    // As in the N64 recomps: the original 4:3 picture, or the view widened to the window.
    s += row_radio("aspect", "Aspect ratio", {{"original", "Original"}, {"expanded", "Expanded"}},
                   ws && draft_expanded() ? "expanded" : "original",
                   ws ? "<b>Original</b> shows the console's 4:3 picture, with black bars when the "
                        "window is wider.<br/><br/><b>Expanded</b> widens the game's view to fill "
                        "the window, whatever its shape, without stretching anything."
                      : "This port shows the original 4:3 picture only.",
                   true, {true, ws});
    if (cfg_.can_window)
        s += row_radio("fullscreen", "Window mode", {{"false", "Windowed"}, {"true", "Fullscreen"}},
                       draft_.get_bool("fullscreen", false) ? "true" : "false",
                       "Borderless fullscreen on the window's display. Alt+Enter switches at any time.",
                       true);
#ifdef _WIN32
    s += row_radio("latency", "Latency", {{"standard", "Standard"}, {"low", "Low"}},
                   draft_.get("latency", "standard") == "low" ? "low" : "standard",
                   "<b>Standard</b> keeps a few frames queued for the screen: perfectly smooth "
                   "everywhere.<br/><br/><b>Low</b> hands each frame to the screen as directly as "
                   "Windows allows, about 13 ms sooner. It needs a computer that finishes every "
                   "frame in time: on a laptop running on battery it falls back to queueing "
                   "frames, and an occasional frame may repeat.",
                   false);
#endif
    if (cfg_.port && cfg_.port->hud.enabled) {
        // With Expanded the HUD is never stretched; this only says where it sits.
        const bool wide = draft_expanded();
        s += row_radio("hud_layout", "HUD layout",
                       {{"center", "Original"}, {"edges", "Expanded"}},
                       draft_.get("hud_layout", "edges") == "center" ? "center" : "edges",
                       "How the HUD sits on a widened picture. It always keeps its original "
                       "proportions.<br/><br/><b>Original</b> keeps the whole HUD in the 4:3 area "
                       "in the middle of the screen, as the console showed it.<br/><br/><b>Expanded"
                       "</b> anchors each part to its side: health bars and names to the left and "
                       "right edges, the timer in the centre, at any width.<br/><br/>Only with the "
                       "Expanded aspect ratio.",
                       true, {wide, wide});
    }
    return s;
}

std::string MenuUi::tab_sound() {
    std::string s = row_slider("volume", "Master volume",
                               std::clamp(draft_.get_int("volume", 100), 0, 100),
                               "The level of everything the game plays.", true);
    s += row_radio("clip", "Loud sounds", {{"hard", "Original"}, {"soft", "Softened"}},
                   draft_.get("clip", "hard"),
                   "<b>Original</b> is the console's own output: its mix is loud, and the loudest "
                   "hits clip, which you hear as crackle.<br/><br/><b>Softened</b> rounds those "
                   "peaks off smoothly instead. Everything below them is unchanged.",
                   true);
    return s;
}

std::string MenuUi::tab_mods() {
    std::string s;
    const std::string pack = draft_.get("texture_pack");
    const auto dir = texture_pack_dir(pack, config_dir_);
    s += row_onoff("texture_pack", "Texture pack", texture_pack_on(pack),
                    "Replaces the game's textures with the PNG files in<br/>" + esc(u8(dir)), false);
    if (cfg_.can_open_external)
        s += row_button("r-opentex", "open-textures", "Open texture folder", "Open",
                    "Opens " + esc(u8(dir)) + " (created if missing). Put a pack's files there.");
    s += row_onoff("dump_textures", "Dump textures", draft_.get_bool("dump_textures", false),
                    "For pack authors: writes every new texture the game uses to<br/>" +
                        esc(u8(config_dir_ / "texture_dump")),
                    false);
    s += "<div class=\"psection\">Mods</div>";
    const auto mods_dir = config_dir_ / "mods";
    const auto mods = list_mods(mods_dir, draft_.get("mods"));
    const std::string mod_help =
        "Each folder in the mods folder mirrors the disc's files and replaces the files it "
        "contains. Enabled mods load top to bottom; the first one providing a file wins. Use "
        "\xE2\x96\xB2 \xE2\x96\xBC to change the order.";
    int order = 0;
    for (std::size_t i = 0; i < mods.size(); ++i) {
        const auto& m = mods[i];
        const std::string id = id_safe(m.name);
        const bool can_up = m.enabled && i > 0;
        const bool can_down = m.enabled && i + 1 < mods.size() && mods[i + 1].enabled;
        descs_["m-" + id] = {m.name, mod_help, false};
        s += "<div class=\"mod-row\"><span class=\"mod-order\">" +
             (m.enabled ? std::to_string(++order) : std::string()) + "</span>";
        s += "<div id=\"m-" + id + "\" class=\"prow focusable" +
             std::string(draft_.pending("mods") ? " pending" : "") + "\" act=\"mod:" + esc(m.name) +
             "\"><div class=\"plabel\">" + name_html("mods", m.name, false) +
             "</div><div class=\"pvalue\"><span class=\"switch-label\">" +
             (m.enabled ? "On" : "Off") + "</span><span class=\"switch" + (m.enabled ? " on" : "") +
             "\"><span class=\"knob\"></span></span></div></div>";
        s += "<div id=\"mu-" + id + "\" class=\"mini" + std::string(can_up ? " focusable" : " off") +
             "\" act=\"" + (can_up ? "modup:" + esc(m.name) : std::string()) +
             "\">\xE2\x96\xB2</div>";
        s += "<div id=\"md-" + id + "\" class=\"mini" +
             std::string(can_down ? " focusable" : " off") + "\" act=\"" +
             (can_down ? "moddown:" + esc(m.name) : std::string()) + "\">\xE2\x96\xBC</div></div>";
    }
    if (mods.empty())
        s += "<div class=\"pnote\">No mods installed.</div>";
    if (cfg_.can_open_external)
        s += row_button("r-openmods", "open-mods", "Open mods folder", "Open",
                    "Opens " + esc(u8(mods_dir)) + " (created if missing). Add each mod as a "
                    "folder there.<br/><br/>" + mod_help);
    return s;
}

std::string MenuUi::page_panel() {
    descs_.clear();
    std::string s = "<div id=\"shade\"></div><div id=\"panel\"><div id=\"ptabs\">";
    for (int i = 0; i < kTabCount; ++i)
        s += std::string("<div id=\"tab-") + kTabs[i][0] + "\" class=\"ptab focusable" +
             (i == tab_ ? " active" : "") + "\" act=\"tab:" + std::to_string(i) + "\">" +
             kTabs[i][1] + "</div>";
    s += "<div class=\"pspacer\"></div>";
    if (cfg_.can_quit)
        s += "<div id=\"p-quit\" class=\"pquit focusable\" act=\"ask-quit\">Quit Game</div>";
    s += "<div id=\"p-close\" class=\"pclose focusable\" "
         "act=\"close\">\xC3\x97</div></div><div id=\"pbody\"" +
         std::string(tab_ == 1 && ctl_view_ == 0 ? " class=\"wide\"" : "") +
         "><div id=\"prows\">";
    switch (tab_) {
    case 0: s += tab_general(); break;
    case 1: s += tab_controls(); break;
    case 2: s += tab_graphics(); break;
    case 3: s += tab_sound(); break;
    default: s += tab_mods(); break;
    }
    s += "</div><div id=\"pdesc\"><div id=\"pdesc-title\"></div><div id=\"pdesc-text\"></div><div "
         "id=\"pdesc-note\"></div></div></div><div id=\"pfoot\"><div id=\"phints\">";
    if (has_pad())
        s += glyph(glyph_a()) + hint("Select") + glyph(glyph_b()) + hint("Close") +
             glyph(glyph_lb()) + glyph(glyph_rb()) + hint("Tabs");
    else
        s += glyph(kGlyphEnter) + hint("Select") + glyph(kGlyphEsc) + hint("Close") +
             keycap("Q") + keycap("E") + hint("Tabs");
    s += "</div>";
    if (tab_ == 1 && ctl_view_ == 1)
        s += controls_footer();
    s += "<div id=\"pending\">";
    if (!toast_.empty())
        s += esc(toast_);
    else if (draft_.any_pending())
        s += "\xE2\x97\x8F  Not applied yet";
    s += "</div>";
    if (tab_has_settings(tab_))
        s += std::string("<div id=\"btn-apply\" class=\"button focusable") +
             (draft_.any_pending() ? " attention" : "") + "\" act=\"apply\">Apply</div>";
    s += "</div></div>";
    return s;
}

std::string MenuUi::page_modal() const {
    std::string s = std::string("<div id=\"mshade\"></div><div id=\"modal\"") +
                    (modal_.confirm_quit ? " class=\"confirm\"" : "") +
                    "><div class=\"mtitle\">" + esc(modal_.title) + "</div><div class=\"mtext\">" +
                    esc(modal_.text) + "</div>" +
                    (modal_.extra.empty() ? std::string()
                                          : "<div class=\"hashes\">" + modal_.extra + "</div>") +
                    "<div class=\"mbuttons\">";
    if (modal_.confirm_quit)
        s += "<div id=\"modal-quit\" class=\"button danger focusable\" act=\"exit\">Quit</div>"
             "<div id=\"modal-cancel\" class=\"button focusable\" act=\"modal-ok\">Cancel</div>";
    else
        s += "<div id=\"modal-ok\" class=\"button focusable\" act=\"modal-ok\">OK</div>";
    return s + "</div></div>";
}

void MenuUi::describe(Rml::Element* e) {
    if (!doc_ || !panel_)
        return;
    for (; e; e = e->GetParentNode()) {
        auto it = descs_.find(e->GetId());
        if (it == descs_.end())
            continue;
        if (desc_shown_ == it->first)
            return;
        desc_shown_ = it->first;
        if (auto* t = doc_->GetElementById("pdesc-title"))
            t->SetInnerRML(esc(it->second.title));
        if (auto* t = doc_->GetElementById("pdesc-text"))
            t->SetInnerRML(it->second.text);
        if (auto* t = doc_->GetElementById("pdesc-note"))
            t->SetInnerRML(mode_ == Mode::InGame && !it->second.live
                               ? "Takes effect the next time the game starts."
                               : "");
        redraw_ = true;
        return;
    }
}

void MenuUi::rebuild() {
    dirty_ = false;
    redraw_ = true;
    Rml::Element* focused_el = ctx_->GetFocusElement();
    std::string focus_id = focused_el && focused_el != doc_ ? focused_el->GetId() : std::string();
    if (focus_id.empty())
        focus_id = last_focus_;
    std::string body;
    if (mode_ == Mode::Pregame)
        body = page_launcher();
    if (panel_)
        body += page_panel();
    if (modal_open_)
        body += page_modal();
    doc_->SetClass("panel-open", panel_);
    desc_shown_.clear();
    if (auto* page = doc_->GetElementById("page"))
        page->SetInnerRML(body);
    ctx_->Update();
    Rml::Element* target = nullptr;
    if (!want_focus_.empty()) {
        target = doc_->GetElementById(want_focus_);
        want_focus_.clear();
    }
    if (!target && focus_first_) {
        if (panel_) {
            if (auto* rows = doc_->GetElementById("prows"))
                target = rows->QuerySelector(".focusable");
            if (!target)
                target = doc_->GetElementById(std::string("tab-") + kTabs[tab_][0]);
        } else {
            target = doc_->GetElementById("l-start");
        }
    }
    focus_first_ = false;
    if (!target && !focus_id.empty())
        target = doc_->GetElementById(focus_id);
    if (!target)
        target = doc_->QuerySelector(".focusable");
    if (target) {
        target->Focus(true);
        target->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
        last_focus_ = target->GetId();
        describe(target);
    }
    ctx_->Update();
}

// ---- actions ------------------------------------------------------------------------------------

void MenuUi::set_tab(int t) {
    if (!panel_)
        return;
    tab_ = (t % kTabCount + kTabCount) % kTabCount;
    Rml::Element* f = focused();
    if (f && f->GetId().rfind("tab-", 0) == 0)
        want_focus_ = std::string("tab-") + kTabs[tab_][0];
    else
        focus_first_ = true;
    dirty_ = true;
}

void MenuUi::adjust(const std::string& key, int step) {
    if (key == "scale") {
        const int v = std::clamp(draft_.get_int("scale", 2), 1, 8) + step;
        draft_.set("scale", std::to_string(((v - 1) % 8 + 8) % 8 + 1));
    } else if (key == "aspect" && cfg_.port && cfg_.port->widescreen) {
        draft_.set("aspect", cycle(aspect_choices(true, cfg_.port->max_aspect),
                                   draft_.get("aspect", "16:9"), step));
    } else if (key == "fit") {
        draft_.set("fit", cycle(fit_choices(), draft_.get("fit", "crop"), step));
    } else if (key == "fps") {
        draft_.set("fps", cycle(fps_choices(), draft_.get("fps", "auto"), step));
    } else if (key == "frame_timing") {
        draft_.set("frame_timing", cycle({{"display", ""}, {"exact", ""}},
                                         draft_.get("frame_timing", "display"), step));
    } else if (key == "latency") {
        draft_.set("latency", cycle({{"standard", ""}, {"low", ""}}, draft_.get("latency", "standard"), step));
    } else if (key == "hud_layout") {
        draft_.set("hud_layout",
                   cycle({{"edges", ""}, {"center", ""}}, draft_.get("hud_layout", "edges"), step));
    } else if (key == "rumble" || key == "volume") {
        const int v = std::clamp(draft_.get_int(key, 100) + step * 10, 0, 100);
        draft_.set(key, std::to_string(v));
    }
    dirty_ = true;
}

void MenuUi::toggle(const std::string& key, int dir) {  // dir: 0 flip, -1 off, +1 on
    if (key == "texture_pack") {
        const bool on = texture_pack_on(draft_.get("texture_pack"));
        const bool want = dir == 0 ? !on : dir > 0;
        if (want != on) {
            const std::string saved = cfg_.settings->get("texture_pack");
            draft_.set("texture_pack", want ? (texture_pack_on(saved) ? saved : "") : "off");
        }
    } else {
        const bool def = key == "hud_fix";
        const bool on = draft_.get_bool(key, def);
        const bool want = dir == 0 ? !on : dir > 0;
        draft_.set(key, want ? "true" : "false");
    }
    dirty_ = true;
}

void MenuUi::mod_action(const std::string& kind, const std::string& name) {
    auto mods = list_mods(config_dir_ / "mods", draft_.get("mods"));
    for (std::size_t i = 0; i < mods.size(); ++i) {
        if (mods[i].name != name)
            continue;
        if (kind == "mod")
            mods[i].enabled = !mods[i].enabled;
        else if (kind == "modup" && i > 0)
            std::swap(mods[i], mods[i - 1]);
        else if (kind == "moddown" && i + 1 < mods.size())
            std::swap(mods[i], mods[i + 1]);
        break;
    }
    draft_.set("mods", mods_setting(mods));
    dirty_ = true;
}

void MenuUi::open_folder(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!SDL_OpenURL(file_url(dir).c_str()))
        std::fprintf(stderr, "launcher: cannot open %s: %s\n", u8(dir).c_str(), SDL_GetError());
}

void MenuUi::apply() {
    if (!draft_.any_pending())
        return;
    toast_ = draft_.apply() ? "Applied" : "Could not write the settings file";
    toast_until_ = SDL_GetTicks() + 2500;
    std::printf("launcher: apply: %s (%s)\n", toast_.c_str(), u8(cfg_.settings->file()).c_str());
    if (host_.applied)
        host_.applied();
    dirty_ = true;
}

void MenuUi::back() {
    if (modal_open_) {  // OK on a message, Cancel on the quit question
        modal_open_ = false;
        want_focus_ = modal_.confirm_quit && panel_ ? "p-quit" : "l-start";
        dirty_ = true;
        return;
    }
    if (panel_ && tab_ == 1 && ctl_view_ == 1) {
        ctl_view_ = 0;
        want_focus_ = "c-edit-0";
        dirty_ = true;
        return;
    }
    if (panel_) {
        close_panel();
        return;
    }
    if (auto* e = doc_->GetElementById("l-quit")) {
        e->Focus(true);
        redraw_ = true;
    }
}

void MenuUi::run_act(const std::string& act, Rml::Event* ev) {
    const auto colon = act.find(':');
    const std::string kind = act.substr(0, colon);
    const std::string arg = colon == std::string::npos ? std::string() : act.substr(colon + 1);
    if (kind == "start") {
        if (playable() && host_.start)
            host_.start();
    } else if (kind == "load") {
        if (!dialog_open_ && host_.pick_disc) {
            dialog_open_ = true;
            dirty_ = true;
            host_.pick_disc();
        }
    } else if (modal_open_ && kind != "exit" && kind != "modal-ok") {
        return;  // a dialog is up: only its own buttons act
    } else if (kind == "panel") {
        open_panel(arg);
    } else if (kind == "close") {
        close_panel();
    } else if (kind == "exit") {
        if (host_.exit_game && cfg_.can_quit)
            host_.exit_game();
    } else if (kind == "modal-ok") {
        back();
    } else if (kind == "ask-quit" && cfg_.can_quit) {
        modal_open_ = true;
        modal_ = {"Quit " + title() + "?",
                  mode_ == Mode::InGame
                      ? "The game closes. Your memory card is saved as you play; settings you have "
                        "not applied are not kept."
                      : "The launcher closes.",
                  "", true};
        want_focus_ = "modal-cancel";
        dirty_ = true;
    } else if (kind == "tab") {
        tab_ = std::clamp(std::atoi(arg.c_str()), 0, kTabCount - 1);
        want_focus_ = std::string("tab-") + kTabs[tab_][0];
        dirty_ = true;
    } else if (kind == "set") {
        const auto eq = arg.find('=');
        if (eq != std::string::npos)
            select(arg.substr(0, eq), arg.substr(eq + 1));
    } else if (kind == "cycle" || kind == "next") {
        adjust(arg, +1);
    } else if (kind == "prev") {
        adjust(arg, -1);
    } else if (kind == "toggle") {
        toggle(arg, 0);
    } else if (kind == "slider") {
        // A click on the bar sets the value there; Enter steps by 10 and wraps.
        const float mx = ev ? ev->GetParameter<float>("mouse_x", -1.0f) : -1.0f;
        Rml::Element* bar = doc_->GetElementById(arg + "-slider");
        if (mx >= 0.0f && bar) {
            const float x0 = bar->GetAbsoluteLeft() + bar->GetClientLeft();
            const float w = bar->GetClientWidth();
            if (w > 0.0f && mx >= x0 - 20.0f && mx <= x0 + w + 20.0f) {
                const int v =
                    static_cast<int>(std::clamp((mx - x0) / w, 0.0f, 1.0f) * 10.0f + 0.5f) * 10;
                draft_.set(arg, std::to_string(v));
                dirty_ = true;
            }
        } else {
            const int v = draft_.get_int(arg, 100);
            draft_.set(arg, std::to_string(v >= 100 ? 0 : v + 10));
            dirty_ = true;
        }
    } else if (kind == "mod" || kind == "modup" || kind == "moddown") {
        mod_action(kind, arg);
    } else if ((kind == "open-textures" || kind == "open-mods" || kind == "readme") &&
               !cfg_.can_open_external) {
        // not on this platform (the button is not shown either)
    } else if (kind == "open-textures") {
        open_folder(texture_pack_dir(draft_.get("texture_pack"), config_dir_));
    } else if (kind == "open-mods") {
        open_folder(config_dir_ / "mods");
    } else if (kind == "readme") {
        std::error_code ec;
        const auto local = cfg_.exe_dir / "README.md";
        const std::string url = std::filesystem::exists(local, ec)
                                    ? file_url(local)
                                    : "https://github.com/danielgomesvieira2000/dreamcomp#readme";
        SDL_OpenURL(url.c_str());
    } else if (kind == "ctl-edit") {
        const unsigned p = static_cast<unsigned>(std::atoi(arg.c_str()));
        ctl_view_ = 1;
        ctl_device_ = p < pads_.size() || !cfg_.keyboard ? 1 : 0;
        want_focus_ = "s-" + id_safe(dream::render::pad_control_key(kCtlRows[0].c)) + "-0";
        dirty_ = true;
    } else if (kind == "ctl-back") {
        back();
    } else if (kind == "ctl-device" && cfg_.keyboard) {
        ctl_device_ = 1 - ctl_device_;
        capture_.active = false;
        want_focus_ = "c-dev";
        dirty_ = true;
    } else if (kind == "bind") {
        const auto c2 = arg.find(':');
        capture_.active = true;
        capture_.control = static_cast<unsigned>(std::atoi(arg.substr(0, c2).c_str()));
        capture_.slot = c2 == std::string::npos
                            ? 0u
                            : std::min(1u, static_cast<unsigned>(std::atoi(arg.c_str() + c2 + 1)));
        want_focus_ = focus_id();
        dirty_ = true;
    } else if (kind == "ctl-clear") {
        const auto c = static_cast<dream::render::PadControl>(std::atoi(arg.c_str()));
        for (unsigned slot = 0; slot < dream::render::kBindingSlots; ++slot)
            editing().slot(c, slot) = dream::render::Binding{};
        save_bindings();
        dirty_ = true;
    } else if (kind == "ctl-reset") {
        const auto d = dream::render::Bindings::defaults();
        editing() = ctl_device_ == 0 ? d.keyboard : d.gamepad;
        save_bindings();
        dirty_ = true;
    } else if (kind == "rebind") {
        if (host_.open_bindings)
            host_.open_bindings();
    } else if (kind == "apply") {
        apply();
    }
    redraw_ = true;
}

Rml::Element* MenuUi::focused() const { return ctx_ ? ctx_->GetFocusElement() : nullptr; }

std::string MenuUi::focus_id() const {
    Rml::Element* f = focused();
    return f ? f->GetId() : std::string();
}

Rml::Element* MenuUi::element(const std::string& id) const {
    return doc_ ? doc_->GetElementById(id) : nullptr;
}

void MenuUi::ProcessEvent(Rml::Event& ev) {
    if (ev == Rml::EventId::Focus) {
        Rml::Element* t = ev.GetTargetElement();
        if (t && !t->GetId().empty() && t != doc_)
            last_focus_ = t->GetId();
        describe(t);
        redraw_ = true;
        return;
    }
    if (ev == Rml::EventId::Mouseover) {
        describe(ev.GetTargetElement());
        return;
    }
    if (ev == Rml::EventId::Click) {
        for (Rml::Element* e = ev.GetTargetElement(); e; e = e->GetParentNode()) {
            const std::string act = e->GetAttribute<Rml::String>("act", "");
            if (!act.empty()) {
                run_act(act, &ev);
                ev.StopPropagation();
                break;
            }
        }
        return;
    }
    if (ev == Rml::EventId::Keydown) {
        const auto k =
            static_cast<Rml::Input::KeyIdentifier>(ev.GetParameter<int>("key_identifier", 0));
        const bool arrow = k == Rml::Input::KI_UP || k == Rml::Input::KI_DOWN ||
                           k == Rml::Input::KI_LEFT || k == Rml::Input::KI_RIGHT;
        Rml::Element* focus = focused();
        if (!focus || focus == doc_ || focus->GetTagName() == "body") {
            if (arrow) {
                Rml::Element* to = last_focus_.empty() ? nullptr : doc_->GetElementById(last_focus_);
                if (!to)
                    to = doc_->QuerySelector(panel_ ? "#panel .focusable" : ".focusable");
                if (to)
                    to->Focus(true);
                ev.StopPropagation();
            }
            return;
        }
        if (k == Rml::Input::KI_LEFT || k == Rml::Input::KI_RIGHT) {
            const int step = k == Rml::Input::KI_LEFT ? -1 : +1;
            const std::string act = focus->GetAttribute<Rml::String>("act", "");
            const auto colon = act.find(':');
            const std::string kind = act.substr(0, colon);
            const std::string arg =
                colon == std::string::npos ? std::string() : act.substr(colon + 1);
            if (kind == "cycle" || kind == "slider") {
                adjust(arg, step);
                ev.StopPropagation();
                return;
            }
            if (kind == "toggle") {
                toggle(arg, step);
                ev.StopPropagation();
                return;
            }
        }
        if (arrow) {
            navigate(focus, k);
            ev.StopPropagation();
        }
    }
}

// Focus moves by the launcher's own spatial search (RmlUi's `nav: auto` does not enter or leave a
// scroll container), in regions: the launcher list, and in the panel the tab bar, the rows and the
// footer. Left/right stay in a region; up/down cross into the neighbouring one.
void MenuUi::navigate(Rml::Element* focus, Rml::Input::KeyIdentifier key) {
    Rml::Element* region = region_of(focus);
    if (modal_open_) {
        // Only the dialog's buttons, left and right.
        if (key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT)
            if (Rml::Element* t = nav_search(doc_->GetElementById("modal"), focus, key)) {
                t->Focus(true);
                redraw_ = true;
            }
        return;
    }
    if (!region && focus && focus->GetId() == "l-change")
        region = doc_->GetElementById("lmenu");
    Rml::Element* tabs = doc_->GetElementById("ptabs");
    Rml::Element* rows = doc_->GetElementById("prows");
    Rml::Element* foot = doc_->GetElementById("pfoot");
    Rml::Element* target = nullptr;
    if (region && region->GetId() == "lmenu") {
        // The launcher list, plus "Change disc" under it.
        Rml::ElementList items;
        doc_->QuerySelectorAll(items, "#lmenu .focusable, #l-change");
        auto it = std::find(items.begin(), items.end(), focus);
        if (it != items.end()) {
            if (key == Rml::Input::KI_DOWN && it + 1 != items.end())
                target = *(it + 1);
            else if (key == Rml::Input::KI_UP && it != items.begin())
                target = *(it - 1);
        }
    } else if (focus->IsClassSet("opt") &&
               (key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT)) {
        target = nav_search(focus->GetParentNode(), focus, key);
    } else if (region == rows && (key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN)) {
        // Up/down: the nearest row in that direction, on its selected choice (else the one
        // closest horizontally). Then out to the tabs or the footer as below.
        Rml::ElementList list;
        rows->QuerySelectorAll(list, ".focusable");
        const Rml::Rectanglef src = box_of(focus);
        const bool down = key == Rml::Input::KI_DOWN;
        float best_gap = 1e9f;
        for (Rml::Element* e : list) {
            const Rml::Rectanglef r = box_of(e);
            const float gap = down ? r.p0.y - src.p1.y : src.p0.y - r.p1.y;
            if (e != focus && gap >= -1.0f && gap < best_gap)
                best_gap = gap;
        }
        float best_x = 1e9f;
        bool best_sel = false;
        for (Rml::Element* e : list) {
            const Rml::Rectanglef r = box_of(e);
            const float gap = down ? r.p0.y - src.p1.y : src.p0.y - r.p1.y;
            if (e == focus || gap < -1.0f || gap > best_gap + 6.0f)
                continue;
            const bool sel = e->IsClassSet("sel");
            const float dx = std::fabs((r.p0.x + r.p1.x) * 0.5f - (src.p0.x + src.p1.x) * 0.5f);
            if (!target || (sel && !best_sel) || (sel == best_sel && dx < best_x)) {
                target = e;
                best_x = dx;
                best_sel = sel;
            }
        }
        if (!target && down && foot)
            target = foot->QuerySelector(".focusable");
        if (!target && !down)
            target = doc_->GetElementById(std::string("tab-") + kTabs[tab_][0]);
    } else {
        target = nav_search(region, focus, key);
        if (!target && key == Rml::Input::KI_DOWN) {
            if (region == tabs)
                target = rows ? rows->QuerySelector(".focusable") : nullptr;
            if (!target && region != foot && foot)
                target = foot->QuerySelector(".focusable");
        } else if (!target && key == Rml::Input::KI_UP) {
            if (region == foot)
                target = nav_search(rows, focus, key);
            if (!target && region != tabs)
                target = doc_->GetElementById(std::string("tab-") + kTabs[tab_][0]);
        }
    }
    if (!target)
        return;
    target->Focus(true);
    target->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
    redraw_ = true;
}

// ---- input --------------------------------------------------------------------------------------

const char* MenuUi::glyph_a() const { return ps_glyphs() ? kGlyphCross : kGlyphA; }
const char* MenuUi::glyph_b() const { return ps_glyphs() ? kGlyphCircle : kGlyphB; }
const char* MenuUi::glyph_lb() const { return ps_glyphs() ? kGlyphL1 : kGlyphLB; }
const char* MenuUi::glyph_rb() const { return ps_glyphs() ? kGlyphR1 : kGlyphRB; }

void MenuUi::refresh_pads() {
    std::vector<std::string> names;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
            const char* n = SDL_GetGamepadNameForID(ids[i]);
            names.push_back(n && *n ? n : "Controller");
            if (i == 0) {
                const SDL_GamepadType t = SDL_GetGamepadTypeForID(ids[i]);
                ps_pad_ = t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 ||
                          t == SDL_GAMEPAD_TYPE_PS5;
            }
        }
        SDL_free(ids);
    }
    set_pads(std::move(names));
}

void MenuUi::set_pads(std::vector<std::string> names) {
    pads_ = std::move(names);
    dirty_ = true;
}

void MenuUi::key(Rml::Input::KeyIdentifier k) {
    ctx_->ProcessKeyDown(k, 0);
    ctx_->ProcessKeyUp(k, 0);
    redraw_ = true;
}

void MenuUi::pad_dir(Rml::Input::KeyIdentifier k) {
    held_ = k;
    repeat_at_ = SDL_GetTicks() + 380;
    key(k);
}

bool MenuUi::handle(const SDL_Event& ev, const MouseMap& map) {
    if (!ctx_)
        return false;
    if (capture_.active && capture_event(ev))
        return true;
    switch (ev.type) {
    case SDL_EVENT_KEY_DOWN: {
        const SDL_Keycode k = ev.key.key;
        if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE) {
            if (!ev.key.repeat)
                back();
            return true;
        }
        if (k == SDLK_Q || k == SDLK_E) {
            set_tab(tab_ + (k == SDLK_Q ? -1 : 1));
            return true;
        }
        if (k == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT))
            return false;  // Alt+Enter: the window's fullscreen toggle
        ctx_->ProcessKeyDown(RmlSDL::ConvertKey(static_cast<int>(k)), RmlSDL::GetKeyModifierState());
        redraw_ = true;
        return true;
    }
    case SDL_EVENT_KEY_UP:
        ctx_->ProcessKeyUp(RmlSDL::ConvertKey(static_cast<int>(ev.key.key)),
                           RmlSDL::GetKeyModifierState());
        return true;
    case SDL_EVENT_TEXT_INPUT:
        return true;
    case SDL_EVENT_MOUSE_MOTION: {
        float cx = 0, cy = 0;
        if (map(ev.motion.x, ev.motion.y, cx, cy)) {
            ctx_->ProcessMouseMove(static_cast<int>(cx), static_cast<int>(cy),
                                   RmlSDL::GetKeyModifierState());
            redraw_ = true;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        ctx_->ProcessMouseButtonDown(RmlSDL::ConvertMouseButton(ev.button.button),
                                     RmlSDL::GetKeyModifierState());
        redraw_ = true;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        ctx_->ProcessMouseButtonUp(RmlSDL::ConvertMouseButton(ev.button.button),
                                   RmlSDL::GetKeyModifierState());
        redraw_ = true;
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        ctx_->ProcessMouseWheel(-ev.wheel.y, RmlSDL::GetKeyModifierState());
        redraw_ = true;
        return true;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        refresh_pads();
        return false;  // the window keeps its own device list too
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        switch (ev.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: pad_dir(Rml::Input::KI_UP); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: pad_dir(Rml::Input::KI_DOWN); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: pad_dir(Rml::Input::KI_LEFT); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: pad_dir(Rml::Input::KI_RIGHT); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: key(Rml::Input::KI_RETURN); break;
        case SDL_GAMEPAD_BUTTON_EAST: back(); break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: set_tab(tab_ - 1); break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: set_tab(tab_ + 1); break;
        case SDL_GAMEPAD_BUTTON_START:
            // Before the game Start starts it; in game Start belongs to the game.
            if (mode_ == Mode::Pregame && !panel_ && playable() && host_.start)
                host_.start();
            break;
        case SDL_GAMEPAD_BUTTON_BACK:
            if (mode_ == Mode::InGame)
                close_panel();
            break;
        default: break;
        }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        held_ = Rml::Input::KI_UNKNOWN;
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (ev.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTX && ev.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY)
            return true;
        const float v = static_cast<float>(ev.gaxis.value) / 32767.0f;
        const bool x = ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX;
        int& state = x ? stick_x_ : stick_y_;
        const int now = v > 0.6f ? 1 : v < -0.6f ? -1 : (std::fabs(v) < 0.3f ? 0 : state);
        if (now != state) {
            state = now;
            if (now == 0)
                held_ = Rml::Input::KI_UNKNOWN;
            else if (x)
                pad_dir(now > 0 ? Rml::Input::KI_RIGHT : Rml::Input::KI_LEFT);
            else
                pad_dir(now > 0 ? Rml::Input::KI_DOWN : Rml::Input::KI_UP);
        }
        return true;
    }
    default:
        return false;
    }
}

void MenuUi::tick() {
    if (verifying_ && verify_.valid() &&
        verify_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        disc_ = verify_.get();
        verifying_ = false;
        dirty_ = true;
    }
    if (held_ != Rml::Input::KI_UNKNOWN && SDL_GetTicks() >= repeat_at_) {
        repeat_at_ = SDL_GetTicks() + 95;
        key(held_);
    }
    if (!toast_.empty() && SDL_GetTicks() >= toast_until_) {
        toast_.clear();
        dirty_ = true;
    }
    if (dirty_)
        rebuild();
}

void MenuUi::set_visible(bool on) {
    if (!doc_)
        return;
    if (on)
        doc_->Show();
    else
        doc_->Hide();
    redraw_ = true;
}

bool MenuUi::script_event(const std::string& k, SDL_Event& e) {
    e = SDL_Event{};
    if (k.rfind("pad:", 0) == 0) {
        const std::string b = k.substr(4);
        const SDL_GamepadButton btn = b == "up"      ? SDL_GAMEPAD_BUTTON_DPAD_UP
                                      : b == "down"  ? SDL_GAMEPAD_BUTTON_DPAD_DOWN
                                      : b == "left"  ? SDL_GAMEPAD_BUTTON_DPAD_LEFT
                                      : b == "right" ? SDL_GAMEPAD_BUTTON_DPAD_RIGHT
                                      : b == "a"     ? SDL_GAMEPAD_BUTTON_SOUTH
                                      : b == "b"     ? SDL_GAMEPAD_BUTTON_EAST
                                      : b == "x"     ? SDL_GAMEPAD_BUTTON_WEST
                                      : b == "y"     ? SDL_GAMEPAD_BUTTON_NORTH
                                      : b == "lb"    ? SDL_GAMEPAD_BUTTON_LEFT_SHOULDER
                                      : b == "rb"    ? SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER
                                      : b == "start" ? SDL_GAMEPAD_BUTTON_START
                                      : b == "back" || b == "select" ? SDL_GAMEPAD_BUTTON_BACK
                                                                     : SDL_GAMEPAD_BUTTON_INVALID;
        if (btn == SDL_GAMEPAD_BUTTON_INVALID)
            return false;
        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        e.gbutton.button = static_cast<Uint8>(btn);
        e.gbutton.down = true;
        return true;
    }
    const SDL_Keycode code = k == "up"      ? SDLK_UP
                             : k == "down"  ? SDLK_DOWN
                             : k == "left"  ? SDLK_LEFT
                             : k == "right" ? SDLK_RIGHT
                             : k == "enter" ? SDLK_RETURN
                             : k == "esc"   ? SDLK_ESCAPE
                             : k == "f1"    ? SDLK_F1
                             : k == "f2"    ? SDLK_F2
                             : k == "s"     ? SDLK_S
                             : k == "p"     ? SDLK_P
                             : k == "tab"   ? SDLK_TAB
                             : k == "q"     ? SDLK_Q
                             : k == "e"     ? SDLK_E
                             : k == "space" ? SDLK_SPACE
                             : k == "k"     ? SDLK_K
                                            : SDLK_UNKNOWN;
    if (code == SDLK_UNKNOWN)
        return false;
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = code;
    e.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
    e.key.down = true;
    return true;
}

}  // namespace dreamcomp::frontend
