// Generic launcher (WP2.6 acceptance, WP3.1): boots a translated title on the runtime without any
// graphics and reports what it did. Each game target compiles this file together with its emitted
// units; the per-game TOML says what to load.
//
//   <game>_boot --config games/crazytaxi/crazytaxi.toml [--stop-on-ta] [--max-frames N]
//               [--max-seconds S] [--report FILE]
//
// Exit code 0 when the run reached the requested stop (the first TA FIFO write with --stop-on-ta,
// otherwise the frame or time limit), 1 on a fault, 2 on a setup error.
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__APPLE__) || defined(__linux__)
#include <execinfo.h>
#endif
#include <ctime>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "dream/runtime/aica/aica.h"
#include "dream/runtime/aica/rtc.h"
#include "dream/runtime/gdrom/disc.h"
#include "dream/runtime/gdrom/mod_disc.h"
#include "dream/runtime/sha1.h"
#include "dream/runtime/host_ext.h"
#include "dream/runtime/hle/bios.h"
#include "dream/runtime/holly/g2dma.h"
#include "dream/runtime/holly/sysblock.h"
#include "dream/runtime/maple/maple.h"
#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/pvr/core.h"
#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/dmac.h"
#include "dream/runtime/sh4/ops.h"
#include "dream/runtime/system.h"
#ifdef DREAM_DEV_INTERPRETER
#include "dream/runtime/devinterp/interpreter.h"
#include "dream/runtime/devinterp/replay.h"
#endif
#include "dream/translator/config/game_config.h"
#ifdef DREAM_WITH_AUDIO
#include "dream/audio/sink.h"
#endif
#ifdef DREAM_WITH_RENDERER
#include <thread>

#include "dream/render/background.h"
#include "dream/render/display_list.h"
#include "dream/render/framebuffer.h"
#include "dream/render/interpolate.h"
#include "dream/render/input_menu.h"
#include "dream/render/overlay.h"
#include "dream/render/vk/offscreen.h"
#include "dream/render/vk/present.h"
#include "dream/render/vk/renderer.h"
#include "dream/render/vk/window.h"
#endif

namespace {

struct StopRun {
    const char* why;
};

// The guest program counter in the context is only refreshed where the emitted code checks for an
// interrupt, so it names the last such point and not the instruction that faulted. The host stack
// does name it: every guest function is a real C++ function called `fn_<address>`, so walking the
// host stack and keeping those frames gives the guest call chain exactly, innermost first. Cheap,
// and it needs nothing tracked while the run is going well.
void print_guest_backtrace() {
#if defined(__APPLE__) || defined(__linux__)
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    char** names = ::backtrace_symbols(frames, n);
    if (!names)
        return;
    std::fprintf(stderr, "  guest call chain (innermost first):\n");
    unsigned shown = 0;
    for (int i = 0; i < n && shown < 12; ++i) {
        // Keep the emitted guest functions and drop the runtime's own frames, which are the same
        // half-dozen every time and say nothing about the title.
        const char* fn = std::strstr(names[i], "fn_0c");
        if (!fn)
            fn = std::strstr(names[i], "fn_8c");
        if (!fn)
            continue;
        char addr[16] = {};
        std::snprintf(addr, sizeof addr, "%.11s", fn);
        std::fprintf(stderr, "    %s\n", addr);
        ++shown;
    }
    if (shown == 0)
        std::fprintf(stderr,
                     "    (no emitted frames on the stack; the fault is in the runtime "
                     "or in interpreted code)\n");
    std::free(names);
#else
    std::fprintf(stderr, "  guest call chain: not available on this platform\n");
#endif
}

#ifdef DREAM_WITH_RENDERER
// The windowed mode (WP2.3 step 6). Nothing here is constructed without --window, so the default
// run stays exactly as headless as it was, which is what CI builds and what the emitter's
// differential tests use.
//
// The guest still nominates what is shown: at vertical blank the address in FB_R_SOF1 decides.
// What differs is where those pixels are read from.
//
//   the guest displays a buffer we rendered    the rendered image is shown directly, at whatever
//                                              resolution --scale asked for
//   the guest displays anything else           that buffer is decoded out of video memory, which
//                                              is how a title's own pixel writes appear
//
// What it deliberately does *not* do by default is write the rendered frame back into video
// memory. That is exact, and it is what a render to texture needs, but on Crazy Taxi the write
// lands on top of texture data the title has in the same memory and erases its logo. Until the
// written region can be worked out precisely enough to be safe, --framebuffer-writeback turns it
// on and the default leaves video memory alone. Flycast reaches the same conclusion from the other
// direction: its framebuffer emulation is off unless a title needs it.
// CPU cycles the game thread used, set when the title's run ends (dreamcomp; report line).
static std::uint64_t g_game_thread_cycles = 0;

// Display sync (dreamcomp, --sync-display): when the display refreshes within 0.5 % of the guest's
// frame rate, the pacing clock runs at the display's rate instead of the guest's. A 59.94 Hz game
// on a 60 Hz panel otherwise falls one frame behind every 16.7 s and the panel shows a frame twice.
// The rate is aimed 0.01 % under the display's, so the swapchain queue stays drained (the lowest
// latency) and the panel repeats one frame about every 2.8 minutes instead. The game runs 0.1 %
// fast (60 for 59.94); the audio sink's rate control absorbs it. Only the rate is matched, not
// the phase: on Intel's Windows Vulkan driver (a compositor copy) presenting at a chosen point
// before vblank changed neither latency nor smoothness (docs/PACING.md in dreamcomp).
struct DisplaySync {
    bool enabled = false;
    double guest_frame_s = 0;  // average guest frame length, from cycles per vblank
    std::uint64_t last_cycles = 0;
    double display_hz = 0, factor = 1.0;
    std::uint64_t locked = 0, unlocked = 0;
    // Recent guest frame lengths in host time (pads read -> next vblank), for the report.
    std::vector<float> emu_ms;
    // Phase lock (dreamcomp), where frames are flipped (Window::flips(), the DXGI path): each
    // frame starts so that even a slow one is presented `margin` before its vblank. The start is
    // `budget` (the slowest frame of the last second plus `pad`) before that point, so the pads
    // are still read just before the guest runs, and a typical frame lands a few ms before the
    // vblank instead of straddling it. Without it, presents drifted across vblanks with the
    // emulation time and the panel dropped one frame and repeated another (181 in a minute).
    std::chrono::nanoseconds margin{3'000'000};  // DREAM_SYNC_MARGIN_MS
    std::chrono::nanoseconds pad{1'000'000};     // DREAM_SYNC_PAD_MS
    std::chrono::steady_clock::time_point target{};  // the vblank the frame being run aims at
    std::uint64_t phase_locked = 0, late = 0, reanchored = 0;
    std::vector<float> aim_ms;  // present time against target - margin
    std::chrono::nanoseconds budget(std::chrono::nanoseconds period) const {
        const std::size_t n = emu_ms.size(), from = n > 60 ? n - 60 : 0;
        float worst = 10.0f;
        if (n > from)
            worst = *std::max_element(emu_ms.begin() + static_cast<std::ptrdiff_t>(from), emu_ms.end());
        const auto b = std::chrono::nanoseconds(static_cast<long long>(worst * 1e6)) + pad;
        return std::min(b, period - margin - std::chrono::nanoseconds(1'000'000));
    }
    double note_guest_frame(std::uint64_t cycles) {
        double s = 0;
        if (last_cycles && cycles > last_cycles) {
            s = static_cast<double>(cycles - last_cycles) / 200e6;
            guest_frame_s = guest_frame_s == 0 ? s : guest_frame_s * 0.95 + s * 0.05;
        }
        last_cycles = cycles;
        return s;
    }
    void report() const {
        if (enabled)
            std::printf("display sync: %llu frames at the display's rate (%.3f Hz display, guest "
                        "%.3f Hz, clock x%.5f), %llu at the guest's\n",
                        static_cast<unsigned long long>(locked), display_hz,
                        guest_frame_s > 0 ? 1.0 / guest_frame_s : 0.0, factor,
                        static_cast<unsigned long long>(unlocked));
        if (phase_locked) {
            std::printf("display sync: %llu frames phase-locked (margin %.1f ms), %llu presented after "
                        "their aim by more than the margin, %llu re-aimed",
                        static_cast<unsigned long long>(phase_locked),
                        static_cast<double>(margin.count()) * 1e-6, static_cast<unsigned long long>(late),
                        static_cast<unsigned long long>(reanchored));
            if (aim_ms.size() >= 10) {
                std::vector<float> v = aim_ms;
                std::sort(v.begin(), v.end());
                std::printf("; present vs aim p5 %.2f p50 %.2f p95 %.2f max %.1f ms",
                            static_cast<double>(v[v.size() / 20]), static_cast<double>(v[v.size() / 2]),
                            static_cast<double>(v[v.size() * 19 / 20]), static_cast<double>(v.back()));
            }
            std::printf("\n");
        }
        if (emu_ms.size() >= 10) {
            std::vector<float> v = emu_ms;
            std::sort(v.begin(), v.end());
            std::printf("pacing: host time per guest frame (pads read -> vblank) p50 %.2f p90 %.2f "
                        "p99 %.2f max %.1f ms\n",
                        static_cast<double>(v[v.size() / 2]), static_cast<double>(v[v.size() * 9 / 10]),
                        static_cast<double>(v[v.size() * 99 / 100]), static_cast<double>(v.back()));
        }
    }
};

// Frame pacing as the player sees it (dreamcomp): the wall-clock interval between successive
// vblanks of a real-time run, and how far the pacing sleep overshot. Printed as the `pacing:`
// report line; a stutter shows up as late frames long before it shows in the average speed.
struct Pacing {
    std::vector<float> intervals_ms;
    double overshoot_ms_sum = 0, overshoot_ms_max = 0;
    std::uint64_t sleeps = 0;
    std::chrono::steady_clock::time_point last{};
    // A late frame with what happened during it: textures decoded, pipelines created and host
    // time spent drawing, so a hitch can be attributed without a profiler.
    struct Late {
        std::uint64_t frame;
        float ms;
        std::uint64_t textures, pipelines;
        float render_ms, decode_ms;
    };
    std::vector<Late> late_frames;
    // Input age (dreamcomp): from reading the pads to presenting the frame the guest ran with them.
    std::vector<float> input_age_ms;
    std::chrono::steady_clock::time_point input_read{};
    void pads_read() { input_read = std::chrono::steady_clock::now(); }
    void presenting() {
        if (input_read.time_since_epoch().count() != 0 && input_age_ms.size() < 2'000'000)
            input_age_ms.push_back(std::chrono::duration<float, std::milli>(
                                       std::chrono::steady_clock::now() - input_read)
                                       .count());
    }
    std::uint64_t last_textures = 0, last_pipelines = 0, last_render_ns = 0, last_decode_ns = 0;
    void vblank(std::uint64_t frame, std::uint64_t textures, std::uint64_t pipelines,
                std::uint64_t render_ns, std::uint64_t decode_ns) {
        const auto now = std::chrono::steady_clock::now();
        if (last.time_since_epoch().count() != 0 && intervals_ms.size() < 2'000'000) {
            const float ms = std::chrono::duration<float, std::milli>(now - last).count();
            intervals_ms.push_back(ms);
            if (ms > 20.0f && late_frames.size() < 64)
                late_frames.push_back({frame, ms, textures - last_textures,
                                       pipelines - last_pipelines,
                                       static_cast<float>(render_ns - last_render_ns) * 1e-6f,
                                       static_cast<float>(decode_ns - last_decode_ns) * 1e-6f});
        }
        last = now;
        last_textures = textures;
        last_pipelines = pipelines;
        last_render_ns = render_ns;
        last_decode_ns = decode_ns;
    }
    void slept(std::chrono::steady_clock::time_point target) {
        const double over =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - target)
                .count();
        ++sleeps;
        overshoot_ms_sum += over;
        overshoot_ms_max = std::max(overshoot_ms_max, over);
    }
    void report() const {
        if (intervals_ms.size() < 10)
            return;
        std::vector<float> v = intervals_ms;
        std::sort(v.begin(), v.end());
        auto pct = [&](double q) { return v[static_cast<std::size_t>(q * (v.size() - 1))]; };
        std::size_t late = 0, very_late = 0;
        double sum = 0;
        for (float x : intervals_ms) {
            sum += x;
            late += x > 20.0f;
            very_late += x > 33.4f;
        }
        std::printf("pacing: %zu frames, mean %.2f ms, p50 %.2f p95 %.2f p99 %.2f max %.1f ms, "
                    "%zu over 20 ms, %zu over 33 ms; sleep overshoot mean %.2f max %.2f ms "
                    "(%llu sleeps)\n",
                    v.size(), sum / static_cast<double>(v.size()), pct(0.5), pct(0.95), pct(0.99),
                    v.back(), late, very_late, sleeps ? overshoot_ms_sum / static_cast<double>(sleeps) : 0.0,
                    overshoot_ms_max, static_cast<unsigned long long>(sleeps));
        if (input_age_ms.size() >= 10) {
            std::vector<float> a = input_age_ms;
            std::sort(a.begin(), a.end());
            double asum = 0;
            for (float x : a) asum += x;
            std::printf("pacing: input age at present (pads read -> frame presented): mean %.2f ms, "
                        "p50 %.2f p95 %.2f max %.1f ms\n",
                        asum / static_cast<double>(a.size()), a[a.size() / 2],
                        a[static_cast<std::size_t>(0.95 * (a.size() - 1))], a.back());
        }
        if (!late_frames.empty()) {
            std::printf("pacing: late frames (guest frame: ms, textures decoded, pipelines made, "
                        "render ms, of which texture decode ms):");
            for (const auto& l : late_frames)
                std::printf(" %llu:%.0f/%llut/%llup/%.0fr/%.0fd",
                            static_cast<unsigned long long>(l.frame), static_cast<double>(l.ms),
                            static_cast<unsigned long long>(l.textures),
                            static_cast<unsigned long long>(l.pipelines),
                            static_cast<double>(l.render_ms), static_cast<double>(l.decode_ms));
            std::printf("\n");
        }
    }
};

struct Live {
    Live(dream::mem::DcMemory& memory, const dream::pvr::Core& pvr) : memory_(memory), pvr_(pvr) {}
    Pacing pacing;

    // `scale` multiplies the resolution the geometry is drawn at. The frame is resampled back to
    // the guest's own framebuffer on the way into video memory, so a higher setting sharpens the
    // geometry without lying to the guest about the size of its screen.
    // The render target's size for a scale and shape (dreamcomp: also used when the shape
    // changes at run time).
    void target_size(float render_aspect, std::uint32_t& w, std::uint32_t& h) const {
        const float wide = render_aspect > 0.0f ? render_aspect / (4.0f / 3.0f) : 1.0f;
        w = static_cast<std::uint32_t>(static_cast<float>(kGuestWidth * scale_) * wide + 0.5f);
        h = kGuestHeight * scale_;
    }

    // Rebuilds the render target(s) at a new shape (dreamcomp; Expanded aspect follows the
    // window). On the guest thread at a vblank: the device is idled, the old frame is not shown
    // again, and the next render fills the new target.
    // A new scale (dreamcomp: Resolution applied in game) takes the same path.
    void retarget() {
        const bool new_aspect =
            want_render_aspect > 0.0f && std::fabs(want_render_aspect - render_aspect_) >= 0.005f;
        const bool new_scale = want_scale != 0 && want_scale != scale_;
        if (!new_aspect && !new_scale)
            return;
        if (new_aspect)
            render_aspect_ = want_render_aspect;
        if (new_scale)
            scale_ = want_scale;
        std::uint32_t w = 0, h = 0;
        target_size(render_aspect_, w, h);
        if (w == offscreen.width() && h == offscreen.height())
            return;
        vkDeviceWaitIdle(window.context().device());
        const bool readback = offscreen.readback;
        offscreen.destroy();
        offscreen.readback = readback;
        if (!offscreen.create(window.context(), w, h))
            std::fprintf(stderr, "window: cannot rebuild the render target: %s\n",
                         offscreen.error().c_str());
        // The old pass is gone: pipelines made from now on use the new (compatible) one.
        renderer.set_render_pass(offscreen.render_pass());
        if (interpolate) {
            interp_off.destroy();
            interp_off.create(window.context(), w, h);
            interp_ready = false;
            have_prev = false;
        }
        await_render = true;
        std::printf("window: render target now %ux%u (aspect %.3f)\n", w, h,
                    static_cast<double>(render_aspect_));
    }

    bool start(unsigned scale, bool validation, const std::string& title,
               float render_aspect = 0.0f, bool fullscreen = false) {
        scale_ = scale;
        render_aspect_ = want_render_aspect = render_aspect;
        // --render-aspect (dreamcomp): an anamorphic widescreen title still addresses a 640x480
        // screen, but its view is squeezed horizontally; drawing it into a target that is wider
        // by the same factor keeps the horizontal resolution of a native wide render.
        const float wide = render_aspect > 0.0f ? render_aspect / (4.0f / 3.0f) : 1.0f;
        const std::uint32_t w =
            static_cast<std::uint32_t>(static_cast<float>(kGuestWidth * scale) * wide + 0.5f);
        const std::uint32_t h = kGuestHeight * scale;
        const std::uint32_t win_w = static_cast<std::uint32_t>(720.0f * (render_aspect > 0.0f ? render_aspect : 4.0f / 3.0f));
        if (!window.create(title.c_str(), win_w, 720, validation)) {
            error = window.error();
            return false;
        }
        if (fullscreen)
            window.set_fullscreen(true);
        if (!offscreen.create(window.context(), w, h)) {
            error = offscreen.error();
            return false;
        }
        if (interpolate && interpolate_only_above_60) {
            const float hz = window.refresh_rate();
            interpolate = hz > 61.0f;
            std::printf("interpolation: display %.0f Hz, %s\n", static_cast<double>(hz),
                        interpolate ? "on" : "off (60 Hz or unknown)");
        }
        // Geometry interpolation (dreamcomp): a second target for the frame between two renders.
        if (interpolate && !interp_off.create(window.context(), w, h)) {
            error = interp_off.error();
            return false;
        }
        // The renderer's pipelines are built against the offscreen render pass, not the window's:
        // that is where the geometry is drawn.
        if (!renderer.create(window.context(), offscreen.render_pass())) {
            error = renderer.error();
            return false;
        }
        if (!presenter.create(window.context(), window.render_pass())) {
            error = presenter.error();
            return false;
        }
        overlay_presenter.overlay_layer = true;  // dreamcomp: the in-game menu, blended on top
        if (!overlay_presenter.create(window.context(), window.render_pass())) {
            error = overlay_presenter.error();
            return false;
        }
        // dreamcomp: pipelines from earlier runs, created now rather than mid-game.
        // DREAM_NO_PIPELINE_CACHE=1 starts cold, for measuring.
        if (!std::getenv("DREAM_NO_PIPELINE_CACHE")) {
            const std::string bindings = dream::render::vk::default_bindings_path();
            const auto slash = bindings.find_last_of("/\\");
            if (slash != std::string::npos) {
                const auto t0 = std::chrono::steady_clock::now();
                renderer.use_pipeline_cache(bindings.substr(0, slash + 1) + "pipeline_cache.bin");
                if (renderer.prewarmed)
                    std::printf("pipelines: %u prepared in %.1f ms\n", renderer.prewarmed,
                                std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count());
            }
        }
        renderer.set_memory(memory_.vram(), dream::mem::DcMemory::kVramSize,
                            pvr_.reg_block() + 0x1000 / 4, palette_format());
        std::printf("window: %s, drawing at %ux%u\n", window.context().caps().device_name.c_str(),
                    w, h);
        std::printf("window: presenting through %s\n", window.present_path().c_str());
        return true;
    }

    // The guest has handed the hardware a display list. Draw it, then put the result where the
    // hardware would have put it.
    void render(const std::vector<std::uint32_t>& stream) {
        last_stream = stream;  // kept so F11 can write the list that drew what is on screen
        decoder.reset();
        decoder.feed_stream(stream.data(), stream.size());
        // The plane the whole frame sits on is not in the stream: the title writes it into the
        // parameter buffer and points a register at it. Without it the screen behind the geometry
        // is whatever the renderer cleared to.
        if (dream::render::add_background(decoder.frame(), pvr_.reg_block(), memory_.vram(),
                                          dream::mem::DcMemory::kVramSize))
            ++backgrounds;
        for (auto* e : dream::host::extensions()) e->on_frame(decoder.frame());
        const dream::render::Frame& frame = decoder.frame();
        // PT_ALPHA_REF: the punch-through threshold the guest chose, which changes per scene.
        geometry.alpha_ref = static_cast<float>(pvr_.reg(0x11C) & 0xFFu) / 255.0f;
        geometry.shadow_scale = static_cast<float>(pvr_.reg(0x074) & 0xFFu) / 256.0f;
        dream::render::describe_fog(pvr_.reg_block(), geometry.fog);
        // Auto-sort: with FPU_PARAM_CFG bit 21 the region array's per-tile PreSort bit decides
        // (not read yet: games that use type-2 headers sort, as Soulcalibur's tiles all do);
        // otherwise ISP_FEED_CFG bit 0 clear means the hardware sorts.
        geometry.autosort = (pvr_.reg(0x07C) & (1u << 21)) != 0 || (pvr_.reg(0x098) & 1u) == 0;
        // Palette memory is re-read every render but only invalidates the cache when it has
        // actually changed, which is rare; dropping it every frame would decode every texture
        // every frame.
        renderer.textures().begin_frame();  // LRU eviction between renders (dreamcomp)
        // Textures the guest has overwritten since the last render are decoded again (dreamcomp;
        // Flycast write-protects texture pages for the same reason). DREAM_NO_VRAM_INVALIDATE=1
        // keeps the old behaviour, for comparison.
        {
            static const bool keep = std::getenv("DREAM_NO_VRAM_INVALIDATE") != nullptr;
            memory_.take_vram_dirty([&](std::uint32_t begin, std::uint32_t end) {
                if (!keep)
                    vram_invalidated +=
                        renderer.textures().invalidate_range(begin, end, /*rendered=*/false);
            });
        }
        if (renderer.textures().set_palette(pvr_.reg_block() + 0x1000 / 4, palette_format()))
            ++palette_changes;
        if (interpolate) {
            interp_ready = false;
            if (have_prev) {
                const auto t0 = std::chrono::steady_clock::now();
                dream::render::BlendStats st;
                const bool blended = dream::render::blend_frames(prev_frame, frame, interp_opts,
                                                                 interp_pairs, interp_frame, st);
                interp_ms += std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
                interp_vertices += st.match.vertices_b;
                interp_matched += st.match.matched_vertices;
                interp_jumped += st.jumped_strips;
                if (!blended) {
                    ++interp_cuts;
                } else if (interp_off.render(renderer, interp_frame, geometry)) {
                    interp_ready = true;
                    ++interp_rendered;
                }
            }
            prev_frame = frame;
            have_prev = true;
        }
        await_render = false;
        if (!offscreen.render(renderer, frame, geometry)) {
            if (!reported_error) {
                reported_error = true;
                std::fprintf(stderr, "render: %s\n", offscreen.error().c_str());
            }
            return;
        }
        write_back();
        ++rendered;
    }

    // True for a buffer the renderer has drawn into at some point. A title alternates between two
    // and does not swap the displayed one on every render, so asking whether the displayed buffer
    // is *the* most recent target is too strict; asking whether it is one of ours is the question
    // that matters. A buffer we have never rendered to holds pixels the title put there itself,
    // and those have to come out of video memory.
    bool rendered_into(std::uint32_t address) const {
        for (std::uint32_t a : targets_)
            if (a == address)
                return true;
        return false;
    }

    // Where the frame went, and optionally the frame itself. The address is always worked out,
    // because presentation needs to know which buffer holds what the renderer drew.
    void write_back() {
        dream::render::FramebufferInfo display;
        std::uint32_t w = kGuestWidth, h = kGuestHeight;
        if (dream::render::describe_framebuffer(pvr_.reg_block(), display)) {
            w = display.width;
            h = display.height;
        }
        dream::render::FramebufferInfo target;
        if (!dream::render::describe_write_framebuffer(pvr_.reg_block(), w, h, target))
            return;
        target_address = target.address;
        if (!rendered_into(target.address) && targets_.size() < 8)
            targets_.push_back(target.address);
        if (!writeback)
            return;
        if (!dream::render::encode_framebuffer(target, offscreen.pixels(), offscreen.width(),
                                               offscreen.height(), memory_.vram(),
                                               dream::mem::DcMemory::kVramSize))
            return;
        ++written_back;
        // Whatever the cache decoded from those bytes is now out of date. A title that is merely
        // double buffering has nothing cached there and loses nothing; a title rendering a texture
        // gets the frame it just drew.
        const std::uint32_t bytes =
            (target.width + target.modulus) * target.height * bytes_per_written_pixel(target);
        // DREAM_NO_WRITEBACK_INVALIDATE holds the cache across the write, to tell a texture that
        // went wrong here apart from one that was already wrong: if the picture does not change,
        // the fault is not in this step.
        if (!std::getenv("DREAM_NO_WRITEBACK_INVALIDATE"))
            renderer.textures().invalidate_range(target.address, target.address + bytes);
    }

    // Window events and pad state, once per frame (dreamcomp). The vblank hook calls this after
    // its pacing sleep, just before the guest's pads are read, so the input the game sees is
    // sampled right before it runs the frame rather than a whole sleep earlier; present() then
    // works on that same poll (its hotkeys are edges computed per poll, so a second poll would
    // lose them). Returns false when the user has closed the window.
    bool poll_input() {
        input_polled = true;
        return window_open = window.poll();
    }

    // Vertical blank: show what the video hardware would be scanning out, and read the keyboard.
    // Returns false when the user has closed the window.
    bool present(std::uint64_t frame, std::uint64_t guest_cycles) {
        guest_frame = frame;
        if (!input_polled)
            poll_input();
        input_polled = false;
        if (!window_open)
            return false;
        retarget();
        update_counter(guest_cycles);
        pump_menu();

        dream::render::FramebufferInfo fb;
        const bool described = dream::render::describe_framebuffer(pvr_.reg_block(), fb);
        // The guest is displaying the buffer the renderer drew into, so show what was drawn rather
        // than a copy of it squeezed back through the guest's pixel format. This is also what makes
        // --scale visible: the window gets the full rendered resolution.
        // Not while a rebuilt target waits for its first render: its image is not drawn yet.
        from_renderer = described && fb.enabled && rendered_into(fb.address) && !await_render;
        if (from_renderer) {
            shown = fb;
            shown.width = offscreen.width();
            shown.height = offscreen.height();
            have_frame = true;
        } else if (described && fb.enabled &&
                   dream::render::decode_framebuffer(fb, memory_.vram(),
                                                     dream::mem::DcMemory::kVramSize, pixels)) {
            shown = fb;
            have_frame = true;
            ++decoded_frames;
        }
        if (!have_frame)
            return true;  // nothing displayable yet: the window keeps its cleared background

        // The in-between frame goes first, before the real frame's swapchain image is acquired
        // (acquiring twice would wait on a fence the first acquire just reset).
        const bool overlay_up = overlay_open();
        // An open overlay owns the keyboard and pads; the game keeps running with them released.
        window.set_game_input_blocked(overlay_up);
        if (interpolate && interp_ready && from_renderer && !offscreen.readback && !show_fps &&
            !menu.is_open() && !overlay_up) {
            interp_ready = false;
            std::uint32_t iimage = 0;
            VkCommandBuffer icmd = VK_NULL_HANDLE;
            if (window.begin_frame(iimage, icmd)) {
                VkClearValue iclear[2]{};
                iclear[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
                iclear[1].depthStencil = {0.0f, 0};
                VkRenderPassBeginInfo ib{};
                ib.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                ib.renderPass = window.render_pass();
                ib.framebuffer = window.framebuffer(iimage);
                ib.renderArea.extent = window.extent();
                ib.clearValueCount = 2;
                ib.pClearValues = iclear;
                vkCmdBeginRenderPass(icmd, &ib, VK_SUBPASS_CONTENTS_INLINE);
                const VkViewport iv{0.0f, 0.0f, static_cast<float>(window.extent().width),
                                    static_cast<float>(window.extent().height), 0.0f, 1.0f};
                const VkRect2D isc{{0, 0}, window.extent()};
                vkCmdSetViewport(icmd, 0, 1, &iv);
                vkCmdSetScissor(icmd, 0, 1, &isc);
                presenter.draw_view(icmd, interp_off.sampled_view(), interp_off.width(),
                                    interp_off.height(), window.extent());
                vkCmdEndRenderPass(icmd);
                if (!window.end_frame(iimage))
                    window.recreate_swapchain();
                ++interp_presented;
                // Half a guest frame (59.94 Hz) before the real one.
                if (pace_interpolation) {
                    const auto due = last_present + std::chrono::microseconds(8342);
                    if (due > std::chrono::steady_clock::now())
                        std::this_thread::sleep_until(due);
                }
            }
        }
        std::uint32_t image = 0;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (!window.begin_frame(image, cmd)) {
            window.recreate_swapchain();
            return true;
        }
        // Direct present (dreamcomp): a rendered frame nothing needs on the host is sampled where it
        // is, on the GPU. Overlays drawn on the CPU and framebuffer write-back still take the copy.
        // An overlay that can hand over a window-sized image is blended on the GPU (dreamcomp),
        // so it does not force the frame back to the CPU.
        const std::uint32_t* overlay_px = nullptr;
        bool overlay_changed = false;
        if (overlay_up) {
            const VkExtent2D ext = window.extent();
            for (auto* e : overlays)
                if (e->overlay_open() &&
                    (overlay_px = e->overlay_image(ext.width, ext.height, overlay_changed)))
                    break;
            if (overlay_px && (overlay_changed || overlay_w != ext.width ||
                               overlay_h != ext.height)) {
                overlay_w = ext.width;
                overlay_h = ext.height;
                overlay_dirty = true;
            }
        }
        const bool cpu_overlay = overlay_up && !overlay_px;
        const bool direct =
            from_renderer && !offscreen.readback && !show_fps && !menu.is_open() && !cpu_overlay;
        const std::uint32_t* source =
            direct ? nullptr : (from_renderer ? offscreen.pixels() : pixels.data());
        // Composited into the frame as it is handed to the GPU, so it costs no extra copy of the
        // picture. Deliberately not into the buffers screenshot() and capture() read: F12 and F11
        // are meant to produce the game's own pixels, which is what makes one comparable with
        // another and usable as a reference image.
        if (direct) {
            // nothing to upload
        } else if (cpu_overlay && source) {
            // dreamcomp: an extension overlay blends into the frame, which means reading it back.
            // The staging buffer `decorate` hands over is write-combined on some GPUs (reads cost
            // ~100x), so the overlay works on a cached host copy that is then uploaded as is.
            overlay_frame.assign(source, source + static_cast<std::size_t>(shown.width) *
                                                      shown.height);
            std::uint32_t* px = overlay_frame.data();
            if (show_fps)
                draw_counter(px, shown.width, shown.height);
            for (auto* e : overlays)
                if (e->overlay_open())
                    e->draw_overlay(px, shown.width, shown.height, window.extent().width,
                                    window.extent().height);
            if (menu.is_open())
                menu.draw(px, shown.width, shown.width, shown.height, window.devices());
            presenter.upload(cmd, px, shown.width, shown.height);
        } else if (show_fps || menu.is_open())
            presenter.upload(cmd, source, shown.width, shown.height,
                             [this](std::uint32_t* px, std::uint32_t w, std::uint32_t h) {
                                 if (show_fps)
                                     draw_counter(px, w, h);
                                 // Last, so the screen sits over the counter rather than under it.
                                 if (menu.is_open())
                                     menu.draw(px, w, w, h, window.devices());
                             });
        else
            presenter.upload(cmd, source, shown.width, shown.height);
        // The overlay image goes up only when the menu redrew or the window changed size.
        if (overlay_px && overlay_dirty) {
            overlay_presenter.upload(cmd, overlay_px, overlay_w, overlay_h);
            overlay_dirty = false;
        }
        VkClearValue clears[2]{};
        clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        clears[1].depthStencil = {0.0f, 0};
        VkRenderPassBeginInfo rpbi{};
        rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass = window.render_pass();
        rpbi.framebuffer = window.framebuffer(image);
        rpbi.renderArea.extent = window.extent();
        rpbi.clearValueCount = 2;
        rpbi.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{0.0f,
                                  0.0f,
                                  static_cast<float>(window.extent().width),
                                  static_cast<float>(window.extent().height),
                                  0.0f,
                                  1.0f};
        const VkRect2D scissor{{0, 0}, window.extent()};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        if (direct)
            presenter.draw_view(cmd, offscreen.sampled_view(), offscreen.width(), offscreen.height(),
                                window.extent());
        else
            presenter.draw(cmd, window.extent());
        if (overlay_px)
            overlay_presenter.draw(cmd, window.extent());
        vkCmdEndRenderPass(cmd);
        if (!window.end_frame(image))
            window.recreate_swapchain();
        last_present = std::chrono::steady_clock::now();
        ++presented;
        if (window.pressed(dream::render::vk::Control::ToggleFps)) {
            show_fps = !show_fps;
            fps_mark = {};  // start the next average fresh rather than from whenever it last ran
        }
        if (window.pressed(dream::render::vk::Control::Screenshot))
            screenshot();
        if (window.pressed(dream::render::vk::Control::Capture))
            capture();
        if (screenshot_at.count(presented))
            screenshot();
        if (capture_at && presented == capture_at)
            capture();
        return true;
    }

    // What the player is pressing, as a Maple controller sees it. The buttons are active low.
    //
    // Everything here comes through the player's bindings rather than off fixed keys, so a pad and
    // the keyboard are both live and either can be rebound. The stick and the triggers are read as
    // values rather than as flags: a real pad's travel reaches the guest, and a keyboard still
    // produces the extremes it always did, because a digital source reads a full 1.0.
    void read_controls(dream::maple::ControllerState& pad, unsigned player = 0) const {
        using dream::render::PadControl;
        auto pad_held = [&](PadControl c) { return window.pad_held(player, c); };
        auto pad_value = [&](PadControl c) { return window.pad_value(player, c); };
        std::uint16_t held = 0;
        if (pad_held(PadControl::Up))
            held |= dream::maple::kUp;
        if (pad_held(PadControl::Down))
            held |= dream::maple::kDown;
        if (pad_held(PadControl::Left))
            held |= dream::maple::kLeft;
        if (pad_held(PadControl::Right))
            held |= dream::maple::kRight;
        if (pad_held(PadControl::A))
            held |= dream::maple::kA;
        if (pad_held(PadControl::B))
            held |= dream::maple::kB;
        if (pad_held(PadControl::X))
            held |= dream::maple::kX;
        if (pad_held(PadControl::Y))
            held |= dream::maple::kY;
        if (pad_held(PadControl::Start))
            held |= dream::maple::kStart;
        pad.buttons = static_cast<std::uint16_t>(0xFFFFu & ~held);
        pad.joy_x = dream::maple::axis_byte(pad_value(PadControl::StickLeft),
                                            pad_value(PadControl::StickRight));
        pad.joy_y = dream::maple::axis_byte(pad_value(PadControl::StickUp),
                                            pad_value(PadControl::StickDown));
        pad.ltrigger = dream::maple::trigger_byte(pad_value(PadControl::LeftTrigger));
        pad.rtrigger = dream::maple::trigger_byte(pad_value(PadControl::RightTrigger));
    }

    // --- the binding screen --------------------------------------------------------------------

    // Its keys are the launcher's fixed ones, never the player's: rebinding the game's buttons can
    // never take away the way back out. Called once per poll, running or paused.
    void pump_menu() {
        using dream::render::vk::Control;
        // Escape does two things a keystroke apart: leave the screen, then end the run. The key has
        // to be let go before it means the second one. Without this, closing the screen with escape
        // re-arms quit under a finger that is still down, and one key repeat ends the run -- which
        // from the outside is indistinguishable from a crash.
        if (rearm_escape && !window.held(Control::Back)) {
            rearm_escape = false;
            window.set_escape_quits(overlays.empty());  // an overlay owns escape (dreamcomp)
        }
        // dreamcomp: an overlay host has its own controls screen and gives F1 to the HUD editor.
        // The key is polled here, not taken from the event the overlay consumed, so it has to be
        // refused here as well.
        if (window.pressed(Control::Menu) && overlays.empty()) {
            if (menu.is_open())
                close_menu();
            else
                open_menu();
            return;
        }
        if (!menu.is_open())
            return;
        // A finished capture takes the frame to itself. The escape that cancelled one is also a
        // Back press, and closing the screen on it would make cancelling and leaving the same
        // keystroke.
        dream::render::Binding captured;
        bool cancelled = false;
        if (window.take_capture(captured, cancelled)) {
            menu.apply_capture(captured, cancelled);
            apply_bindings();
            return;
        }
        if (window.pressed(Control::Back)) {
            menu.back();  // one leaves a capture, a second leaves the screen
            if (!menu.is_open())
                close_menu();
            return;
        }
        if (window.pressed(Control::Up))
            menu.move(-1);
        if (window.pressed(Control::Down))
            menu.move(1);
        // The device row counts what is actually plugged in, so the selector wraps over the
        // keyboard and however many pads are connected right now.
        const unsigned devices = static_cast<unsigned>(window.devices().size());
        if (window.pressed(Control::Left))
            menu.adjust(-1, devices);
        if (window.pressed(Control::Right))
            menu.adjust(1, devices);
        if (window.pressed(Control::Start) || window.pressed(Control::A)) {
            if (menu.activate() == dream::render::InputMenu::Action::Close) {
                close_menu();
                return;
            }
            // Activating a binding row asks for an input; the window collects it for us.
            if (menu.awaiting())
                window.begin_capture();
        }
        apply_bindings();
    }

    void open_menu() {
        menu.open(window.bindings());
        bindings_dirty = false;
        rearm_escape = false;
        // While the screen is up, escape backs out of it rather than ending the run.
        window.set_escape_quits(false);
    }

    void close_menu() {
        menu.close();
        window.cancel_capture();
        rearm_escape = true;  // not until escape is released; see pump_menu()
        if (!bindings_dirty)
            return;
        bindings_dirty = false;
        if (bindings_path.empty()) {
            std::printf("bindings changed for this run only: no file to save them in\n");
            return;
        }
        if (save_bindings())
            std::printf("bindings saved to %s\n", bindings_path.c_str());
        else
            std::fprintf(stderr, "cannot write the bindings to %s\n", bindings_path.c_str());
    }

    // Applied live, so a rebound control works the moment it is bound rather than after a restart.
    // The file waits until the screen closes: a dead zone nudged five times is one save, not five.
    void apply_bindings() {
        if (!menu.dirty())
            return;
        menu.mark_saved();
        bindings_dirty = true;
        window.set_bindings(menu.bindings());
    }

    // A missing file is the ordinary first run, not a fault: the defaults are the layout the
    // launcher always had, so nothing needs to exist for the keyboard to work.
    void load_bindings() {
        dream::render::Bindings b = dream::render::Bindings::defaults();
        std::ifstream in(bindings_path, std::ios::binary);
        if (!bindings_path.empty() && in) {
            const std::string text((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            std::string warnings;
            b = dream::render::Bindings::from_text(text, &warnings);
            if (!warnings.empty())
                std::fprintf(stderr, "%s:\n%s", bindings_path.c_str(), warnings.c_str());
            std::printf("bindings: %s\n", bindings_path.c_str());
        }
        window.set_bindings(b);
    }

    bool save_bindings() const {
        std::ofstream out(bindings_path, std::ios::binary);
        if (!out)
            return false;
        const std::string text = menu.bindings().to_text();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return static_cast<bool>(out);
    }

    // Averaged over a window rather than taken frame to frame: an instantaneous figure on a
    // 60 Hz display flickers between two integers and is unreadable, which is the usual reason an
    // on-screen counter gets turned off again. Half a second is long enough to settle and short
    // enough to react to a stutter.
    void update_counter(std::uint64_t guest_cycles) {
        const auto now = std::chrono::steady_clock::now();
        const double guest_s = static_cast<double>(guest_cycles) / 200e6;
        if (fps_mark == std::chrono::steady_clock::time_point{}) {
            fps_mark = now;
            fps_frames_at_mark = presented;
            fps_guest_at_mark = guest_s;
            return;
        }
        const double elapsed = std::chrono::duration<double>(now - fps_mark).count();
        if (elapsed < 0.5)
            return;
        fps_shown = static_cast<double>(presented - fps_frames_at_mark) / elapsed;
        speed_shown = (guest_s - fps_guest_at_mark) / elapsed;
        fps_mark = now;
        fps_frames_at_mark = presented;
        fps_guest_at_mark = guest_s;
    }

    // Drawn into the frame the presenter is about to upload, so its size follows --scale and it
    // stays the same size relative to the picture however the window is resized.
    void draw_counter(std::uint32_t* px, std::uint32_t w, std::uint32_t h) {
        char line[64];
        std::snprintf(line, sizeof line, "%.1f FPS  %.2fX", fps_shown, speed_shown);
        dream::render::TextStyle style;
        // One text pixel per two guest pixels at --scale 1, and the same apparent size at every
        // scale above it, so the counter neither disappears nor swamps the picture.
        style.scale = std::max(2u, 2u * (w / kGuestWidth));
        const int margin = static_cast<int>(4 * style.scale);
        dream::render::draw_text(px, w, w, h, margin, margin, line, style);
    }

    void screenshot() {
        char name[64];
        std::snprintf(name, sizeof name, "screenshot-%03u.ppm", screenshots++);
        screenshot_to(name);
        // DREAM_SHOT_INTERP=1 (dreamcomp): also the interpolated frame drawn before this one.
        if (interpolate && interp_rendered && std::getenv("DREAM_SHOT_INTERP")) {
            if (const std::uint32_t* src = interp_off.pixels()) {
                std::snprintf(name, sizeof name, "screenshot-%03u-interp.ppm", screenshots - 1);
                if (FILE* f = std::fopen(name, "wb")) {
                    std::fprintf(f, "P6\n%u %u\n255\n", interp_off.width(), interp_off.height());
                    const std::size_t n =
                        static_cast<std::size_t>(interp_off.width()) * interp_off.height();
                    for (std::size_t i = 0; i < n; ++i) {
                        const unsigned char rgb[3] = {static_cast<unsigned char>(src[i]),
                                                      static_cast<unsigned char>(src[i] >> 8),
                                                      static_cast<unsigned char>(src[i] >> 16)};
                        std::fwrite(rgb, 1, 3, f);
                    }
                    std::fclose(f);
                }
            }
        }
        std::printf("wrote %s: presented frame %llu, guest frame %llu\n", name,
                    static_cast<unsigned long long>(presented),
                    static_cast<unsigned long long>(guest_frame));
    }

    void screenshot_to(const std::string& name) {
        // --screenshot-presented: what is actually on the screen, through the presenter, with the
        // overlays and the resolve. The default below is the game's own pixels instead, which is
        // what makes one screenshot comparable with another; this is for looking at the display
        // path itself, which nothing else can see.
        if (shot_presented) {
            std::vector<std::uint8_t> px;
            std::uint32_t w = 0, h = 0;
            if (window.read_pixels(px, w, h)) {
                if (FILE* pf = std::fopen(name.c_str(), "wb")) {
                    std::fprintf(pf, "P6\n%u %u\n255\n", w, h);
                    std::fwrite(px.data(), 1, px.size(), pf);
                    std::fclose(pf);
                }
                return;
            }
            std::fprintf(stderr, "could not read the presented image back\n");
            return;
        }
        FILE* f = std::fopen(name.c_str(), "wb");
        if (!f)
            return;
        std::fprintf(f, "P6\n%u %u\n255\n", shown.width, shown.height);
        const std::uint32_t* src = from_renderer ? offscreen.pixels() : pixels.data();
        const std::size_t n = static_cast<std::size_t>(shown.width) * shown.height;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t p = src[i];
            const unsigned char rgb[3] = {static_cast<unsigned char>(p),
                                          static_cast<unsigned char>(p >> 8),
                                          static_cast<unsigned char>(p >> 16)};
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
    }

    // Everything needed to reproduce what is on screen right now, in one keypress: the picture, the
    // display list that drew it, video memory and the PVR registers from the same instant, and the
    // numbers that say where the run had got to. A report that comes with one of these can be
    // rendered again offline with dream_render_view; a report without one is a description of a
    // picture nobody else can see.
    void capture() {
        char stem[64];
        std::snprintf(stem, sizeof stem, "capture-%03u", captures++);
        screenshot_to(std::string(stem) + ".ppm");
        if (!last_stream.empty()) {
            if (FILE* f = std::fopen((std::string(stem) + ".ta").c_str(), "wb")) {
                std::fwrite(last_stream.data(), 4, last_stream.size(), f);
                std::fclose(f);
            }
        }
        if (FILE* f = std::fopen((std::string(stem) + ".vram").c_str(), "wb")) {
            std::fwrite(memory_.vram(), 1, dream::mem::DcMemory::kVramSize, f);
            std::fclose(f);
        }
        if (FILE* f = std::fopen((std::string(stem) + ".vram.regs").c_str(), "wb")) {
            std::fwrite(pvr_.reg_block(), 4, 0x2000 / 4, f);
            std::fclose(f);
        }
        if (FILE* f = std::fopen((std::string(stem) + ".txt").c_str(), "w")) {
            std::fprintf(f, "guest frame %llu, presented frame %llu\n",
                         static_cast<unsigned long long>(guest_frame),
                         static_cast<unsigned long long>(presented));
            std::fprintf(f, "renders %llu, backgrounds %llu, written back %llu\n",
                         static_cast<unsigned long long>(rendered),
                         static_cast<unsigned long long>(backgrounds),
                         static_cast<unsigned long long>(written_back));
            std::fprintf(f, "textures: %u decoded, %u failed, %u overwritten\n",
                         renderer.textures().decoded, renderer.textures().failed,
                         renderer.textures().overwritten);
            std::fprintf(f, "display: %s\n", shown.describe().c_str());
            std::fprintf(f, "source: %s\n", from_renderer ? "the renderer" : "video memory");
            std::fprintf(f, "parameter words: %zu\n", last_stream.size());
            std::fclose(f);
        }
        std::printf("captured %s.{ppm,ta,vram,vram.regs,txt} at guest frame %llu\n", stem,
                    static_cast<unsigned long long>(guest_frame));
    }

    dream::render::PaletteFormat palette_format() const {
        return static_cast<dream::render::PaletteFormat>(pvr_.reg(0x108) & 3u);
    }

    static unsigned bytes_per_written_pixel(const dream::render::FramebufferInfo& info) {
        switch (info.format) {
            case dream::render::FramebufferFormat::Rgb888:
                return 3;
            case dream::render::FramebufferFormat::Argb8888:
                return 4;
            default:
                return 2;
        }
    }

    static constexpr std::uint32_t kGuestWidth = 640, kGuestHeight = 480;

    dream::render::vk::Window window;
    dream::render::vk::Renderer renderer;
    dream::render::vk::Offscreen offscreen;
    // Geometry interpolation (dreamcomp, docs/INTERPOLATION.md): --interpolate.
    bool interpolate = false, pace_interpolation = true, have_prev = false, interp_ready = false;
    unsigned scale_ = 1;
    float render_aspect_ = 0.0f, want_render_aspect = 0.0f;
    unsigned want_scale = 0;  // dreamcomp: a scale asked for at run time, 0: none
    bool await_render = false;
    bool interpolate_only_above_60 = false;
    dream::render::vk::Offscreen interp_off;
    dream::render::Frame prev_frame, interp_frame;
    std::vector<dream::render::StripPair> interp_pairs;
    dream::render::BlendOptions interp_opts;
    std::uint64_t interp_rendered = 0, interp_presented = 0, interp_cuts = 0, interp_jumped = 0,
                  interp_vertices = 0, interp_matched = 0;
    double interp_ms = 0.0;
    std::uint64_t vram_invalidated = 0;  // textures dropped because the guest rewrote them
    std::chrono::steady_clock::time_point last_present{};
    dream::render::vk::Presenter presenter;
    dream::render::vk::Presenter overlay_presenter;  // dreamcomp: the in-game menu layer
    std::uint32_t overlay_w = 0, overlay_h = 0;
    bool overlay_dirty = true;
    dream::render::DisplayList decoder;
    dream::render::vk::FrameGeometry geometry;
    dream::render::FramebufferInfo shown;
    std::vector<std::uint32_t> pixels;
    std::string error;
    std::uint64_t rendered = 0, written_back = 0, presented = 0, palette_changes = 0,
                  decoded_frames = 0, backgrounds = 0;
    std::uint32_t target_address = 0;
    std::uint64_t guest_frame = 0;
    std::vector<std::uint32_t> last_stream;
    unsigned screenshots = 0, captures = 0;
    // The on-screen counter. Two numbers, because they answer different questions: how smooth it
    // looks (frames actually presented per second of wall clock) and whether it is keeping up
    // (guest time elapsed per second of wall clock, where 1.00x is the console's own pace).
    bool show_fps = false;
    bool input_polled = false, window_open = true;  // poll_input() (dreamcomp)
    DisplaySync sync;                                // --sync-display (dreamcomp)
    // The binding screen (F1, or a pad's select button). The guest is stopped while it is up, so
    // nobody rebinds a control mid-corner.
    dream::render::InputMenu menu;
    // Extensions drawing an in-game overlay (host_ext.h, dreamcomp). While one is open the guest
    // is paused exactly as for the binding screen, and frames take the host-copy path so the
    // overlay can be composited.
    std::vector<dream::host::Extension*> overlays;
    std::vector<std::uint32_t> overlay_frame;  // the frame an overlay is composited into
    bool overlay_open() const {
        for (auto* e : overlays)
            if (e->overlay_open())
                return true;
        return false;
    }
    // Where a change is written back. Empty means this run only: --bindings was pointed nowhere
    // usable, or SDL could not name a settings directory on this host.
    std::string bindings_path;
    bool bindings_dirty = false;  // changed since the screen was opened
    // Escape closes the screen and also quits: quit stays disarmed until the key comes back up.
    bool rearm_escape = false;
    // --screenshot-at N / --capture-at N: the same thing F12 and F11 do, at the Nth presented
    // frame, for a run with nobody at the keyboard. These were environment variables, which is
    // fine for a one-off and wrong for something the documentation tells people to use.
    std::set<std::uint64_t> screenshot_at;  // --screenshot-at N[,N...] (list: dreamcomp)
    std::uint64_t capture_at = 0;
    std::chrono::steady_clock::time_point fps_mark{};
    std::uint64_t fps_frames_at_mark = 0;
    double fps_guest_at_mark = 0;
    double fps_shown = 0, speed_shown = 0;
    bool writeback = false;       // --framebuffer-writeback
    bool shot_presented = false;  // --screenshot-presented
    bool have_frame = false, from_renderer = false, reported_error = false;

private:
    dream::mem::DcMemory& memory_;
    const dream::pvr::Core& pvr_;
    std::vector<std::uint32_t> targets_;  // every buffer the renderer has drawn into
};
#endif  // DREAM_WITH_RENDERER

// The serial port Katana's debug output goes to (SCIF, 0xFFE80000): transmitted bytes are kept for
// the report; status reads say the FIFO is empty so polling senders never wait.
struct SerialCapture final : dream::mem::MmioHandler {
    std::string transmitted;
    std::uint32_t read(std::uint32_t addr, unsigned) override {
        switch (addr & 0xFFu) {
            case 0x10:
                return 0x0060u;  // SCFSR2: TEND | TDFE
            default:
                return 0;
        }
    }
    void write(std::uint32_t addr, std::uint32_t value, unsigned) override {
        if ((addr & 0xFFu) == 0x0C)
            transmitted.push_back(static_cast<char>(value & 0xFFu));
    }
};

std::string report_text(dream::System& sys, dream::hle::Bios& bios, const dream::pvr::Core& pvr,
                        const char* stop, double host_seconds, const std::vector<char>& image,
                        std::uint32_t image_base, const dream::maple::Bus& maple,
                        const dream::maple::Controller* pad_ptr,
                        const dream::maple::MemoryCard* card_ptr, const dream::aica::Aica& aica,
                        const dream::holly::G2Dma& g2, const dream::holly::SysBlock& sb,
                        const dream::sh4::Dmac& dmac) {
    std::string s;
    char buf[512];
    const double guest_s = static_cast<double>(sys.ctx.cycles) / 200e6;
    std::snprintf(buf, sizeof buf,
                  "stop: %s\nguest: %.3f s (%llu cycles), %llu frames, pc 0x%08x\nhost: %.2f s "
                  "(%.1fx real time)\n",
                  stop, guest_s, static_cast<unsigned long long>(sys.ctx.cycles),
                  static_cast<unsigned long long>(sys.spg.frames()), sys.ctx.pc, host_seconds,
                  host_seconds > 0 ? guest_s / host_seconds : 0.0);
    s += buf;
    if (g_game_thread_cycles) {
        std::snprintf(buf, sizeof buf, "game thread: %.3f G CPU cycles\n",
                      static_cast<double>(g_game_thread_cycles) * 1e-9);
        s += buf;
    }
    std::snprintf(buf, sizeof buf, "interrupts delivered: %llu (max nesting %u), traps: %llu\n",
                  static_cast<unsigned long long>(sys.interrupts_delivered), sys.max_nesting,
                  static_cast<unsigned long long>(sys.traps_taken));
    s += buf;
    std::snprintf(buf, sizeof buf, "task-switching RTEs: %llu\n",
                  static_cast<unsigned long long>(sys.task_switch_rtes));
    s += buf;
    for (const auto& sw : sys.first_switches) {
        std::snprintf(buf, sizeof buf, "  rte from pc 0x%08x sp 0x%08x -> pc 0x%08x sp 0x%08x\n",
                      sw[0], sw[1], sw[2], sw[3]);
        s += buf;
    }
    s += "syscalls:";
    for (const auto& [k, v] : bios.counts) {
        std::snprintf(buf, sizeof buf, " %s=%llu", k.c_str(), static_cast<unsigned long long>(v));
        s += buf;
    }
    std::snprintf(buf, sizeof buf, "\nGD-ROM sectors read: %llu\n",
                  static_cast<unsigned long long>(bios.sectors_read));
    s += buf;
    std::snprintf(
        buf, sizeof buf,
        "PVR: %llu TA chunks, %llu list inits, %llu renders (%llu done), %llu frame swaps, "
        "%llu texture words, %llu yuv words\n",
        static_cast<unsigned long long>(pvr.ta.chunks),
        static_cast<unsigned long long>(pvr.list_inits),
        static_cast<unsigned long long>(pvr.renders),
        static_cast<unsigned long long>(pvr.renders_done),
        static_cast<unsigned long long>(pvr.frame_swaps),
        static_cast<unsigned long long>(pvr.texture_words),
        static_cast<unsigned long long>(pvr.yuv_words));
    s += buf;
    static const char* names[5] = {"opaque", "opaque-mod", "translucent", "translucent-mod",
                                   "punch-through"};
    for (unsigned i = 0; i < 5; ++i) {
        const auto& l = pvr.ta.lists[i];
        if (l.chunks == 0 && l.ends == 0)
            continue;
        std::snprintf(buf, sizeof buf,
                      "  %s: %u polygons, %u sprites, %u modvols, %u vertices, %u ends\n", names[i],
                      l.polygons, l.sprites, l.modvols, l.vertices, l.ends);
        s += buf;
    }
    if (card_ptr) {
        std::snprintf(buf, sizeof buf,
                      "memory card: %llu block reads, %llu block writes, %llu media-info reads, "
                      "%llu screen writes\n",
                      static_cast<unsigned long long>(card_ptr->block_reads),
                      static_cast<unsigned long long>(card_ptr->block_writes),
                      static_cast<unsigned long long>(card_ptr->media_info_reads),
                      static_cast<unsigned long long>(card_ptr->screen_writes));
        s += buf;
    }
    {
        s += "maple frames by recipient:";
        for (unsigned i = 0; i < 256; ++i)
            if (maple.frames_by_recipient[i]) {
                std::snprintf(buf, sizeof buf, " %02x:%llu", i,
                              static_cast<unsigned long long>(maple.frames_by_recipient[i]));
                s += buf;
            }
        s += "\nmaple frames by command:";
        for (unsigned i = 0; i < 256; ++i)
            if (maple.frames_by_command[i]) {
                std::snprintf(buf, sizeof buf, " %02x:%llu", i,
                              static_cast<unsigned long long>(maple.frames_by_command[i]));
                s += buf;
            }
        s += "\n";
    }
    std::snprintf(buf, sizeof buf,
                  "maple: %llu transfers, %llu frames, %llu to empty ports, %llu pad polls\n",
                  static_cast<unsigned long long>(maple.transfers),
                  static_cast<unsigned long long>(maple.frames),
                  static_cast<unsigned long long>(maple.no_device),
                  static_cast<unsigned long long>(pad_ptr->condition_reads));
    s += buf;
    std::snprintf(buf, sizeof buf,
                  "ch2 DMA: %llu transfers (%llu TA bytes, %llu texture bytes, %llu refused), "
                  "%llu sort-DMA starts, %llu manual DMAC transfers\n",
                  static_cast<unsigned long long>(sb.ch2_transfers),
                  static_cast<unsigned long long>(sb.ch2_ta_bytes),
                  static_cast<unsigned long long>(sb.ch2_texture_bytes),
                  static_cast<unsigned long long>(sb.ch2_errors),
                  static_cast<unsigned long long>(sb.sort_dma_starts),
                  static_cast<unsigned long long>(dmac.manual_transfers));
    s += buf;
    if (sb.ch2_errors) {
        for (const auto* r : {&sb.first_refused, &sb.last_refused}) {
            std::snprintf(buf, sizeof buf,
                          "  %s refused: DMAOR 0x%08x SAR2 0x%08x C2DSTAT 0x%08x C2DLEN 0x%x\n",
                          r == &sb.first_refused ? "first" : "last", r->dmaor, r->src, r->dst,
                          r->len);
            s += buf;
        }
    }
    // Image integrity: code or data of the executable rewritten at run time. Katana patches a few
    // pool words; anything larger is a stray write worth chasing.
    {
        const std::uint8_t* ram = sys.memory.ram();
        const std::uint32_t off = image_base & (dream::mem::DcMemory::kRamSize - 1);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
        for (std::size_t i = 0; i < image.size(); ++i) {
            if (ram[off + i] == static_cast<std::uint8_t>(image[i]))
                continue;
            if (!ranges.empty() && ranges.back().second + 8 >= i)
                ranges.back().second = static_cast<std::uint32_t>(i);
            else
                ranges.push_back({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i)});
        }
        std::snprintf(buf, sizeof buf, "image bytes changed in RAM: %zu range(s)\n", ranges.size());
        s += buf;
        for (std::size_t k = 0; k < ranges.size() && k < 24; ++k) {
            std::snprintf(buf, sizeof buf, "  0x%08x..0x%08x (%u bytes)\n",
                          image_base + ranges[k].first, image_base + ranges[k].second + 1,
                          ranges[k].second - ranges[k].first + 1);
            s += buf;
        }
    }
    std::snprintf(
        buf, sizeof buf, "G2 DMA: %llu transfers (%llu bytes, %llu to sound RAM), %llu refused\n",
        static_cast<unsigned long long>(g2.transfers), static_cast<unsigned long long>(g2.bytes),
        static_cast<unsigned long long>(g2.to_aica_ram),
        static_cast<unsigned long long>(g2.refused));
    s += buf;
    std::snprintf(
        buf, sizeof buf,
        "AICA: arm7 %s at pc 0x%08x, %llu samples, %llu ARM instructions, %llu FIQs, "
        "%llu SWIs, %llu undefined, %llu timer irqs, %llu SH-4 irq raises, %llu channel "
        "writes, %llu dsp writes (%llu to its work registers); mixer: %llu key-ons, %u channels active, %llu of %llu "
        "samples non-zero, dsp %s; SCPU sh4->arm %llu, arm->sh4 %llu; reg writes sh4 %llu "
        "arm %llu\n",
        aica.arm.enabled() ? "running" : "held", aica.arm.next_pc(),
        static_cast<unsigned long long>(aica.samples),
        static_cast<unsigned long long>(aica.arm.instructions),
        static_cast<unsigned long long>(aica.arm.fiqs),
        static_cast<unsigned long long>(aica.arm.swis),
        static_cast<unsigned long long>(aica.arm.undefined_ops),
        static_cast<unsigned long long>(aica.timer_irqs),
        static_cast<unsigned long long>(aica.sh4_irq_raises),
        static_cast<unsigned long long>(aica.channel_writes),
        static_cast<unsigned long long>(aica.dsp_writes),
        static_cast<unsigned long long>(aica.mixer.dsp_state_writes),
        static_cast<unsigned long long>(aica.mixer.key_ons), aica.mixer.active_channels(),
        static_cast<unsigned long long>(aica.mixer.nonzero_samples),
        static_cast<unsigned long long>(aica.mixer.samples),
        aica.mixer.dsp_running() ? "running" : "stopped",
        static_cast<unsigned long long>(aica.scpu_to_arm),
        static_cast<unsigned long long>(aica.scpu_to_sh4),
        static_cast<unsigned long long>(aica.sh4_side_reg_writes),
        static_cast<unsigned long long>(aica.arm_side_reg_writes));
    s += buf;
    std::snprintf(
        buf, sizeof buf,
        "holly: ISTNRM %08x ISTEXT %08x IML2NRM %08x IML4NRM %08x IML6NRM %08x IML2EXT %08x "
        "IML4EXT %08x IML6EXT %08x; GD-ROM DMA irq %s\n",
        sys.memory.read32(0xA05F6900u), sys.memory.read32(0xA05F6904u),
        sys.memory.read32(0xA05F6910u), sys.memory.read32(0xA05F6920u),
        sys.memory.read32(0xA05F6930u), sys.memory.read32(0xA05F6914u),
        sys.memory.read32(0xA05F6924u), sys.memory.read32(0xA05F6934u),
        sys.holly.raised(dream::holly::Irq::GdromDma) ? "raised" : "clear");
    s += buf;
    std::snprintf(buf, sizeof buf, "untranslated call targets: %llu\n",
                  static_cast<unsigned long long>(sys.untranslated.total()));
    s += buf;
    s += sys.untranslated.format();
    for (const auto& [target, from] : sys.untranslated_sites) {
        std::snprintf(buf, sizeof buf, "  0x%08x called from 0x%08x\n", target, from);
        s += buf;
    }
    // For each untranslated target, look for the code the game placed there inside the executable
    // image: a match means a run-time copy the TOML's [[relocations]] should describe.
    for (const auto& rec : sys.untranslated.records()) {
        const std::uint32_t phys = rec.addr & 0x1FFFFFFFu;
        if ((phys & 0x1C000000u) != 0x0C000000u)
            continue;
        const std::uint8_t* ram = sys.memory.ram() + (phys & (dream::mem::DcMemory::kRamSize - 1));
        std::size_t best_len = 0, best_off = 0;
        for (std::size_t off = 0; off + 32 <= image.size(); off += 2) {
            if (std::memcmp(image.data() + off, ram, 32) != 0)
                continue;
            std::size_t len = 32;
            while (off + len < image.size() &&
                   ram + len < sys.memory.ram() + dream::mem::DcMemory::kRamSize &&
                   image[off + len] == static_cast<char>(ram[len]))
                ++len;
            if (len > best_len) {
                best_len = len;
                best_off = off;
            }
        }
        if (best_len) {
            // Matching the image at the address it already lives at is the code matching itself,
            // not a copy of it. Saying "relocation source = X, dest = X" invites an entry that
            // describes a move that never happened; what this address actually wants is a
            // [functions] extra seed, which suggest_entries writes.
            const auto found_at = static_cast<std::uint32_t>(image_base + best_off);
            if (found_at == rec.addr)
                std::snprintf(buf, sizeof buf,
                              "  0x%08x: in the image, %zu bytes of it, but discovery never "
                              "reached it (add to [functions] extra)\n",
                              rec.addr, best_len);
            else
                std::snprintf(buf, sizeof buf,
                              "  0x%08x: bytes match the image at 0x%08x for %zu bytes (relocation "
                              "source = 0x%08x, dest = 0x%08x)\n",
                              rec.addr, found_at, best_len, found_at, rec.addr);
            s += buf;
        }
    }
    std::snprintf(buf, sizeof buf, "unmapped memory accesses: %llu\n",
                  static_cast<unsigned long long>(sys.memory.faults().total()));
    s += buf;
    s += sys.memory.faults().format();
    return s;
}

// The config lines this run earned: every call target that had no translation and lies inside the
// executable's own range. Those are not relocations and not run-time-generated code; they are
// addresses discovery simply did not reach, and the TOML's [functions] extra list is how you tell
// it to start there.
//
// This closes the loop the tool otherwise leaves open. Everything needed was already in the report
// and the only way to act on it was to read an address off the screen, hand-edit the TOML and
// rebuild, once per address, which is most of what bringing a new title up consists of.
//
// A target whose bytes appear elsewhere in the image is a copy and belongs in [[relocations]], so
// it is left to suggest_relocations. A target at the same address it was copied from is not a copy
// at all, however much it looks like one: the match is the code matching itself.
std::string suggest_entries(dream::System& sys, std::uint32_t image_base, std::size_t image_size,
                            const std::vector<std::uint32_t>& known) {
    std::vector<std::pair<std::uint32_t, std::uint64_t>> misses;
    for (const auto& rec : sys.untranslated.records()) {
        const std::uint32_t link = (rec.addr & 0x1FFFFFFFu) | 0x0C000000u;
        if (link < image_base || link >= image_base + image_size)
            continue;
        // Already a seed and still untranslated means something else is wrong, and repeating it
        // here would send the reader round the same loop a second time.
        bool already = false;
        for (std::uint32_t e : known)
            already = already || ((e & 0x1FFFFFFFu) | 0x0C000000u) == link;
        if (!already)
            misses.push_back({link, rec.count});
    }
    if (misses.empty())
        return {};
    std::sort(misses.begin(), misses.end());
    std::string s =
        "# Call targets this run reached that the translator had not found. Paste into the game's\n"
        "# TOML, rebuild, and run again: each one seeds discovery, which usually finds more than\n"
        "# the one function. Addresses are in link space, which is what [functions] expects.\n"
        "[functions]\n";
    char buf[160];
    for (std::size_t i = 0; i < misses.size(); ++i) {
        const char* lead = i == 0 ? "extra = [" : "         ";
        const char* tail = i + 1 == misses.size() ? "]" : ",";
        std::snprintf(buf, sizeof buf, "%s0x%08X%s  # called %llu time%s\n", lead, misses[i].first,
                      tail, static_cast<unsigned long long>(misses[i].second),
                      misses[i].second == 1 ? "" : "s");
        s += buf;
    }
    return s;
}

// Runs of RAM below the load address that reproduce image bytes are code the program copied at run
// time (Katana's stub moves its interrupt trampoline and vector handlers there). Each run is a
// [[relocations]] entry for the TOML; the translator then emits a unit for it.
std::string suggest_relocations(dream::System& sys, const std::vector<char>& image,
                                std::uint32_t image_base, std::uint32_t load_address,
                                const std::vector<std::uint8_t>& ipbin,
                                const std::vector<dream::translator::Relocation>& known) {
    std::string s;
    const std::uint8_t* ram = sys.memory.ram();
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>>
        index;  // 16-byte window -> image offsets
    auto key = [](const void* p) {
        std::uint64_t a, b;
        std::memcpy(&a, p, 8);
        std::memcpy(&b, static_cast<const char*>(p) + 8, 8);
        return a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull);
    };
    for (std::size_t off = 0; off + 16 <= image.size(); off += 4) {
        // Skip uniform windows (zero fill, 0xFF fill, repeated words): they match everywhere.
        bool uniform = true;
        for (std::size_t k = 4; k < 16 && uniform; k += 4)
            uniform = std::memcmp(image.data() + off, image.data() + off + k, 4) == 0;
        if (uniform)
            continue;
        index[key(image.data() + off)].push_back(static_cast<std::uint32_t>(off));
    }
    const std::uint32_t lo = 0, hi = std::min<std::uint32_t>(load_address & 0xFFFFFFu, 0x10000u);
    std::uint32_t a = lo;
    char buf[512];
    while (a + 16 <= hi) {
        auto it = index.find(key(ram + a));
        std::size_t best_len = 0, best_off = 0;
        if (it != index.end()) {
            for (std::uint32_t off : it->second) {
                std::size_t len = 0;
                while (a + len < hi && off + len < image.size() &&
                       ram[a + len] == static_cast<std::uint8_t>(image[off + len]))
                    ++len;
                if (len > best_len) {
                    best_len = len;
                    best_off = off;
                }
            }
        }
        // IP.BIN was loaded from the disc into 0x8C008000..; bytes still equal to that load are not
        // copies the program made.
        const bool still_ipbin = a >= 0x8000 && a + best_len <= 0x8000 + ipbin.size() && best_len &&
                                 std::memcmp(ram + a, ipbin.data() + (a - 0x8000), best_len) == 0;
        // A region the config already describes is one the translator already handled: repeating
        // it would have the reader paste a duplicate entry, which is worse than saying nothing.
        // Containment rather than equality, because the run sees the copy's real extent while the
        // config may describe a larger region it sits inside.
        const std::uint32_t dest_addr = 0x8C000000u + a;
        bool described = false;
        for (const auto& k : known)
            described = described || (dest_addr >= k.dest && dest_addr < k.dest + k.size);
        if (best_len >= 48 && !still_ipbin && !described) {
            std::snprintf(buf, sizeof buf,
                          "  [[relocations]]  # RAM 0x%08x reproduces image 0x%08x for %zu bytes\n "
                          " source = 0x%08X\n  size = 0x%zX\n  dest = 0x%08X\n",
                          0x8C000000u + a, static_cast<std::uint32_t>(image_base + best_off),
                          best_len, static_cast<std::uint32_t>(image_base + best_off),
                          (best_len + 3) & ~std::size_t{3}, 0x8C000000u + a);
            s += buf;
            a += static_cast<std::uint32_t>(best_len & ~std::size_t{3});
        } else {
            a += 4;
        }
    }
    return s;
}

}  // namespace

// Every flag, grouped by what someone is trying to do. The old usage line named eight of them and
// the other twenty were findable only by reading this file, which is how a capture flag that
// already existed went unused for a week.
void usage(const char* argv0, std::FILE* out) {
    std::fprintf(
        out,
        "usage: %s --config GAME.toml [options]\n"
        "\n"
        "Runs a recompiled title. Without --window it runs headless and prints a report, which is\n"
        "usually the faster way to find out what happened.\n"
        "\n"
        "Required:\n"
        "  --config FILE          the game's TOML (games/<id>/<id>.toml)\n"
        "\n"
        "Playing:\n"
        "  --window               open a window and play; implies sound\n"
        "  --scale N              draw at N times the guest's 640x480 (1 to 8, default 1)\n"
        "  --render-aspect A      render target shape for anamorphic widescreen (e.g. 1.7778)\n"
        "  --fullscreen           start fullscreen (Alt+Enter toggles)\n"
        "  --interpolate          draw a blended frame between two game frames (needs a display\n"
        "                         above 60 Hz to be seen; docs/INTERPOLATION.md in dreamcomp)\n"
        "  --interpolate-auto     the same, only when the window's display is above 60 Hz\n"
        "  --sync-display         pace at the display's rate when it is within 0.5%% of the\n"
        "                         game's (60 Hz for a 59.94 Hz game: +0.1%% speed, no repeats)\n"
        "  --fps                  start with the on-screen frame-rate counter showing\n"
        "  --present-path P       vulkan (default) or dxgi (Windows: flip model, lower latency)\n"
        "  --present-mode M       vsync (default), mailbox or immediate. The default paces the\n"
        "                         whole run to the panel, so --unthrottled with a window measures\n"
        "                         the refresh rate rather than the emulator.\n"
        "  --bindings FILE        controller bindings; the default is one file per user, shared\n"
        "                         by every title. F1 (or a pad's select button) opens the screen\n"
        "                         that edits them, and writes them back here.\n"
        "  --disc FILE            the disc image (.gdi, .cue or .chd) instead of the config's\n"
        "                         [disc] image. Without an extracted [binary] path the boot\n"
        "                         file is read off this disc and checked against\n"
        "                         [disc] sha1_1st_read.\n"
        "  --vmu FILE             a 128 KB memory-card image; writes are saved back to it.\n"
        "  --mod DIR              serve DIR's files in place of the disc's (repeatable; the first\n"
        "                         --mod providing a file wins; dreamcomp docs/MODS.md)\n"
        "  --rumble N             controller rumble strength, 0 (off) to 100 percent (default)\n"
        "  --volume N             master volume, 0 to 100 percent (default 100)\n"
        "  --soft-clip            round loud peaks off instead of the console's hard clipping\n"
        "  --texture-pack DIR     replace textures with DIR/**/*_<hash>.png (with --window;\n"
        "                         dreamcomp docs/TEXTURE-PACKS.md)\n"
        "  --dump-textures DIR    write each distinct texture once as DIR/<w>x<h>_<fmt>_<hash>.png\n"
        "  --flash FILE           a 128 KB console flash image; without it one is synthesised\n"
        "                         Created, blank and formatted, if the path does not exist. A "
        "file\n"
        "                         that is not a card is refused, never overwritten.\n"
        "  --no-create-vmu        fail instead of creating a missing card\n"
        "  --unthrottled          run as fast as the host can rather than at the guest's clock\n"
        "  --no-audio / --audio   force sound off, or on for a headless run\n"
        "\n"
        "In the window: arrow keys are the d-pad, Z X A S are A B X Y, return is start, Q and W\n"
        "are the triggers, F10 toggles the frame-rate counter, F11 captures the frame, F12 writes\n"
        "a screenshot, escape quits.\n"
        "\n"
        "How long to run:\n"
        "  --max-frames N         stop after N guest frames. Without it, and without\n"
        "                         --max-seconds or --stop-on-ta, the run continues until you quit\n"
        "                         or it faults.\n"
        "  --max-seconds S        stop after S seconds of guest time\n"
        "  --stop-on-ta           stop at the first display-list write\n"
        "\n"
        "Reproducing a run:\n"
        "  --rtc-seed N           fix the console clock so two runs do the same thing\n"
        "  --record-input FILE    write player 1's controller, every vblank, to FILE\n"
        "  --play-input FILE      replay a recording (then the controller is live again)\n"
        "  --press B@F[,B@F...]   hold a button for 8 frames from frame F (a b c x y z start\n"
        "                         up down left right), to get past screens that wait for input\n"
        "\n"
        "Reporting what happened:\n"
        "  --report FILE          write the end-of-run report to a file as well as stdout\n"
        "  --suggest-config FILE  write the TOML this run earned: the call targets discovery\n"
        "                         never reached, and the regions the program copied and ran\n"
        "                         elsewhere. Paste into the game's config and rebuild.\n"
        "  --dump-ta FILE         capture a render's display list; without --dump-ta-frame or\n"
        "                         --dump-ta-at-frame it writes the first 64 as FILE.000, .001, "
        "...\n"
        "  --dump-ta-frame N      capture the Nth render only\n"
        "  --dump-ta-at-frame N   capture the first render at or after guest frame N\n"
        "  --dump-ta-count K      with --dump-ta-at-frame: K consecutive renders, FILE.000...\n"
        "  --dump-vram FILE       video memory and the PVR registers from the same instant\n"
        "  --dump-aram FILE       the 2 MB of sound RAM at the stop\n"
        "  --dump FILE            a memory range at the stop\n"
        "  --screenshot-at N[,N]  write a screenshot at each listed presented frame (F12)\n"
        "  --capture-at N         write a full frame capture at the Nth (F11 by hand)\n"
        "  --wav FILE             record the audio as 16-bit stereo 44.1 kHz\n"
        "  --sample N             sample the CPU every N cycles (--sample-file FILE)\n"
        "  --profile FILE         sample the game thread's host call stacks (~1 kHz) into FILE;\n"
        "                         tools/profile.py names them (--profile-interval US)\n"
        "\n"
        "Finding where two runs diverged (docs/differential-harness.md):\n"
        "  --write-hash FILE      a rolling hash of every guest write, one line per frame\n"
        "  --write-log FILE       every store in a window of the write sequence, with call sites\n"
        "  --write-log-range F:N  which window: N stores from the Fth\n"
        "  --mask-segment         ignore pointer segment bits when hashing and logging\n"
        "\n"
        "Development build only:\n"
        "  --interpret            run everything through the interpreter\n"
        "  --replay               check each translated call against the interpreter\n"
        "  --replay-self-check    check the replay harness against itself\n"
        "  --replay-only ADDR     restrict the replay check to one function\n"
        "  --validation           turn on Vulkan validation layers\n"
        "  --framebuffer-writeback  write rendered frames back into video memory\n",
        argv0);
    for (auto* e : dream::host::extensions()) e->usage(out);
}

// DREAM_PROFILE: host time spent drawing renders (dreamcomp).
static std::uint64_t g_render_ns = 0, g_renders = 0;

// Crash reports (dreamcomp): defined at the end of this file, away from <windows.h>'s macros.
void install_crash_handler();
// Sleeps until `t` within a fraction of a millisecond (dreamcomp; Windows: a high-resolution
// waitable timer, where std::this_thread::sleep_until wakes 1-3 ms late). Defined at the end.
void precise_sleep_until(std::chrono::steady_clock::time_point t);
// The rate clock's sleep, kept out of line so a --profile capture can tell sleeping from working.
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void pacing_sleep_until(std::chrono::steady_clock::time_point t) {
    std::this_thread::sleep_until(t);
}
// Host sampling profiler (dreamcomp, --profile FILE): samples the calling thread's call stack about
// every `interval_us` until host_profiler_stop(), which writes FILE (tools/profile.py names the
// frames). Windows only for now; elsewhere it says so and does nothing. Defined at the end.
void host_profiler_start(const std::string& file, unsigned interval_us);
void host_profiler_stop();
// CPU cycles the calling thread has used (dreamcomp; Windows QueryThreadCycleTime, 0 elsewhere).
// Unlike wall time it hardly moves with the clock speed a laptop picks, so it compares builds.
std::uint64_t thread_cpu_cycles();

int main(int argc, char** argv) {
    install_crash_handler();
    if (std::getenv("DREAM_CRASH_TEST")) {  // checks the crash report itself
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    }
    // Extensions may rewrite the command line before it is parsed (host_ext.h). The rewritten
    // strings must outlive every pointer taken into them below, hence the statics.
    static std::vector<std::string> ext_args;
    static std::vector<char*> ext_argv;
    if (!dream::host::extensions().empty()) {
        ext_args.assign(argv, argv + argc);
        for (auto* e : dream::host::extensions()) e->adjust_args(ext_args);
        for (auto& a : ext_args) ext_argv.push_back(a.data());
        ext_argv.push_back(nullptr);
        argc = static_cast<int>(ext_args.size());
        argv = ext_argv.data();
    }
    std::string config, report, sample_file, dump;
    std::string profile_file;          // --profile FILE (dreamcomp): host sampling profiler
    unsigned profile_interval_us = 1000;  // --profile-interval US
    bool stop_on_ta = false;
    // No limit unless one is asked for. A run that stops on its own after a number of frames
    // nobody chose is indistinguishable from a crash, and the flag is right there for the runs
    // that do want bounding: every headless check in this repository passes it explicitly.
    std::uint64_t max_frames = 0, max_seconds = 0, sample_every = 0;
    bool interpret_all = false;  // dev builds: run everything through the interpreter
    std::string wav;             // --wav FILE: record the AICA output (16-bit stereo 44.1 kHz)
    std::string dump_aram;       // --dump-aram FILE: write the 2 MB of sound RAM at the stop
    std::string dump_ta;         // --dump-ta FILE: write one render's TA parameter stream
    // --no-create-vmu: fail rather than make a card when --vmu names nothing. For a scripted run
    // that should not be quietly writing files.
    bool no_create_vmu = false;
    // --screenshot-presented: capture what reached the screen rather than the game's own
    // pixels. For looking at the display path -- overlays, letterboxing, the resolve.
    bool shot_presented = false;
#ifdef DREAM_WITH_RENDERER
    // --present-mode: vsync paces the run to the panel, which is right for playing and wrong for
    // measuring. Nothing about the guest changes either way.
    auto present_mode = dream::render::vk::Window::PresentMode::Fifo;
    // --present-path vulkan|dxgi (dreamcomp; DREAM_PRESENT_PATH the same): dxgi presents through a
    // DXGI flip-model swapchain on Windows, a refresh sooner than some drivers' Vulkan swapchains
    // where the machine has the headroom (dxgi_present.h, docs/PACING.md in dreamcomp).
    auto present_path = dream::render::vk::Window::PresentPath::Auto;
    if (const char* e = std::getenv("DREAM_PRESENT_PATH")) {
        if (!std::strcmp(e, "vulkan"))
            present_path = dream::render::vk::Window::PresentPath::Vulkan;
        else if (!std::strcmp(e, "dxgi"))
            present_path = dream::render::vk::Window::PresentPath::Dxgi;
    }
#endif
    // --bindings FILE: where the controller layout is read from and written back to. Unset means
    // the host's per-user settings directory, so one layout follows the player across every title.
    std::string bindings_file;
    bool bindings_file_set = false;
    std::string flash_dump;           // --dump-flash FILE: write the flash out at the stop, so a
                                      // synthesised one can be compared against a reference.
    std::string flash_image;          // --flash FILE: a 128 KB console flash image. The runtime
                                      // synthesises one when this is absent, which is enough for
                                      // some titles and not others: a title that reads the user's
                                      // settings out of the flash user partition may refuse to
                                      // start on a synthetic one.
    const char* disc_override = nullptr;  // --disc FILE (dreamcomp): the image, overriding [disc]
    float render_aspect = 0.0f;           // --render-aspect A (dreamcomp): render target shape
    // --mod DIR (dreamcomp, repeatable): file-replacement mod roots layered over the disc
    // (gdrom/mod_disc.h); the first root providing a file wins.
    std::vector<std::filesystem::path> mod_roots;
    int rumble_percent = 100;  // --rumble N (dreamcomp): pad rumble strength, 0 = off
    int volume_percent = 100;  // --volume N (dreamcomp): master output level
    bool soft_clip = false;    // --soft-clip (dreamcomp): round loud peaks off instead of clipping
    bool fullscreen = false;              // --fullscreen (dreamcomp); Alt+Enter toggles
    bool interpolate_frames = false;      // --interpolate (dreamcomp): a blended frame between renders
    bool interpolate_auto = false;        // --interpolate-auto: only on displays above 60 Hz
    bool sync_display = false;            // --sync-display (dreamcomp): pace at the display's rate
    std::string vmu;                  // --vmu FILE: a 128 KB memory-card image in the standard
                                      // layout, as any Dreamcast tool or emulator writes. Writes
                                      // go back to the file. Never committed: owner data.
    std::string dump_vram;            // --dump-vram FILE: video memory and the PVR registers, so
                                      // the renderer's tools can decode a capture's textures
    std::uint64_t dump_ta_frame = 0;  // --dump-ta-frame N: which render to capture (default: the
                                      // first with more than a token amount of geometry)
    // --dump-ta-at-frame N: the first render at or after guest frame N. A visual fault is noticed
    // at a moment in the run, and the render index for that moment is not stable between runs.
    std::uint64_t dump_ta_at_frame = 0;
    unsigned dump_ta_count = 1;  // --dump-ta-count K (dreamcomp): K consecutive renders from there
    // --press BUTTON@FRAME[,BUTTON@FRAME...]: hold a controller button for 8 frames from that
    // frame, so a headless run can get past "press start" screens. Names: a b c x y z start up
    // down left right.
    std::string presses;
    // --record-input FILE / --play-input FILE (dreamcomp): player 1's controller as the game saw
    // it at every vblank, written / replayed. With the same disc, settings, --rtc-seed and a
    // fresh --vmu the game is deterministic, so a session played by hand can be replayed exactly
    // (a bug seen on one machine captured on another, or with --dump-ta at the moment it shows).
    std::string record_input, play_input;
    // --window: open a window and play. Without it the run is headless exactly as before.
    // --scale N draws the geometry at N times the guest's resolution; the frame is resampled back
    // into the guest's framebuffer, so the guest never learns its screen changed size.
    bool window_mode = false, validation = false, unthrottled = false, writeback = false;
    // --fps: start with the on-screen counter showing. F10 toggles it either way; this only
    // decides where it starts, so a run nobody is sitting at still records the figure.
    bool start_with_fps = false;
    // --suggest-config FILE: the TOML this run earned, ready to paste into the game's config.
    std::string suggest_config;
    std::set<std::uint64_t> screenshot_at;  // --screenshot-at N[,N...] (list: dreamcomp)
    std::uint64_t capture_at = 0;
    unsigned scale = 1;
    // --texture-pack DIR / --dump-textures DIR (dreamcomp): replacement packs and the dump that
    // authors them, both keyed by the content hash in render/texture_pack.h.
    std::string texture_pack, dump_textures;
    // --rtc-seed N: a fixed console clock, so two runs of the same build do the same thing and can
    // be compared. Any non-zero value will do; the number itself only changes the date a title
    // shows.
    std::uint64_t rtc_seed = 0;
    // --write-hash FILE: one line per frame with a rolling hash of every guest memory write. Two
    // runs that agree line for line did the same thing; the first line that differs is the frame
    // the divergence began (docs/differential-harness.md).
    std::string write_hash;
    // --write-log FILE and --write-log-range FROM:COUNT: every guest store in that window of the
    // write sequence, with the call site. Diff two runs' logs to name the store that differs.
    std::string write_log;
    std::uint64_t log_from = 0, log_count = 0;
    // --mask-segment: ignore the segment bits of stored pointers when hashing and logging, so a
    // return address pushed from P1 in one run and P2 in the other does not read as a divergence.
    bool mask_segment = false;
    // --audio plays the AICA's output through the host's sound device. On by default with a window,
    // since a player expects sound; --no-audio turns it off for a run that is only being measured,
    // where a device that consumes at its own rate would pace the guest.
    bool audio = false, no_audio = false;
    // --replay compares each guest function against the interpreter with the same inputs: the
    // function runs, memory is put back, the interpreter runs it again, and the two exits are
    // compared. Needs a build translated with --replay-hooks. --replay-only limits it to named
    // functions, because comparing every call is slow.
    bool replay_all = false, replay_self_check = false;
    std::string replay_only;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--config") && i + 1 < argc)
            config = argv[++i];
        else if (!std::strcmp(argv[i], "--report") && i + 1 < argc)
            report = argv[++i];
        else if (!std::strcmp(argv[i], "--stop-on-ta"))
            stop_on_ta = true;
        else if (!std::strcmp(argv[i], "--max-frames") && i + 1 < argc)
            max_frames = std::strtoull(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc)
            dump = argv[++i];
        else if (!std::strcmp(argv[i], "--sample-file") && i + 1 < argc)
            sample_file = argv[++i];
        else if (!std::strcmp(argv[i], "--profile") && i + 1 < argc)
            profile_file = argv[++i];
        else if (!std::strcmp(argv[i], "--profile-interval") && i + 1 < argc)
            profile_interval_us = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--sample") && i + 1 < argc)
            sample_every = std::strtoull(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--interpret"))
            interpret_all = true;
        else if (!std::strcmp(argv[i], "--wav") && i + 1 < argc)
            wav = argv[++i];
        else if (!std::strcmp(argv[i], "--dump-aram") && i + 1 < argc)
            dump_aram = argv[++i];
        else if (!std::strcmp(argv[i], "--dump-ta") && i + 1 < argc)
            dump_ta = argv[++i];
        else if (!std::strcmp(argv[i], "--dump-vram") && i + 1 < argc)
            dump_vram = argv[++i];
        else if (!std::strcmp(argv[i], "--no-create-vmu"))
            no_create_vmu = true;
        else if (!std::strcmp(argv[i], "--screenshot-presented"))
            shot_presented = true;
#ifdef DREAM_WITH_RENDERER
        else if (!std::strcmp(argv[i], "--present-path") && i + 1 < argc) {
            const char* p = argv[++i];
            using PP = dream::render::vk::Window::PresentPath;
            present_path = !std::strcmp(p, "vulkan") ? PP::Vulkan
                           : !std::strcmp(p, "dxgi") ? PP::Dxgi
                                                     : PP::Auto;
        }
        else if (!std::strcmp(argv[i], "--present-mode") && i + 1 < argc) {
            const char* m = argv[++i];
            using PM = dream::render::vk::Window::PresentMode;
            if (!std::strcmp(m, "mailbox"))
                present_mode = PM::Mailbox;
            else if (!std::strcmp(m, "immediate"))
                present_mode = PM::Immediate;
            else if (!std::strcmp(m, "vsync") || !std::strcmp(m, "fifo"))
                present_mode = PM::Fifo;
            else {
                std::fprintf(stderr, "unknown present mode %s: vsync, mailbox or immediate\n", m);
                return 2;
            }
        }
#endif
        else if (!std::strcmp(argv[i], "--bindings") && i + 1 < argc) {
            bindings_file = argv[++i];
            bindings_file_set = true;
        } else if (!std::strcmp(argv[i], "--flash") && i + 1 < argc) {
            flash_image = argv[++i];
        } else if (!std::strcmp(argv[i], "--dump-flash") && i + 1 < argc) {
            flash_dump = argv[++i];
        } else if (!std::strcmp(argv[i], "--disc") && i + 1 < argc)
            disc_override = argv[++i];
        else if (!std::strcmp(argv[i], "--mod") && i + 1 < argc)
            mod_roots.emplace_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--rumble") && i + 1 < argc)
            rumble_percent = std::clamp(std::atoi(argv[++i]), 0, 100);
        else if (!std::strcmp(argv[i], "--volume") && i + 1 < argc)
            volume_percent = std::clamp(std::atoi(argv[++i]), 0, 100);
        else if (!std::strcmp(argv[i], "--soft-clip"))
            soft_clip = true;
        else if (!std::strcmp(argv[i], "--vmu") && i + 1 < argc)
            vmu = argv[++i];
        else if (!std::strcmp(argv[i], "--dump-ta-frame") && i + 1 < argc)
            dump_ta_frame = std::strtoull(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--dump-ta-at-frame") && i + 1 < argc)
            dump_ta_at_frame = std::strtoull(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--dump-ta-count") && i + 1 < argc)
            dump_ta_count = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--press") && i + 1 < argc)
            presses = argv[++i];
        else if (!std::strcmp(argv[i], "--record-input") && i + 1 < argc)
            record_input = argv[++i];
        else if (!std::strcmp(argv[i], "--play-input") && i + 1 < argc)
            play_input = argv[++i];
        else if (!std::strcmp(argv[i], "--max-seconds") && i + 1 < argc)
            max_seconds = std::strtoull(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--window"))
            window_mode = true;
        else if (!std::strcmp(argv[i], "--validation"))
            validation = true;
        else if (!std::strcmp(argv[i], "--unthrottled"))
            unthrottled = true;
        else if (!std::strcmp(argv[i], "--framebuffer-writeback"))
            writeback = true;
        else if (!std::strcmp(argv[i], "--fps"))
            start_with_fps = true;
        else if (!std::strcmp(argv[i], "--suggest-config") && i + 1 < argc)
            suggest_config = argv[++i];
        else if (!std::strcmp(argv[i], "--screenshot-at") && i + 1 < argc)
            for (const char* p = argv[++i]; *p;) {
                char* end = nullptr;
                const std::uint64_t n = std::strtoull(p, &end, 0);
                if (end == p)
                    break;
                screenshot_at.insert(n);
                p = (*end == ',') ? end + 1 : end;
            }
        else if (!std::strcmp(argv[i], "--capture-at") && i + 1 < argc)
            capture_at = std::strtoull(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--rtc-seed") && i + 1 < argc)
            rtc_seed = std::strtoull(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--write-hash") && i + 1 < argc)
            write_hash = argv[++i];
        else if (!std::strcmp(argv[i], "--write-log") && i + 1 < argc)
            write_log = argv[++i];
        else if (!std::strcmp(argv[i], "--mask-segment"))
            mask_segment = true;
        else if (!std::strcmp(argv[i], "--audio"))
            audio = true;
        else if (!std::strcmp(argv[i], "--no-audio"))
            no_audio = true;
        else if (!std::strcmp(argv[i], "--replay"))
            replay_all = true;
        else if (!std::strcmp(argv[i], "--replay-self-check"))
            replay_self_check = true;
        else if (!std::strcmp(argv[i], "--replay-only") && i + 1 < argc)
            replay_only = argv[++i];
        else if (!std::strcmp(argv[i], "--write-log-range") && i + 1 < argc) {
            const char* spec = argv[++i];
            log_from = std::strtoull(spec, nullptr, 0);
            if (const char* colon = std::strchr(spec, ':'))
                log_count = std::strtoull(colon + 1, nullptr, 0);
        } else if (!std::strcmp(argv[i], "--texture-pack") && i + 1 < argc)
            texture_pack = argv[++i];
        else if (!std::strcmp(argv[i], "--dump-textures") && i + 1 < argc)
            dump_textures = argv[++i];
        else if (!std::strcmp(argv[i], "--render-aspect") && i + 1 < argc)
            render_aspect = std::strtof(argv[++i], nullptr);
        else if (!std::strcmp(argv[i], "--fullscreen"))
            fullscreen = true;
        else if (!std::strcmp(argv[i], "--interpolate"))
            interpolate_frames = true;
        else if (!std::strcmp(argv[i], "--interpolate-auto"))
            interpolate_auto = true;
        else if (!std::strcmp(argv[i], "--sync-display"))
            sync_display = true;
        else if (!std::strcmp(argv[i], "--scale") && i + 1 < argc)
            scale = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            usage(argv[0], stdout);
            return 0;
        } else {
            int used = 0;
            for (auto* e : dream::host::extensions())
                if ((used = e->parse_arg(i, argc, argv)) > 0)
                    break;
            if (used > 0) {
                i += used - 1;
                continue;
            }
            std::fprintf(stderr, "unknown option: %s\n\n", argv[i]);
            usage(argv[0], stderr);
            return 2;
        }
    }
    if (config.empty()) {
        std::fprintf(stderr, "--config is required\n");
        return 2;
    }
    if (scale < 1 || scale > 8) {
        std::fprintf(stderr, "--scale must be 1 to 8\n");
        return 2;
    }
#ifndef DREAM_WITH_RENDERER
    if (window_mode) {
        std::fprintf(stderr,
                     "this build has no renderer: configure with -DDREAM_RENDERER=ON and make sure "
                     "Vulkan, SDL3 and glslc are found\n");
        return 2;
    }
    (void)validation;
    (void)unthrottled;
    (void)rumble_percent;
    (void)volume_percent;
    (void)writeback;
#endif
    dream::translator::GameConfig cfg;
    std::string err;
    if (!dream::translator::load_game_config(config, cfg, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    std::unique_ptr<dream::gdrom::Disc> disc;
    if (disc_override)
        cfg.disc_image = std::filesystem::path(disc_override);
    if (cfg.disc_image) {
        disc = dream::gdrom::open_disc(*cfg.disc_image, err);
        if (!disc) {
            std::fprintf(stderr, "disc: %s\n", err.c_str());
            return 2;
        }
    }
    std::vector<char> bytes;
    if (std::ifstream in{cfg.binary_path, std::ios::binary}) {
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } else if (disc) {
        // dreamcomp: no extracted copy beside the config, so take the boot file off the disc and
        // refuse anything but the exact build this executable was translated from.
        std::vector<std::uint8_t> boot;
        std::string boot_name;
        if (!dream::gdrom::read_root_file(*disc, "", boot, &boot_name)) {
            std::fprintf(stderr, "disc: no boot file in the image's filesystem\n");
            return 2;
        }
        const std::string sha = dream::sha1_hex(boot.data(), boot.size());
        if (!cfg.sha1_1st_read.empty() && sha != cfg.sha1_1st_read) {
            std::fprintf(stderr,
                         "disc: %s has SHA-1 %s, but this build was made from %s.\n"
                         "This is a different release or revision of the game; it cannot run "
                         "here.\n",
                         boot_name.c_str(), sha.c_str(), cfg.sha1_1st_read.c_str());
            return 3;
        }
        bytes.assign(boot.begin(), boot.end());
    } else {
        std::fprintf(stderr, "cannot read %s\n", cfg.binary_path.string().c_str());
        return 2;
    }
    // dreamcomp: file-replacement mods. Applied after the boot file was read and verified, so
    // the executable always comes from the disc this build was translated from.
    if (!mod_roots.empty()) {
        if (!disc) {
            std::fprintf(stderr, "--mod needs a disc image (--disc or [disc] image)\n");
            return 2;
        }
        std::string mod_log;
        disc = dream::gdrom::ModDisc::wrap(std::move(disc), mod_roots, mod_log);
        std::printf("%s", mod_log.c_str());
        if (!mod_log.empty() && mod_log.back() != '\n')
            std::printf("\n");
    }

    dream::System sys;
    dream::hle::Bios bios(sys);
    bios.attach_disc(disc.get());
    // Load the flash before setup_boot: Bios::install() formats a synthetic one only when the
    // factory string is not already present, so an image loaded here is used as it stands.
    if (!flash_image.empty()) {
        std::ifstream f(flash_image, std::ios::binary);
        std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        if (raw.size() != dream::hle::Flash::kSize) {
            std::fprintf(stderr, "--flash %s: expected %u bytes, got %zu\n", flash_image.c_str(),
                         dream::hle::Flash::kSize, raw.size());
            return 2;
        }
        std::memcpy(sys.memory.flash(), raw.data(), raw.size());
        std::printf("flash: loaded %s\n", flash_image.c_str());
    }
    // The console's locale, before setup_boot() installs and may synthesise a flash. The disc's
    // region is the only evidence here of which console the title was sold for, and a title that
    // reads the factory partition should not be told it is running on a Japanese machine because
    // that happened to be the hardcoded default.
    const auto region = dream::hle::region_from_name(cfg.region);
    bios.set_locale(dream::hle::Language::English, region, dream::hle::broadcast_for(region));
    sys.install();
    bios.setup_boot(cfg.entry);
    const std::vector<std::uint8_t> ipbin(sys.memory.ram() + 0x8000, sys.memory.ram() + 0x10000);
    std::memcpy(sys.memory.ram() + (cfg.load_address & (dream::mem::DcMemory::kRamSize - 1)),
                bytes.data(), bytes.size());
    // PowerVR core: register block and TA FIFO (headless: lists are parsed and counted, nothing
    // drawn).
    dream::pvr::Core pvr(sys.sched, sys.holly, sys.spg, sys.memory);
    sys.memory.map_mmio(dream::pvr::Core::kRegBase, dream::pvr::Core::kRegEnd, &pvr);
    sys.memory.map_mmio(dream::pvr::Core::kFifoBase, dream::pvr::Core::kFifoEnd, &pvr);
    // The hook fires before the chunk is parsed, so `ta.chunks` is still zero when the stop
    // throws; the success check below has to read this flag rather than the counter.
    bool reached_ta = false;
    if (stop_on_ta)
        pvr.on_first_ta_data = [&] {
            reached_ta = true;
            throw StopRun{"first TA FIFO data"};
        };
    // --dump-ta: capture one render's parameter stream for the renderer's tests. By default the
    // first substantial one is taken, since the earliest renders are often near-empty.
    if (!dump_ta.empty() || dump_ta_at_frame) {
        std::uint64_t seen = 0;
        bool vram_written = false;
        // A one-shot capture has to stop capturing, not merely forget where to write. Clearing
        // dump_ta alone left every later render falling through to the numbered branch below with
        // an empty stem, which writes ".000", ".001" and so on into the working directory: 64
        // hidden files for asking for one.
        bool one_shot_done = false;
        pvr.on_render = [&, seen, vram_written,
                         one_shot_done](const std::vector<std::uint32_t>& stream) mutable {
            if (one_shot_done)
                return;
            ++seen;
            // The "substantial" filter skips the near-empty renders a boot starts with, but a
            // caller who names a render by number means that one: a simple screen can be small,
            // and silently declining to capture it looks like the render never happened.
            const bool substantial = stream.size() > 8 * 64;
            if (!substantial && !dump_ta_frame && !dump_ta_at_frame)
                return;
            if (dump_ta_at_frame && sys.spg.frames() < dump_ta_at_frame)
                return;
            // --dump-ta-frame N captures that render alone; without it the first sixteen
            // substantial renders are written as FILE.000, FILE.001 and so on, so a bring-up can
            // look for the one that holds the scene.
            char name[512];
            static unsigned at_frame_written = 0;
            if (dump_ta_at_frame && dump_ta_count > 1) {
                std::snprintf(name, sizeof name, "%s.%03u", dump_ta.c_str(), at_frame_written);
            } else if (dump_ta_at_frame) {
                std::snprintf(name, sizeof name, "%s", dump_ta.c_str());
            } else if (dump_ta_frame) {
                if (seen != dump_ta_frame)
                    return;
                std::snprintf(name, sizeof name, "%s", dump_ta.c_str());
            } else {
                static unsigned written = 0;
                if (written >= 64)
                    return;
                std::snprintf(name, sizeof name, "%s.%03u", dump_ta.c_str(), written++);
            }
            if (FILE* f = std::fopen(name, "wb")) {
                std::fwrite(stream.data(), 4, stream.size(), f);
                std::fclose(f);
                // The frame number as well as the render index: a visual fault is noticed at a
                // moment in the run, and without it there is no way to say which capture is the
                // screen being looked at.
                std::printf(
                    "wrote %s: render %llu at frame %llu, %zu parameter words (%zu chunks)\n", name,
                    static_cast<unsigned long long>(seen),
                    static_cast<unsigned long long>(sys.spg.frames()), stream.size(),
                    stream.size() / 8);
            }
            // Video memory belongs to the same moment as the display list: a texture a capture
            // refers to may be overwritten seconds later, and a dump taken at the end of the run
            // then decodes as noise. Written beside the first capture.
            if (!dump_vram.empty() && !vram_written) {
                vram_written = true;
                const std::string vpath = std::string(name) + ".vram";
                if (FILE* f = std::fopen(vpath.c_str(), "wb")) {
                    std::fwrite(sys.memory.vram(), 1, dream::mem::DcMemory::kVramSize, f);
                    std::fclose(f);
                }
                if (FILE* f = std::fopen((vpath + ".regs").c_str(), "wb")) {
                    for (std::uint32_t off = 0; off < 0x2000; off += 4) {
                        const std::uint32_t word = pvr.reg(off);
                        std::fwrite(&word, 4, 1, f);
                    }
                    std::fclose(f);
                }
                std::printf("wrote %s: video memory as it was for that render\n", vpath.c_str());
            }
            if (dump_ta_frame || (dump_ta_at_frame && ++at_frame_written >= dump_ta_count))
                one_shot_done = true;
        };
    }
    // SH-4 DMAC and the Holly system block: channel-2 DMA is how Kamui submits display lists and
    // textures. The other system-bus DMA blocks (G1 at 0x005F7400, G2 at 0x005F7800, PVR DMA at
    // 0x005F7C00) are stored registers until WP2.5 (AICA DMA) needs them.
    dream::sh4::Dmac dmac(sys.memory);
    dream::holly::SysBlock sysblock(dmac, sys.holly, sys.memory, pvr);
    sys.memory.map_p4(dream::sh4::Dmac::kBase, dream::sh4::Dmac::kEnd, &dmac);
    sys.memory.map_mmio(dream::holly::SysBlock::kBase, dream::holly::SysBlock::kEnd, &sysblock);
    dream::holly::G2Dma g2(sys.sched, sys.holly, sys.memory);
    dream::mem::RegisterFile g1(0x005F7400u, 0x100 / 4), pvrdma(0x005F7C00u, 0x100 / 4),
        dma_triggers(0x005F6940u, 0x20 / 4);
    sys.memory.map_mmio(0x005F6940u, 0x005F6960u, &dma_triggers);  // SB_PDTNRM .. SB_G2DTEXT
    if (!std::getenv("DREAM_NO_G1_STUB"))  // unset the stub to see the G1 registers a title touches
        sys.memory.map_mmio(0x005F7400u, 0x005F7500u, &g1);
    sys.memory.map_mmio(dream::holly::G2Dma::kBase, dream::holly::G2Dma::kEnd, &g2);
    sys.memory.map_mmio(0x005F7C00u, 0x005F7D00u, &pvrdma);
    // On-chip modules the startup code programs but the runtime does not model yet: stored so the
    // writes neither fault nor abort (BSC, UBC, CPG/WDT, on-chip RTC), plus the serial port and
    // the cache address/data arrays, which cache-maintenance loops write.
    dream::mem::RegisterFile bsc(0xFF800000u, 0x50 / 4), ubc(0xFF200000u, 0x24 / 4),
        cpg(0xFFC00000u, 0x10 / 4), rtc(0xFFC80000u, 0x40 / 4);
    dream::mem::NullRegion cache_arrays;
    SerialCapture scif;
    sys.memory.map_p4(0xF0000000u, 0xF8000000u, &cache_arrays);
    sys.memory.map_p4(0xFF800000u, 0xFF800050u, &bsc);
    sys.memory.map_p4(0xFF200000u, 0xFF200024u, &ubc);
    sys.memory.map_p4(0xFFC00000u, 0xFFC00010u, &cpg);
    sys.memory.map_p4(0xFFC80000u, 0xFFC80040u, &rtc);
    sys.memory.map_p4(0xFFE80000u, 0xFFE80028u, &scif);
    // Maple bus with a standard controller in port A (no buttons pressed); VBlank-triggered
    // transfers come from the SPG.
    dream::maple::Bus maple(sys.sched, sys.holly, sys.memory);
    auto pad = std::make_unique<dream::maple::Controller>();
    dream::maple::Controller* pad_ptr = pad.get();
    // A memory card in the controller's first expansion slot, where the hardware puts one.
    dream::maple::MemoryCard* card_ptr = nullptr;
    if (!vmu.empty()) {
        auto card = std::make_unique<dream::maple::MemoryCard>();
        switch (card->load(vmu)) {
            case dream::maple::CardStatus::Ok:
                std::printf("memory card %s: %s\n", vmu.c_str(),
                            card->formatted() ? "formatted" : "present but not formatted");
                break;
            case dream::maple::CardStatus::Missing:
                // Made only when nothing is there. Titles do not all offer to format a blank card
                // -- Crazy Taxi reads the system block, finds no marker and simply refuses to save
                // -- so an empty file would be no better than none.
                if (no_create_vmu) {
                    std::fprintf(stderr,
                                 "no memory card at %s (drop --no-create-vmu to make one)\n",
                                 vmu.c_str());
                    return 2;
                }
                card->format();
                if (!card->save_as(vmu)) {
                    std::fprintf(stderr, "cannot create a memory card at %s\n", vmu.c_str());
                    return 2;
                }
                std::printf("memory card %s: created, formatted and empty\n", vmu.c_str());
                break;
            case dream::maple::CardStatus::WrongSize:
                // Never overwritten. save() rewrites the whole 128 KB after every block write, so
                // adopting a file that is not a card destroys it the moment the title saves.
                std::fprintf(stderr,
                             "%s is not a memory card image: a card is exactly %zu bytes.\n"
                             "Refusing to touch it. Point --vmu at a card, or at a path that does "
                             "not exist yet and one will be made.\n",
                             vmu.c_str(), dream::maple::MemoryCard::kImageSize);
                return 2;
            case dream::maple::CardStatus::Unreadable:
                std::fprintf(stderr, "cannot read the memory card image %s\n", vmu.c_str());
                return 2;
        }
        card_ptr = card.get();
        maple.attach_expansion(0, 0, std::move(card));
    }
    maple.attach(0, std::move(pad));
    // dreamcomp: a vibration pack in every attached controller's slot 1, reported to the window
    // as gamepad rumble for that player. `rumble_fn` stays empty headless.
    std::function<void(unsigned, float, unsigned)> rumble_fn;
    auto make_pack = [&rumble_fn](unsigned port) {
        auto pack = std::make_unique<dream::maple::VibrationPack>();
        pack->on_vibrate = [&rumble_fn, port](float strength, unsigned ms) {
            if (rumble_fn)
                rumble_fn(port, strength, ms);
        };
        return pack;
    };
    maple.attach_expansion(0, 1, make_pack(0));
    // Players 2-4: a controller (and pack) on ports B-D exists only while a pad drives that
    // player, so a game sees the second controller appear when it is plugged in.
    std::array<dream::maple::Controller*, dream::maple::Bus::kPorts> port_pads{pad_ptr};
    auto sync_ports = [&](unsigned players) {
        for (unsigned port = 1; port < dream::maple::Bus::kPorts; ++port) {
            const bool want = port < players;
            if (want == (port_pads[port] != nullptr))
                continue;
            if (want) {
                auto c = std::make_unique<dream::maple::Controller>();
                port_pads[port] = c.get();
                maple.attach(port, std::move(c));
                maple.attach_expansion(port, 1, make_pack(port));
            } else {
                port_pads[port] = nullptr;
                maple.attach(port, nullptr);
                maple.attach_expansion(port, 1, nullptr);
            }
            std::printf("maple: controller %s on port %c\n", want ? "connected" : "removed",
                        static_cast<char>('A' + port));
        }
    };
    sys.memory.map_mmio(dream::maple::Bus::kBase, dream::maple::Bus::kEnd, &maple);
    // Scripted controller presses (--press start@120,a@300): each is held for 8 frames.
    std::vector<std::pair<std::uint64_t, std::uint16_t>> scripted;
    {
        static const struct {
            const char* name;
            std::uint16_t bit;
        } kNames[] = {{"c", dream::maple::kC},       {"b", dream::maple::kB},
                      {"a", dream::maple::kA},       {"start", dream::maple::kStart},
                      {"up", dream::maple::kUp},     {"down", dream::maple::kDown},
                      {"left", dream::maple::kLeft}, {"right", dream::maple::kRight},
                      {"z", dream::maple::kZ},       {"y", dream::maple::kY},
                      {"x", dream::maple::kX}};
        std::string rest = presses;
        while (!rest.empty()) {
            const std::size_t comma = rest.find(',');
            std::string one = rest.substr(0, comma);
            rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
            const std::size_t at = one.find('@');
            if (at == std::string::npos)
                continue;
            const std::string name = one.substr(0, at);
            const std::uint64_t frame = std::strtoull(one.c_str() + at + 1, nullptr, 0);
            for (const auto& n : kNames)
                if (name == n.name)
                    scripted.push_back({frame, n.bit});
        }
    }
    // Input recording (see --record-input): "DREAMINPUT1\n", then per vblank the guest frame
    // (u64) and the controller state (8 bytes), little-endian as the host is.
    struct InputRecord {
        std::uint64_t frame;
        dream::maple::ControllerState state;
    };
    static_assert(sizeof(dream::maple::ControllerState) == 8);
    std::FILE* rec_file = nullptr;
    std::vector<InputRecord> playback;
    std::size_t playback_at = 0;
    bool playing = false;
    if (!record_input.empty()) {
        rec_file = std::fopen(record_input.c_str(), "wb");
        if (!rec_file) {
            std::fprintf(stderr, "cannot write the input recording %s\n", record_input.c_str());
            return 2;
        }
        std::fputs("DREAMINPUT1\n", rec_file);
        std::printf("recording player 1's input to %s\n", record_input.c_str());
    }
    if (!play_input.empty()) {
        std::FILE* f = std::fopen(play_input.c_str(), "rb");
        char magic[12] = {};
        if (!f || std::fread(magic, 1, 12, f) != 12 || std::memcmp(magic, "DREAMINPUT1\n", 12)) {
            std::fprintf(stderr, "%s is not an input recording\n", play_input.c_str());
            if (f) std::fclose(f);
            return 2;
        }
        for (InputRecord r; std::fread(&r.frame, 8, 1, f) == 1 && std::fread(&r.state, 8, 1, f) == 1;)
            playback.push_back(r);
        std::fclose(f);
        playing = !playback.empty();
        std::printf("playing %zu frames of input from %s (guest frames %llu..%llu)\n",
                    playback.size(), play_input.c_str(),
                    playing ? static_cast<unsigned long long>(playback.front().frame) : 0ull,
                    playing ? static_cast<unsigned long long>(playback.back().frame) : 0ull);
    }
    sys.spg.on_vblank_out = [&] {
        for (auto* e : dream::host::extensions()) e->on_vblank(sys);
        if (playing) {
            const std::uint64_t f = sys.spg.frames();
            while (playback_at < playback.size() && playback[playback_at].frame < f)
                ++playback_at;
            if (playback_at < playback.size()) {
                if (playback[playback_at].frame == f)
                    pad_ptr->state = playback[playback_at].state;
            } else {
                playing = false;
                pad_ptr->state = {};
                std::printf("input playback ended at guest frame %llu: the controller is live\n",
                            static_cast<unsigned long long>(f));
            }
        }
        if (!scripted.empty()) {
            const std::uint64_t f = sys.spg.frames();
            std::uint16_t held = 0;
            for (const auto& [start, bit] : scripted)
                if (f >= start && f < start + 8)
                    held |= bit;
            pad_ptr->state.buttons = static_cast<std::uint16_t>(0xFFFFu & ~held);
        }
        if (rec_file) {
            const std::uint64_t f = sys.spg.frames();
            std::fwrite(&f, 8, 1, rec_file);
            std::fwrite(&pad_ptr->state, 8, 1, rec_file);
            // Flushed every second: a console title is ended by closing it.
            if (f % 60 == 0)
                std::fflush(rec_file);
        }
        maple.vblank();
    };
#ifdef DREAM_WITH_RENDERER
    // --window: the renderer, the offscreen target and the window, hung off the same two hooks the
    // headless run uses. Both hooks may already be taken (--dump-ta captures renders, the scripted
    // presses run at vblank), so each is chained rather than replaced.
    std::unique_ptr<Live> live;
    if (window_mode) {
        live = std::make_unique<Live>(sys.memory, pvr);
        live->writeback = writeback;
        live->interpolate = interpolate_frames || interpolate_auto;
        live->interpolate_only_above_60 = interpolate_auto && !interpolate_frames;
        live->pace_interpolation = !unthrottled;
        live->sync.enabled = sync_display;
        auto env_ms = [](const char* name, std::chrono::nanoseconds& out) {
            if (const char* e = std::getenv(name))
                out = std::chrono::nanoseconds(static_cast<long long>(std::atof(e) * 1e6));
        };
        env_ms("DREAM_SYNC_MARGIN_MS", live->sync.margin);
        env_ms("DREAM_SYNC_PAD_MS", live->sync.pad);
        // The host copy of every frame is only needed for write-back; everything else reads it on
        // demand. DREAM_NO_DIRECT_PRESENT=1 restores the copy for A/B measurements.
        {
            const char* e = std::getenv("DREAM_NO_DIRECT_PRESENT");
            live->offscreen.readback = writeback || (e && *e && *e != '0');
        }
        live->show_fps = start_with_fps;
        live->shot_presented = shot_presented;
        live->window.set_present_mode(present_mode);
        live->window.set_present_path(present_path);
        live->screenshot_at = screenshot_at;
        live->capture_at = capture_at;
        // The window names the title being run. The config is the only place that name is written
        // down for a human, and a config without one still gets a window with a name on it.
        if (!live->start(
                scale, validation,
                cfg.title.empty() ? std::string("Dream Recomp") : cfg.title + " - Dream Recomp",
                render_aspect, fullscreen)) {
            std::fprintf(stderr, "window: %s\n", live->error.c_str());
            return 2;
        }
        auto& replacer = live->renderer.textures().replacer();
        if (!texture_pack.empty()) {
            std::string err;
            const auto t0 = std::chrono::steady_clock::now();
            if (replacer.open_pack(texture_pack, &err))
                std::printf("texture pack: %s, %zu textures (indexed in %.1f ms)\n",
                            texture_pack.c_str(), replacer.pack_size(),
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0)
                                .count());
            else
                std::fprintf(stderr, "texture pack: %s\n", err.c_str());
        }
        if (!dump_textures.empty()) {
            std::string err;
            if (replacer.set_dump_dir(dump_textures, &err))
                std::printf("texture dump: writing new textures to %s\n", dump_textures.c_str());
            else
                std::fprintf(stderr, "texture dump: %s\n", err.c_str());
        }
        // After the window, because resolving a binding's name to a code needs SDL initialised.
        live->bindings_path =
            bindings_file_set ? bindings_file : dream::render::vk::default_bindings_path();
        live->load_bindings();
        live->window.rumble_scale = static_cast<float>(rumble_percent) / 100.0f;
        // dreamcomp: an extension's in-game overlay takes Escape and the pad's Back button, sees
        // events first, and may ask the launcher to quit or open the binding screen.
        for (auto* e : dream::host::extensions())
            if (e->wants_overlay())
                live->overlays.push_back(e);
        if (!live->overlays.empty()) {
            live->window.set_escape_quits(false);
            live->window.set_pad_menu_button(false);
            Live* lp = live.get();
            live->window.event_filter = [lp](const void* ev) {
                if (lp->menu.is_open())
                    return false;  // the binding screen keeps its own keys
                for (auto* e : lp->overlays)
                    if (e->on_event(ev))
                        return true;
                return false;
            };
            auto& hc = dream::host::host_controls();
            hc.quit = [lp] { lp->window.request_close(); };
            hc.open_bindings = [lp] {
                if (!lp->menu.is_open())
                    lp->open_menu();
            };
            hc.set_rumble = [lp](float v) { lp->window.rumble_scale = std::clamp(v, 0.0f, 1.0f); };
            hc.set_fullscreen = [lp](bool on) { lp->window.set_fullscreen(on); };
            hc.fullscreen = [lp] { return lp->window.fullscreen(); };
            hc.window_aspect = [lp] {
                const VkExtent2D e = lp->window.extent();
                return e.height ? static_cast<float>(e.width) / static_cast<float>(e.height) : 0.0f;
            };
            hc.set_render_aspect = [lp](float a) { lp->want_render_aspect = a; };
            hc.set_render_scale = [lp](unsigned sc) { lp->want_scale = std::clamp(sc, 1u, 8u); };
            hc.bindings_path = [lp] { return lp->bindings_path; };
            hc.reload_bindings = [lp] { lp->load_bindings(); };
            hc.set_volume = [&volume_percent](float v) {
                volume_percent = static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 100.0f + 0.5f);
            };
            hc.set_soft_clip = [&soft_clip](bool on) { soft_clip = on; };
        }
        if (rumble_percent != 100)
            std::printf("rumble: %d%%\n", rumble_percent);
        rumble_fn = [&live](unsigned port, float strength, unsigned ms) {
            live->window.rumble(port, strength, ms);
        };
        if (live->overlays.empty())
            std::printf("F1 (or a pad's select button) opens the controller bindings\n");
        else
            std::printf("Escape or a pad's select button opens the menu (an overlay may take F1: "
                        "dreamcomp's HUD editor)\n");
        auto previous_render = std::move(pvr.on_render);
        pvr.on_render = [&live, previous_render](const std::vector<std::uint32_t>& stream) {
            if (previous_render)
                previous_render(stream);
            const auto t0 = std::chrono::steady_clock::now();
            live->render(stream);
            g_render_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count());
            ++g_renders;
        };
        // Real time, so the title runs at the speed it was written for. The guest clock is the
        // reference: sleep only while ahead of it, never speed anything up to catch up, because a
        // frame that took too long is gone and pretending otherwise makes the audio stutter.
        // The pacing clock (dreamcomp): the wall-clock time the current guest frame is due,
        // advanced by each guest frame's length (divided by the display-sync factor). Held by the
        // callback itself: this block ends before the first vblank (a reference to a local here
        // once read a dead stack slot and pacing never slept). It restarts from now after a pause
        // or a long stall, so the run never sprints to catch up.
        auto previous_vblank = std::move(sys.spg.on_vblank_out);
        sys.spg.on_vblank_out = [&, previous_vblank,
                                 due = std::chrono::steady_clock::time_point{}]() mutable {
            if (previous_vblank)
                previous_vblank();
            const auto vblank_at = std::chrono::steady_clock::now();
            auto& sync = live->sync;
            if (live->pacing.input_read.time_since_epoch().count() != 0 &&
                sync.emu_ms.size() < 2'000'000)
                sync.emu_ms.push_back(std::chrono::duration<float, std::milli>(
                                          vblank_at - live->pacing.input_read)
                                          .count());
            const double frame_s = sync.note_guest_frame(sys.ctx.cycles);
            if (sync.target.time_since_epoch().count() != 0) {
                const auto aim = sync.target - sync.margin;
                const float off = std::chrono::duration<float, std::milli>(vblank_at - aim).count();
                if (sync.aim_ms.size() < 2'000'000)
                    sync.aim_ms.push_back(off);
                if (off > 0.0f)
                    ++sync.late;
                // Early: hold the frame until its moment, so presents keep one per refresh at a
                // fixed point before the vblank (a frame presented early would otherwise replace
                // the one before it while that still waits for its flip).
                else
                    precise_sleep_until(aim);
            }
            live->pacing.presenting();
            if (!live->present(sys.spg.frames(), sys.ctx.cycles))
                throw StopRun{"the window was closed"};
            // The binding screen stops the guest rather than drawing over a running one: nobody
            // rebinds a control mid-corner, and a control being captured cannot also be played.
            // The window keeps presenting, so the screen still draws and still answers the player.
            if (live->menu.is_open()) {
                while (live->menu.is_open()) {
                    if (!live->present(sys.spg.frames(), sys.ctx.cycles))
                        throw StopRun{"the window was closed"};
                    // Enough to keep the screen responsive without spinning a core on a still
                    // picture; the guest clock is not advancing, so there is nothing to keep up
                    // with.
                    std::this_thread::sleep_for(std::chrono::milliseconds(8));
                }
                due = {};  // start the clock again from now
                sync.target = {};
            }
            // An extension's pause (dreamcomp: the HUD editor holds a HUD on screen while it is
            // edited). The last display list is drawn again each pass, so changes an extension
            // makes in on_frame() show on the still picture.
            auto ext_paused = [] {
                for (auto* e : dream::host::extensions())
                    if (e->guest_paused())
                        return true;
                return false;
            };
            if (ext_paused()) {
                std::printf("paused by an extension at frame %llu\n",
                            static_cast<unsigned long long>(sys.spg.frames()));
                while (ext_paused()) {
                    for (auto* e : dream::host::extensions()) e->on_paused_tick();
                    if (!live->last_stream.empty()) {
                        const std::vector<std::uint32_t> stream = live->last_stream;
                        live->render(stream);
                    }
                    if (!live->present(sys.spg.frames(), sys.ctx.cycles))
                        throw StopRun{"the window was closed"};
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
                std::printf("resumed\n");
                due = {};
                sync.target = {};
            }
            // Pads are read after the pacing sleep (dreamcomp), right before the guest runs the
            // frame that uses them: read before the sleep, the input was a sleep older (about
            // 10 ms at scale 2) by the time the game saw it.
            auto read_pads = [&] {
                if (!live->poll_input())
                    throw StopRun{"the window was closed"};
                // Scripted presses win while they are held, so --press still works with a window
                // open.
                if (!playing && (scripted.empty() || pad_ptr->state.buttons == 0xFFFFu))
                    live->read_controls(pad_ptr->state, 0);
                sync_ports(live->window.players());
                for (unsigned port = 1; port < port_pads.size(); ++port)
                    if (port_pads[port])
                        live->read_controls(port_pads[port]->state, port);
                live->pacing.pads_read();
            };
            if (unthrottled) {
                read_pads();
                return;
            }
            // Display sync: the clock's rate.
            sync.factor = 1.0;
            bool phased = false;
            if (sync.enabled && !live->interpolate && sync.guest_frame_s > 0) {
                std::chrono::steady_clock::time_point vb;
                std::chrono::nanoseconds period{};
                if (live->window.vblank_grid(vb, period) && period.count() > 0) {
                    sync.display_hz = 1e9 / static_cast<double>(period.count());
                    const double guest_hz = 1.0 / sync.guest_frame_s;
                    if (std::abs(sync.display_hz - guest_hz) / guest_hz < 0.005) {
                        sync.factor = sync.display_hz * (1.0 - 1e-4) / guest_hz;
                        static const bool phase_off = [] {
                            const char* e = std::getenv("DREAM_PHASE_LOCK");
                            return e && *e == '0';
                        }();
                        // Only while one frame waits: deeper (frames arriving late, e.g. on
                        // battery) the slot grid never makes up a late frame's time and the panel
                        // ran dry (7 repeats in 90 s at depth 3), while the rate clock below
                        // catches up (0), and the phase buys no latency at that depth anyway.
                        if (live->window.flips() && live->window.flip_queue_depth() == 1 && !phase_off) {
                            // The next vblank on the grid after the last target; started afresh
                            // (or after falling a whole refresh behind) from the first vblank a
                            // frame starting now could make.
                            const auto now = std::chrono::steady_clock::now();
                            const auto per = std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
                            auto snap = [&](std::chrono::steady_clock::time_point t) {
                                const double k = std::round(static_cast<double>((t - vb).count()) /
                                                            static_cast<double>(per.count()));
                                return vb + per * static_cast<long long>(k);
                            };
                            const auto budget = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                sync.budget(period));
                            const auto lead = budget + sync.margin;
                            auto next = sync.target.time_since_epoch().count() == 0
                                            ? sync.target
                                            : snap(sync.target + per);
                            if (next.time_since_epoch().count() == 0 || next - lead < now - per) {
                                next = snap(now + lead + per / 2);
                                if (sync.target.time_since_epoch().count() != 0)
                                    ++sync.reanchored;
                            }
                            sync.target = next;
                            const auto wake = next - lead;
                            if (wake > now) {
                                precise_sleep_until(wake);
                                live->pacing.slept(wake);
                            }
                            due = std::chrono::steady_clock::now();  // the rate clock follows
                            ++sync.phase_locked;
                            phased = true;
                        }
                    }
                }
                ++(sync.factor != 1.0 ? sync.locked : sync.unlocked);
            }
            if (!phased)
                sync.target = {};
            const auto now = std::chrono::steady_clock::now();
            const auto step = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(frame_s / sync.factor));
            // A stall of more than a few frames (loading, the window dragged) is not caught up:
            // the frames that took too long are gone, and sprinting would only stutter the audio.
            if (!phased) {
                if (due.time_since_epoch().count() == 0 ||
                    now - (due + step) > std::chrono::milliseconds(50))
                    due = now;
                else
                    due += step;
                if (due > now) {
                    pacing_sleep_until(due);
                    live->pacing.slept(due);
                }
            }
            read_pads();
            live->pacing.vblank(sys.spg.frames(), live->renderer.textures().decoded,
                                live->renderer.pipelines, g_render_ns,
                                live->renderer.textures().decode_ns);
        };
    }
#endif
    // Real-time clock: the host date at start, then one tick per guest second.
    // AICA control block with the ARM7 that runs the title's sound driver from sound RAM
    // (WP2.5; no sound generation yet).
    dream::aica::Aica aica(sys.sched, sys.holly, sys.memory);
    aica.mixer.soft_clip = &soft_clip;
    sys.memory.map_mmio(dream::aica::Aica::kRegBase, dream::aica::Aica::kRegEnd, &aica);
    // ARM7 program-counter histogram sampled once per 44.1 kHz tick (where the driver spends its
    // time), and the optional recording.
    // A count per instruction slot of the 2 MB sound RAM (dreamcomp): a hash map here cost ~1 % of
    // the game thread, at 44 100 updates a second.
    std::vector<std::uint32_t> arm_pcs((dream::aica::Arm7::kAramMask + 1) / 4, 0);
    std::vector<std::int16_t> pcm;  // interleaved L/R
#ifdef DREAM_WITH_AUDIO
    // Playback follows the window by default: a run with a picture should have sound, and a
    // headless run is usually being measured, where a device consuming at its own rate would pace
    // the guest rather than the other way round.
    dream::audio::Sink sink;
    const bool want_audio = !no_audio && (audio || window_mode);
    if (want_audio) {
        if (sink.open())
            std::printf("audio: playing through the default device\n");
        else
            std::fprintf(stderr, "audio: %s\n", sink.error().c_str());
    }
#else
    // Asking for sound from a build that cannot make any should say so, but it is not worth
    // refusing the run over: --window implies audio, and a build without SDL3 should still show a
    // picture. --wav still records what the mixer produced.
    if (audio && !no_audio)
        std::fprintf(stderr,
                     "audio: this build has no playback (SDL3 was not found); --wav still records "
                     "the mixer's output\n");
    (void)no_audio;
#endif
    aica.on_sample = [&](std::int16_t l, std::int16_t r) {
        ++arm_pcs[(aica.arm.next_pc() & dream::aica::Arm7::kAramMask) >> 2];
#ifdef DREAM_WITH_AUDIO
        if (volume_percent != 100) {  // --volume (dreamcomp); --wav keeps the mixer's own level
            sink.push(static_cast<std::int16_t>(l * volume_percent / 100),
                      static_cast<std::int16_t>(r * volume_percent / 100));
        } else {
            sink.push(l, r);
        }
#endif
        if (!wav.empty() && pcm.size() < 44100u * 2u * 600u) {  // cap at ten minutes
            pcm.push_back(l);
            pcm.push_back(r);
        }
    };
    // The console's clock. Seeded from the host clock so a title shows the right date, but that
    // makes two runs of the same build diverge, and a run that cannot be repeated cannot be
    // compared against another. --rtc-seed fixes it.
    dream::aica::Rtc aica_rtc((rtc_seed ? static_cast<std::uint32_t>(rtc_seed)
                                        : static_cast<std::uint32_t>(std::time(nullptr))) +
                              dream::aica::Rtc::kUnixEpochOffset);
    sys.memory.map_mmio(dream::aica::Rtc::kBase, dream::aica::Rtc::kEnd, &aica_rtc);
    int rtc_tick = -1;
    rtc_tick = sys.sched.add("rtc", [&](std::uint64_t, std::uint64_t) {
        aica_rtc.tick();
        sys.sched.request(rtc_tick, 200'000'000ull);
    });
    sys.sched.request(rtc_tick, 200'000'000ull);
    // Frame and time limits, checked once per scanline.
    int tick = -1;
    const std::uint64_t period = sys.spg.line_cycles();
    // The first access to an address that is not in the memory map is the moment a run started
    // going wrong: everything after it is the guest following garbage. The fault log counts them
    // but records neither when nor from where, so the watchdog catches the transition and reports
    // the frame and the call site. DREAM_STOP_ON_UNMAPPED=1 stops there instead of carrying on,
    // which is what makes the cause reachable rather than the 220-millionth symptom.
    const bool stop_on_unmapped = std::getenv("DREAM_STOP_ON_UNMAPPED") != nullptr;
    bool unmapped_reported = false;
    if (stop_on_unmapped) {
        sys.memory.on_unmapped = [&](std::uint32_t addr, unsigned size, bool write) {
            if (unmapped_reported)
                return;
            unmapped_reported = true;
            std::fprintf(stderr,
                         "unmapped %s%u of 0x%08x at frame %llu: pc 0x%08x pr 0x%08x r15 0x%08x\n",
                         write ? "write" : "read", size * 8, addr,
                         static_cast<unsigned long long>(sys.spg.frames()), sys.ctx.pc, sys.ctx.pr,
                         sys.ctx.r[15]);
            for (unsigned i = 0; i < 16; i += 4)
                std::fprintf(stderr, "  r%-2u %08x  r%-2u %08x  r%-2u %08x  r%-2u %08x\n", i,
                             sys.ctx.r[i], i + 1, sys.ctx.r[i + 1], i + 2, sys.ctx.r[i + 2], i + 3,
                             sys.ctx.r[i + 3]);
            print_guest_backtrace();
            throw StopRun{"first unmapped memory access"};
        };
    }
    // --write-hash: one line per frame, so two runs can be lined up against each other. The first
    // line that differs is the frame a divergence began, which is usually long before the run
    // visibly fails.
    FILE* hash_file = nullptr;
    std::uint64_t hashed_frame = ~0ull;
    if (!write_hash.empty()) {
        hash_file = std::fopen(write_hash.c_str(), "w");
        if (!hash_file) {
            std::fprintf(stderr, "cannot write %s\n", write_hash.c_str());
            return 2;
        }
        sys.memory.hash_writes = true;
        sys.memory.hash_mask_segment = mask_segment;
        std::fprintf(hash_file, "# frame cycles hash writes\n");
    }
    FILE* log_file = nullptr;
    if (!write_log.empty()) {
        log_file = std::fopen(write_log.c_str(), "w");
        if (!log_file) {
            std::fprintf(stderr, "cannot write %s\n", write_log.c_str());
            return 2;
        }
        sys.memory.hash_writes = true;
        sys.memory.hash_mask_segment = mask_segment;
        sys.memory.log_from = log_from;
        sys.memory.log_to = log_from + (log_count ? log_count : 1000000ull);
        sys.memory.on_hashed_write = [&](std::uint64_t index, std::uint32_t addr,
                                         std::uint64_t value, unsigned size) {
            std::fprintf(log_file, "%llu %08x %u %016llx pc %08x pr %08x\n",
                         static_cast<unsigned long long>(index), addr, size,
                         static_cast<unsigned long long>(value), sys.ctx.pc, sys.ctx.pr);
        };
    }
    tick = sys.sched.add("watchdog", [&](std::uint64_t now, std::uint64_t) {
        if (hash_file && sys.spg.frames() != hashed_frame) {
            hashed_frame = sys.spg.frames();
            std::fprintf(hash_file, "%llu %llu %016llx %llu\n",
                         static_cast<unsigned long long>(hashed_frame),
                         static_cast<unsigned long long>(now),
                         static_cast<unsigned long long>(sys.memory.write_hash),
                         static_cast<unsigned long long>(sys.memory.writes_hashed));
        }
        if (!unmapped_reported && sys.memory.faults().total() != 0) {
            unmapped_reported = true;
            std::fprintf(stderr,
                         "first unmapped access by frame %llu (%.3f guest s): pc 0x%08x pr 0x%08x "
                         "r15 0x%08x\n",
                         static_cast<unsigned long long>(sys.spg.frames()),
                         static_cast<double>(now) / 200e6, sys.ctx.pc, sys.ctx.pr, sys.ctx.r[15]);
            for (unsigned i = 0; i < 16; i += 4)
                std::fprintf(stderr, "  r%-2u %08x  r%-2u %08x  r%-2u %08x  r%-2u %08x\n", i,
                             sys.ctx.r[i], i + 1, sys.ctx.r[i + 1], i + 2, sys.ctx.r[i + 2], i + 3,
                             sys.ctx.r[i + 3]);
            unsigned shown = 0;
            for (const auto& r : sys.memory.faults().records()) {
                if (shown++ >= 8)
                    break;
                std::fprintf(stderr, "  %s%u 0x%08x x%llu\n", r.write ? "write" : "read", r.size,
                             r.addr, static_cast<unsigned long long>(r.count));
            }
            if (stop_on_unmapped)
                throw StopRun{"first unmapped memory access"};
        }
        if (max_frames && sys.spg.frames() >= max_frames)
            throw StopRun{"frame limit"};
        if (max_seconds && now >= max_seconds * 200'000'000ull)
            throw StopRun{"time limit"};
        sys.sched.request(tick, period);
    });
    sys.sched.request(tick, period);
    // Optional register sampler: every N cycles record pc (last call site), r15, r0, sr.
    struct Sample {
        std::uint64_t cycles;
        std::uint32_t pc, r15, r10, sr, pr;
    };
    std::vector<Sample> samples;
    int sampler = -1;
    if (sample_every) {
        sampler = sys.sched.add("sampler", [&](std::uint64_t now, std::uint64_t) {
            samples.push_back({now, sys.ctx.pc, sys.ctx.r[15], sys.ctx.r[10],
                               dream::sh4::read_sr(sys.ctx), sys.ctx.pr});
            sys.sched.request(sampler, sample_every);
        });
        sys.sched.request(sampler, sample_every);
    }

#ifdef DREAM_DEV_INTERPRETER
    // DREAM_INTERP_RANGE=lo:hi hides the translations in that range so the interpreter runs them
    // (bisection aid for translated-code bugs; see docs/runtime-devinterp.md).
    if (const char* range = std::getenv("DREAM_INTERP_RANGE")) {
        const char* colon = std::strchr(range, ':');
        if (colon)
            dream::sh4::set_interpret_range(
                static_cast<std::uint32_t>(std::strtoul(range, nullptr, 0)),
                static_cast<std::uint32_t>(std::strtoul(colon + 1, nullptr, 0)));
    }
    // DREAM_INTERP_FUNCS=addr,addr,... hides those individual functions, so a set can be shrunk
    // until putting any member back brings a fault return. A range finds the neighbourhood; this
    // finds the function.
    // DREAM_INTERP_FUNCS=lo:hi,lo:hi,... hides those whole functions, so a set can be shrunk until
    // putting any member back brings a fault return. A range finds the neighbourhood; this finds
    // the function.
    if (const char* list = std::getenv("DREAM_INTERP_FUNCS")) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> fns;
        for (const char* p = list; *p;) {
            char* end = nullptr;
            const unsigned long lo = std::strtoul(p, &end, 0);
            if (end == p)
                break;
            unsigned long hi = lo;
            if (*end == ':')
                hi = std::strtoul(end + 1, &end, 0);
            fns.push_back({static_cast<std::uint32_t>(lo), static_cast<std::uint32_t>(hi)});
            p = (*end == ',') ? end + 1 : end;
        }
        dream::sh4::set_interpret_functions(fns);
        std::printf("interpreting %zu named functions\n", fns.size());
    }
#endif
    // DREAM_WATCH_WRITE=addr[:len]: log every guest store into that range with the last call
    // site, PR and r15 (development aid).
    if (const char* wv = std::getenv("DREAM_WATCH_WRITE")) {
        const std::uint32_t lo =
            static_cast<std::uint32_t>(std::strtoul(wv, nullptr, 0)) & 0x1FFFFFFFu;
        const char* colon = std::strchr(wv, ':');
        const std::uint32_t len =
            colon ? static_cast<std::uint32_t>(std::strtoul(colon + 1, nullptr, 0)) : 4u;
        sys.memory.watch_lo = lo;
        sys.memory.watch_hi = lo + len;
        sys.memory.on_watch_write = [&](std::uint32_t a, std::uint32_t v, unsigned size) {
            std::fprintf(
                stderr,
                "watch: write%u 0x%08x = 0x%08x at pc 0x%08x pr 0x%08x r15 0x%08x frame %llu\n",
                size * 8, a, v, sys.ctx.pc, sys.ctx.pr, sys.ctx.r[15],
                static_cast<unsigned long long>(sys.spg.frames()));
        };
    }
#ifdef DREAM_DEV_INTERPRETER
    dream::devinterp::Replay replay(sys.ctx, sys.memory);
    if (replay_all || replay_self_check || !replay_only.empty()) {
        for (const char* p = replay_only.c_str(); *p;) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 0);
            if (end == p)
                break;
            replay.only.push_back(static_cast<std::uint32_t>(v));
            p = (*end == ',') ? end + 1 : end;
        }
        replay.interrupts = &sys.interrupts_delivered;
        replay.self_check = replay_self_check;
        dream::devinterp::set_replay(&replay);
        std::printf(
            "replay: comparing %s against %s\n",
            replay.only.empty() ? "every function" : "the named functions",
            replay_self_check ? "the translated code again (self-check)" : "the interpreter");
    }
#else
    (void)replay_all;
    (void)replay_self_check;
    (void)replay_only;
#endif
    dream::sh4::GuestFn entry = dream::sh4::find_function(cfg.entry);
    if (!entry) {
        std::fprintf(stderr, "no translated function at entry 0x%08x\n", cfg.entry);
        return 2;
    }
    const char* stop = "returned from the entry function";
    int rc = 0;
    sys.memory.refresh_fast_path();  // after the write hash / watch flags are final
    // DREAM_PROFILE=1 (dreamcomp): host time per scheduler event and in rendering, printed after
    // the report; the remainder is translated guest code, interrupt delivery and the HLE.
    const bool host_profile = [] {
        const char* e = std::getenv("DREAM_PROFILE");
        return e && *e && *e != '0';
    }();
    sys.sched.profile = host_profile;
    for (auto* e : dream::host::extensions()) e->on_start(sys);
    const auto t0 = std::chrono::steady_clock::now();
    auto run_title = [&] {
    if (!profile_file.empty())
        host_profiler_start(profile_file, profile_interval_us);
    struct ProfileStop {
        bool on;
        ~ProfileStop() {
            g_game_thread_cycles = thread_cpu_cycles();
            if (on)
                host_profiler_stop();
        }
    } profile_stop{!profile_file.empty()};
    try {
#ifdef DREAM_DEV_INTERPRETER
        if (interpret_all) {
            // Everything interpreted: the translated code is never entered, so a divergence
            // between this run and the translated one points at the emitter.
            dream::devinterp::Options iopt;
            iopt.call_translated = false;
            iopt.native_lo = dream::hle::Bios::kHookSystem;
            iopt.native_hi = dream::hle::Bios::kHookGd2 + 2;
            sys.interpret_all = true;
            sys.ctx.pc = cfg.entry;
            // A cooperative task switch abandons the guest context it was running in: rte()
            // throws NonLocalReturn past every host frame between there and here. run_guest()
            // drives that loop for translated code, and this path does not go through it, so
            // until now the first task switch under --interpret reached terminate -- and the
            // handler below could not have caught it either, NonLocalReturn being a plain struct
            // rather than a std::exception. Resuming an interpreted context needs nothing more
            // than interpreting from the new PC; there is no entry point to re-enter.
            for (;;) {
                try {
                    dream::devinterp::run(sys.ctx, sys.memory, 0, iopt, nullptr);
                    break;
                } catch (const dream::sh4::NonLocalReturn& n) {
                    if (n.pc == 0)
                        break;  // the return_to this run was started with
                    sys.ctx.pc = n.pc;
                }
            }
        } else
#endif
        {
            (void)interpret_all;
            (void)entry;
            dream::sh4::run_guest(sys.ctx, sys.memory, cfg.entry);
        }
    } catch (const StopRun& s) {
        stop = s.why;
    } catch (const std::exception& e) {
        stop = "fault";
        std::fprintf(stderr, "fault: %s (hooks at fault: %p, system: %p)\n", e.what(),
                     static_cast<void*>(dream::sh4::hooks()), static_cast<void*>(&sys));
        rc = 1;
    }
    };
#ifdef DREAM_WITH_RENDERER
    // dreamcomp: with a window, the guest runs on its own thread and this one only pumps window
    // events, so moving or resizing the window (a modal loop on Windows) does not stop the game.
    // DREAM_SINGLE_THREAD=1 keeps everything on one thread, as upstream does.
    const bool single_thread = [] {
        const char* e = std::getenv("DREAM_SINGLE_THREAD");
        return e && *e && *e != '0';
    }();
    if (live && !single_thread) {
        live->window.set_threaded(true);
        std::atomic<bool> guest_done{false};
        std::thread guest([&] {
            run_title();
            guest_done = true;
        });
        while (!guest_done.load()) live->window.pump_events(5);
        guest.join();
        live->window.set_threaded(false);
    } else
#endif
        run_title();
    for (auto* e : dream::host::extensions()) e->on_stop(sys, stop);
    const double host_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (host_profile && host_s > 0.0) {
        struct Row {
            std::string name;
            double s;
            std::uint64_t calls;
        };
        std::vector<Row> rows;
        double accounted = 0.0;
        for (std::size_t i = 0; i < sys.sched.events(); ++i) {
            const auto& st = sys.sched.stat(static_cast<int>(i));
            if (!st.calls)
                continue;
            rows.push_back({sys.sched.name(static_cast<int>(i)), st.ns * 1e-9, st.calls});
            accounted += st.ns * 1e-9;
        }
        if (g_renders) {
            rows.push_back({"pvr render (TA list -> GPU)", g_render_ns * 1e-9, g_renders});
            accounted += g_render_ns * 1e-9;
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.s > b.s; });
        std::printf("host profile (%.2f s):\n", host_s);
        for (const auto& r : rows)
            std::printf("  %6.1f%%  %8.3f s  %10llu calls  %s\n", 100.0 * r.s / host_s, r.s,
                        static_cast<unsigned long long>(r.calls), r.name.c_str());
        std::printf("  %6.1f%%  %8.3f s  %10s        guest code, interrupts, HLE (the rest)\n",
                    100.0 * (host_s - accounted) / host_s, host_s - accounted, "");
    }
#ifdef DREAM_DEV_INTERPRETER
    if (dream::devinterp::replay()) {
        dream::devinterp::set_replay(nullptr);
        std::printf(
            "replay: %llu calls compared, %llu skipped (device writes %llu, interrupts %llu, "
            "non-local returns %llu, too long %llu), %zu disagreements\n",
            static_cast<unsigned long long>(replay.compared),
            static_cast<unsigned long long>(replay.skipped_device + replay.skipped_interrupt +
                                            replay.skipped_nonlocal + replay.skipped_too_long),
            static_cast<unsigned long long>(replay.skipped_device),
            static_cast<unsigned long long>(replay.skipped_interrupt),
            static_cast<unsigned long long>(replay.skipped_nonlocal),
            static_cast<unsigned long long>(replay.skipped_too_long), replay.divergences().size());
        std::printf(
            "replay: %llu calls entered in a floating-point mode the emitter did not "
            "compile for, in %zu functions\n",
            static_cast<unsigned long long>(replay.mode_mismatches),
            replay.mode_mismatch_functions.size());
        for (std::uint32_t f : replay.mode_mismatch_functions) std::printf("  fn_%08x\n", f);
        for (const auto& d : replay.divergences())
            std::printf("  fn_%08x: %s\n", d.function, d.what.c_str());
    }
#endif
#ifdef DREAM_WITH_AUDIO
    if (sink.is_open()) {
        sink.flush();
        std::printf("audio: %llu samples played in %llu blocks, %llu dropped, %llu underruns, "
                    "%u still queued, playback ratio %.4f\n",
                    static_cast<unsigned long long>(sink.pushed),
                    static_cast<unsigned long long>(sink.blocks),
                    static_cast<unsigned long long>(sink.dropped),
                    static_cast<unsigned long long>(sink.underruns), sink.queued_samples(),
                    static_cast<double>(sink.ratio));
    }
#endif
    if (hash_file) {
        std::fprintf(hash_file, "%llu %llu %016llx %llu\n",
                     static_cast<unsigned long long>(sys.spg.frames()),
                     static_cast<unsigned long long>(sys.ctx.cycles),
                     static_cast<unsigned long long>(sys.memory.write_hash),
                     static_cast<unsigned long long>(sys.memory.writes_hashed));
        std::fclose(hash_file);
        std::printf("wrote %s: %llu writes hashed\n", write_hash.c_str(),
                    static_cast<unsigned long long>(sys.memory.writes_hashed));
    }
    if (log_file) {
        std::fclose(log_file);
        std::printf("wrote %s\n", write_log.c_str());
    }
#ifdef DREAM_WITH_RENDERER
    if (live) {
        std::printf(
            "window: %llu frames drawn (%llu with a background plane), %llu presented (%llu "
            "decoded from video memory), %llu written back; textures %u decoded, %u failed, %u "
            "overwritten by a render, %llu palette changes\n",
            static_cast<unsigned long long>(live->rendered),
            static_cast<unsigned long long>(live->backgrounds),
            static_cast<unsigned long long>(live->presented),
            static_cast<unsigned long long>(live->decoded_frames),
            static_cast<unsigned long long>(live->written_back), live->renderer.textures().decoded,
            live->renderer.textures().failed, live->renderer.textures().overwritten,
            static_cast<unsigned long long>(live->palette_changes));
        if (const auto& r = live->renderer.textures().replacer(); r.active())
            std::printf(
                "textures: %llu hashed (%.1f ms), %u replaced from the pack (%u failed to load, "
                "%.1f ms loading), %u dumped (%.1f ms writing)\n",
                static_cast<unsigned long long>(r.hashed), r.hash_ms, r.replaced,
                r.replace_failed, r.load_ms, r.dumped, r.dump_ms);
        if (live->interpolate)
            std::printf(
                "interpolation: %llu blended frames drawn, %llu presented, %llu camera cuts; "
                "%.1f%% of vertices paired, %llu strips not blended (implausible jump); "
                "%.2f ms per blend\n",
                static_cast<unsigned long long>(live->interp_rendered),
                static_cast<unsigned long long>(live->interp_presented),
                static_cast<unsigned long long>(live->interp_cuts),
                live->interp_vertices ? 100.0 * static_cast<double>(live->interp_matched) /
                                            static_cast<double>(live->interp_vertices)
                                      : 0.0,
                static_cast<unsigned long long>(live->interp_jumped),
                live->interp_rendered ? live->interp_ms / static_cast<double>(live->rendered) : 0.0);
        live->pacing.report();
        live->sync.report();
        std::printf("textures: %u decoded in %.1f ms, uploads recorded in %.1f ms, %llu dropped "
                    "because the guest rewrote them\n",
                    live->renderer.textures().decoded,
                    static_cast<double>(live->renderer.textures().decode_ns) * 1e-6,
                    static_cast<double>(live->renderer.textures().upload_ns) * 1e-6,
                    static_cast<unsigned long long>(live->vram_invalidated));
        if (live->have_frame)
            std::printf("window: last shown %s\n", live->shown.describe().c_str());
        // Vulkan objects are destroyed now rather than at scope exit, while everything they were
        // created from is still alive.
        live.reset();
    }
#endif
    if (!dump_vram.empty()) {
        if (FILE* f = std::fopen(dump_vram.c_str(), "wb")) {
            std::fwrite(sys.memory.vram(), 1, dream::mem::DcMemory::kVramSize, f);
            std::fclose(f);
            std::printf("wrote %s: %u bytes of video memory\n", dump_vram.c_str(),
                        dream::mem::DcMemory::kVramSize);
        }
        // The PVR register block carries palette memory (at +0x1000) and its format.
        const std::string regs_path = dump_vram + ".regs";
        if (FILE* f = std::fopen(regs_path.c_str(), "wb")) {
            for (std::uint32_t off = 0; off < 0x2000; off += 4) {
                const std::uint32_t w = pvr.reg(off);
                std::fwrite(&w, 4, 1, f);
            }
            std::fclose(f);
        }
    }
    if (!dump_aram.empty()) {
        if (FILE* f = std::fopen(dump_aram.c_str(), "wb")) {
            std::fwrite(sys.memory.aram(), 1, dream::mem::DcMemory::kAramSize, f);
            std::fclose(f);
        }
    }
    {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> top;
        for (std::size_t i = 0; i < arm_pcs.size(); ++i)
            if (arm_pcs[i])
                top.push_back({arm_pcs[i], static_cast<std::uint32_t>(i * 4)});
        std::sort(top.rbegin(), top.rend());
        std::printf("ARM7 pc histogram (per sample tick):");
        for (std::size_t i = 0; i < top.size() && i < 12; ++i)
            std::printf(" %08x:%llu", top[i].second, static_cast<unsigned long long>(top[i].first));
        std::printf("\n");
    }
    if (!wav.empty()) {
        if (FILE* f = std::fopen(wav.c_str(), "wb")) {
            const std::uint32_t data_bytes = static_cast<std::uint32_t>(pcm.size() * 2);
            auto put32 = [&](std::uint32_t v) {
                const unsigned char b[4] = {
                    static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                    static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
                std::fwrite(b, 1, 4, f);
            };
            auto put16 = [&](std::uint32_t v) {
                const unsigned char b[2] = {static_cast<unsigned char>(v),
                                            static_cast<unsigned char>(v >> 8)};
                std::fwrite(b, 1, 2, f);
            };
            std::fwrite("RIFF", 1, 4, f);
            put32(36 + data_bytes);
            std::fwrite("WAVEfmt ", 1, 8, f);
            put32(16);
            put16(1);      // PCM
            put16(2);      // stereo
            put32(44100);  // rate
            put32(44100 * 4);
            put16(4);
            put16(16);
            std::fwrite("data", 1, 4, f);
            put32(data_bytes);
            for (std::int16_t v : pcm) put16(static_cast<std::uint16_t>(v));
            std::fclose(f);
            std::printf("wrote %s: %zu frames (%.1f s)\n", wav.c_str(), pcm.size() / 2,
                        static_cast<double>(pcm.size() / 2) / 44100.0);
        }
    }
    const std::string text = report_text(sys, bios, pvr, stop, host_s, bytes, cfg.link_address,
                                         maple, pad_ptr, card_ptr, aica, g2, sysblock, dmac);
    std::fputs(text.c_str(), stdout);
    {
        const std::string rel = suggest_relocations(sys, bytes, cfg.link_address, cfg.load_address,
                                                    ipbin, cfg.relocations);
        if (!rel.empty())
            std::printf(
                "run-time copies of image code below the load address (candidate relocations):\n%s",
                rel.c_str());
        const std::string entries =
            suggest_entries(sys, cfg.link_address, bytes.size(), cfg.extra_functions);
        if (!suggest_config.empty()) {
            // One file, both halves, in the order they go into the TOML. Written even when empty
            // apart from a note, because "the run found nothing to add" is an answer and an
            // absent file is not.
            if (FILE* f = std::fopen(suggest_config.c_str(), "wb")) {
                std::fprintf(f, "# Suggested by %s from a run that stopped: %s.\n", argv[0], stop);
                if (entries.empty() && rel.empty())
                    std::fprintf(f,
                                 "# Nothing to add: every call target this run reached was "
                                 "already translated.\n");
                if (!entries.empty())
                    std::fputs(entries.c_str(), f);
                if (!rel.empty()) {
                    std::fprintf(f,
                                 "\n# Regions the program copied out of the image and ran from "
                                 "somewhere else.\n");
                    std::fputs(rel.c_str(), f);
                }
                std::fclose(f);
                std::printf("wrote %s: %s\n", suggest_config.c_str(),
                            entries.empty() && rel.empty()
                                ? "nothing to add"
                                : "config to paste into the game's TOML, then rebuild");
            } else {
                std::fprintf(stderr, "cannot write %s\n", suggest_config.c_str());
            }
        } else if (!entries.empty()) {
            std::printf(
                "call targets discovery never reached (--suggest-config FILE writes this "
                "as TOML):\n%s",
                entries.c_str());
        }
    }
    if (!flash_dump.empty()) {
        std::ofstream o(flash_dump, std::ios::binary);
        o.write(reinterpret_cast<const char*>(sys.memory.flash()), dream::hle::Flash::kSize);
    }
    if (!dump.empty()) {  // --dump ADDR:LEN: hex dump of guest memory at stop
        const auto colon = dump.find(':');
        const std::uint32_t da =
            static_cast<std::uint32_t>(std::strtoul(dump.substr(0, colon).c_str(), nullptr, 0));
        const std::uint32_t dl =
            static_cast<std::uint32_t>(std::strtoul(dump.substr(colon + 1).c_str(), nullptr, 0));
        for (std::uint32_t i = 0; i < dl; i += 16) {
            std::printf("%08x:", da + i);
            for (std::uint32_t j = 0; j < 16 && i + j < dl; j += 4)
                std::printf(" %08x", sys.memory.read32(da + i + j));
            std::printf("\n");
        }
    }
    if (!scif.transmitted.empty())
        std::printf("serial output (%zu bytes):\n%s\n", scif.transmitted.size(),
                    scif.transmitted.c_str());
    if (!sample_file.empty()) {
        std::ofstream o(sample_file);
        for (const auto& smp : samples) {
            char line[128];
            std::snprintf(line, sizeof line, "%llu %08x %08x %08x %08x %08x\n",
                          static_cast<unsigned long long>(smp.cycles), smp.pc, smp.r15, smp.r10,
                          smp.sr, smp.pr);
            o << line;
        }
    }
    if (!samples.empty() && sample_file.empty()) {
        std::printf("samples (first 40):\n");
        for (std::size_t i = 0; i < std::min<std::size_t>(samples.size(), 40); ++i)
            std::printf("  %12llu pc %08x r15 %08x r10 %08x sr %08x pr %08x\n",
                        static_cast<unsigned long long>(samples[i].cycles), samples[i].pc,
                        samples[i].r15, samples[i].r10, samples[i].sr, samples[i].pr);
        std::printf("samples (last %zu):\n", std::min<std::size_t>(samples.size(), 24));
        for (std::size_t i = samples.size() > 24 ? samples.size() - 24 : 0; i < samples.size(); ++i)
            std::printf("  %12llu pc %08x r15 %08x r10 %08x sr %08x pr %08x\n",
                        static_cast<unsigned long long>(samples[i].cycles), samples[i].pc,
                        samples[i].r15, samples[i].r10, samples[i].sr, samples[i].pr);
    }
    if (!report.empty()) {
        std::ofstream o(report);
        o << text;
    }
    if (stop_on_ta && !reached_ta && rc == 0)
        rc = 1;
    return rc;
}

// ---- crash reports (dreamcomp) ------------------------------------------------------------------
// A player's crash used to leave an empty log: stdout is buffered and dies with the process. Now
// stdout is unbuffered, and an unhandled exception on any thread prints what, where (module and
// offset, for the map file) and on which thread, and writes a minidump beside the bindings file
// (%APPDATA%/dream-recomp/dream-recomp/crash-<time>.dmp) for a debugger.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

#pragma pack(push, 4)  // as dbghelp.h declares MINIDUMP_EXCEPTION_INFORMATION
struct MiniDumpExceptionInfo {
    DWORD ThreadId;
    EXCEPTION_POINTERS* ExceptionPointers;
    BOOL ClientPointers;
};
#pragma pack(pop)
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, MiniDumpExceptionInfo*,
                                          void*, void*);

LONG WINAPI on_crash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    void* addr = r->ExceptionAddress;
    char module[MAX_PATH] = "?";
    HMODULE mod = nullptr;
    std::uintptr_t offset = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &mod)) {
        GetModuleFileNameA(mod, module, sizeof module);
        offset = reinterpret_cast<std::uintptr_t>(addr) - reinterpret_cast<std::uintptr_t>(mod);
    }
    std::fprintf(stderr,
                 "\ncrash: exception 0x%08lX at %p (%s + 0x%llx) on thread %lu",
                 static_cast<unsigned long>(r->ExceptionCode), addr, module,
                 static_cast<unsigned long long>(offset),
                 static_cast<unsigned long>(GetCurrentThreadId()));
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        std::fprintf(stderr, ", %s of %p", r->ExceptionInformation[0] == 1 ? "write" : "read",
                     reinterpret_cast<void*>(r->ExceptionInformation[1]));
    std::fprintf(stderr, "\n");
    // The dump: where the bindings live, which every port has and the player can find.
    std::string dir = dream::render::vk::default_bindings_path();
    const auto slash = dir.find_last_of("/\\");
    dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
    char name[64];
    std::snprintf(name, sizeof name, "crash-%llu.dmp",
                  static_cast<unsigned long long>(std::time(nullptr)));
    const std::string path = dir + name;
    if (HMODULE dbghelp = LoadLibraryA("dbghelp.dll")) {
        auto write = reinterpret_cast<MiniDumpWriteDumpFn>(
            reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
        HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (write && file != INVALID_HANDLE_VALUE) {
            MiniDumpExceptionInfo info{GetCurrentThreadId(), ep, FALSE};
            // MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo: small, with stacks.
            if (write(GetCurrentProcess(), GetCurrentProcessId(), file, 0x40 | 0x1000, &info,
                      nullptr, nullptr))
                std::fprintf(stderr, "crash: minidump written to %s\n", path.c_str());
        }
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }
    std::fflush(stderr);
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void install_crash_handler() {
    // Unbuffered: what was printed before a crash is in the log after it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(on_crash);
}

__declspec(noinline) void precise_sleep_until(std::chrono::steady_clock::time_point t) {
    // One high-resolution timer per thread (Windows 10 1803+); without one, plain sleep.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    // DREAM_SPIN_MS=N busy-waits the last N ms of each wait. A laptop CPU clocks down while the
    // game thread sleeps, and emulating a frame then takes about twice as long (i5-1335U: 11 ms
    // against 5); spinning 3 ms keeps it up, for ~5 ms fresher input at the cost of power and
    // heat. Off by default (docs/PACING.md in dreamcomp).
    static const auto spin = std::chrono::microseconds(static_cast<long long>(
        (std::getenv("DREAM_SPIN_MS") ? std::atof(std::getenv("DREAM_SPIN_MS")) : 0.0) * 1000));
    if (spin.count() > 0) {
        const auto until = t - spin;
        if (until > std::chrono::steady_clock::now())
            precise_sleep_until(until);
        while (std::chrono::steady_clock::now() < t) {}
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (t <= now)
        return;
    if (!timer) {
        std::this_thread::sleep_until(t);
        return;
    }
    // Relative due time in 100 ns units (negative = relative).
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(t - now).count() / 100);
    if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
        WaitForSingleObject(timer, INFINITE);
    else
        std::this_thread::sleep_until(t);
}
// ---- host sampling profiler (dreamcomp) ------------------------------------------------------
// A sampler thread suspends the game thread about every `interval`, reads its registers, walks its
// stack with the x64 unwind tables (every non-leaf function has them, generated code included) and
// resumes it. Nothing is allocated while the thread is suspended: it may hold the heap lock. Stacks
// are aggregated as module-relative frames and written at stop as
//     <count> <module>+<offset>;<module>+<offset>;...      (innermost frame first)
// after a "module <name> <size>" line per module seen.
namespace {
struct HostProfiler {
    HANDLE thread = nullptr, sampler = nullptr;
    std::atomic<bool> stop{false};
    unsigned interval_us = 1000;
    std::string file;
    std::unordered_map<std::string, std::uint64_t> stacks;  // "frames" -> count
    std::uint64_t samples = 0, failed = 0;
    std::chrono::steady_clock::time_point started;
};
HostProfiler* g_prof = nullptr;

std::string module_frame(DWORD64 pc, std::unordered_map<DWORD64, std::string>& names) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(pc), &mod) || !mod) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "?+%llx", static_cast<unsigned long long>(pc));
        return buf;
    }
    const DWORD64 base = reinterpret_cast<DWORD64>(mod);
    auto it = names.find(base);
    if (it == names.end()) {
        wchar_t wpath[MAX_PATH] = {};
        GetModuleFileNameW(mod, wpath, MAX_PATH);
        const wchar_t* slash = std::wcsrchr(wpath, L'\\');
        char name[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : wpath, -1, name, MAX_PATH - 1, nullptr, nullptr);
        it = names.emplace(base, name).first;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "+%llx", static_cast<unsigned long long>(pc - base));
    return it->second + buf;
}

DWORD WINAPI sampler_main(void*) {
    HostProfiler& p = *g_prof;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    constexpr int kMaxFrames = 64;
    DWORD64 pcs[kMaxFrames];
    std::unordered_map<DWORD64, std::string> names;
    std::string key;
    while (!p.stop.load()) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(p.interval_us) * 10;
        if (timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
            WaitForSingleObject(timer, INFINITE);
        else
            Sleep(1);
        if (SuspendThread(p.thread) == static_cast<DWORD>(-1)) {
            ++p.failed;
            continue;
        }
        int n = 0;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        // GetThreadContext also waits for the suspension to take effect.
        if (GetThreadContext(p.thread, &ctx)) {
            while (n < kMaxFrames && ctx.Rip) {
                pcs[n++] = ctx.Rip;
                DWORD64 image = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
                if (!fn) {
                    // A leaf function: the return address is on top of the stack.
                    ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                    ctx.Rsp += 8;
                    continue;
                }
                void* handler_data = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &frame, nullptr);
            }
        }
        ResumeThread(p.thread);
        if (n == 0) {
            ++p.failed;
            continue;
        }
        // Resumed: now it is safe to allocate.
        key.clear();
        for (int i = 0; i < n; ++i) {
            if (i)
                key += ';';
            key += module_frame(pcs[i], names);
        }
        ++p.stacks[key];
        ++p.samples;
    }
    if (timer)
        CloseHandle(timer);
    return 0;
}
}  // namespace

std::uint64_t thread_cpu_cycles() {
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
}

void host_profiler_start(const std::string& file, unsigned interval_us) {
    if (g_prof)
        return;
    g_prof = new HostProfiler;
    g_prof->file = file;
    g_prof->interval_us = std::max(100u, interval_us);
    g_prof->started = std::chrono::steady_clock::now();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_prof->thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    g_prof->sampler = CreateThread(nullptr, 0, sampler_main, nullptr, 0, nullptr);
    std::printf("profile: sampling the game thread every %u us into %s\n", g_prof->interval_us,
                file.c_str());
}

void host_profiler_stop() {
    if (!g_prof)
        return;
    HostProfiler& p = *g_prof;
    p.stop = true;
    if (p.sampler) {
        WaitForSingleObject(p.sampler, INFINITE);
        CloseHandle(p.sampler);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - p.started).count();
    FILE* f = std::fopen(p.file.c_str(), "w");
    if (f) {
        std::fprintf(f, "# dreamcomp host profile: %llu samples over %.2f s, interval %u us, %llu failed\n",
                     static_cast<unsigned long long>(p.samples), secs, p.interval_us,
                     static_cast<unsigned long long>(p.failed));
        for (const auto& [stack, count] : p.stacks)
            std::fprintf(f, "%llu %s\n", static_cast<unsigned long long>(count), stack.c_str());
        std::fclose(f);
        std::printf("profile: %llu samples (%.0f per second, %llu failed) written to %s\n",
                    static_cast<unsigned long long>(p.samples),
                    secs > 0 ? static_cast<double>(p.samples) / secs : 0.0,
                    static_cast<unsigned long long>(p.failed), p.file.c_str());
    } else {
        std::fprintf(stderr, "profile: cannot write %s (%llu samples lost)\n", p.file.c_str(),
                     static_cast<unsigned long long>(p.samples));
    }
    if (p.thread)
        CloseHandle(p.thread);
    delete g_prof;
    g_prof = nullptr;
}
#else
void install_crash_handler() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
}
void host_profiler_start(const std::string&, unsigned) {
    std::printf("profile: --profile is only implemented on Windows so far\n");
}
std::uint64_t thread_cpu_cycles() { return 0; }
void host_profiler_stop() {}
void precise_sleep_until(std::chrono::steady_clock::time_point t) {
    std::this_thread::sleep_until(t);
}
#endif
