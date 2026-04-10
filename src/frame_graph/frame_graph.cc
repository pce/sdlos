// FrameGraph — parse, compile, CSS integration, EventBus wiring.
//
// compile() is the only place strings are resolved to pointers.
// Everything produced here feeds into CompiledGraph::execute() which
// contains no string work and no heap traffic.

#include "frame_graph.h"

#include "pug_parser.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <ranges>
#include <sstream>

namespace pce::sdlos::fg {

namespace {

/// FNV-1a hash — string_view overload for std::string keys.
[[nodiscard]]
static uint32_t fnv1a_str(std::string_view s) noexcept {
    uint32_t h = 2'166'136'261u;
    for (unsigned char c : s)
        h = (h ^ c) * 16'777'619u;
    return h;
}

/// Load a text file from disk into a std::string.
[[nodiscard]]
static std::expected<std::string, std::string> read_file(std::string_view path) noexcept {
    std::ifstream f{std::string(path), std::ios::binary};
    if (!f)
        return std::unexpected(std::string("cannot open: ") + std::string(path));

    std::ostringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

/// Flatten a PassDesc::params StyleMap into CompiledParams + slot reverse-map.
/// Meta-keys (enabled / reads / shader / writes) are skipped.
[[nodiscard]]
static std::pair<CompiledParams, CompiledPass> build_pass_params(const StyleMap &params) noexcept {
    static constexpr std::string_view k_meta[] = {"enabled", "reads", "shader", "writes"};
    auto is_meta                               = [](std::string_view k) noexcept {
        for (auto m : k_meta)
            if (k == m)
                return true;
        return false;
    };

    CompiledParams cp;
    CompiledPass stub;  // carries the slot_map only

    uint8_t slot = 0;
    for (const auto &e : params) {
        if (is_meta(e.key))
            continue;
        if (slot >= 16)
            break;  // hard cap

        float val = 0.f;
        std::from_chars(e.val.data(), e.val.data() + e.val.size(), val);
        cp.set(slot, val);

        if (stub.slot_count < 16) {
            stub.slot_map[stub.slot_count++] = {fnv1a_str(e.key), slot};
        }

        // Record the "time" slot so execute() can inject wall-clock time
        // each frame with zero string work on the hot path.
        if (e.key == "time")
            stub.time_slot = slot;

        ++slot;
    }

    return {cp, stub};
}

}  // anonymous namespace

/// ShaderLibrary::load_shader
//
// File lookup: <shader_dir>/<name>.<vert|frag>.{metal|spv|dxil}
// num_frag_samplers must match the binary's declared sampler count exactly;
// over-declaring wastes descriptor space and triggers Vulkan validation warnings.
/**
 * @brief Loads shader
 *
 * @param name               Human-readable name or identifier string
 * @param stage              String tag used for lookup or categorisation
 * @param num_frag_samplers  Numeric count
 *
 * @return Pointer to the result, or nullptr on failure
 */
SDL_GPUShader *ShaderLibrary::load_shader(
    std::string_view name,
    SDL_GPUShaderStage stage,
    uint8_t num_frag_samplers) noexcept {
    if (!device_)
        return nullptr;

    const char *ext = [&]() noexcept -> const char * {
        switch (shader_format_) {
        case SDL_GPU_SHADERFORMAT_MSL:
            return ".metal";
        case SDL_GPU_SHADERFORMAT_SPIRV:
            return ".spv";
        case SDL_GPU_SHADERFORMAT_DXIL:
            return ".dxil";
        default:
            return ".spv";
        }
    }();

    const char *stage_str = (stage == SDL_GPU_SHADERSTAGE_VERTEX) ? ".vert" : ".frag";

    // Build path: <shader_dir>/<name><stage_str><ext>
    std::string path;
    path.reserve(shader_dir_.size() + 1 + name.size() + std::strlen(stage_str) + std::strlen(ext));
    path  = shader_dir_;
    path += '/';
    path += name;
    path += stage_str;
    path += ext;

    // Load binary.
    const auto file = read_file(path);
    if (!file) {
        SDL_Log("[FrameGraph] shader not found: %s", path.c_str());
        return nullptr;
    }

    SDL_GPUShaderCreateInfo info{};
    info.code      = reinterpret_cast<const uint8_t *>(file->data());
    info.code_size = file->size();
    info.format    = shader_format_;
    info.stage     = stage;
    // Convention: every shader exports the function named "main0".
    info.entrypoint = "main0";
    // Sampler count: caller provides the exact count from PassDesc::reads so
    // we never over-declare.  Vertex shaders have no sampler bindings.
    info.num_samplers =
        (stage == SDL_GPU_SHADERSTAGE_FRAGMENT) ? static_cast<Uint32>(num_frag_samplers) : 0u;
    // One uniform block (Bucket-C params) at binding 0.
    // Passes with no Bucket-C params still declare the block; the push is a
    // no-op when CompiledParams::empty() is true, so this is harmless.
    info.num_uniform_buffers = (stage == SDL_GPU_SHADERSTAGE_FRAGMENT) ? 1u : 0u;

    return SDL_CreateGPUShader(device_, &info);
}

// ShaderLibrary::get_or_create
//
// Returns a cached pipeline or compiles a new one.  Vertex stage is always
// the shared fullscreen triangle shader; positions are generated from
// SV_VertexID so no vertex buffer is required.
SDL_GPUGraphicsPipeline
    *
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
     *          pointer parameter — ownership is ambiguous; consider std::span (non-owning
     *          view), std::unique_ptr (transfer), or const T* (borrow)
     */
    ShaderLibrary::get_or_create(
        std::string_view shader_key,
        SDL_GPUTextureFormat target_format,
        uint8_t num_samplers) noexcept {
    if (!device_)
        return nullptr;

    // Cache key: shader_key + target_format + sampler_count.
    // sampler_count is included because a shader loaded with num_samplers=1
    // produces a different shader object than one loaded with num_samplers=2;
    // the pipeline must match what was declared at shader-creation time.
    const std::string cache_key = std::string(shader_key) + ':'
                                + std::to_string(static_cast<int>(target_format)) + ':'
                                + std::to_string(static_cast<int>(num_samplers));

    if (const auto it = cache_.find(cache_key); it != cache_.end())
        return it->second;

    // Load vertex shader (shared fullscreen triangle).
    // Vertex stage has no sampler bindings (num_frag_samplers=0 ignored).
    SDL_GPUShader *vert = load_shader("fullscreen", SDL_GPU_SHADERSTAGE_VERTEX, 0u);
    if (!vert) {
        SDL_Log("[FrameGraph] failed to load fullscreen vertex shader");
        return nullptr;
    }

    // Load fragment shader for this pass with the exact sampler count derived
    // from PassDesc::reads so the shader metadata matches the binary.
    SDL_GPUShader *frag = load_shader(shader_key, SDL_GPU_SHADERSTAGE_FRAGMENT, num_samplers);
    if (!frag) {
        SDL_Log(
            "[FrameGraph] failed to load fragment shader: %.*s",
            static_cast<int>(shader_key.size()),
            shader_key.data());
        SDL_ReleaseGPUShader(device_, vert);
        return nullptr;
    }

    // Build graphics pipeline.
    SDL_GPUGraphicsPipelineCreateInfo ci{};

    // Vertex stage — no vertex buffers; positions reconstructed in shader.
    ci.vertex_shader   = vert;
    ci.fragment_shader = frag;

    // Colour attachment — post-process passes always write one colour target.
    SDL_GPUColorTargetDescription color_target{};
    color_target.format                   = target_format;
    color_target.blend_state.enable_blend = false;

    ci.target_info.color_target_descriptions = &color_target;
    ci.target_info.num_color_targets         = 1;
    ci.target_info.has_depth_stencil_target  = false;

    // Rasterizer — full coverage, no backface culling for a screen quad.
    ci.rasterizer_state.fill_mode  = SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode  = SDL_GPU_CULLMODE_NONE;
    ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    // No depth test.
    ci.depth_stencil_state.enable_depth_test  = false;
    ci.depth_stencil_state.enable_depth_write = false;

    // Primitive type — triangle list (3 verts = 1 full-screen triangle).
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(device_, &ci);

    if (!pipeline) {
        SDL_Log(
            "[FrameGraph] SDL_CreateGPUGraphicsPipeline failed for: %.*s",
            static_cast<int>(shader_key.size()),
            shader_key.data());

        // Log extra details for debugging
        SDL_Log("  Vertex shader: %p", (void *)vert);
        SDL_Log("  Fragment shader: %p", (void *)frag);
        SDL_Log("  Target format: %d", (int)target_format);
        SDL_Log("  Num samplers: %d", (int)num_samplers);

        return nullptr;
    }

    cache_[cache_key] = pipeline;
    return pipeline;
}

// ShaderLibrary::linear_sampler / release_all
/**
 * @brief Linear sampler
 *
 * @return Pointer to the result, or nullptr on failure
 */
SDL_GPUSampler *ShaderLibrary::linear_sampler() noexcept {
    if (sampler_)
        return sampler_;
    if (!device_)
        return nullptr;

    SDL_GPUSamplerCreateInfo si{};
    si.min_filter     = SDL_GPU_FILTER_LINEAR;
    si.mag_filter     = SDL_GPU_FILTER_LINEAR;
    si.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

    sampler_ = SDL_CreateGPUSampler(device_, &si);
    return sampler_;
}

/**
 * @brief Releases all
 */
void ShaderLibrary::release_all() noexcept {
    if (!device_)
        return;

    for (auto &[k, p] : cache_) {
        if (p)
            SDL_ReleaseGPUGraphicsPipeline(device_, p);
    }
    cache_.clear();

    if (sampler_) {
        SDL_ReleaseGPUSampler(device_, sampler_);
        sampler_ = nullptr;
    }
}

// FrameGraph::from_pug
[[nodiscard]]
std::
    expected<FrameGraph, std::string>
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
    FrameGraph::from_pug(
        std::string_view source,
        SDL_GPUDevice *device,
        std::string_view shader_dir,
        SDL_GPUShaderFormat shader_format,
        uint32_t swapchain_w,
        uint32_t swapchain_h) noexcept {
    if (!device)
        return std::unexpected(std::string("from_pug: device is null"));

    // Parse the source text.
    auto result = pug::parse(source);
    if (!result)
        return std::unexpected(result.error());

    if (result->passes.empty())
        return std::unexpected(std::string("from_pug: no passes declared"));

    FrameGraph fg;
    fg.device_ = device;
    fg.parsed_ = std::move(*result);

    // Initialise subsystems.
    fg.pool_.init(device, swapchain_w, swapchain_h);
    fg.pool_.register_descriptors(fg.parsed_.resources);

    fg.shaders_.init(device, shader_dir, shader_format);

    // Initial pass state from parsed descriptors.
    fg.pass_overrides_ = fg.parsed_.passes;

    return fg;
}

// FrameGraph::from_file
[[nodiscard]]
std::
    expected<FrameGraph, std::string>
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
    FrameGraph::from_file(
        std::string_view path,
        SDL_GPUDevice *device,
        std::string_view shader_dir,
        SDL_GPUShaderFormat shader_format,
        uint32_t swapchain_w,
        uint32_t swapchain_h) noexcept {
    auto src = read_file(path);
    if (!src)
        return std::unexpected(src.error());

    return from_pug(*src, device, shader_dir, shader_format, swapchain_w, swapchain_h);
}

// FrameGraph::compile
//
// Resolve all descriptors into a flat CompiledGraph.
//
// For each pass in pass_overrides_ (in declaration order):
//   1. Look up the SDL_GPUGraphicsPipeline* for the shader_key.
//   2. Resolve the output texture from the ResourcePool
//      (nullptr sentinel for "swapchain").
//   3. Resolve each reads= resource to a ResourceBinding.
//   4. Compile params into a CompiledParams struct + slot reverse-map.
//   5. Set id_hash = fnv1a(pass.id) for O(1) patch() dispatch.
//
// The swapchain_fmt is needed to create pipelines with the correct
// render-target format for the final (swapchain-writing) pass.
// All intermediate passes use RGBA16F to preserve HDR headroom.
/**
 * @brief Compile
 *
 * @param swapchain_fmt  printf-style format string
 *
 * @return CompiledGraph result
 */
CompiledGraph FrameGraph::compile(SDL_GPUTextureFormat swapchain_fmt) noexcept {
    CompiledGraph cg;
    cg.sampler = shaders_.linear_sampler();

    cg.passes.reserve(pass_overrides_.size());

    for (const PassDesc &pd : pass_overrides_) {
        CompiledPass cp;
        cp.id_hash = fnv1a_str(pd.id);
        cp.enabled = pd.enabled;

        // "swapchain" → nullptr sentinel; execute() substitutes the per-frame pointer.
        if (pd.writes == "swapchain" || pd.writes.empty()) {
            cp.output = nullptr;
        } else {
            cp.output = pool_.acquire(pd.writes);
        }

        // Target format: swapchain_fmt for the final pass, RGBA16F for intermediates.
        const SDL_GPUTextureFormat rt_fmt =
            (cp.output == nullptr) ? swapchain_fmt : SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

        // Sampler count from pd.reads (descriptor), not cp.bind_count (set below);
        // unresolved reads warn but must not change the binary's declared count.
        if (!pd.shader_key.empty())
            cp.pipeline = shaders_.get_or_create(
                pd.shader_key,
                rt_fmt,
                static_cast<uint8_t>(pd.reads.size()));

        SDL_GPUSampler *sampler = cg.sampler;
        cp.bind_count           = 0;
        for (const auto &rid : pd.reads) {
            if (cp.bind_count >= 8)
                break;
            SDL_GPUTexture *tex = pool_.acquire(rid);
            if (!tex) {
                SDL_Log(
                    "[FrameGraph] compile: unresolved read resource '%s' in pass '%s'",
                    rid.c_str(),
                    pd.id.c_str());
                continue;
            }
            cp.bindings[cp.bind_count++] = {tex, sampler, static_cast<uint32_t>(cp.bind_count)};
        }

        auto [params, stub] = build_pass_params(pd.params);
        cp.params           = params;
        cp.slot_map         = stub.slot_map;
        cp.slot_count       = stub.slot_count;
        cp.time_slot        = stub.time_slot;  // 255 when no "time" param declared

        cg.passes.push_back(std::move(cp));
    }

    return cg;
}

/**
 * @brief Resizes
 *
 * @param new_w  Width in logical pixels
 * @param new_h  Opaque resource handle
 */
void FrameGraph::resize(uint32_t new_w, uint32_t new_h) noexcept {
    pool_.resize(new_w, new_h);
    // Caller must call compile() again to update output pointers in the
    // CompiledGraph — swapchain-sized textures have been recreated.
}

// FrameGraph::apply_style
//
// Direct imperative style setter — mutates the mutable pass_overrides_ table.
// "enabled" → PassDesc::enabled (bool).
// All other keys → PassDesc::params (string→string, Bucket C).
//
// For Bucket-C changes that do NOT affect the shader variant, the caller can
// update the live CompiledGraph via compiled_graph.apply_style() without a
// full recompile.
/**
 * @brief Applies style
 *
 * @param pass_id  Unique object identifier
 * @param key      Lookup key
 * @param val      Value to store or compare
 */
void FrameGraph::apply_style(
    std::string_view pass_id,
    std::string_view key,
    std::string_view val) noexcept {
    for (PassDesc &pd : pass_overrides_) {
        if (pd.id != pass_id)
            continue;

        if (key == "enabled") {
            pd.enabled = (val != "false" && val != "0");
        } else if (key == "shader") {
            pd.shader_key = std::string(val);
        } else {
            pd.params.insert_or_assign(std::string(key), std::string(val));
        }
        return;
    }
}

// Walk the stylesheet rules and apply matching properties to pass_overrides_.
//
// Selector matching:
//   #fog             → matches pass with id "fog"
//   pipeline.night #grade  → matches pass "grade" only when "night" is in
//                            classes_  (pipeline root class tokens)
//
// This mirrors how StyleSheet::applyTo() targets RenderTree nodes.
// After apply_css(), call compile() for Bucket-A variant changes, or
// call compiled_graph.apply_style() for Bucket-C param changes only.
/**
 * @brief Applies css
 *
 * @param sheet  Opaque resource handle
 */
void FrameGraph::apply_css(const css::StyleSheet &sheet) noexcept {
    for (const css::Rule &rule : sheet.rules) {
        // Only apply rules without a pseudo-class (hover/active/focus
        // are UI-only concepts for render passes).
        if (!rule.pseudo.empty())
            continue;

        // Parse compound selector of the form "pipeline.class #pass_id"
        // or simple "#pass_id".
        std::string_view sel = rule.selector;

        // Optional scope: "pipeline.night" prefix.
        std::string_view pass_sel = sel;
        bool scoped               = false;
        std::string required_class;

        const auto space = sel.rfind(' ');
        if (space != std::string_view::npos) {
            const auto scope = sel.substr(0, space);
            pass_sel         = sel.substr(space + 1);
            scoped           = true;

            // Extract the class token after "pipeline.".
            const auto dot = scope.find('.');
            if (dot != std::string_view::npos)
                required_class = std::string(scope.substr(dot + 1));
        }

        // If scoped, check that the required class is active on the pipeline root.
        if (scoped && !required_class.empty()) {
            const bool has_class = std::ranges::any_of(classes_, [&](const std::string &c) {
                return c == required_class;
            });
            if (!has_class)
                continue;
        }

        // The pass selector must start with '#' (id selector).
        if (pass_sel.empty() || pass_sel[0] != '#')
            continue;
        const auto pass_id = pass_sel.substr(1);

        // Apply all properties in the rule to the matching pass.
        for (PassDesc &pd : pass_overrides_) {
            if (pd.id != pass_id)
                continue;
            for (const auto &[key, val] : rule.props)
                apply_style(pd.id, key, val);
            break;
        }
    }
}

// Toggle a class token on the pipeline root.  Re-applies scoped CSS rules
// and updates the live CompiledGraph for Bucket-C param changes.
// A full recompile is triggered if any Bucket-A shader_key changes.
/**
 * @brief Adds class
 *
 * @param cls    Signed 32-bit integer
 * @param cg     Green channel component [0, 1]
 * @param sheet  Opaque resource handle
 */
void FrameGraph::add_class(
    std::string_view cls,
    CompiledGraph &cg,
    css::StyleSheet &sheet) noexcept {
    // Idempotent — don't add duplicates.
    const auto already =
        std::ranges::any_of(classes_, [&](const std::string &c) { return c == cls; });
    if (already)
        return;

    classes_.emplace_back(cls);

    // Re-apply CSS so scoped rules (.night, .low-power …) take effect.
    apply_css(sheet);

    // Propagate Bucket-C changes directly to the live CompiledGraph.
    // (Bucket-A changes require the caller to call compile() manually.)
    for (const PassDesc &pd : pass_overrides_) {
        cg.set_enabled(pd.id, pd.enabled);
        for (const auto &e : pd.params) {
            float f = 0.f;
            std::from_chars(e.val.data(), e.val.data() + e.val.size(), f);
            cg.patch(pd.id, e.key, f);
        }
    }
}

/**
 * @brief Removes class
 *
 * @param cls    Signed 32-bit integer
 * @param cg     Green channel component [0, 1]
 * @param sheet  Opaque resource handle
 */
void FrameGraph::remove_class(
    std::string_view cls,
    CompiledGraph &cg,
    css::StyleSheet &sheet) noexcept {
    const auto it = std::ranges::find(classes_, std::string(cls));
    if (it == classes_.end())
        return;

    classes_.erase(it);

    // Reset pass_overrides_ to the original parsed values, then re-apply
    // the remaining active CSS rules.
    pass_overrides_ = parsed_.passes;
    apply_css(sheet);

    // Propagate to the live CompiledGraph.
    for (const PassDesc &pd : pass_overrides_) {
        cg.set_enabled(pd.id, pd.enabled);
        for (const auto &e : pd.params) {
            float f = 0.f;
            std::from_chars(e.val.data(), e.val.data() + e.val.size(), f);
            cg.patch(pd.id, e.key, f);
        }
    }
}

// Connect common pipeline mutations to EventBus topics.
//
// Standard topics wired here:
//   "quality:high"     → enable all passes
//   "quality:low"      → disable dof + motion (common power-save pair)
//   "theme:<name>"     → add_class("<name>", cg, sheet)
//   "theme:default"    → remove all theme classes, re-apply base CSS
//   "pipeline:rebuild" → full recompile (for Bucket-A changes)
//
// Apps can publish additional topics to call cg.apply_style() directly.
/**
 * @brief Wire bus
 *
 * @param bus    Blue channel component [0, 1]
 * @param cg     Green channel component [0, 1]
 * @param sheet  Opaque resource handle
 */
void FrameGraph::wire_bus(EventBus &bus, CompiledGraph &cg, css::StyleSheet &sheet) noexcept {
    // quality:high — re-enable all passes (restore from low-power state).
    bus.subscribe("quality:high", [&cg](const std::string &) {
        for (auto &pass : cg.passes)
            pass.enabled = true;
    });

    // quality:low — disable the two most expensive post-process passes.
    bus.subscribe("quality:low", [&cg](const std::string &) {
        cg.set_enabled("dof", false);
        cg.set_enabled("motion", false);
    });

    // theme:<name> — add a class to the pipeline root.
    // The bus payload is the class name (e.g. bus.publish("theme:night", "night")).
    bus.subscribe("theme:add", [this, &cg, &sheet](const std::string &cls) {
        add_class(cls, cg, sheet);
    });

    // theme:remove — remove a class from the pipeline root.
    bus.subscribe("theme:remove", [this, &cg, &sheet](const std::string &cls) {
        remove_class(cls, cg, sheet);
    });

    // pipeline:pass:toggle — payload "pass_id:true|false".
    bus.subscribe("pipeline:pass:toggle", [&cg](const std::string &payload) {
        const auto sep = payload.find(':');
        if (sep == std::string::npos)
            return;
        const auto id  = std::string_view(payload).substr(0, sep);
        const auto val = std::string_view(payload).substr(sep + 1);
        cg.set_enabled(id, val != "false" && val != "0");
    });

    // pipeline:param — payload "pass_id:key:value".
    bus.subscribe("pipeline:param", [&cg](const std::string &payload) {
        const auto sep1 = payload.find(':');
        if (sep1 == std::string::npos)
            return;
        const auto sep2 = payload.find(':', sep1 + 1);
        if (sep2 == std::string::npos)
            return;

        const auto pass_id = std::string_view(payload).substr(0, sep1);
        const auto key     = std::string_view(payload).substr(sep1 + 1, sep2 - sep1 - 1);
        const auto val_str = std::string_view(payload).substr(sep2 + 1);

        float val = 0.f;
        std::from_chars(val_str.data(), val_str.data() + val_str.size(), val);
        cg.patch(pass_id, key, val);
    });
}

/**
 * @brief Compile params
 *
 * @param params  Red channel component [0, 1]
 *
 * @return CompiledParams result
 */
CompiledParams FrameGraph::compile_params(const StyleMap &params) noexcept {
    auto [cp, stub] = build_pass_params(params);
    (void)stub;  // slot_map written directly into CompiledPass in compile()
    return cp;
}

/**
 * @brief Resolves output
 *
 * @param writes_id  Unique object identifier
 *
 * @return Pointer to the result, or nullptr on failure
 */
SDL_GPUTexture *FrameGraph::resolve_output(std::string_view writes_id) noexcept {
    if (writes_id == "swapchain" || writes_id.empty())
        return nullptr;  // sentinel — execute() substitutes per-frame pointer
    return pool_.acquire(writes_id);
}

}  // namespace pce::sdlos::fg
