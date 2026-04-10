#include "compiled_graph.h"

#include <charconv>
#include <cstdio>
#include <cstring>

namespace pce::sdlos::fg {

namespace {

// FNV-1a hash — identical to the one used in frame_graph.cc.
// Kept here so compiled_graph.cc compiles independently.
[[nodiscard]]
inline uint32_t fnv1a(std::string_view s) noexcept {
    uint32_t h = 2'166'136'261u;
    for (unsigned char c : s)
        h = (h ^ c) * 16'777'619u;
    return h;
}

/// Begin a colour-only render pass targeting one texture.
/// LOADOP_DONT_CARE — post-process passes overwrite every pixel.
///
/// @param cycle  Set true for owned intermediate textures written with
///               DONT_CARE every frame.  SDL will allocate a fresh backing
///               store if the GPU is still reading the previous frame's
///               version, eliminating the read-after-write pipeline stall
///               that tile-based GPUs (Apple Silicon, all mobile) would
///               otherwise suffer.
///               Set false for the swapchain (SDL manages its own rotation)
///               and for any LOADOP_LOAD pass (we need the previous contents).
[[nodiscard]]
inline SDL_GPURenderPass *begin_pass(
    SDL_GPUCommandBuffer *cmd,
    SDL_GPUTexture *target,
    uint32_t w,
    uint32_t h,
    bool cycle) noexcept {
    SDL_GPUColorTargetInfo ct{};
    ct.texture              = target;
    ct.load_op              = SDL_GPU_LOADOP_DONT_CARE;
    ct.store_op             = SDL_GPU_STOREOP_STORE;
    ct.mip_level            = 0;
    ct.layer_or_depth_plane = 0;
    ct.cycle                = cycle;

    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);

    if (rp) {
        SDL_GPUViewport vp{};
        vp.x         = 0.f;
        vp.y         = 0.f;
        vp.w         = static_cast<float>(w);
        vp.h         = static_cast<float>(h);
        vp.min_depth = 0.f;
        vp.max_depth = 1.f;
        SDL_SetGPUViewport(rp, &vp);

        SDL_Rect scissor{};
        scissor.x = 0;
        scissor.y = 0;
        scissor.w = static_cast<int>(w);
        scissor.h = static_cast<int>(h);
        SDL_SetGPUScissor(rp, &scissor);
    }

    return rp;
}

/// Bind all sampler inputs declared in a compiled pass.
inline void bind_samplers(SDL_GPURenderPass *rp, const CompiledPass &pass) noexcept {
    if (pass.bind_count == 0)
        return;

    SDL_GPUTextureSamplerBinding bindings[8]{};
    for (uint8_t i = 0; i < pass.bind_count; ++i) {
        bindings[i].texture = pass.bindings[i].texture;
        bindings[i].sampler = pass.bindings[i].sampler;
    }

    SDL_BindGPUFragmentSamplers(rp, 0, bindings, pass.bind_count);
}

/// Push Bucket-C float uniforms to fragment binding slot 0.
inline void push_params(SDL_GPUCommandBuffer *cmd, const CompiledParams &p) noexcept {
    if (p.empty())
        return;
    SDL_PushGPUFragmentUniformData(
        cmd,
        0,
        p.data.data(),
        p.count << 2);  // Bit shift left by 2: multiply by 4 (sizeof(float))
}

}  // namespace

// CompiledGraph::execute
//
// Hot path — called every frame, zero string work, zero heap traffic.
//
// Each enabled pass:
//   1. Begins an SDL3 GPU render pass targeting pass.output.
//      When pass.output is nullptr the pass writes to the swapchain texture
//      (the last pass in the pipeline typically does this).
//   2. Binds the pre-selected PSO.
//   3. Binds input samplers.
//   4. Pushes packed float uniforms.
//   5. Draws a full-screen triangle (3 verts, no vertex buffer required —
//      vertex positions are reconstructed in the vertex shader via SV_VertexID).
//   6. Ends the render pass.
//
// Disabled passes are skipped in O(1); their output resource retains
// whatever the previous pass wrote into it.
/**
 * @brief Execute the compiled pipeline each frame
 *
 * @param cmd        Active SDL_GPUCommandBuffer
 * @param swapchain  Current frame's swapchain texture
 * @param vp_w       Viewport width in physical pixels
 * @param vp_h       Viewport height in physical pixels
 * @param time       Wall-clock time in seconds for animation parameters
 */
void CompiledGraph::execute(
    SDL_GPUCommandBuffer *cmd,
    SDL_GPUTexture *swapchain,
    uint32_t vp_w,
    uint32_t vp_h,
    float time) noexcept {
    for (const CompiledPass &pass : passes) {
        if (!pass.enabled) [[unlikely]]
            continue;
        if (!pass.pipeline) [[unlikely]]
            continue;

        // The sentinel nullptr output means "write to swapchain".
        SDL_GPUTexture *target = pass.output ? pass.output : swapchain;
        if (!target) [[unlikely]]
            continue;

        // Cycle owned intermediate textures so the GPU can pipeline writes
        // without waiting for the previous frame's reads to drain.
        // The swapchain (output == nullptr, target = swapchain param) is NOT
        // cycled here — SDL rotates swapchain images internally.
        const bool cycle      = (pass.output != nullptr);
        SDL_GPURenderPass *rp = begin_pass(cmd, target, vp_w, vp_h, cycle);
        if (!rp) [[unlikely]]
            continue;

        SDL_BindGPUGraphicsPipeline(rp, pass.pipeline);
        bind_samplers(rp, pass);

        // If this pass declared a "time" param, inject the current wall-clock
        // time into a stack copy before pushing — no heap traffic, no write to
        // the stored CompiledParams (patch() owns that storage).
        if (pass.time_slot != 255u) [[unlikely]] {
            CompiledParams p       = pass.params;  // stack copy (68 bytes)
            p.data[pass.time_slot] = time;
            push_params(cmd, p);
        } else {
            push_params(cmd, pass.params);
        }

        // Full-screen triangle — vertex shader reconstructs NDC positions from
        // SV_VertexID without a vertex buffer.  3 verts, 1 instance.
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);

        SDL_EndGPURenderPass(rp);
    }
}

// CompiledGraph::active_count
/**
 * @brief Get number of enabled passes
 *
 * @return Number of currently enabled passes (disabled passes not counted)
 */
std::size_t CompiledGraph::active_count() const noexcept {
    std::size_t n = 0;
    for (const auto &p : passes)
        if (p.enabled)
            ++n;
    return n;
}

// Cold path helpers
/**
 * @brief Find a pass by FNV hash
 *
 * @param id_hash  FNV-1a hash of pass id string
 *
 * @return Pointer to the CompiledPass, or nullptr if not found
 */
CompiledPass *CompiledGraph::find_pass(uint32_t id_hash) noexcept {
    for (auto &p : passes)
        if (p.id_hash == id_hash)
            return &p;
    return nullptr;
}

// CompiledGraph::patch
//
// Update a single float param on a named pass.
// Called by apply_style() and directly from CSS property change handlers.
// Strings only appear here — never in execute().
/**
 * @brief Update a single float parameter on a named pass
 *
 * @param pass_id  Pass id string (e.g. "fog")
 * @param key      Parameter key string (e.g. "density")
 * @param val      New float value to set
 */
void CompiledGraph::patch(std::string_view pass_id, std::string_view key, float val) noexcept {
    const uint32_t pid = fnv1a(pass_id);
    CompiledPass *pass = find_pass(pid);
    if (!pass)
        return;

    const uint32_t kid = fnv1a(key);
    const uint8_t slot = pass->find_slot(kid);
    if (slot == 255u)
        return;

    pass->params.set(slot, val);
}

/**
 * @brief Enable or disable a pass by name
 *
 * @param pass_id  Pass id string (e.g. "dof")
 * @param enabled  true to enable, false to disable the pass
 */
void CompiledGraph::set_enabled(std::string_view pass_id, bool enabled) noexcept {
    const uint32_t pid = fnv1a(pass_id);
    if (CompiledPass *pass = find_pass(pid))
        pass->enabled = enabled;
}

/**
 * @brief Apply CSS property change to pipeline
 *
 * @param pass_id  Pass id string
 * @param key      Property key (either "enabled" or a param name)
 * @param val      Property value as string
 */
void CompiledGraph::apply_style(
    std::string_view pass_id,
    std::string_view key,
    std::string_view val) noexcept {
    if (key == "enabled") {
        set_enabled(pass_id, val != "false" && val != "0");
        return;
    }

    // All other keys are Bucket-C float uniforms.
    float f = 0.f;
    std::from_chars(val.data(), val.data() + val.size(), f);
    patch(pass_id, key, f);
}

/**
 * @brief Dump pipeline state to stdout for development
 *
 * Prints pass configuration, enabled status, bindings, and parameters
 */
void CompiledGraph::dump() const noexcept {
    std::printf("CompiledGraph  passes=%zu  active=%zu\n", passes.size(), active_count());

    for (std::size_t i = 0; i < passes.size(); ++i) {
        const auto &p = passes[i];
        std::printf(
            "  [%zu] id_hash=0x%08x  enabled=%-5s  bindings=%u  params=%u\n",
            i,
            p.id_hash,
            p.enabled ? "true" : "false",
            static_cast<unsigned>(p.bind_count),
            static_cast<unsigned>(p.params.count));

        for (uint32_t j = 0; j < p.params.count; ++j)
            std::printf("       param[%u] = %.4f\n", j, p.params.data[j]);
    }
}

}  // namespace pce::sdlos::fg
