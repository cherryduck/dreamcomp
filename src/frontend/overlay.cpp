// The frontend menu over the running game (docs/FRONTEND.md): MenuUi in InGame mode, drawn by
// the CPU renderer (soft_render.h) into an RGBA image that the engine blends over the game on the
// GPU (Extension::overlay_image), or composites into the frame for a test screenshot. Escape or a
// pad's Select/Back opens and closes it; the game keeps running. F1 opens the HUD editor
// (hud_editor.h) in the same overlay instead of the engine's binding screen.
//
// Threads: everything RmlUi does -- input, layout, rasterising -- runs on the overlay's own worker
// thread, so a redraw (10-15 ms at window size) never holds up a game frame. The game thread
// only queues events, picks up the latest finished image, and runs what the menu asks of the
// game (Apply, Quit, the binding screen) at its next vblank or present, where the game expects
// such calls. The two share a small mutex for that hand-over and nothing else.
#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dream/render/png.h"
#include "dream/render/vk/present.h"
#include "dream/runtime/host_ext.h"
#include "dreamcomp/frontend.h"
#include "dreamcomp/settings.h"
#include "frontend_common.h"
#include "hud_editor.h"
#include "menu_ui.h"
#include "soft_render.h"

namespace dreamcomp::frontend {

namespace {

class OverlaySystem final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override { return static_cast<double>(SDL_GetTicksNS()) * 1e-9; }
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type <= Rml::Log::LT_WARNING)
            std::fprintf(stderr, "overlay: %s\n", message.c_str());
        return true;
    }
};

std::vector<std::string> split(const char* s, char sep) {
    std::vector<std::string> out;
    if (!s)
        return out;
    std::string all = s;
    std::size_t at = 0;
    while (at < all.size()) {
        std::size_t e = all.find(sep, at);
        if (e == std::string::npos)
            e = all.size();
        if (e > at)
            out.push_back(all.substr(at, e - at));
        at = e + 1;
    }
    return out;
}

// What the game thread wants drawn: the window's size (blended on the GPU), or a frame of w x h
// shown in a vw x vh window (composited on the CPU, for a test screenshot).
struct DrawMode {
    unsigned w = 0, h = 0, vw = 0, vh = 0;
    bool frame = false;
    bool operator==(const DrawMode& o) const noexcept {
        return w == o.w && h == o.h && vw == o.vw && vh == o.vh && frame == o.frame;
    }
    bool operator!=(const DrawMode& o) const noexcept { return !(*this == o); }
};

// The finished menu image, handed from the worker to the game thread.
struct Image {
    std::vector<std::uint32_t> px;
    DrawMode mode;
    unsigned w = 0, h = 0, x = 0, y = 0;  // where it sits in the frame (frame mode)
    std::uint64_t seq = 0;
};

bool consumed_while_open(const SDL_Event& ev) {
    switch (ev.type) {
    case SDL_EVENT_KEY_DOWN:
        // Alt+Enter is the window's fullscreen toggle. (F1 is the overlay's: the HUD editor.)
        if (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT))
            return false;
        return true;
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return true;
    default:
        return false;
    }
}

}  // namespace

struct Overlay::Impl {
    OverlayContext ctx;

    // ---- shared between the game thread and the worker, under `q` ----------------------------
    std::mutex q;
    std::condition_variable cv;
    std::deque<SDL_Event> events;
    std::vector<std::function<void()>> guest_actions;  // run on the game thread
    DrawMode want;
    Image front;
    bool want_open = false, want_hud = false, stop = false;
    std::uint64_t vblank_frame = 0;
    bool shot_pending = false;
    std::string pending_shot;
    std::atomic<bool> open{false};
    bool icon_set = false;  // the port's icon put on the game window (first event naming it)
    std::atomic<bool> hud_mode{false};  // the HUD editor (F1) rather than the menu
    std::atomic<bool> hud_list{false};  // opened with F2: the list of elements with dropdowns

    // ---- game thread only ------------------------------------------------------------------------
    Image shown;

    // ---- worker only -----------------------------------------------------------------------------
    std::thread worker;
    bool failed = false, ready = false;
    std::unique_ptr<Utf8FileInterface> files;
    std::unique_ptr<OverlaySystem> system;
    std::unique_ptr<SoftRenderer> soft;
    std::unique_ptr<MenuUi> ui;
    std::unique_ptr<HudEditor> hud;
    Rml::Context* context = nullptr;
    unsigned fw = 0, fh = 0, vis_x = 0, vis_y = 0, vis_w = 0, vis_h = 0;
    float draw_w = 0, draw_h = 0, draw_x = 0, draw_y = 0;  // the frame's rectangle, window pixels
    DrawMode laid_out;
    bool need_render = true;
    bool profile = std::getenv("DREAMCOMP_OVERLAY_PROFILE") != nullptr;
    std::uint64_t prof_mark = 0, prof_renders = 0, prof_render_ns = 0;
    // Test script (DREAMCOMP_OVERLAY_KEYS="@FRAME,esc,shot:F.png,down,...").
    std::vector<std::string> script;
    std::uint64_t script_frame = 0, script_at = 0, script_done_at = 0;
    bool script_started = false;
    bool has_script = false;  // fixed at construction: safe to read from either thread

    explicit Impl(const OverlayContext& c) : ctx(c) {
        script = split(std::getenv("DREAMCOMP_OVERLAY_KEYS"), ',');
        if (!script.empty() && script.front().size() > 1 && script.front()[0] == '@') {
            script_frame = std::strtoull(script.front().c_str() + 1, nullptr, 10);
            script.erase(script.begin());
        }
        has_script = !script.empty();
        worker = std::thread([this] { run(); });
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(q);
            stop = true;
        }
        cv.notify_all();
        if (worker.joinable())
            worker.join();
        // Every document owner goes before Rml::Shutdown, which frees the elements: the HUD
        // editor detaches its listeners from its document when destroyed (a crash on Quit Game
        // when it outlived the shutdown, 2026-10-06).
        hud.reset();
        ui.reset();
        if (ready)
            Rml::Shutdown();
    }

    // ---- hand-over -------------------------------------------------------------------------------
    void post(std::function<void()> f) {
        std::lock_guard<std::mutex> lock(q);
        guest_actions.push_back(std::move(f));
    }

    void run_guest_actions() {
        std::vector<std::function<void()>> todo;
        {
            std::lock_guard<std::mutex> lock(q);
            todo.swap(guest_actions);
        }
        for (auto& f : todo) f();
    }

    // ---- worker ----------------------------------------------------------------------------------
    void run() {
        std::unique_lock<std::mutex> lock(q);
        while (!stop) {
            // Awake often while there is something to animate or a script to step; otherwise
            // only when the game thread has news.
            const bool busy = open.load() || (!script.empty() && !script_started) || script_started;
            if (busy)
                cv.wait_for(lock, std::chrono::milliseconds(15));
            else
                cv.wait(lock);
            if (stop)
                break;
            std::deque<SDL_Event> evs;
            evs.swap(events);
            const bool do_open = want_open, do_hud = want_hud;
            want_open = want_hud = false;
            const DrawMode mode = want;
            const std::uint64_t frame = vblank_frame;
            lock.unlock();
            if (do_hud)
                open_hud(hud_list.load());
            work(evs, do_open, mode, frame);
            lock.lock();
        }
    }

    void work(std::deque<SDL_Event>& evs, bool do_open, const DrawMode& mode, std::uint64_t frame) {
        if (do_open)
            open_menu();
        if (!script_started && !script.empty() && frame >= script_frame && frame > 0) {
            script_started = true;
            script_at = SDL_GetTicks();
            std::printf("overlay: script starts at frame %llu\n",
                        static_cast<unsigned long long>(frame));
        }
        step_script();
        if (!open.load() || !ready || failed)
            return;
        for (const SDL_Event& ev : evs) {
            const unsigned wid = ev.type == SDL_EVENT_MOUSE_MOTION ? ev.motion.windowID : 0;
            auto mouse = [this, wid](float x, float y, float& cx, float& cy) {
                return map(x, y, wid, cx, cy);
            };
            if (hud_mode.load()) {
                hud->handle(ev, mouse);
            } else if (ev.type == SDL_EVENT_KEY_DOWN && (ev.key.key == SDLK_F1 || ev.key.key == SDLK_F2) &&
                       !ev.key.repeat) {
                open_hud(ev.key.key == SDLK_F2);  // from the menu straight to the HUD editor
            } else if (ui->handle(ev, mouse)) {
                need_render = true;
            }
            if (!open.load())
                return;  // closed by this event
        }
        if (mode.w == 0 || mode.h == 0)
            return;
        if (mode != laid_out) {
            if (mode.frame)
                layout(mode.w, mode.h, mode.vw, mode.vh);
            else
                layout_window(mode.w, mode.h);
            laid_out = mode;
            need_render = true;
        }
        bool redraw = need_render;
        if (hud_mode.load()) {
            hud->tick();
            redraw |= hud->take_redraw();
        } else {
            ui->tick();
            redraw |= ui->take_redraw();
        }
        if (!redraw)
            return;
        const std::uint64_t t0 = SDL_GetTicksNS();
        context->Update();
        soft->begin(vis_w, vis_h);
        context->Render();
        need_render = false;
        {
            std::lock_guard<std::mutex> lock(q);
            front.px = soft->pixels();
            front.mode = mode;
            front.w = soft->width();
            front.h = soft->height();
            front.x = vis_x;
            front.y = vis_y;
            ++front.seq;
        }
        ++prof_renders;
        prof_render_ns += SDL_GetTicksNS() - t0;
        const std::uint64_t now = SDL_GetTicks();
        if (profile && now - prof_mark > 2000) {
            std::printf("overlay: %llu renders, %.2f ms each (on the overlay thread)\n",
                        static_cast<unsigned long long>(prof_renders),
                        prof_render_ns / 1e6 / std::max<std::uint64_t>(1, prof_renders));
            prof_mark = now;
            prof_renders = prof_render_ns = 0;
        }
    }

    bool init() {
        if (ready)
            return !failed;
        if (failed)
            return false;
        files = std::make_unique<Utf8FileInterface>();
        system = std::make_unique<OverlaySystem>();
        soft = std::make_unique<SoftRenderer>();
        Rml::SetFileInterface(files.get());
        Rml::SetSystemInterface(system.get());
        Rml::SetRenderInterface(soft.get());
        Rml::Initialise();
        ready = true;
        std::string err;
        if (!load_fonts(ctx.exe_dir, &err)) {
            std::fprintf(stderr, "overlay: %s\n", err.c_str());
            failed = true;
            return false;
        }
        context = Rml::CreateContext("overlay", Rml::Vector2i(1280, 720));
        UiConfig cfg{ctx.settings, ctx.port, ctx.config, ctx.exe_dir, ctx.version};
        cfg.can_window = ctx.can_window;
        cfg.can_open_external = ctx.can_open_external;
        cfg.can_quit = ctx.can_quit;
        cfg.max_scale = ctx.max_scale;
        cfg.default_scale = ctx.default_scale;
        cfg.keyboard = ctx.keyboard;
        cfg.playstation = ctx.playstation;
        UiHost host;
        host.close_panel = [this] { close(); };
        host.exit_game = [this] {
            std::printf("overlay: exit game\n");
            post([] {
                if (dream::host::host_controls().quit)
                    dream::host::host_controls().quit();
            });
        };
        host.open_bindings = [this] {
            std::printf("overlay: opening the binding screen\n");
            close();
            post([] {
                if (dream::host::host_controls().open_bindings)
                    dream::host::host_controls().open_bindings();
            });
        };
        host.applied = [this] {
            post([this] {
                if (ctx.applied)
                    ctx.applied();
            });
        };
        host.bindings_path = [] {
            const auto& hc = dream::host::host_controls();
            return hc.bindings_path ? hc.bindings_path() : std::string();
        };
        host.bindings_changed = [this] {
            post([] {
                if (dream::host::host_controls().reload_bindings)
                    dream::host::host_controls().reload_bindings();
            });
        };
        ui = std::make_unique<MenuUi>(cfg, Mode::InGame, host);
        if (!context || !ui->load(context, &err)) {
            std::fprintf(stderr, "overlay: %s\n", err.c_str());
            failed = true;
            return false;
        }
        hud = std::make_unique<HudEditor>(
            ctx, [this](std::function<void()> f) { post(std::move(f)); }, [this] { close(); });
        if (!hud->load(context, &err)) {
            std::fprintf(stderr, "overlay: %s\n", err.c_str());
            hud.reset();
        }
        return true;
    }

    // The HUD editor (F1 outlines, F2 the list): the menu's page hidden, the editor's shown, over
    // the running game.
    void open_hud(bool list = false) {
        if (!init() || !hud) {
            open = false;
            hud_mode = false;
            return;
        }
        open = true;
        hud_mode = true;
        ui->set_visible(false);
        hud->show(list);
        need_render = true;
        laid_out = DrawMode{};
        std::printf("overlay: HUD editor open\n");
    }

    void open_menu() {
        if (!init()) {
            // No menu to show: Escape still has to end the game somehow.
            open = false;
            if (!ctx.can_quit)
                return;  // a console: the game goes on (the reason is in the log)
            post([] {
                if (dream::host::host_controls().quit)
                    dream::host::host_controls().quit();
            });
            return;
        }
        need_render = true;
        laid_out = DrawMode{};
        ui->refresh_pads();
        ui->open_panel();  // the last tab used
        std::printf("overlay: open\n");
    }

    void close() {
        if (hud_mode.exchange(false) && hud) {
            hud->hide();
            if (ui)
                ui->set_visible(true);
        }
        if (!open.exchange(false))
            return;
        std::printf("overlay: closed\n");
    }

    bool map(float wx, float wy, unsigned window_id, float& cx, float& cy) const {
        float d = 1.0f;
        if (SDL_Window* w = SDL_GetWindowFromID(window_id))
            d = SDL_GetWindowPixelDensity(w);
        const float px = wx * d, py = wy * d;
        if (draw_w <= 0 || draw_h <= 0 || fw == 0)
            return false;
        const float fx = (px - draw_x) / draw_w * static_cast<float>(fw);
        const float fy = (py - draw_y) / draw_h * static_cast<float>(fh);
        cx = fx - static_cast<float>(vis_x);
        cy = fy - static_cast<float>(vis_y);
        return true;
    }

    // The panel covers the window 1:1 (the GPU path), so the mouse maps straight through.
    void layout_window(unsigned vw, unsigned vh) {
        fw = vis_w = vw;
        fh = vis_h = vh;
        vis_x = vis_y = 0;
        draw_x = draw_y = 0.0f;
        draw_w = static_cast<float>(vw);
        draw_h = static_cast<float>(vh);
        context->SetDimensions(Rml::Vector2i(static_cast<int>(vw), static_cast<int>(vh)));
        context->SetDensityIndependentPixelRatio(static_cast<float>(vh) / 720.0f);
        if (hud)
            hud->set_window(static_cast<float>(vw), static_cast<float>(vh));
    }

    // The part of the frame the window shows, as the presenter fits it (present.cpp).
    void layout(unsigned w, unsigned h, unsigned vw, unsigned vh) {
        const auto& o = dream::render::vk::present_options();
        const float ia = o.display_aspect > 0.0f ? o.display_aspect
                                                 : static_cast<float>(w) / static_cast<float>(h);
        const float ta = static_cast<float>(vw) / static_cast<float>(std::max(1u, vh));
        float s0 = 1.0f, s1 = 1.0f;
        using Fit = dream::render::vk::PresentOptions::Fit;
        if (o.fit == Fit::Letterbox) {
            if (ta > ia)
                s0 = ia / ta;
            else
                s1 = ta / ia;
        } else if (o.fit == Fit::Crop) {
            if (ta > ia)
                s1 = ta / ia;
            else
                s0 = ia / ta;
        }
        draw_w = static_cast<float>(vw) * s0;
        draw_h = static_cast<float>(vh) * s1;
        draw_x = (static_cast<float>(vw) - draw_w) * 0.5f;
        draw_y = (static_cast<float>(vh) - draw_h) * 0.5f;
        const unsigned nw = std::min(w, static_cast<unsigned>(static_cast<float>(w) / s0 + 0.5f));
        const unsigned nh = std::min(h, static_cast<unsigned>(static_cast<float>(h) / s1 + 0.5f));
        fw = w;
        fh = h;
        vis_w = std::max(1u, nw);
        vis_h = std::max(1u, nh);
        vis_x = (w - vis_w) / 2;
        vis_y = (h - vis_h) / 2;
        context->SetDimensions(Rml::Vector2i(static_cast<int>(vis_w), static_cast<int>(vis_h)));
        context->SetDensityIndependentPixelRatio(static_cast<float>(vis_h) / 720.0f);
        if (hud)  // the context is the visible part of the frame, which is the picture
            hud->set_frame(-static_cast<float>(vis_x), -static_cast<float>(vis_y),
                           static_cast<float>(fw), static_cast<float>(fh));
    }

    // One scripted input every 150 ms: keys and pad buttons go into SDL's queue, so they take the
    // same path as real ones (event pump -> engine poll -> event filter -> on_event).
    void step_script() {
        if (!script_started)
            return;
        const std::uint64_t now = SDL_GetTicks();
        if (script.empty()) {
            if (open.load() && script_done_at && now > script_done_at + 5000) {
                std::printf("overlay: script finished with the menu open; resuming\n");
                close();
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(q);
            if (now < script_at || shot_pending)
                return;
        }
        const std::string k = script.front();
        script.erase(script.begin());
        script_at = now + 150;
        if (script.empty())
            script_done_at = now;
        const bool showing = ui && open.load();
        std::printf("overlay: script %s (screen %s, focus %s)\n", k.c_str(),
                    showing ? ui->screen_name().c_str() : "-",
                    showing ? ui->focus_id().c_str() : "-");
        if (k.rfind("shot:", 0) == 0) {
            std::lock_guard<std::mutex> lock(q);
            pending_shot = k.substr(5);
            shot_pending = true;
            need_render = true;
            return;
        }
        // select:<id>:<value> -- a dropdown set as if chosen (the HUD editor's F2 list).
        if (k.rfind("select:", 0) == 0) {
            const std::string rest = k.substr(7);
            const auto c = rest.find(':');
            if (c != std::string::npos && showing && hud_mode.load() && hud)
                hud->choose(rest.substr(0, c), rest.substr(c + 1));
            return;
        }
        // click:<id>, or drag:<id>:<dx>:<dy> -- press near the element's top-left corner and
        // move by dx, dy context pixels before releasing.
        const bool drag = k.rfind("drag:", 0) == 0;
        if (k.rfind("click:", 0) == 0 || drag) {
            std::string id = k.substr(drag ? 5 : 6);
            float ddx = 0, ddy = 0;
            if (drag) {
                const auto c1 = id.find(':');
                if (c1 == std::string::npos)
                    return;
                std::sscanf(id.c_str() + c1 + 1, "%f:%f", &ddx, &ddy);
                id.resize(c1);
            }
            Rml::Element* el = !showing ? nullptr
                               : hud_mode.load() && hud ? hud->element(id)
                                                        : ui->element(id);
            if (!el || draw_w <= 0)
                return;
            const Rml::Vector2f pos = el->GetAbsoluteOffset(Rml::BoxArea::Border);
            const Rml::Vector2f size = el->GetBox().GetSize(Rml::BoxArea::Border);
            const float fx = static_cast<float>(vis_x) + pos.x + (drag ? 20.0f : size.x * 0.5f);
            const float fy = static_cast<float>(vis_y) + pos.y + (drag ? 20.0f : size.y * 0.5f);
            SDL_Window* win = SDL_GetKeyboardFocus();
            if (!win) {
                int n = 0;
                SDL_Window** list = SDL_GetWindows(&n);
                if (list && n > 0)
                    win = list[0];
                SDL_free(list);
            }
            const float d = win ? std::max(0.1f, SDL_GetWindowPixelDensity(win)) : 1.0f;
            SDL_Event e{};
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.windowID = win ? SDL_GetWindowID(win) : 0;
            e.motion.x = (draw_x + fx / static_cast<float>(fw) * draw_w) / d;
            e.motion.y = (draw_y + fy / static_cast<float>(fh) * draw_h) / d;
            SDL_PushEvent(&e);
            e = SDL_Event{};
            e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.down = true;
            SDL_PushEvent(&e);
            if (drag) {
                SDL_Event m{};
                m.type = SDL_EVENT_MOUSE_MOTION;
                m.motion.windowID = win ? SDL_GetWindowID(win) : 0;
                m.motion.x = (draw_x + (fx + ddx) / static_cast<float>(fw) * draw_w) / d;
                m.motion.y = (draw_y + (fy + ddy) / static_cast<float>(fh) * draw_h) / d;
                SDL_PushEvent(&m);
            }
            e.type = SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.down = false;
            SDL_PushEvent(&e);
            return;
        }
        SDL_Event e;
        if (k == "wait")
            return;  // 150 ms of nothing
        if (!MenuUi::script_event(k, e)) {
            std::fprintf(stderr, "overlay: unknown script step %s\n", k.c_str());
            return;
        }
        SDL_PushEvent(&e);
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            e.type = SDL_EVENT_GAMEPAD_BUTTON_UP;
            e.gbutton.down = false;
        } else {
            e.type = SDL_EVENT_KEY_UP;
            e.key.down = false;
        }
        SDL_PushEvent(&e);
    }

    // ---- game thread -----------------------------------------------------------------------------
    bool on_event(const SDL_Event& ev) {
        // The engine creates the game window; its first event is the earliest point dreamcomp
        // sees it, on the thread that polls it.
        if (!icon_set) {
            if (SDL_Window* w = SDL_GetWindowFromEvent(&ev)) {
                apply_port_icon(w);
                icon_set = true;
            }
        }
        if (!open.load() && ev.type == SDL_EVENT_KEY_DOWN && (ev.key.key == SDLK_F1 || ev.key.key == SDLK_F2) &&
            !ev.key.repeat) {
            hud_list = ev.key.key == SDLK_F2;
            // F1: the HUD editor (it used to be the engine's binding screen; the Controls tab
            // edits bindings now). Consumed, so the engine never sees it.
            std::printf("overlay: HUD editor opened by %s\n", hud_list.load() ? "F2 (list)" : "F1");
            open = true;
            shown = Image{};
            {
                std::lock_guard<std::mutex> lock(q);
                want_hud = true;
                events.clear();
            }
            cv.notify_one();
            return true;
        }
        if (!open.load()) {
            const bool esc = ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE &&
                             !ev.key.repeat;
            const bool select = ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                ev.gbutton.button == SDL_GAMEPAD_BUTTON_BACK;
            if (!esc && !select)
                return false;
            std::printf("overlay: opened by %s\n", esc ? "Escape" : "a pad's Select/Back");
            open = true;  // the game's input stops at once; the first image follows
            shown = Image{};  // never show the panel as it was when last closed
            {
                std::lock_guard<std::mutex> lock(q);
                want_open = true;
                events.clear();
            }
            cv.notify_one();
            return true;
        }
        const bool used = consumed_while_open(ev);
        if (used || ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
            {
                std::lock_guard<std::mutex> lock(q);
                if (events.size() < 512)
                    events.push_back(ev);
            }
            cv.notify_one();
        }
        return used;
    }

    // The newest finished image for `mode`, if it is newer than what `shown` holds.
    bool take_image(const DrawMode& mode) {
        bool notify = false;
        bool got = false;
        {
            std::lock_guard<std::mutex> lock(q);
            if (want != mode) {
                want = mode;
                notify = true;
            }
            if (front.seq != shown.seq && front.mode == mode && !front.px.empty()) {
                shown.px.swap(front.px);
                shown.mode = front.mode;
                shown.w = front.w;
                shown.h = front.h;
                shown.x = front.x;
                shown.y = front.y;
                shown.seq = front.seq;
                got = true;
            }
        }
        if (notify)
            cv.notify_one();
        return got;
    }

    const std::uint32_t* render_window(unsigned vw, unsigned vh, bool& changed) {
        changed = false;
        run_guest_actions();
        if (!open.load() || vw == 0 || vh == 0)
            return nullptr;
        {
            std::lock_guard<std::mutex> lock(q);
            if (shot_pending)
                return nullptr;  // the screenshot is taken through draw()
        }
        const DrawMode mode{vw, vh, vw, vh, false};
        changed = take_image(mode);
        if (shown.mode != mode || shown.px.size() != static_cast<std::size_t>(vw) * vh)
            return nullptr;  // not drawn at this size yet (a resize): nothing this frame
        return shown.px.data();
    }

    void draw(std::uint32_t* px, unsigned w, unsigned h, unsigned vw, unsigned vh) {
        run_guest_actions();
        if (!open.load() || !px || w == 0 || h == 0)
            return;
        const DrawMode mode{w, h, vw, vh, true};
        take_image(mode);
        if (shown.mode != mode || shown.px.empty())
            return;
        composite(px, w, h, shown.px.data(), shown.w, shown.h, shown.x, shown.y);
        std::string shot;
        {
            std::lock_guard<std::mutex> lock(q);
            if (shot_pending) {
                shot = pending_shot;
                shot_pending = false;
                pending_shot.clear();
            }
        }
        if (!shot.empty()) {
            if (dream::render::png::write_file(std::filesystem::u8path(shot), px, w, h))
                std::printf("overlay: wrote %s (%ux%u)\n", shot.c_str(), w, h);
            else
                std::fprintf(stderr, "overlay: cannot write %s\n", shot.c_str());
        }
    }

    void on_vblank(std::uint64_t frame) {
        run_guest_actions();
        {
            std::lock_guard<std::mutex> lock(q);
            vblank_frame = frame;
        }
        if (has_script && frame == script_frame)
            cv.notify_one();
    }
};

Overlay::Overlay(const OverlayContext& ctx) : impl_(new Impl(ctx)) {}
Overlay::~Overlay() { delete impl_; }

bool Overlay::on_event(const void* sdl_event) {
    if (!sdl_event)
        return false;
    return impl_->on_event(*static_cast<const SDL_Event*>(sdl_event));
}
bool Overlay::is_open() const { return impl_->open.load(); }
void Overlay::draw(std::uint32_t* rgba, unsigned w, unsigned h, unsigned view_w, unsigned view_h) {
    impl_->draw(rgba, w, h, view_w, view_h);
}
const std::uint32_t* Overlay::render_window(unsigned view_w, unsigned view_h, bool& changed) {
    return impl_->render_window(view_w, view_h, changed);
}
void Overlay::on_vblank(std::uint64_t frame) { impl_->on_vblank(frame); }
void Overlay::on_paused_tick() { impl_->run_guest_actions(); }

}  // namespace dreamcomp::frontend
