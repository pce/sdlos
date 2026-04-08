#pragma once
// Zero-overhead runtime execution of a compiled render pipeline.
//
// Design contract
// All string work, map lookups, and pointer resolution happen exactly once in
// FrameGraph::compile().  The structures below are the output: plain POD,
// direct pointers, no heap traffic in the hot path.
//
// Hot path (every frame):
//   CompiledGraph::execute()  →  tight for-loop, O(1) per pass
//
// Cold path (on CSS change, event-driven, never per-frame):
//   CompiledGraph::patch()    →  string lookup, writes one float
//   CompiledGraph::set_enabled() → string lookup, flips one bool

#include "pug_parser.h"

#include <SDL3/SDL_gpu.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace pce::sdlos::fg {

// CompiledParams
//
// Bucket-C runtime uniforms pre-flattened into a packed float array.
// Slot assignment is alphabetical by key to match the shader's
// cbuffer / push-constant layout (also sorted alphabetically).
//
// Pushed to the GPU via SDL_PushGPUFragmentUniformData once per pass.
// Maximum 16 floats (64 bytes) per pass — covers all practical post-FX needs.
struct CompiledParams {
    std::array<float, 16> data  {};
    uint32_t              count = 0;   ///< populated slots; only this many bytes pushed

    /**
     * @brief Set a uniform parameter by slot
     *
     * @param slot  Parameter slot index (0-15)
     * @param v     32-bit floating-point scalar
     */
    void set(uint32_t slot, float v) noexcept {
        if (slot < 16u) {
            data[slot] = v;
            if (slot + 1u > count) count = slot + 1u;
        }
    }

    /**
     * @brief Check if parameters are empty
     *
     * @return true when no parameters have been set
     */
    constexpr bool  empty()     const noexcept { return count == 0; }
    constexpr float operator[](uint32_t i) const noexcept { return data[i]; }

    /// Raw byte span pushed to SDL_PushGPUFragmentUniformData.
    std::span<const float> span() const noexcept {
        return { data.data(), count };
    }
};

// ResourceBinding
//
// One resolved texture + sampler pair bound to a specific fragment sampler
// slot.  Resolved from the ResourcePool during compile(); never a string
// lookup at execute() time.
struct ResourceBinding {
    SDL_GPUTexture* texture = nullptr;  ///< non-owning; owned by ResourcePool
    SDL_GPUSampler* sampler = nullptr;  ///< shared linear sampler from device
    uint32_t        slot    = 0;
};

// CompiledPass
//
// The smallest unit of GPU work in the pipeline.
//
// All expensive resolution (string → pointer, key → slot) happens in
// FrameGraph::compile().  execute() sees only this struct.
//
// Memory layout is designed for cache-friendly sequential iteration:
//   - hot fields first (pipeline, output, enabled)
//   - params and bindings follow
//   - id_hash last (only used by patch(), never by execute())
struct CompiledPass {
    // Hot fields
    SDL_GPUGraphicsPipeline* pipeline   = nullptr;  ///< pre-selected PSO
    SDL_GPUTexture*          output     = nullptr;  ///< render target for this pass
    bool                     enabled    = true;     ///< false → skip, forward input

    // Bindings
    uint8_t                        bind_count = 0;
    std::array<ResourceBinding, 8> bindings   = {};

    // Params (Bucket C uniforms)
    CompiledParams params;

    // Per-frame time injection
    // When a pass declares a "time" param, its slot index is recorded here so
    // execute() can inject the current wall-clock time each frame without any
    // string lookup or heap traffic.  255 = no time param declared.
    uint8_t time_slot = 255u;

    // Cold fields (patch() only — never read in execute())
    uint32_t id_hash = 0;   ///< fnv1a(pass_id) — dispatch key for patch()

    // Param slot reverse-map (for named patch by key string)
    // Maps fnv1a(key) → slot index.  Built at compile time; used only in
    // the cold patch() path, not in execute().
    struct SlotEntry { uint32_t key_hash; uint8_t slot; };
    std::array<SlotEntry, 16> slot_map   = {};
    uint8_t                   slot_count = 0;

    /// Look up a param slot by key hash.  Returns 255 if not found.
    uint8_t find_slot(uint32_t key_hash) const noexcept {
        for (uint8_t i = 0; i < slot_count; ++i)
            if (slot_map[i].key_hash == key_hash) return slot_map[i].slot;
        return 255u;
    }
};

// CompiledGraph
//
// The fully compiled render pipeline — a flat, ordered array of CompiledPass.
// Built once by FrameGraph::compile().  Mutated only by patch() on CSS change.
// execute() contract:
//   - Called every frame inside the SDL3 command buffer.
//   - Must be called after BFS layout compute and spring compute dispatches.
//   - Swapchain texture is passed in each frame (its pointer changes per frame
//     in SDL3 GPU; the pool holds the intermediate targets which are stable).
//   - No heap allocation.  No string operations.  No map lookups.
struct CompiledGraph {
    std::vector<CompiledPass> passes;       ///< topological order, baked
    SDL_GPUSampler*           sampler = nullptr;  ///< shared linear clamp sampler



    /// Execute the compiled pipeline.
    ///
    /// Each enabled pass:
    ///   1. Begins a GPU render pass targeting pass.output.
    ///   2. Binds the PSO.
    ///   3. Binds input samplers (pass.bindings).
    ///   4. Pushes params (pass.params) as fragment uniform data.
    ///      If the pass declared a "time" param, its slot is overwritten with
    ///      the current `time` value before pushing — zero heap traffic.
    ///   5. Draws a full-screen triangle (3 vertices, no vertex buffer).
    ///   6. Ends the render pass.
    ///
    /// Disabled passes are skipped in O(1); their output resource retains
    /// the previous frame's content or the input it was assigned to forward.
    ///
    /// @param cmd        Active SDL_GPUCommandBuffer.
    /// @param swapchain  Current frame's swapchain texture
    ///                   (acquired via SDL_AcquireGPUSwapchainTexture).
    /// @param vp_w       Viewport width  in physical pixels.
    /// @param vp_h       Viewport height in physical pixels.
    /// @param time       Wall-clock time in seconds, forwarded to any pass
    ///                   that declares a "time" param.  Defaults to 0.
    void execute(SDL_GPUCommandBuffer* cmd,
                 SDL_GPUTexture*       swapchain,
                 uint32_t              vp_w,
                 uint32_t              vp_h,
                 float                 time = 0.f) noexcept;


    /// Update a single float param on a named pass.
    ///
    /// Called by FrameGraph when the CSS system writes a property change,
    /// e.g. `#fog { density: 0.04; }`.  Never called from execute().
    ///
    /// @param pass_id  Pass id string, e.g. "fog".
    /// @param key      Param key string, e.g. "density".
    /// @param val      New float value.
    void patch(std::string_view pass_id,
               std::string_view key,
               float            val) noexcept;

    /// Enable or disable a pass by name.
    ///
    /// Disabled pass → execute() skips it in O(1).
    /// e.g. `pipeline.low-power #dof { enabled: false; }` → set_enabled("dof", false).
    void set_enabled(std::string_view pass_id, bool enabled) noexcept;

    /// Generic CSS property setter — dispatches to patch() or set_enabled().
    ///
    /// Called by the CSS integration layer for every property change on a
    /// pipeline node.  Converts string value to float for numeric properties.
    void apply_style(std::string_view pass_id,
                     std::string_view key,
                     std::string_view val) noexcept;


    /**
     * @brief Check if graph is empty
     *
     * @return true when no passes are present
     */
    bool              empty()       const noexcept { return passes.empty(); }
    /**
     * @brief Get total number of passes
     *
     * @return Number of passes in the compiled graph
     */
    std::size_t       pass_count()  const noexcept { return passes.size();  }
    /**
     * @brief Get number of enabled passes
     *
     * @return Number of currently enabled passes (disabled passes skipped)
     */
    std::size_t       active_count() const noexcept;

    /// pass states to stdout for development.
    void dump() const noexcept;

private:
    /// Find a pass by FNV hash.  Returns nullptr if not found.
    CompiledPass* find_pass(uint32_t id_hash) noexcept;
};

} // namespace pce::sdlos::fg
