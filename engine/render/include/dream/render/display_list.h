// Display-list decoding (WP2.3 step 2): the PowerVR2 Tile Accelerator's parameter stream turned
// into triangle strips with full render state. Device-independent on purpose, so it is unit-tested
// on every CI runner with no GPU and no window; the Vulkan backend consumes what this produces.
//
// The hardware receives 32-byte parameter chunks through a FIFO. Each chunk is one of: a polygon
// header (which opens a strip and sets the render state), a sprite header, a modifier-volume
// header, a vertex, or an end-of-list marker. Some headers and vertices are 64 bytes, and which
// ones depends on bits of the parameter control word, so the parser is a small state machine.
// Vertex layout varies further with the colour type, whether the polygon is textured, whether the
// texture coordinates are 16-bit, and whether two volumes are in use: sixteen combinations.
//
// Reference: Flycast's core/hw/pvr/ta_vtx.cpp and ta_structs.h (GPL-2.0, ADR 1). The structures
// here are our own, chosen so a strip is contiguous and the backend can draw it without a second
// pass.
#pragma once

#include <limits>
#include <array>
#include <cstdint>
#include <vector>

namespace dream::render {

// The five display lists, in the order the hardware numbers them.
enum class ListType : unsigned {
    Opaque = 0,
    OpaqueModifier = 1,
    Translucent = 2,
    TranslucentModifier = 3,
    PunchThrough = 4,
    Count = 5
};
constexpr unsigned kListCount = 5;

// A vertex as the renderer wants it. Screen-space x and y in pixels, z is 1/w (larger is nearer),
// colours are ARGB8888 whatever the source encoding was.
struct Vertex {
    float x = 0, y = 0, z = 0;
    float u = 0, v = 0;
    std::uint32_t base = 0;    // base colour
    std::uint32_t offset = 0;  // offset (specular) colour, 0 when the polygon has none
};

// One triangle strip, with the raw hardware words that describe how to draw it. The backend
// decodes those words itself; keeping them raw here means this layer does not have to model
// blending, depth or texture state, and cannot get them subtly wrong.
struct Polygon {
    std::uint32_t first = 0;  // index into Frame::vertices
    std::uint32_t count = 0;  // vertices in the strip (>= 3), a triangle strip
    std::uint32_t pcw = 0;    // parameter control word
    std::uint32_t isp = 0;    // ISP/TSP instruction word: depth, culling
    std::uint32_t tsp = 0;    // TSP instruction word: blending, filtering, fog
    std::uint32_t tcw = 0;    // texture control word: format and address
    // Second volume, for two-volume polygons; 0xFFFFFFFF when unused.
    std::uint32_t tsp1 = 0xFFFFFFFFu, tcw1 = 0xFFFFFFFFu;
    std::uint32_t tile_clip = 0;  // user tile clip mode, from the parameter control word
    // The user clip rectangle in force for this polygon, in guest pixels (dreamcomp): the last
    // user-tile-clip parameter's tiles, 32 pixels each, inclusive. Mode 2 draws only inside it,
    // mode 3 only outside (Flycast tileclip / drawer SetTileClip).
    std::uint16_t clip_x0 = 0, clip_y0 = 0, clip_x1 = 1280, clip_y1 = 480;
    // Translucent sort keys of the triangle and strip this polygon was cut from by
    // clip_far_polygons (dreamcomp); NaN otherwise. A piece must sort where its parent would: the
    // far vertex it lost is what made the parent sort nearest.
    float sort_triangle = std::numeric_limits<float>::quiet_NaN();
    float sort_strip = std::numeric_limits<float>::quiet_NaN();
};

// A modifier volume triangle (shadows and volume effects). These are not strips: each is three
// independent vertices with position only.
struct ModifierTriangle {
    float x[3] = {}, y[3] = {}, z[3] = {};
    std::uint32_t isp = 0;
    std::uint32_t header = 0;  // which modifier-volume header it follows (dreamcomp): the unit the
                               // hardware's volume instructions apply to
    bool volume_last = false;  // the header's PCW Volume bit: last polygon of a volume (Flycast)
    bool translucent = false;  // from the translucent modifier list (not drawn, as in Flycast)
};

// Everything one render needs.
struct Frame {
    std::vector<Vertex> vertices;
    std::array<std::vector<Polygon>, kListCount> lists;
    std::vector<ModifierTriangle> modifiers;
    // The depth range the frame actually uses, for the projection the backend sets up.
    float min_z = 0, max_z = 0;
    // Diagnostics, and what the headless tests assert on.
    std::uint32_t strips = 0, sprites = 0, dropped_vertices = 0, unknown_parameters = 0;

    void clear();
    std::size_t polygon_count() const noexcept;
};

// Whether a strip actually repeats its texture, meaning any of its vertices carries a texture
// coordinate outside the unit square. This is the difference between a texture the hardware wraps
// and one the game merely maps once across the polygon, and it decides how the backend addresses
// the texture's edge.
//
// It matters only above the guest's own resolution. At 640x480 every pixel centre lands on a texel
// centre and no sample ever reaches past the edge, so wrapping is unobservable. Drawing the same
// geometry at twice the size puts the outermost half-texel outside the texture, where repeat
// addressing fetches the opposite edge and leaves a seam along every quad boundary: Crazy Taxi's
// title screen is 240 quads sharing one 16x16 texture, and at --scale 2 every one of them was
// outlined. Clamping instead is right whenever the polygon never leaves the unit square, and does
// not touch the interior boundary between two quads that share a larger texture.
bool polygon_tiles(const Frame& frame, const Polygon& polygon);

// Clips the polygons that reach far off the screen to a rectangle `limit` guest pixels around it
// (dreamcomp), and returns how many it clipped. A game hands the PowerVR geometry that passes
// behind the camera: Ecco's water surface has vertices at x = -56,000,000 with 1/w = 1000. The
// PowerVR copes, and so does NVIDIA's clipper; AMD's (RADV, the PS5) loses so much precision on
// a triangle that size that its depth comes out wrong in bands, and the surface showed through
// the rock in stripes. Everything the shaders interpolate is linear in screen space here (1/w,
// and colour and texture coordinates multiplied by it), so the cut is exact: an affected strip
// becomes separate triangles (same words, same facing), each clipped, each fanned back out.
// Modifier-volume triangles are clipped the same way (their pieces in their place).
std::size_t clip_far_polygons(Frame& frame, float limit = 2048.0f);

// Feed it the same 32-byte chunks the Tile Accelerator FIFO receives.
class DisplayList {
public:
    DisplayList() { reset(); }

    void reset();
    void feed(const std::uint32_t words[8]);
    // Convenience for a buffered stream (pvr::Core keeps one).
    void feed_stream(const std::uint32_t* words, std::size_t count);

    const Frame& frame() const noexcept { return frame_; }
    Frame& frame() noexcept { return frame_; }
    // True while a list is open (between a first parameter and its end-of-list marker).
    bool list_open() const noexcept { return list_ < kListCount; }

private:
    // Which 64-byte second half we are waiting for, if any.
    enum class Pending : unsigned { None, PolyHeader, Vertex, SpriteVertex, ModifierVertex };

    void handle_header(const std::uint32_t w[8]);
    void handle_vertex(const std::uint32_t w[8], bool have_second_half,
                       const std::uint32_t second[8]);
    void handle_sprite_vertex(const std::uint32_t w[8], const std::uint32_t second[8]);
    void handle_modifier_vertex(const std::uint32_t first[8], const std::uint32_t second[8]);
    void end_strip();
    void close_list();
    void note_z(float z) noexcept;

    Frame frame_;
    unsigned list_ = kListCount;  // kListCount: no list open
    Polygon current_{};           // the strip being built
    // A polygon header stays in force until another header or the end of the list: one header can
    // be followed by several strips, each closed by an end-of-strip flag.
    bool header_active_ = false;
    std::uint32_t modifier_headers_ = 0;
    bool modifier_last_ = false, modifier_translucent_ = false;
    std::uint16_t clip_x0_ = 0, clip_y0_ = 0, clip_x1_ = 1280, clip_y1_ = 480;
    Pending pending_ = Pending::None;
    std::uint32_t held_[8]{};  // first half of a 64-byte parameter

    // Face colours, used by the intensity colour modes. Colour type 3 keeps the previous
    // polygon's face colour rather than carrying its own.
    std::uint32_t face_base_ = 0xFFFFFFFFu, face_offset_ = 0;
    // Sprite state: the header's colours apply to all four corners.
    std::uint32_t sprite_base_ = 0xFFFFFFFFu, sprite_offset_ = 0;
    bool in_sprite_ = false;
};

}  // namespace dream::render
