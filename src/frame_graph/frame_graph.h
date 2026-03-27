#pragma once
// FrameGraph — top-level owner of the data-driven render pipeline.
//
// Lifecycle
//   1.  FrameGraph::from_pug(source, device, w, h)
//         Parse pipeline.pug → FrameGraph.
//
//   2.  fg.apply_css(sheet)           (optional — CSS drives pass style)
//       fg.apply_style(id, key, val)  (optional — direct imperative override)
//
//   3.  CompiledGraph cg = fg.compile(device, swapchain_fmt, w, h)
//         All strings resolved.  All pointers direct.  Strings gone.
//
//   4.  Every frame:
//         cg.execute(cmd, swapchain, w, h)
//
//   5.  On CSS change (event-driven, never per-frame):
//         cg.apply_style("fog", "density", "0.04")
//         cg.set_enabled("dof", false)
//
//   6.  On window resize:
//         fg.resize(new_w, new_h)           → recreates swapchain-sized textures
//         cg = fg.compile(...)              → rebuild compiled graph
//
// Two trees, one style system
//   scene.jade   → RenderTree  (UI nodes)
//   pipeline.pug → FrameGraph  (render passes)
//   app.css      → targets both via the same selector / property system
//   EventBus     → bus.publish("quality:low","") → set_enabled / patch
//
// Shader variant taxonomy (no combinatorial explosion)
//   Bucket A  compile-time  BRDF, shadow algorithm          ~24 variants total
//   Bucket B  pipeline-time specialization constants        ~6 per A variant
//   Bucket C  runtime       fog density, DoF focal, …       0 extra variants
//

#include "compiled_graph.h"
#include "pug_parser.h"
#include "resource_pool.h"

#include "../css_loader.h"
#include "../event_bus.h"

#include <SDL3/SDL_gpu.h>

#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pce::sdlos::fg {


class ShaderLibrary {
public:
    /**
     * @brief Shader library
     */
    ShaderLibrary() = default;

    /**
     * @brief Shader library
     *
     * @param param0  Red channel component [0, 1]
     */
    ShaderLibrary(const ShaderLibrary&)            = delete;
    ShaderLibrary& operator=(const ShaderLibrary&) = delete;
    /**
     * @brief Shader library
     *
     * @param param0  Red channel component [0, 1]
     */
    ShaderLibrary(ShaderLibrary&&)                 = default;
    ShaderLibrary& operator=(ShaderLibrary&&)      = default;

    /**
     * @brief ~shader library
     */
    ~ShaderLibrary() { release_all(); }


    /**
     * @brief Initialises
     *
     * @param device         SDL3 GPU device handle
     * @param shader_dir     Directory path
     * @param shader_format  Format descriptor
     */
    void init(SDL_GPUDevice*   device,
              std::string_view shader_dir,
              SDL_GPUShaderFormat shader_format) noexcept
    {
        device_         = device;
        shader_dir_     = shader_dir;
        shader_format_  = shader_format;
    }

    // get_or_create — return a cached pipeline or compile a new one.
    //
    // The pipeline for a full-screen post-process pass uses a fixed
    // full-screen-triangle vertex shader paired with the named fragment shader.
    //
    // target_format must match the render target the pass writes to.
    // Returns nullptr if the shader binary cannot be loaded.
    //
    /// @param num_samplers  Exact count of texture/sampler bindings the fragment
    ///                      shader declares.  Derived from PassDesc::reads.size()
    ///                      so the shader metadata matches the binary exactly.
    ///                      Over-declaring wastes descriptor space and triggers
    ///                      Vulkan validation warnings.
    /**
     * @brief Returns or create
     *
     * @param shader_key     Lookup key
     * @param target_format  Format descriptor
     * @param num_samplers   Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'get_or_create' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Returns or create
     *
     * @param shader_key     Lookup key
     * @param target_format  Format descriptor
     * @param num_samplers   Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'get_or_create' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Returns or create
     *
     * @param shader_key     Lookup key
     * @param target_format  Format descriptor
     * @param num_samplers   Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'get_or_create' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Returns or create
     *
     * @param shader_key     Lookup key
     * @param target_format  Format descriptor
     * @param num_samplers   Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'get_or_create' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Returns or create
     *
     * @param shader_key     Lookup key
     * @param target_format  Format descriptor
     * @param num_samplers   Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'get_or_create' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Returns or create
     *
     * @param shader_key     Lookup key
     * @param target_format  Format descriptor
     * @param num_samplers   Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'get_or_create' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Returns or create
     *
     * @param shader_key     Lookup key
     * @param target_format  Format descriptor
     * @param num_samplers   Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'get_or_create' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Returns or create
     *
     * @param shader_key     Lookup key
     * @param target_format  Format descriptor
     * @param num_samplers   Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'get_or_create' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Returns or create
     *
     * @param shader_key     Lookup key
     * @param target_format  Format descriptor
     * @param num_samplers   Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'get_or_create' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    [[nodiscard]]
    SDL_GPUGraphicsPipeline* get_or_create(
        std::string_view     shader_key,
        SDL_GPUTextureFormat target_format,
        uint8_t              num_samplers) noexcept;


    // create_sampler — shared linear-clamp sampler used for all pass inputs.
    // Created once; caller does NOT own the returned pointer.
    /**
     * @brief Linear sampler
     *
     * @return Pointer to the result, or nullptr on failure
     */
    /**
     * @brief Linear sampler
     *
     * @return Pointer to the result, or nullptr on failure
     */
    /**
     * @brief Linear sampler
     *
     * @return Pointer to the result, or nullptr on failure
     */
    /**
     * @brief Linear sampler
     *
     * @return Pointer to the result, or nullptr on failure
     */
    /**
     * @brief Linear sampler
     *
     * @return Pointer to the result, or nullptr on failure
     */
    /**
     * @brief Linear sampler
     *
     * @return Pointer to the result, or nullptr on failure
     */
    /**
     * @brief Linear sampler
     *
     * @return Pointer to the result, or nullptr on failure
     */
    /**
     * @brief Linear sampler
     *
     * @return Pointer to the result, or nullptr on failure
     */
    /**
     * @brief Linear sampler
     *
     * @return Pointer to the result, or nullptr on failure
     */
    [[nodiscard]]
    SDL_GPUSampler* linear_sampler() noexcept;

    /**
     * @brief Releases all
     */
    void release_all() noexcept;

private:
    // load_shader — read a compiled shader binary from disk.
    // Tries <shader_dir>/<name>.{metal,spv,dxil} depending on shader_format_.
    // Returns nullptr on failure.
    //
    /// @param num_frag_samplers  Number of sampler bindings declared in the
    ///                           fragment shader.  Ignored for vertex shaders
    ///                           (always 0).  Must match the binary exactly.
    /**
     * @brief Loads shader
     *
     * @param name               Human-readable name or identifier string
     * @param stage              String tag used for lookup or categorisation
     * @param num_frag_samplers  Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     */
    /**
     * @brief Loads shader
     *
     * @param name               Human-readable name or identifier string
     * @param stage              String tag used for lookup or categorisation
     * @param num_frag_samplers  Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     */
    /**
     * @brief Loads shader
     *
     * @param name               Human-readable name or identifier string
     * @param stage              String tag used for lookup or categorisation
     * @param num_frag_samplers  Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     */
    /**
     * @brief Loads shader
     *
     * @param name               Human-readable name or identifier string
     * @param stage              String tag used for lookup or categorisation
     * @param num_frag_samplers  Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     */
    /**
     * @brief Loads shader
     *
     * @param name               Human-readable name or identifier string
     * @param stage              String tag used for lookup or categorisation
     * @param num_frag_samplers  Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     */
    /**
     * @brief Loads shader
     *
     * @param name               Human-readable name or identifier string
     * @param stage              String tag used for lookup or categorisation
     * @param num_frag_samplers  Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     */
    /**
     * @brief Loads shader
     *
     * @param name               Human-readable name or identifier string
     * @param stage              String tag used for lookup or categorisation
     * @param num_frag_samplers  Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     */
    /**
     * @brief Loads shader
     *
     * @param name               Human-readable name or identifier string
     * @param stage              String tag used for lookup or categorisation
     * @param num_frag_samplers  Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     */
    /**
     * @brief Loads shader
     *
     * @param name               Human-readable name or identifier string
     * @param stage              String tag used for lookup or categorisation
     * @param num_frag_samplers  Numeric count
     *
     * @return Pointer to the result, or nullptr on failure
     */
    [[nodiscard]]
    SDL_GPUShader* load_shader(std::string_view   name,
                               SDL_GPUShaderStage stage,
                               uint8_t            num_frag_samplers) noexcept;

    SDL_GPUDevice*      device_         = nullptr;
    SDL_GPUShaderFormat shader_format_  = SDL_GPU_SHADERFORMAT_INVALID;
    std::string         shader_dir_;

    std::unordered_map<std::string, SDL_GPUGraphicsPipeline*> cache_;
    SDL_GPUSampler*     sampler_ = nullptr;
};


/// FrameGraph
//
// The authoritative description of the render pipeline, parsed from
// pipeline.pug and mutated by CSS / EventBus.  Compiles down to a
// CompiledGraph for zero-overhead execution.
class FrameGraph {
public:
    //
    // Construction
    //
    /**
     * @brief Frame graph
     */
    FrameGraph() = default;

    /**
     * @brief Frame graph
     *
     * @param param0  Red channel component [0, 1]
     */
    FrameGraph(const FrameGraph&)            = delete;
    FrameGraph& operator=(const FrameGraph&) = delete;

    /**
     * @brief Frame graph
     *
     * @param param0  Red channel component [0, 1]
     */
    FrameGraph(FrameGraph&&)                 = default;
    FrameGraph& operator=(FrameGraph&&)      = default;

    // from_pug — parse pipeline.pug source and create a FrameGraph.
    //
    // @param source          Full text of the pipeline.pug file.
    // @param device          Active SDL_GPUDevice.
    // @param shader_dir      Path to compiled shader binaries.
    // @param shader_format   SDL_GPUShaderFormat for this platform.
    // @param swapchain_w/h   Current swapchain dimensions in physical pixels.
    //
    // Returns FrameGraph on success, or a human-readable error string.
    //
    static std::expected<FrameGraph, std::string>
    /**
     * @brief From pug
     *
     * @param source         Red channel component [0, 1]
     * @param device         SDL3 GPU device handle
     * @param shader_dir     Directory path
     * @param shader_format  Format descriptor
     * @param swapchain_w    Opaque resource handle
     * @param swapchain_h    Opaque resource handle
     *
     * @return Integer result; negative values indicate an error code
     */
    from_pug(std::string_view     source,
             SDL_GPUDevice*       device,
             std::string_view     shader_dir,
             SDL_GPUShaderFormat  shader_format,
             uint32_t             swapchain_w,
             uint32_t             swapchain_h) noexcept;

    //
    static std::expected<FrameGraph, std::string>
    /**
     * @brief From file
     *
     * @param path           Filesystem path
     * @param device         SDL3 GPU device handle
     * @param shader_dir     Directory path
     * @param shader_format  Format descriptor
     * @param swapchain_w    Opaque resource handle
     * @param swapchain_h    Opaque resource handle
     *
     * @return Integer result; negative values indicate an error code
     */
    from_file(std::string_view     path,
              SDL_GPUDevice*       device,
              std::string_view     shader_dir,
              SDL_GPUShaderFormat  shader_format,
              uint32_t             swapchain_w,
              uint32_t             swapchain_h) noexcept;

    //
    // compile — resolve all descriptors into a zero-overhead CompiledGraph.
    //
    // Must be called:
    //   • after from_pug() / from_file()
    //   • after any apply_css() or apply_style() call that changes a shader
    //     variant (which requires a new pipeline object)
    //   • after resize() if swapchain-sized textures changed
    //
    // CSS-only param changes (Bucket C — density, focal, …) do NOT require
    // a recompile; call CompiledGraph::apply_style() directly instead.
    //
    // @param swapchain_fmt  SDL_GPUTextureFormat of the final swapchain texture.
    //                       Used for the last pass's render target format.
    //
    /**
     * @brief Compile
     *
     * @param swapchain_fmt  printf-style format string
     *
     * @return CompiledGraph result
     */
    CompiledGraph compile(SDL_GPUTextureFormat swapchain_fmt) noexcept;

    //
    // resize — notify that the swapchain dimensions changed.
    //
    // Invalidates all Swapchain-sized ResourcePool entries.
    // The caller must call compile() again after resize() to produce a fresh
    // CompiledGraph with updated output pointers.
    //
    /**
     * @brief Resizes
     *
     * @param new_w  Width in logical pixels
     * @param new_h  Opaque resource handle
     */
    void resize(uint32_t new_w, uint32_t new_h) noexcept;

    //
    // CSS integration
    //
    // apply_css() walks all rules in the stylesheet and applies matching
    // properties to pass descriptors — exactly like StyleSheet::applyTo() does
    // for RenderTree nodes.
    //
    // The selector targets the pass id:
    //   #fog { density: 0.04; }           → PassDesc "fog" params["density"] = "0.04"
    //   pipeline.night #grade { lut: … }  → applied when "night" class is set
    //
    // After apply_css(), call compile() to propagate Bucket-A variant changes,
    // or call compiled_graph.apply_style() directly for Bucket-C param changes.
    //
    /**
     * @brief Applies css
     *
     * @param sheet  Opaque resource handle
     */
    void apply_css(const css::StyleSheet& sheet) noexcept;

    /// Direct style setter — bypasses CSS, mutates the PassDesc immediately.
    /// Key "enabled" sets PassDesc::enabled (string "false" → false).
    /// All other keys update PassDesc::params.
    void apply_style(std::string_view pass_id,
                     std::string_view key,
                     std::string_view val) noexcept;

    //
    // EventBus wiring
    //
    // Wire common pipeline mutations to EventBus topics so CSS class changes
    // and game events can morph the pipeline without touching the FrameGraph
    // directly.
    //
    // Example:
    //   fg.wire_bus(bus, compiled_graph);
    //   bus.publish("quality:low",   "")  → disables dof + motion
    //   bus.publish("theme:night",   "")  → adds "night" class, re-applies CSS
    //   bus.publish("theme:default", "")  → removes "night" class, re-applies CSS
    //
    /**
     * @brief Wire bus
     *
     * @param bus    Blue channel component [0, 1]
     * @param cg     Green channel component [0, 1]
     * @param sheet  Opaque resource handle
     */
    void wire_bus(EventBus&       bus,
                  CompiledGraph&  cg,
                  css::StyleSheet& sheet) noexcept;

    //
    // Class toggle — add or remove a class token on the pipeline root node.
    // Triggers a CSS re-apply for all rules scoped to that class.
    // Does NOT require a full recompile unless a Bucket-A variant changes.
    /**
     * @brief Adds class
     *
     * @param cls    Signed 32-bit integer
     * @param cg     Green channel component [0, 1]
     * @param sheet  Opaque resource handle
     */
    void add_class(std::string_view    cls,
                   CompiledGraph&      cg,
                   css::StyleSheet&    sheet) noexcept;

    /**
     * @brief Removes class
     *
     * @param cls    Signed 32-bit integer
     * @param cg     Green channel component [0, 1]
     * @param sheet  Opaque resource handle
     */
    void remove_class(std::string_view cls,
                      CompiledGraph&   cg,
                      css::StyleSheet& sheet) noexcept;

    //
    // Accessors
    //
    /**
     * @brief Variants
     *
     * @return Integer result; negative values indicate an error code
     */
    const std::vector<VariantDesc>&  variants()  const noexcept { return parsed_.variants;  }
    /**
     * @brief Resources
     *
     * @return Integer result; negative values indicate an error code
     */
    const std::vector<ResourceDesc>& resources() const noexcept { return parsed_.resources; }
    /**
     * @brief Passes
     *
     * @return Integer result; negative values indicate an error code
     */
    const std::vector<PassDesc>&     passes()    const noexcept { return parsed_.passes;    }

    /**
     * @brief Valid
     *
     * @return true on success, false on failure
     */
    bool valid()  const noexcept { return !parsed_.passes.empty() && device_ != nullptr; }
    /**
     * @brief Empty
     *
     * @return true on success, false on failure
     */
    bool empty()  const noexcept { return  parsed_.passes.empty(); }

    /**
     * @brief Swaps chain w
     *
     * @return Integer result; negative values indicate an error code
     */
    uint32_t swapchain_w() const noexcept { return pool_.swapchain_w(); }
    /**
     * @brief Swaps chain h
     *
     * @return Integer result; negative values indicate an error code
     */
    uint32_t swapchain_h() const noexcept { return pool_.swapchain_h(); }

private:
    //
    // compile_params — flatten a PassDesc::params StyleMap into CompiledParams.
    //
    // Keys are iterated in the order small_flat_map stores them (insertion
    // order for our implementation, alphabetical if always inserted sorted).
    // The shader's cbuffer must declare uniforms in the same order.
    // "enabled", "reads", "shader", "writes" are meta-keys and are skipped.
    //
    /**
     * @brief Compile params
     *
     * @param params  Red channel component [0, 1]
     *
     * @return CompiledParams result
     */
    static CompiledParams compile_params(const StyleMap& params) noexcept;

    //
    // resolve_output — return the SDL_GPUTexture* for a pass's writes= field.
    // "swapchain" maps to the swapchain texture passed in each frame;
    // all other names are looked up in the ResourcePool.
    //
    // We store nullptr for the swapchain entry — execute() substitutes the
    // real per-frame swapchain pointer at draw time.
    //
    /**
     * @brief Resolves output
     *
     * @param writes_id  Unique object identifier
     *
     * @return Pointer to the result, or nullptr on failure
     */
    SDL_GPUTexture* resolve_output(std::string_view writes_id) noexcept;

    SDL_GPUDevice*  device_ = nullptr;
    ParseResult     parsed_;                ///< immutable after from_pug()
    ResourcePool    pool_;                  ///< owns transient textures
    ShaderLibrary   shaders_;              ///< owns compiled pipelines

    /// Pipeline-root CSS class tokens (e.g. "night", "low-power").
    /// apply_css() re-runs scoped rules when this set changes.
    std::vector<std::string> classes_;

    /// Mutable pass descriptors — apply_style() and apply_css() write here.
    /// compile() reads from here to produce CompiledGraph.
    std::vector<PassDesc> pass_overrides_;
};

} // namespace pce::sdlos::fg
