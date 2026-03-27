#pragma once
// pce::sdlos::fg::pug  —  pipeline.pug parser
//
// Parses a JadeLite pipeline descriptor into three descriptor vectors:
//   variant  — Bucket-A compiled shader variants  (brdf, shadow model …)
//   resource — named transient GPU textures
//   pass     — render passes in declaration (topological) order
//
// Grammar (line-oriented, indentation ignored at pipeline level):
//
//   comment     := '//' .* newline
//   declaration := tag '#' id '(' attrs? ')' newline
//   tag         := 'variant' | 'resource' | 'pass'
//   attrs       := attr (' ' attr)*
//   attr        := key '=' '"' value '"'
//
// Example:
//   variant#pbr_standard(brdf="ggx" shadow="pcf")
//   resource#lit(format="rgba16f" size="swapchain")
//   pass#fog(shader="volumetric" reads="lit depth" writes="lit"
//            density="0.02" color="#aabbcc")

#include <charconv>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL_gpu.h>

namespace pce::sdlos::fg {

// ---------------------------------------------------------------------------
// StyleMap  —  flat string→string attribute store (same concept as RenderNode)
//              Stored as a sorted vector of pairs; N=16 covers all pass attrs.
// ---------------------------------------------------------------------------
struct StyleEntry { std::string key; std::string val; };

class StyleMap {
public:
    using value_type = StyleEntry;

    void insert_or_assign(std::string key, std::string val) {
        for (auto& e : entries_)
            if (e.key == key) { e.val = std::move(val); return; }
        entries_.push_back({ std::move(key), std::move(val) });
    }

    [[nodiscard]] const std::string* find(std::string_view key) const noexcept {
        for (auto& e : entries_)
            if (e.key == key) return &e.val;
        return nullptr;
    }

    [[nodiscard]] bool   empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] size_t size()  const noexcept { return entries_.size();  }

    auto begin() const noexcept { return entries_.begin(); }
    auto end()   const noexcept { return entries_.end();   }

private:
    std::vector<StyleEntry> entries_;
};

// ---------------------------------------------------------------------------
// TexFormat / TexSize
// ---------------------------------------------------------------------------
enum class TexFormat : uint8_t {
    RGBA8,       // SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM
    RGBA16F,     // SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
    Depth32F,    // SDL_GPU_TEXTUREFORMAT_D32_FLOAT
};

enum class TexSize : uint8_t {
    Swapchain,   // recreated on every window resize to match swapchain
    Fixed,       // explicit w / h pixels
};

[[nodiscard]] inline SDL_GPUTextureFormat to_sdl_format(TexFormat f) noexcept {
    switch (f) {
        case TexFormat::RGBA16F:  return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        case TexFormat::Depth32F: return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        default:                  return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    }
}

// ---------------------------------------------------------------------------
// Descriptors  —  produced by parse(), immutable after parse
// ---------------------------------------------------------------------------

/// Bucket-A variant: maps to a pre-compiled SDL_GPUGraphicsPipeline.
/// `defines` carries the structural choices (brdf, shadow) used to select
/// the right compiled shader binary at FrameGraph::compile() time.
struct VariantDesc {
    std::string id;       ///< e.g. "pbr_standard"
    StyleMap    defines;  ///< brdf="ggx"  shadow="pcf"  …
};

/// Named transient texture that lives in the ResourcePool.
struct ResourceDesc {
    std::string id;
    TexFormat   format = TexFormat::RGBA8;
    TexSize     size   = TexSize::Swapchain;
    uint32_t    w      = 0;  ///< only used when size == Fixed
    uint32_t    h      = 0;
};

/// One render pass in the pipeline.
///
/// `reads`   — resource ids consumed as fragment sampler inputs.
/// `writes`  — resource id used as render target, or "swapchain".
/// `params`  — Bucket-C runtime uniforms (fog density, DoF focal, …).
///             Keys are mapped to float slots in alphabetical order to match
///             the shader's uniform struct layout.
/// `enabled` — false → frame graph skips this pass entirely (zero cost).
struct PassDesc {
    std::string              id;
    std::string              shader_key;   ///< matches a VariantDesc id or a raw shader name
    std::vector<std::string> reads;        ///< ["lit", "depth"]
    std::string              writes;       ///< "lit"  or  "swapchain"
    StyleMap                 params;       ///< density="0.02"  focal="10.0"  …
    bool                     enabled = true;
};

// ---------------------------------------------------------------------------
// ParseResult
// ---------------------------------------------------------------------------
struct ParseResult {
    std::vector<VariantDesc>  variants;
    std::vector<ResourceDesc> resources;
    std::vector<PassDesc>     passes;
};

} // namespace pce::sdlos::fg

// ---------------------------------------------------------------------------
// pce::sdlos::fg::pug — parse entry point
// ---------------------------------------------------------------------------
namespace pce::sdlos::fg::pug {

/// Parse a pipeline.pug source string.
///
/// Returns ParseResult on success.
/// Returns a human-readable error string on failure.
/// The result preserves declaration order — passes are already in
/// topological order as authored.
[[nodiscard]]
std::expected<ParseResult, std::string> parse(std::string_view src) noexcept;

} // namespace pce::sdlos::fg::pug
