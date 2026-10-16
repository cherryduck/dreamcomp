// The launcher: the window a player sees when they double-click a port's executable (or pass
// --launcher). Pick and verify the disc, choose graphics/controls/enhancement settings, then
// Start Game. Built only when SDL3 is available (cmake/DreamcompFrontend.cmake), in which case
// DREAMCOMP_WITH_FRONTEND is defined for dreamcomp_core. Usage and customisation: docs/FRONTEND.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "dreamcomp/hud.h"

namespace dreamcomp {

class Settings;
struct PortInfo;

// The port's own version, shown in the launcher. dreamcomp_add_port() generates a source file that
// calls this from a static initialiser with the port project's VERSION. Defined in core.cpp.
void set_port_version(const char* version);

// The port's icon as PNG bytes (static storage), used as the window icon of the launcher and the
// game window. dreamcomp_add_port(... ICON <png>) generates a source file that calls this from a
// static initialiser; without it the windows keep the platform default. Defined in core.cpp.
void set_port_icon(const unsigned char* png, std::size_t size);
// The bytes set_port_icon() received; false (and nothing written) when the port has no icon.
bool port_icon(const unsigned char** png, std::size_t* size);

namespace frontend {

// Sets port_icon() as `sdl_window`'s icon (an SDL_Window*); no icon or an undecodable PNG leaves
// the window as it is. Called on the launcher window and, from the overlay's first event, on the
// game window (the engine creates that one). On Windows the exe's own icon resource (ICON_ICO)
// already shows in the title bar and taskbar; this covers Linux/macOS. Defined in window_icon.cpp.
void apply_port_icon(void* sdl_window);

struct LaunchContext {
    Settings* settings = nullptr;          // edited on Apply / Start Game, saved to its file
    const PortInfo* port = nullptr;        // title, widescreen capability
    std::filesystem::path config;          // the game's TOML ([game] title/product, [disc] sha1)
    std::filesystem::path exe_dir;         // frontend/ assets live here
    std::filesystem::path screenshot;      // non-empty: render, write PNG(s), no interaction
    std::string tab;                       // screen: launcher | settings/<general|controls|graphics|sound|mods>
    std::string version;                   // shown bottom-left: "dreamcomp 0.1.0 · Soulcalibur 0.1.0"
    int width = 1280, height = 720;        // window (and screenshot) size
};

enum class Outcome {
    Start,  // settings applied and saved: continue into the game
    Quit,   // the player closed the launcher
    Error,  // could not open (no assets, no display): `error` says why; start the game as before
};

// The same menu over the running game (Escape / pad Select toggles it). Created by the core
// extension when the engine opens its window. Its public calls come from the game thread; the
// menu itself (RmlUi, rendering) runs on its own worker thread, and what it asks of the game
// (OverlayContext::applied, quitting, the binding screen) runs back on the game thread.
struct OverlayContext {
    Settings* settings = nullptr;
    const PortInfo* port = nullptr;
    std::filesystem::path config, exe_dir;
    std::string version;
    // What the platform offers (dreamcomp). A console title (the PS5's) has no window to switch,
    // no program to open a folder or a page with, and nothing to quit to from inside the game:
    // the rows and buttons for them are left out.
    bool can_window = true;        // Graphics: Window mode
    bool can_open_external = true; // Open texture / mods folder, Read me
    bool can_quit = true;          // Quit Game
    bool keyboard = true;          // false: no keyboard; hints and Controls are the pad's only
    bool playstation = false;      // PlayStation button glyphs whatever pad SDL reports
    std::function<void()> applied;  // after Apply: re-read the settings that apply live
    // The F1 HUD editor (docs/HUD.md). hud_watch starts/stops the per-frame snapshots,
    // hud_snapshot copies the newest (false: none yet); hud_edit and hud_save must be called on
    // the game thread (the overlay posts them).
    std::function<void(bool)> hud_watch;
    std::function<bool(hud::Snapshot&)> hud_snapshot;
    std::function<void(const hud::Box&, hud::Edit)> hud_edit;
    std::function<void()> hud_save;
    // Holds the game still (true) or lets it run (false), so the HUD on screen can be edited; the
    // still frame is redrawn with each change. Any thread.
    std::function<void(bool)> hud_pause;
    // The F2 list's dropdown (game thread, posted): "auto" (back to automatic, or add a grey piece
    // with an automatic anchor), "left", "center", "right", "stretch", "none" (a grey piece stays
    // out of the HUD).
    std::function<void(const hud::Box&, const std::string&)> hud_set;
};

class Overlay {
public:
    explicit Overlay(const OverlayContext& ctx);
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool on_event(const void* sdl_event);  // true: the overlay used it (host_ext.h)
    bool is_open() const;
    void draw(std::uint32_t* rgba, unsigned w, unsigned h, unsigned view_w, unsigned view_h);
    // The menu rendered at the window's own size (premultiplied RGBA), for the engine to blend
    // over the game on the GPU. `changed`: it was redrawn since the last call. Null while closed
    // (or while a test screenshot of the composited frame is pending: draw() takes that one).
    const std::uint32_t* render_window(unsigned view_w, unsigned view_h, bool& changed);
    // Each vblank: runs what the menu asked of the game, and times the test script
    // (DREAMCOMP_OVERLAY_KEYS).
    void on_vblank(std::uint64_t frame);
    // While the game is paused: runs what the menu asked of the game (edits, saving).
    void on_paused_tick();

    struct Impl;

private:
    Impl* impl_;
};

// Runs the launcher to completion. The SDL window, renderer and every SDL subsystem the launcher
// initialised are gone when it returns, so the engine can open its own Vulkan window.
Outcome run_launcher(const LaunchContext& ctx, std::string* error);

}  // namespace frontend
}  // namespace dreamcomp
