// The frontend itself (docs/FRONTEND.md), laid out like Daniel's N64 recomp ports:
//
//   launcher   (Pregame only) backdrop, title, a centred list: Load Game / Start Game, Controls,
//              Settings, Mods, Quit; version bottom-left
//   panel      the settings panel: a large centred modal with tabs along the top (General,
//              Controls, Graphics, Sound, Mods), an X close button, option rows on the left and a
//              description pane on the right, prompts and Apply at the bottom
//
// Pregame shows both in the launcher's own window (launcher.cpp); InGame shows only the panel,
// composited over the running game (overlay.cpp). Both feed SDL events to handle() and draw the
// same frontend/launcher.rml + dreamcomp.rcss. Values take effect on Apply; closing the panel
// keeps unapplied edits, still marked, for next time.
#pragma once

#include <RmlUi/Core.h>

#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <string>
#include <vector>

#include "dream/render/input.h"
#include "dream/translator/config/game_config.h"
#include "launcher_model.h"

union SDL_Event;

namespace dreamcomp {
class Settings;
struct PortInfo;
}  // namespace dreamcomp

namespace dreamcomp::frontend {

enum class Mode { Pregame, InGame };

struct UiHost {
    std::function<void()> pick_disc;      // Pregame: open the file dialog (result: disc_picked)
    std::function<void()> start;          // Pregame: Start Game
    std::function<void()> close_panel;    // InGame: the panel was closed (X, Esc, B, Select)
    std::function<void()> exit_game;      // quit the program
    std::function<void()> open_bindings;  // InGame: the engine's controller screen
    std::function<void()> applied;        // after Apply: settings that apply live
    // The controller bindings file (empty: the engine's default) and, after the Controls tab
    // rewrote it, a call to make the running game use it.
    std::function<std::string()> bindings_path;
    std::function<void()> bindings_changed;
};

struct UiConfig {
    Settings* settings = nullptr;
    const PortInfo* port = nullptr;
    std::filesystem::path config;   // the game's TOML
    std::filesystem::path exe_dir;  // frontend/ lives here
    std::string version;            // "dreamcomp 0.1.0 · Soulcalibur 0.1.0"
    // What the platform offers (dreamcomp). A console title (the PS5's) has no window to switch,
    // no program to open a folder or a page with, and nothing to quit to from inside the game:
    // the rows and buttons for them are left out.
    bool can_window = true;        // Graphics: Window mode
    bool can_open_external = true; // Open texture / mods folder, Read me
    bool can_quit = true;          // Quit Game
    bool keyboard = true;          // false: no keyboard; hints and Controls are the pad's only
    bool playstation = false;      // PlayStation button glyphs whatever pad SDL reports
};

class MenuUi final : public Rml::EventListener {
public:
    MenuUi(const UiConfig& cfg, Mode mode, UiHost host);
    ~MenuUi() override;

    bool load(Rml::Context* ctx, std::string* error);

    // Screens: "launcher", "settings/<tab>" (general, controls, graphics, sound, mods).
    void show(const std::string& screen);
    static std::vector<std::string> all_screens();
    std::string screen_name() const;
    void open_panel(const std::string& tab = {});  // empty: the last tab used
    // The page itself, hidden while the HUD editor (F1) shares the overlay.
    void set_visible(bool on);
    bool panel_open() const noexcept { return panel_; }

    using MouseMap = std::function<bool(float wx, float wy, float& cx, float& cy)>;
    bool handle(const SDL_Event& ev, const MouseMap& map);
    void tick();
    bool dirty() const noexcept { return dirty_; }
    bool take_redraw() {
        const bool r = redraw_;
        redraw_ = false;
        return r;
    }

    void disc_picked(const std::string& path);
    void verify_now();
    bool playable() const;
    void set_pads(std::vector<std::string> names);
    void refresh_pads();
    Rml::Element* focused() const;
    std::string focus_id() const;
    Rml::Element* element(const std::string& id) const;
    void draft_set(const std::string& key, const std::string& value) { draft_.set(key, value); }
    static bool script_event(const std::string& name, SDL_Event& out);

    void ProcessEvent(Rml::Event& ev) override;

private:
    struct Desc {
        std::string title, text;
        bool live = true;
    };
    std::string title() const;
    void rebuild();
    std::string page_launcher() const;
    std::string page_panel();
    std::string page_modal() const;
    std::string tab_general();
    std::string tab_controls();
    std::string tab_graphics();
    std::string tab_sound();
    std::string tab_mods();
    std::string row_cycle(const std::string& key, const std::string& name, const std::string& value,
                          const std::string& help, bool live);
    // RecompFrontend's option row: the name, then every choice in a row; the selected one white
    // and underlined, the others dim, unavailable ones greyed out. Click (or Enter / A) selects.
    std::string row_radio(const std::string& key, const std::string& name,
                          const std::vector<Choice>& choices, const std::string& current,
                          const std::string& help, bool live,
                          const std::vector<bool>& enabled = {});
    std::string row_onoff(const std::string& key, const std::string& name, bool on,
                          const std::string& help, bool live);
    void select(const std::string& key, const std::string& value);
    std::string row_toggle(const std::string& key, const std::string& name, bool on,
                           const std::string& help, bool live);
    std::string row_slider(const std::string& key, const std::string& name, int percent,
                           const std::string& help, bool live);
    std::string row_button(const std::string& id, const std::string& act, const std::string& name,
                           const std::string& button, const std::string& help,
                           bool enabled = true);
    std::string name_html(const std::string& key, const std::string& name, bool live) const;
    void describe(Rml::Element* e);
    float draft_aspect() const;
    bool draft_expanded() const;
    bool tab_has_settings(int tab) const;

    void run_act(const std::string& act, Rml::Event* ev);
    void adjust(const std::string& key, int step);
    void toggle(const std::string& key, int dir);
    void mod_action(const std::string& kind, const std::string& name);
    void apply();
    void close_panel();
    void back();
    void set_tab(int t);
    void navigate(Rml::Element* focus, Rml::Input::KeyIdentifier key);
    void key(Rml::Input::KeyIdentifier k);
    void pad_dir(Rml::Input::KeyIdentifier k);
    void start_verify(bool sync);
    void open_folder(const std::filesystem::path& dir);

    // Controls tab (N64 recomp layout): player cards, then one profile's mappings with two slots
    // per input. Bindings are edited in place and saved at once, as RecompFrontend does.
    std::string controls_cards();
    std::string controls_mappings();
    std::string controls_footer();
    void load_bindings();
    void save_bindings();
    dream::render::DeviceBindings& editing();
    bool capture_event(const SDL_Event& ev);
    void finish_capture(const dream::render::Binding* b);

    UiConfig cfg_;
    Mode mode_;
    UiHost host_;
    Draft draft_;
    std::filesystem::path config_dir_;
    dream::translator::GameConfig game_;
    bool game_ok_ = false;
    std::string game_error_;

    Rml::Context* ctx_ = nullptr;
    Rml::ElementDocument* doc_ = nullptr;
    bool panel_ = false;
    int tab_ = 0;
    std::string panel_from_;  // launcher item to return focus to
    std::map<std::string, Desc> descs_;
    std::string desc_shown_;

    struct Modal {
        std::string title, text, extra;
        bool confirm_quit = false;  // "Quit <game>?" with Quit / Cancel
    };
    bool modal_open_ = false;
    Modal modal_;

    DiscCheck disc_;
    bool verifying_ = false;
    std::future<DiscCheck> verify_;
    bool dialog_open_ = false;

    std::vector<std::string> pads_;
    dream::render::Bindings bindings_;
    bool bindings_loaded_ = false;
    int ctl_view_ = 0;    // 0 player cards, 1 mappings
    int ctl_device_ = 1;  // 0 keyboard, 1 controller
    bool ps_pad_ = false;  // the first pad is a PlayStation one (dreamcomp)
    // Pad hints: with a pad connected, or always where there is no keyboard (a console).
    bool has_pad() const { return !pads_.empty() || !cfg_.keyboard; }
    bool ps_glyphs() const { return cfg_.playstation || ps_pad_; }
    const char* glyph_a() const;
    const char* glyph_b() const;
    const char* glyph_lb() const;
    const char* glyph_rb() const;
    struct Capture {
        bool active = false;
        unsigned control = 0, slot = 0;
    } capture_;
    Rml::Input::KeyIdentifier held_ = Rml::Input::KI_UNKNOWN;
    std::uint64_t repeat_at_ = 0;
    int stick_x_ = 0, stick_y_ = 0;

    std::string toast_;
    std::uint64_t toast_until_ = 0;
    std::string last_focus_, want_focus_;
    bool dirty_ = true, redraw_ = true, focus_first_ = true;
};

}  // namespace dreamcomp::frontend
