// See renderer.h.
#include <cmath>
#include "dream/render/vk/renderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "dream/render/tsp.h"

#include "geometry_frag.h"
#include "geometry_vert.h"
#include "modvol_frag.h"
#include "modvol_vert.h"

namespace dream::render::vk {

namespace {

// Vertex layout handed to the shaders: position (x, y, 1/w), texture coordinates, base colour and
// offset colour as floats. Nine floats; unpacking the colours here keeps the shader simple and the
// cost is a frame's worth of arithmetic on the CPU, which is nothing beside the draw calls.
constexpr std::size_t kFloatsPerVertex = 3 + 2 + 4 + 4 + 1;  // ... + table fog factor

// ISP/TSP instruction word (Flycast's ISP_TSP union): the fields that decide pipeline state.
constexpr std::uint32_t isp_depth_mode(std::uint32_t isp) {
    return (isp >> 29) & 7u;
}
constexpr std::uint32_t isp_cull_mode(std::uint32_t isp) {
    return (isp >> 27) & 3u;
}
constexpr std::uint32_t isp_z_write_disable(std::uint32_t isp) {
    return (isp >> 26) & 1u;
}

// The hardware's eight depth comparisons, in its own order. The depth buffer holds a logarithm of
// 1/w, so "nearer" is a larger value and the senses are the reverse of the usual convention.
VkCompareOp depth_compare(std::uint32_t mode) {
    switch (mode) {
        case 0:
            return VK_COMPARE_OP_NEVER;
        case 1:
            return VK_COMPARE_OP_LESS;
        case 2:
            return VK_COMPARE_OP_EQUAL;
        case 3:
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        case 4:
            return VK_COMPARE_OP_GREATER;
        case 5:
            return VK_COMPARE_OP_NOT_EQUAL;
        case 6:
            return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default:
            return VK_COMPARE_OP_ALWAYS;
    }
}

// Culling is by signed area on the hardware, with two thresholds we cannot express directly. Modes
// 2 and 3 are the ordinary back-face and front-face cases; 0 and 1 disable it.
VkCullModeFlags cull_flags(std::uint32_t mode) {
    switch (mode) {
        case 2:
            return VK_CULL_MODE_FRONT_BIT;
        case 3:
            return VK_CULL_MODE_BACK_BIT;
        default:
            return VK_CULL_MODE_NONE;
    }
}

void unpack_colour(std::uint32_t argb, float* out) {
    out[0] = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;  // r
    out[1] = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;   // g
    out[2] = static_cast<float>(argb & 0xFFu) / 255.0f;          // b
    out[3] = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;  // a
}

VkShaderModule make_module(VkDevice device, const std::uint32_t* code, std::size_t bytes) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &m);
    return m;
}

struct PushConstants {
    float scale[2];
    float offset[2];
    std::int32_t mode;
    float alpha_ref;
    float pad[2];       // the shader's vec4 members start on a 16-byte boundary
    float fog_vert[4];  // FOG_COL_VERT (per-vertex fog), RGBA
    float fog_ram[4];   // FOG_COL_RAM (table fog), RGBA
    float clip[4];      // user clip mode 3: discard inside this rectangle (target pixels)
};
static_assert(offsetof(PushConstants, fog_vert) == 32, "push constants must match the shaders");

// What identifies a texture within one frame: the control word, plus the TSP bits that carry the
// size. Everything the sampler reads is decided per texture in prepare(), so it does not belong
// here; two polygons that share these bits share the decoded image.
constexpr std::uint64_t texture_key(const Polygon& p) {
    // Size bits, plus the sampler bits (TSP 8-18), which choose the descriptor set too.
    return (static_cast<std::uint64_t>(p.tcw) << 32) | (p.tsp & 0x3Fu) |
           (static_cast<std::uint64_t>((p.tsp >> 8) & 0x7FFu) << 7);
}

// Parameter control word and TSP fields the fragment shader needs.
constexpr std::uint32_t pcw_texture(std::uint32_t p) {
    return (p >> 3) & 1u;
}
constexpr std::uint32_t pcw_offset(std::uint32_t p) {
    return (p >> 2) & 1u;
}
constexpr std::uint32_t pcw_shadow(std::uint32_t p) {
    return (p >> 7) & 1u;
}
// The hardware's eight blend factors. Index 2 and 3 mean "the other colour", which is the
// destination colour for a source factor and the source colour for a destination factor; the rest
// are the same on both sides.
VkBlendFactor src_factor(std::uint32_t instr) {
    switch (instr) {
        case 0:
            return VK_BLEND_FACTOR_ZERO;
        case 1:
            return VK_BLEND_FACTOR_ONE;
        case 2:
            return VK_BLEND_FACTOR_DST_COLOR;
        case 3:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 4:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case 5:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 6:
            return VK_BLEND_FACTOR_DST_ALPHA;
        default:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
}
VkBlendFactor dst_factor(std::uint32_t instr) {
    switch (instr) {
        case 0:
            return VK_BLEND_FACTOR_ZERO;
        case 1:
            return VK_BLEND_FACTOR_ONE;
        case 2:
            return VK_BLEND_FACTOR_SRC_COLOR;
        case 3:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 4:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case 5:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 6:
            return VK_BLEND_FACTOR_DST_ALPHA;
        default:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
}

}  // namespace

Renderer::~Renderer() {
    destroy();
}

bool Renderer::create(Context& ctx, VkRenderPass render_pass) {
    ctx_ = &ctx;
    render_pass_ = render_pass;
    vs_ = make_module(ctx.device(), geometry_vert, sizeof geometry_vert);
    fs_ = make_module(ctx.device(), geometry_frag, sizeof geometry_frag);
    mv_vs_ = make_module(ctx.device(), modvol_vert, sizeof modvol_vert);
    mv_fs_ = make_module(ctx.device(), modvol_frag, sizeof modvol_frag);
    // The offscreen target picks the same depth format; with a stencil, modifier volumes draw.
    stencil_ = format_has_stencil(pick_depth_format(ctx.physical_device())) && mv_vs_ && mv_fs_;
    if (!vs_ || !fs_) {
        error_ = "failed to create the geometry shader modules";
        return false;
    }

    // One combined image sampler: the polygon's texture.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 1;
    dslci.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(ctx.device(), &dslci, nullptr, &set_layout_) != VK_SUCCESS) {
        error_ = "vkCreateDescriptorSetLayout failed";
        return false;
    }
    if (!textures_.create(ctx, set_layout_)) {
        error_ = textures_.error();
        return false;
    }

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(ctx.device(), &plci, nullptr, &layout_) != VK_SUCCESS) {
        error_ = "vkCreatePipelineLayout failed";
        return false;
    }
    return true;
}

VkPipeline Renderer::pipeline_for(const PipelineKey& key) {
    if (auto it = pipelines_.find(key); it != pipelines_.end())
        return it->second;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = static_cast<std::uint32_t>(kFloatsPerVertex * sizeof(float));
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    const std::array<VkVertexInputAttributeDescription, 5> attributes{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, 3 * sizeof(float)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 5 * sizeof(float)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 9 * sizeof(float)},
        {4, 0, VK_FORMAT_R32_SFLOAT, 13 * sizeof(float)},
    }};
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vi.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = cull_flags(key.cull_mode);
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = key.depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = depth_compare(key.depth_mode);
    if (stencil_ && !key.blend) {
        // Opaque and punch-through polygons record whether modifier volumes may shade them:
        // stencil bit 7 = the PCW Shadow bit of the nearest surface (Flycast pipeline.cpp).
        ds.stencilTestEnable = VK_TRUE;
        VkStencilOpState so{};
        so.failOp = VK_STENCIL_OP_KEEP;
        so.passOp = VK_STENCIL_OP_REPLACE;
        so.depthFailOp = VK_STENCIL_OP_KEEP;
        so.compareOp = VK_COMPARE_OP_ALWAYS;
        so.compareMask = 0;
        so.writeMask = 0x80;
        so.reference = key.shadow ? 0x80u : 0u;
        ds.front = so;
        ds.back = so;
    }

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (key.blend) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = src_factor(key.src_blend);
        blend.dstColorBlendFactor = dst_factor(key.dst_blend);
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = blend.srcColorBlendFactor;
        blend.dstAlphaBlendFactor = blend.dstColorBlendFactor;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dynamics;

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dy;
    gpci.layout = layout_;
    gpci.renderPass = render_pass_;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(ctx_->device(), pipeline_cache_, 1, &gpci, nullptr, &pipeline) !=
        VK_SUCCESS) {
        error_ = "vkCreateGraphicsPipelines failed";
        return VK_NULL_HANDLE;
    }
    pipelines_.emplace(key, pipeline);
    pipelines = static_cast<std::uint32_t>(pipelines_.size());
    return pipeline;
}

void Renderer::set_memory(const std::uint8_t* vram, std::size_t vram_size,
                          const std::uint32_t* palette_ram, PaletteFormat palette_format) {
    textures_.set_memory(vram, vram_size, palette_ram, palette_format);
    frame_sets_.clear();
}

// Textures are decoded and uploaded before the render pass begins, because a copy cannot be
// recorded inside one. The descriptor sets are remembered for the draw that follows.
void Renderer::prepare(VkCommandBuffer upload, const Frame& frame) {
    frame_sets_.clear();
    fallback_set_ = textures_.fallback(upload);
    // Two passes, because whether a texture is addressed with repeat or clamp is a property of
    // every polygon that samples it, not of the first one seen. One strip that genuinely wraps is
    // enough to need repeat for all of them; the union is the safe way round, since clamping
    // something that does tile would lose the repetition, while repeating something that does not
    // only costs the seam this is here to remove.
    struct Use {
        std::uint32_t tcw = 0, tsp = 0;
        bool tiled = false;
    };
    std::unordered_map<std::uint64_t, Use> used;
    for (const auto& list : frame.lists)
        for (const Polygon& p : list) {
            if (!pcw_texture(p.pcw))
                continue;
            Use& u = used[texture_key(p)];
            u.tcw = p.tcw;
            u.tsp = p.tsp;
            u.tiled = u.tiled || polygon_tiles(frame, p);
        }
    for (const auto& [key, u] : used)
        frame_sets_[key] = textures_.get(upload, u.tcw, u.tsp, u.tiled);
}

void Renderer::draw(VkCommandBuffer cmd, const Frame& frame, const FrameGeometry& geometry,
                    VkExtent2D target) {
    drawn_polygons = 0;
    drawn_vertices = 0;
    textured_polygons = 0;
    blended_polygons = 0;
    punch_through_polygons = 0;
    if (frame.vertices.empty())
        return;

    // Pack the vertices into the shader's layout.
    // Table fog (FogCtrl 0 and 3) is evaluated per vertex from the hardware's table, only when
    // some polygon uses it; the hardware does it per pixel, which this approximates.
    bool table_fog = false;
    for (const auto& list : frame.lists)
        for (const Polygon& p : list) {
            const std::uint32_t f = tsp_fog_control(p.tsp);
            if (f == 0 || f == 3)
                table_fog = true;
        }
    staging_.resize(frame.vertices.size() * kFloatsPerVertex);
    float* out = staging_.data();
    for (const Vertex& v : frame.vertices) {
        out[0] = v.x;
        out[1] = v.y;
        out[2] = v.z;
        out[3] = v.u;
        out[4] = v.v;
        unpack_colour(v.base, out + 5);
        unpack_colour(v.offset, out + 9);
        out[13] = table_fog ? fog_table_value(geometry.fog, v.z) : 0.0f;
        out += kFloatsPerVertex;
    }
    const VkDeviceSize bytes = staging_.size() * sizeof(float);
    // A ring of vertex buffers (dreamcomp): with renders pipelined, and with an interpolated
    // frame drawn beside each real one, the buffer a previous draw is still reading must not be
    // rewritten. Each target waits for its own previous frame, so four in rotation suffice.
    HostBuffer& vertices_ = vertex_ring_[ring_next_];
    ring_next_ = (ring_next_ + 1) % kVertexRing;
    if (!vertices_.ensure(*ctx_, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
        return;
    vertices_.write(staging_.data(), static_cast<std::size_t>(bytes));

    // The guest draws to its own framebuffer size; the window may be any size, and the viewport
    // the caller set already stretches one to the other.
    PushConstants push{};
    push.scale[0] = 2.0f / geometry.width;
    push.scale[1] = 2.0f / geometry.height;
    push.offset[0] = -1.0f - 2.0f * geometry.left / geometry.width;
    push.offset[1] = -1.0f - 2.0f * geometry.top / geometry.height;
    // Fog colours: RGBA8888 with red in the low byte (fog.h).
    for (int c = 0; c < 4; ++c) {
        push.fog_vert[c] =
            static_cast<float>((geometry.fog.vertex_colour >> (8 * c)) & 0xFFu) / 255.0f;
        push.fog_ram[c] = static_cast<float>((geometry.fog.table_colour >> (8 * c)) & 0xFFu) / 255.0f;
    }
    const bool fog_off = std::getenv("DREAM_NO_FOG") != nullptr;

    const VkDeviceSize zero = 0;
    VkBuffer buffer = vertices_.handle();
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &zero);

    // Opaque geometry only in this step. Punch-through and translucent lists need the alpha test
    // and the sorted pass, which are steps 4 and 5.
    // The hardware draws the lists in a fixed order and the game relies on it: opaque geometry
    // first, then punch-through (opaque with holes, so it can also write depth), then translucent
    // blended over the result.
    //
    // Translucent polygons are sorted back to front per strip. That is the fallback the hardware
    // does not need: a real PowerVR sorts per pixel, so two translucent surfaces that intersect
    // come out right and a per-strip sort cannot. Per-pixel sorting is a later refinement
    // (docs/runtime-render.md); a per-strip sort is what most Dreamcast rendering used for years
    // and is right for all but intersecting transparency.
    struct Item {
        const Polygon* poly;
        float depth;  // 1/w: larger is nearer
        bool blended;
        bool alpha_test;
    };
    std::vector<Item> items;
    const auto add_list = [&](ListType list, bool blended, bool alpha_test) {
        for (const Polygon& p : frame.lists[static_cast<unsigned>(list)]) {
            // Sort key (dreamcomp): the farthest point, as Flycast's sorter uses (minZ): a large
            // strip reaching toward the camera must not be drawn over what lies in front of
            // most of it.
            float nearest = 1e30f;
            for (std::uint32_t i = 0; i < p.count; ++i)
                nearest = std::min(nearest, frame.vertices[p.first + i].z);
            // A piece clip_far_polygons cut sorts as the strip it came from (dreamcomp): the far
            // vertex it lost is what made the strip sort where it does.
            if (blended && !std::isnan(p.sort_strip))
                nearest = p.sort_strip;
            items.push_back({&p, nearest, blended, alpha_test});
        }
    };
    add_list(ListType::Opaque, false, false);
    add_list(ListType::PunchThrough, false, true);
    const std::size_t translucent_start = items.size();
    add_list(ListType::Translucent, true, false);
    // Back to front: the farthest surface has the smallest 1/w and must be drawn first.
    std::stable_sort(items.begin() + static_cast<std::ptrdiff_t>(translucent_start), items.end(),
                     [](const Item& a, const Item& b) { return a.depth < b.depth; });

    // User tile clip (dreamcomp, as Flycast's SetTileClip): mode 2 scissors to the rectangle,
    // mode 3 discards inside it in the shader. Guest pixels to the target's.
    const float sx = static_cast<float>(target.width) / geometry.width;
    const float sy = static_cast<float>(target.height) / geometry.height;
    const VkRect2D full_scissor{{0, 0}, target};
    VkRect2D scissor_now = full_scissor;
    auto clip_rect = [&](const Polygon& p) {
        const float x0 = std::max(0.0f, (static_cast<float>(p.clip_x0) - geometry.left) * sx);
        const float y0 = std::max(0.0f, (static_cast<float>(p.clip_y0) - geometry.top) * sy);
        const float x1 = std::min(static_cast<float>(target.width),
                                  (static_cast<float>(p.clip_x1) - geometry.left) * sx);
        const float y1 = std::min(static_cast<float>(target.height),
                                  (static_cast<float>(p.clip_y1) - geometry.top) * sy);
        VkRect2D r{};
        r.offset = {static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0)};
        r.extent = {static_cast<std::uint32_t>(std::max(0.0f, x1 - x0)),
                    static_cast<std::uint32_t>(std::max(0.0f, y1 - y0))};
        return r;
    };
    const bool clip_off = std::getenv("DREAM_NO_TILE_CLIP") != nullptr;
    VkPipeline bound = VK_NULL_HANDLE;
    modifier_triangles = 0;
    bool volumes_drawn = false;
    for (std::size_t index = 0; index < items.size(); ++index) {
        const Item& item = items[index];
        if (!volumes_drawn && index == translucent_start) {
            // After the opaque and punch-through lists, before the translucent one (Flycast).
            if (scissor_now.extent.width != target.width || scissor_now.extent.height != target.height ||
                scissor_now.offset.x != 0 || scissor_now.offset.y != 0) {
                vkCmdSetScissor(cmd, 0, 1, &full_scissor);
                scissor_now = full_scissor;
            }
            draw_modifier_volumes(cmd, frame, geometry, buffer);
            volumes_drawn = true;
            bound = VK_NULL_HANDLE;
        }
        const Polygon& p = *item.poly;
        PipelineKey key{};
        key.depth_mode = isp_depth_mode(p.isp) & 7u;
        // As Flycast (pipeline.cpp): punch-through always GREATER_EQUAL with depth writes;
        // translucent GREATER_EQUAL when the hardware sorts it.
        if (item.alpha_test || (item.blended && geometry.autosort))
            key.depth_mode = 6u;
        // A translucent surface tests depth but does not write it: writing would hide surfaces
        // behind it that still have to be blended in.
        key.depth_write =
            item.alpha_test ? 1u : (item.blended || isp_z_write_disable(p.isp)) ? 0u : 1u;
        key.cull_mode = isp_cull_mode(p.isp) & 3u;
        key.blend = item.blended ? 1u : 0u;
        key.src_blend = tsp_src_instr(p.tsp) & 7u;
        key.dst_blend = tsp_dst_instr(p.tsp) & 7u;
        key.shadow = (stencil_ && !item.blended) ? pcw_shadow(p.pcw) : 0u;
        key.padding = 0;
        VkPipeline pipeline = pipeline_for(key);
        if (!pipeline)
            continue;
        if (pipeline != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound = pipeline;
        }
        // A polygon whose texture could not be decoded draws untextured rather than not at all:
        // a flat shape in the right place says more during bring-up than a hole.
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (pcw_texture(p.pcw)) {
            const auto it = frame_sets_.find(texture_key(p));
            if (it != frame_sets_.end())
                set = it->second;
        }
        push.mode = 0;
        if (set) {
            push.mode = static_cast<std::int32_t>(tsp_shading_instruction(p.tsp)) | 4;
            if (tsp_ignore_texture_alpha(p.tsp))
                push.mode |= 8;
            ++textured_polygons;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0,
                                    nullptr);
        } else if (fallback_set_) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1,
                                    &fallback_set_, 0, nullptr);
        }
        // The offset colour exists only for textured polygons (Flycast reads it inside its texture
        // block; an untextured header's offset word is unused).
        if (pcw_offset(p.pcw) && pcw_texture(p.pcw))
            push.mode |= 16;
        if (item.alpha_test) {
            push.mode |= 32;
            ++punch_through_polygons;
        }
        if (tsp_use_alpha(p.tsp))
            push.mode |= 64;
        {
            const std::uint32_t mode = clip_off ? 0u : (p.tile_clip & 3u);
            const VkRect2D want = mode == 2 ? clip_rect(p) : full_scissor;
            if (want.offset.x != scissor_now.offset.x || want.offset.y != scissor_now.offset.y ||
                want.extent.width != scissor_now.extent.width ||
                want.extent.height != scissor_now.extent.height) {
                vkCmdSetScissor(cmd, 0, 1, &want);
                scissor_now = want;
            }
            if (mode == 3) {
                const VkRect2D r = clip_rect(p);
                push.mode |= 512;
                push.clip[0] = static_cast<float>(r.offset.x);
                push.clip[1] = static_cast<float>(r.offset.y);
                push.clip[2] = static_cast<float>(r.offset.x) + static_cast<float>(r.extent.width);
                push.clip[3] = static_cast<float>(r.offset.y) + static_cast<float>(r.extent.height);
            }
        }
        // Fog control in bits 7-8 (2 = none).
        push.mode |= static_cast<std::int32_t>((fog_off ? 2u : (tsp_fog_control(p.tsp) & 3u)) << 7);
        if (item.blended)
            ++blended_polygons;
        push.alpha_ref = geometry.alpha_ref;
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof push, &push);
        vkCmdDraw(cmd, p.count, 1, p.first, 0);
        ++drawn_polygons;
        drawn_vertices += p.count;
    }
    vkCmdSetScissor(cmd, 0, 1, &full_scissor);
    if (!volumes_drawn)
        draw_modifier_volumes(cmd, frame, geometry, buffer);
}

VkPipeline Renderer::modvol_pipeline(ModVol mode, std::uint32_t cull) {
    const std::uint32_t key = static_cast<std::uint32_t>(mode) * 4u + (cull & 3u);
    if (auto it = modvol_pipelines_.find(key); it != modvol_pipelines_.end())
        return it->second;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = mv_vs_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = mv_fs_;
    stages[1].pName = "main";
    VkVertexInputBindingDescription binding{0, 3 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &attribute;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = cull_flags(cull);
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Stencil states copied from Flycast's Vulkan renderer (pipeline.cpp CreateModVolPipeline):
    // (fail, pass, depth fail, compare, compare mask, write mask, reference).
    auto op = [](VkStencilOp fail, VkStencilOp pass, VkStencilOp dfail, VkCompareOp cmp,
                 std::uint32_t cmask, std::uint32_t wmask, std::uint32_t ref) {
        VkStencilOpState s{};
        s.failOp = fail;
        s.passOp = pass;
        s.depthFailOp = dfail;
        s.compareOp = cmp;
        s.compareMask = cmask;
        s.writeMask = wmask;
        s.reference = ref;
        return s;
    };
    VkStencilOpState so{};
    switch (mode) {
        case ModVol::Xor:
            so = op(VK_STENCIL_OP_KEEP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_KEEP,
                    VK_COMPARE_OP_ALWAYS, 0, 2, 2);
            break;
        case ModVol::Or:
            so = op(VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP,
                    VK_COMPARE_OP_ALWAYS, 2, 2, 2);
            break;
        case ModVol::Inclusion:
            so = op(VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_ZERO,
                    VK_COMPARE_OP_LESS_OR_EQUAL, 3, 3, 1);
            break;
        case ModVol::Exclusion:
            so = op(VK_STENCIL_OP_ZERO, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO,
                    VK_COMPARE_OP_EQUAL, 3, 3, 1);
            break;
        case ModVol::Final:
            so = op(VK_STENCIL_OP_ZERO, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_ZERO,
                    VK_COMPARE_OP_EQUAL, 0x81, 3, 0x81);
            break;
    }
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = (mode == ModVol::Xor || mode == ModVol::Or) ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER;  // nearer is larger, as for the geometry
    ds.stencilTestEnable = VK_TRUE;
    ds.front = so;
    ds.back = so;
    VkPipelineColorBlendAttachmentState blend{};
    if (mode == ModVol::Final) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dynamics;
    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dy;
    gpci.layout = layout_;
    gpci.renderPass = render_pass_;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(ctx_->device(), pipeline_cache_, 1, &gpci, nullptr, &pipeline) !=
        VK_SUCCESS) {
        error_ = "vkCreateGraphicsPipelines failed (modifier volume)";
        return VK_NULL_HANDLE;
    }
    modvol_pipelines_.emplace(key, pipeline);
    return pipeline;
}

void Renderer::draw_modifier_volumes(VkCommandBuffer cmd, const Frame& frame,
                                     const FrameGeometry& geometry, VkBuffer geometry_buffer) {
    if (!stencil_ || frame.modifiers.empty() || std::getenv("DREAM_NO_MODVOL"))
        return;
    // Only the opaque modifier list shades (Flycast draws global_param_mvo only).
    modvol_tris_.clear();
    for (const ModifierTriangle& t : frame.modifiers)
        if (!t.translucent)
            modvol_tris_.push_back(t);
    if (modvol_tris_.empty())
        return;
    const std::vector<ModifierTriangle>& mods = modvol_tris_;
    // Triangles, then the final quad as two more, all in one buffer: x, y in pixels, z = 1/w.
    const std::size_t tris = mods.size();
    modvol_staging_.resize((tris + 2) * 9);
    float* out = modvol_staging_.data();
    for (const ModifierTriangle& t : mods)
        for (unsigned i = 0; i < 3; ++i) {
            *out++ = t.x[i];
            *out++ = t.y[i];
            *out++ = t.z[i];
        }
    const float l = geometry.left, t = geometry.top;
    const float r = geometry.left + geometry.width, b = geometry.top + geometry.height;
    const float quad[18] = {l, t, 1, r, t, 1, l, b, 1, r, t, 1, r, b, 1, l, b, 1};
    std::copy(std::begin(quad), std::end(quad), out);
    HostBuffer& buf = modvol_ring_[modvol_next_];
    modvol_next_ = (modvol_next_ + 1) % kModvolRing;
    const VkDeviceSize bytes = modvol_staging_.size() * sizeof(float);
    if (!buf.ensure(*ctx_, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
        return;
    buf.write(modvol_staging_.data(), static_cast<std::size_t>(bytes));
    const VkDeviceSize zero = 0;
    VkBuffer vb = buf.handle();
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);

    PushConstants push{};
    push.scale[0] = 2.0f / geometry.width;
    push.scale[1] = 2.0f / geometry.height;
    push.offset[0] = -1.0f - 2.0f * geometry.left / geometry.width;
    push.offset[1] = -1.0f - 2.0f * geometry.top / geometry.height;
    push.alpha_ref = 1.0f - geometry.shadow_scale;
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof push, &push);

    // One "parameter" per modifier header, as in Flycast's DrawModVols.
    std::size_t base = SIZE_MAX;  // first triangle of the volume being summed
    VkPipeline bound = VK_NULL_HANDLE;
    auto bind = [&](VkPipeline p) {
        if (p && p != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            bound = p;
        }
        return p != VK_NULL_HANDLE;
    };
    for (std::size_t first = 0; first < tris;) {
        std::size_t end = first + 1;
        while (end < tris && mods[end].header == mods[first].header)
            ++end;
        const std::uint32_t isp = mods[first].isp;
        const std::uint32_t mv_mode = isp >> 29;            // volume instruction
        const bool volume_last = mods[first].volume_last;  // header PCW Volume bit
        const std::uint32_t cull = (isp >> 27) & 3u;
        if (base == SIZE_MAX)
            base = first;
        if (bind(modvol_pipeline(!volume_last && mv_mode > 0 ? ModVol::Or : ModVol::Xor, cull)))
            vkCmdDraw(cmd, static_cast<std::uint32_t>((end - first) * 3), 1,
                      static_cast<std::uint32_t>(first * 3), 0);
        if (mv_mode == 1 || mv_mode == 2) {
            if (bind(modvol_pipeline(mv_mode == 1 ? ModVol::Inclusion : ModVol::Exclusion, cull)))
                vkCmdDraw(cmd, static_cast<std::uint32_t>((end - base) * 3), 1,
                          static_cast<std::uint32_t>(base * 3), 0);
            base = SIZE_MAX;
        }
        first = end;
    }
    if (bind(modvol_pipeline(ModVol::Final, 0)))
        vkCmdDraw(cmd, 6, 1, static_cast<std::uint32_t>(tris * 3), 0);
    modifier_triangles = static_cast<std::uint32_t>(tris);
    vkCmdBindVertexBuffers(cmd, 0, 1, &geometry_buffer, &zero);
}

namespace {
constexpr std::uint32_t kCacheMagic = 0x43505244u;  // "DRPC"
constexpr std::uint32_t kCacheVersion = 1;
}  // namespace

void Renderer::use_pipeline_cache(const std::string& path) {
    if (!ctx_ || !ctx_->device() || path.empty())
        return;
    pipeline_cache_path_ = path;
    std::vector<std::uint32_t> keys;
    std::vector<char> blob;
    {
        std::ifstream in(path, std::ios::binary);
        const std::vector<char> data((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
        auto word = [&](std::size_t i) {
            std::uint32_t w = 0;
            std::memcpy(&w, data.data() + i * 4, 4);
            return w;
        };
        if (data.size() >= 12 && word(0) == kCacheMagic && word(1) == kCacheVersion) {
            const std::uint32_t n = word(2);
            if (data.size() >= 16 + 4ull * n) {
                for (std::uint32_t i = 0; i < n; ++i) keys.push_back(word(3 + i));
                const std::uint32_t bytes = word(3 + n);
                if (data.size() >= 16 + 4ull * n + bytes)
                    blob.assign(data.begin() + 16 + 4 * n, data.begin() + 16 + 4 * n + bytes);
            }
        }
    }
    VkPipelineCacheCreateInfo pcci{};
    pcci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    // A cache from another driver or GPU is ignored by the driver itself (its header says whose).
    pcci.initialDataSize = blob.size();
    pcci.pInitialData = blob.empty() ? nullptr : blob.data();
    if (vkCreatePipelineCache(ctx_->device(), &pcci, nullptr, &pipeline_cache_) != VK_SUCCESS) {
        pcci.initialDataSize = 0;
        pcci.pInitialData = nullptr;
        if (vkCreatePipelineCache(ctx_->device(), &pcci, nullptr, &pipeline_cache_) != VK_SUCCESS)
            pipeline_cache_ = VK_NULL_HANDLE;
    }
    for (std::uint32_t bits : keys) {
        PipelineKey k{};
        k.depth_mode = bits & 7u;
        k.depth_write = (bits >> 3) & 1u;
        k.cull_mode = (bits >> 4) & 3u;
        k.src_blend = (bits >> 6) & 7u;
        k.dst_blend = (bits >> 9) & 7u;
        k.blend = (bits >> 12) & 1u;
        if (pipeline_for(k))
            ++prewarmed;
    }
}

void Renderer::save_pipeline_cache() {
    if (pipeline_cache_path_.empty() || !pipeline_cache_)
        return;
    std::size_t size = 0;
    std::vector<char> blob;
    if (vkGetPipelineCacheData(ctx_->device(), pipeline_cache_, &size, nullptr) == VK_SUCCESS &&
        size > 0) {
        blob.resize(size);
        if (vkGetPipelineCacheData(ctx_->device(), pipeline_cache_, &size, blob.data()) !=
            VK_SUCCESS)
            blob.clear();
        blob.resize(std::min(blob.size(), size));
    }
    std::vector<std::uint32_t> words{kCacheMagic, kCacheVersion,
                                     static_cast<std::uint32_t>(pipelines_.size())};
    for (const auto& [key, pipeline] : pipelines_) words.push_back(key.bits());
    words.push_back(static_cast<std::uint32_t>(blob.size()));
    // Written to a temporary name and renamed, so a crash mid-write never leaves a torn cache.
    const std::string tmp = pipeline_cache_path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(words.data()),
                  static_cast<std::streamsize>(words.size() * 4));
        out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        if (!out)
            return;
    }
    std::remove(pipeline_cache_path_.c_str());
    std::rename(tmp.c_str(), pipeline_cache_path_.c_str());
}

void Renderer::destroy() {
    if (!ctx_ || !ctx_->device())
        return;
    vkDeviceWaitIdle(ctx_->device());
    save_pipeline_cache();
    if (pipeline_cache_) {
        vkDestroyPipelineCache(ctx_->device(), pipeline_cache_, nullptr);
        pipeline_cache_ = VK_NULL_HANDLE;
    }
    for (auto& [key, pipeline] : pipelines_) vkDestroyPipeline(ctx_->device(), pipeline, nullptr);
    pipelines_.clear();
    for (auto& [key, pipeline] : modvol_pipelines_)
        vkDestroyPipeline(ctx_->device(), pipeline, nullptr);
    modvol_pipelines_.clear();
    for (auto& b : modvol_ring_) b.destroy();
    if (mv_vs_) {
        vkDestroyShaderModule(ctx_->device(), mv_vs_, nullptr);
        mv_vs_ = VK_NULL_HANDLE;
    }
    if (mv_fs_) {
        vkDestroyShaderModule(ctx_->device(), mv_fs_, nullptr);
        mv_fs_ = VK_NULL_HANDLE;
    }
    textures_.destroy();
    if (set_layout_) {
        vkDestroyDescriptorSetLayout(ctx_->device(), set_layout_, nullptr);
        set_layout_ = VK_NULL_HANDLE;
    }
    for (auto& b : vertex_ring_) b.destroy();
    if (layout_) {
        vkDestroyPipelineLayout(ctx_->device(), layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    if (vs_) {
        vkDestroyShaderModule(ctx_->device(), vs_, nullptr);
        vs_ = VK_NULL_HANDLE;
    }
    if (fs_) {
        vkDestroyShaderModule(ctx_->device(), fs_, nullptr);
        fs_ = VK_NULL_HANDLE;
    }
    ctx_ = nullptr;
}

}  // namespace dream::render::vk
