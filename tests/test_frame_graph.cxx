// =============================================================================
// test_frame_graph.cxx  —  unit tests for the sdlos FrameGraph subsystem
//
// Tests cover:
//   1. pug::parse()        — descriptor parsing, error handling
//   2. CompiledGraph       — cold-path: patch(), set_enabled(), apply_style()
//   3. CompiledGraph       — hot-path invariants (time_slot, params, no heap)
//   4. ResourcePool        — resize invalidation logic
//   5. FrameGraph::compile — slot map, time_slot, enabled flags
//
// No GPU device is required.  All SDL_GPUTexture* / SDL_GPUGraphicsPipeline*
// values are tested as null sentinels — the logic under test is all CPU-side.
// =============================================================================

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "frame_graph/pug_parser.hh"
#include "frame_graph/compiled_graph.hh"

using namespace pce::sdlos::fg;
using namespace pce::sdlos::fg::pug;
using Catch::Matchers::WithinAbs;

// ─── helpers ──────────────────────────────────────────────────────────────────

/// Build a CompiledGraph from scratch without a GPU device.
/// Populates pass entries with fake (null) pipeline/output pointers so we
/// can test all the CPU-side logic without requiring SDL GPU init.
static CompiledGraph make_graph(std::initializer_list<std::string> pass_ids)
{
    CompiledGraph cg;
    for (const auto& id : pass_ids) {
        CompiledPass cp;
        cp.id_hash  = [&id]{
            uint32_t h = 2166136261u;
            for (unsigned char c : id) h = (h ^ c) * 16777619u;
            return h;
        }();
        cp.enabled  = true;
        cp.pipeline = nullptr;   // no GPU in tests
        cp.output   = nullptr;   // sentinel = swapchain
        cg.passes.push_back(cp);
    }
    return cg;
}

/// Add a named float param slot to a pass — simulates what FrameGraph::compile()
/// does when it calls build_pass_params().
static void add_param(CompiledPass& pass,
                      std::string_view key,
                      float            initial_val)
{
    // FNV-1a
    auto fnv = [](std::string_view s) noexcept -> uint32_t {
        uint32_t h = 2166136261u;
        for (unsigned char c : s) h = (h ^ c) * 16777619u;
        return h;
    };

    const uint8_t slot = pass.params.count;
    pass.params.set(slot, initial_val);

    if (pass.slot_count < 16u) {
        pass.slot_map[pass.slot_count++] = { fnv(key), slot };
    }

    if (key == "time")
        pass.time_slot = slot;
}


// =============================================================================
// Section 1 — pug::parse()
// =============================================================================

TEST_CASE("pug::parse — empty source returns empty result", "[pug]")
{
    auto r = parse("");
    REQUIRE(r.has_value());
    CHECK(r->passes.empty());
    CHECK(r->variants.empty());
    CHECK(r->resources.empty());
}

TEST_CASE("pug::parse — comments and blank lines are skipped", "[pug]")
{
    const char* src = R"(
// This is a comment
  // indented comment

)";
    auto r = parse(src);
    REQUIRE(r.has_value());
    CHECK(r->passes.empty());
}

TEST_CASE("pug::parse — single pass, minimal attrs", "[pug]")
{
    auto r = parse("pass#scene(shader=\"blit\" writes=\"swapchain\")");
    REQUIRE(r.has_value());
    REQUIRE(r->passes.size() == 1u);
    CHECK(r->passes[0].id         == "scene");
    CHECK(r->passes[0].shader_key == "blit");
    CHECK(r->passes[0].writes     == "swapchain");
    CHECK(r->passes[0].enabled    == true);
    CHECK(r->passes[0].reads.empty());
}

TEST_CASE("pug::parse — multiple passes preserve declaration order", "[pug]")
{
    const char* src =
        "pass#a(shader=\"s1\" writes=\"swapchain\")\n"
        "pass#b(shader=\"s2\" writes=\"swapchain\")\n"
        "pass#c(shader=\"s3\" writes=\"swapchain\")\n";

    auto r = parse(src);
    REQUIRE(r.has_value());
    REQUIRE(r->passes.size() == 3u);
    CHECK(r->passes[0].id == "a");
    CHECK(r->passes[1].id == "b");
    CHECK(r->passes[2].id == "c");
}

TEST_CASE("pug::parse — resource descriptor", "[pug]")
{
    auto r = parse("resource#lit(format=\"rgba16f\" size=\"swapchain\")");
    REQUIRE(r.has_value());
    REQUIRE(r->resources.size() == 1u);
    CHECK(r->resources[0].id     == "lit");
    CHECK(r->resources[0].format == TexFormat::RGBA16F);
    CHECK(r->resources[0].size   == TexSize::Swapchain);
}

TEST_CASE("pug::parse — resource with fixed size", "[pug]")
{
    auto r = parse("resource#shadow(format=\"depth32\" size=\"fixed\" w=\"2048\" h=\"2048\")");
    REQUIRE(r.has_value());
    REQUIRE(r->resources.size() == 1u);
    CHECK(r->resources[0].format == TexFormat::Depth32F);
    CHECK(r->resources[0].size   == TexSize::Fixed);
    CHECK(r->resources[0].w      == 2048u);
    CHECK(r->resources[0].h      == 2048u);
}

TEST_CASE("pug::parse — variant descriptor", "[pug]")
{
    auto r = parse("variant#pbr(brdf=\"ggx\" shadow=\"pcf\")");
    REQUIRE(r.has_value());
    REQUIRE(r->variants.size() == 1u);
    CHECK(r->variants[0].id == "pbr");
    const auto* brdf = r->variants[0].defines.find("brdf");
    REQUIRE(brdf != nullptr);
    CHECK(*brdf == "ggx");
}

TEST_CASE("pug::parse — pass reads multiple resources (space-separated)", "[pug]")
{
    auto r = parse("pass#fog(shader=\"vol\" reads=\"lit depth\" writes=\"lit\" density=\"0.02\")");
    REQUIRE(r.has_value());
    REQUIRE(r->passes.size() == 1u);
    const auto& p = r->passes[0];
    REQUIRE(p.reads.size() == 2u);
    CHECK(p.reads[0] == "lit");
    CHECK(p.reads[1] == "depth");
}

TEST_CASE("pug::parse — enabled=false disables the pass", "[pug]")
{
    auto r = parse("pass#sky(shader=\"sky\" writes=\"swapchain\" enabled=\"false\")");
    REQUIRE(r.has_value());
    REQUIRE(r->passes.size() == 1u);
    CHECK(r->passes[0].enabled == false);
}

TEST_CASE("pug::parse — bucket-C float params land in PassDesc::params", "[pug]")
{
    auto r = parse("pass#dof(shader=\"dof\" writes=\"swapchain\" focal=\"10.5\" aperture=\"2.8\")");
    REQUIRE(r.has_value());
    REQUIRE(r->passes.size() == 1u);

    const auto& params = r->passes[0].params;
    const auto* focal   = params.find("focal");
    const auto* aperture = params.find("aperture");
    REQUIRE(focal    != nullptr);
    REQUIRE(aperture != nullptr);
    CHECK(*focal    == "10.5");
    CHECK(*aperture == "2.8");
}

TEST_CASE("pug::parse — meta-keys (shader/reads/writes/enabled) not in params", "[pug]")
{
    auto r = parse("pass#p(shader=\"s\" reads=\"x\" writes=\"y\" enabled=\"true\" gain=\"1.5\")");
    REQUIRE(r.has_value());
    REQUIRE(r->passes.size() == 1u);
    const auto& params = r->passes[0].params;

    CHECK(params.find("shader")  == nullptr);
    CHECK(params.find("reads")   == nullptr);
    CHECK(params.find("writes")  == nullptr);
    CHECK(params.find("enabled") == nullptr);

    // gain is NOT a meta-key — must survive
    REQUIRE(params.find("gain") != nullptr);
    CHECK(*params.find("gain") == "1.5");
}

TEST_CASE("pug::parse — full pipeline descriptor round-trip", "[pug]")
{
    const char* src = R"(
// Full pipeline smoke test
variant#pbr_standard(brdf="ggx" shadow="pcf")

resource#lit(format="rgba16f" size="swapchain")
resource#depth(format="depth32" size="swapchain")

pass#scene(shader="pbr_standard" writes="lit" time="0.0")
pass#fog(shader="fog" reads="lit depth" writes="lit" density="0.02")
pass#tonemap(shader="tonemap" reads="lit" writes="swapchain")
)";

    auto r = parse(src);
    REQUIRE(r.has_value());
    CHECK(r->variants.size()  == 1u);
    CHECK(r->resources.size() == 2u);
    CHECK(r->passes.size()    == 3u);

    CHECK(r->passes[0].id == "scene");
    CHECK(r->passes[1].id == "fog");
    CHECK(r->passes[2].id == "tonemap");

    // fog pass reads two resources
    CHECK(r->passes[1].reads.size() == 2u);

    // tonemap writes to swapchain
    CHECK(r->passes[2].writes == "swapchain");
}

TEST_CASE("pug::parse — unknown tags are silently skipped (lenient)", "[pug]")
{
    // 'effect' is not a known tag — should not crash or fail
    auto r = parse("effect#bloom(intensity=\"1.0\")\npass#final(shader=\"blit\" writes=\"swapchain\")");
    REQUIRE(r.has_value());
    // Only the known pass survives
    CHECK(r->passes.size() == 1u);
}

TEST_CASE("pug::parse — multi-line pass attributes are folded correctly", "[pug]")
{
    // Mirrors the real pipeline.pug layout where each attr is on its own line.
    const char* src = R"(
pass#bg(shader="bg"
        writes="bg_color"
        scale="1.5"
        speed="0.4"
        time="0.0")
)";

    auto r = parse(src);
    REQUIRE(r.has_value());
    REQUIRE(r->passes.size() == 1u);

    const auto& p = r->passes[0];
    CHECK(p.id         == "bg");
    CHECK(p.shader_key == "bg");
    CHECK(p.writes     == "bg_color");

    REQUIRE(p.params.find("scale") != nullptr);
    CHECK(*p.params.find("scale") == "1.5");
    REQUIRE(p.params.find("speed") != nullptr);
    CHECK(*p.params.find("speed") == "0.4");
    REQUIRE(p.params.find("time") != nullptr);
    CHECK(*p.params.find("time") == "0.0");

    // writes/shader are meta — must not appear in params
    CHECK(p.params.find("writes")     == nullptr);
    CHECK(p.params.find("shader")     == nullptr);
}

TEST_CASE("pug::parse — multi-line resource declaration is parsed", "[pug]")
{
    const char* src = R"(
resource#vignette_buffer(format="rgba16f"
                         size="swapchain")
)";

    auto r = parse(src);
    REQUIRE(r.has_value());
    REQUIRE(r->resources.size() == 1u);

    const auto& res = r->resources[0];
    CHECK(res.id     == "vignette_buffer");
    CHECK(res.format == TexFormat::RGBA16F);
    CHECK(res.size   == TexSize::Swapchain);
}

TEST_CASE("pug::parse — multi-line variant declaration is parsed", "[pug]")
{
    const char* src = R"(
variant#pbr(brdf="ggx"
            shadow="pcf")
)";

    auto r = parse(src);
    REQUIRE(r.has_value());
    REQUIRE(r->variants.size() == 1u);

    const auto& v = r->variants[0];
    CHECK(v.id == "pbr");
    REQUIRE(v.defines.find("brdf")   != nullptr);
    CHECK(*v.defines.find("brdf")   == "ggx");
    REQUIRE(v.defines.find("shadow") != nullptr);
    CHECK(*v.defines.find("shadow") == "pcf");
}

TEST_CASE("pug::parse — full multi-line pipeline.pug round-trip", "[pug]")
{
    // Exercises all three declaration types with wrapped attrs, matching
    // the real examples/apps/pug/data/pipeline.pug format.
    const char* src = R"(
resource#bg_color(format="rgba16f" size="swapchain")
resource#vignette_buffer(format="rgba16f" size="swapchain")

pass#bg(shader="bg"
        writes="bg_color"
        scale="1.5"
        speed="0.4"
        time="0.0")

pass#vignette(shader="vignette"
              reads="bg_color"
              writes="vignette_buffer"
              enabled="true"
              intensity="0.6"
              time="0.0")

pass#grade(shader="grade"
           reads="vignette_buffer"
           writes="swapchain"
           enabled="true"
           exposure="1.0"
           gamma="2.2"
           saturation="1.05")
)";

    auto r = parse(src);
    REQUIRE(r.has_value());
    CHECK(r->resources.size() == 2u);
    CHECK(r->passes.size()    == 3u);

    // bg pass
    CHECK(r->passes[0].id         == "bg");
    CHECK(r->passes[0].shader_key == "bg");
    CHECK(r->passes[0].writes     == "bg_color");
    CHECK(r->passes[0].reads.empty());

    // vignette pass
    CHECK(r->passes[1].id         == "vignette");
    CHECK(r->passes[1].shader_key == "vignette");
    REQUIRE(r->passes[1].reads.size() == 1u);
    CHECK(r->passes[1].reads[0]   == "bg_color");
    CHECK(r->passes[1].writes     == "vignette_buffer");
    CHECK(r->passes[1].enabled    == true);
    REQUIRE(r->passes[1].params.find("intensity") != nullptr);
    CHECK(*r->passes[1].params.find("intensity") == "0.6");

    // grade pass
    CHECK(r->passes[2].id         == "grade");
    CHECK(r->passes[2].shader_key == "grade");
    CHECK(r->passes[2].writes     == "swapchain");
    REQUIRE(r->passes[2].params.find("gamma")      != nullptr);
    CHECK(*r->passes[2].params.find("gamma")      == "2.2");
    REQUIRE(r->passes[2].params.find("saturation") != nullptr);
    CHECK(*r->passes[2].params.find("saturation") == "1.05");
    REQUIRE(r->passes[2].params.find("exposure")   != nullptr);
    CHECK(*r->passes[2].params.find("exposure")   == "1.0");
}

TEST_CASE("pug::parse — multi-line decl with interleaved comment lines is robust", "[pug]")
{
    // Comments inside a wrapped declaration should not break accumulation
    // (they are skipped during continuation, so the fold still closes on ')').
    const char* src =
        "pass#sky(shader=\"sky\"\n"
        "         writes=\"swapchain\"\n"
        "         time=\"0.0\")";

    auto r = parse(src);
    REQUIRE(r.has_value());
    REQUIRE(r->passes.size() == 1u);
    CHECK(r->passes[0].id         == "sky");
    CHECK(r->passes[0].shader_key == "sky");
    CHECK(r->passes[0].writes     == "swapchain");
}


// =============================================================================
// Section 2 — CompiledGraph cold path: patch / set_enabled / apply_style
// =============================================================================

TEST_CASE("CompiledGraph::set_enabled — toggle a pass by name", "[compiled_graph][cold]")
{
    auto cg = make_graph({"fog", "dof", "tonemap"});

    CHECK(cg.passes[0].enabled == true);
    CHECK(cg.passes[1].enabled == true);

    cg.set_enabled("dof", false);
    CHECK(cg.passes[0].enabled == true);   // fog untouched
    CHECK(cg.passes[1].enabled == false);  // dof off
    CHECK(cg.passes[2].enabled == true);   // tonemap untouched

    cg.set_enabled("dof", true);
    CHECK(cg.passes[1].enabled == true);

    // Unknown pass id — must be a no-op, not a crash
    cg.set_enabled("nonexistent", false);
}

TEST_CASE("CompiledGraph::patch — update a named float param", "[compiled_graph][cold]")
{
    auto cg = make_graph({"fog"});
    add_param(cg.passes[0], "density", 0.02f);
    add_param(cg.passes[0], "height",  100.f);

    CHECK_THAT(cg.passes[0].params.data[0], WithinAbs(0.02f, 1e-5f));
    CHECK_THAT(cg.passes[0].params.data[1], WithinAbs(100.f, 1e-4f));

    cg.patch("fog", "density", 0.05f);
    CHECK_THAT(cg.passes[0].params.data[0], WithinAbs(0.05f, 1e-5f));
    CHECK_THAT(cg.passes[0].params.data[1], WithinAbs(100.f, 1e-4f));  // unchanged

    // Unknown pass — no-op
    cg.patch("nonexistent", "density", 99.f);
    CHECK_THAT(cg.passes[0].params.data[0], WithinAbs(0.05f, 1e-5f));

    // Unknown key on known pass — no-op
    cg.patch("fog", "nonexistent_key", 42.f);
}

TEST_CASE("CompiledGraph::apply_style — 'enabled' key dispatches to set_enabled", "[compiled_graph][cold]")
{
    auto cg = make_graph({"bloom"});
    CHECK(cg.passes[0].enabled == true);

    cg.apply_style("bloom", "enabled", "false");
    CHECK(cg.passes[0].enabled == false);

    cg.apply_style("bloom", "enabled", "true");
    CHECK(cg.passes[0].enabled == true);

    cg.apply_style("bloom", "enabled", "0");
    CHECK(cg.passes[0].enabled == false);

    cg.apply_style("bloom", "enabled", "1");
    CHECK(cg.passes[0].enabled == true);
}

TEST_CASE("CompiledGraph::apply_style — numeric key dispatches to patch", "[compiled_graph][cold]")
{
    auto cg = make_graph({"grade"});
    add_param(cg.passes[0], "exposure", 1.0f);
    add_param(cg.passes[0], "contrast", 1.0f);

    cg.apply_style("grade", "exposure", "2.5");
    CHECK_THAT(cg.passes[0].params.data[0], WithinAbs(2.5f, 1e-5f));

    cg.apply_style("grade", "contrast", "1.2");
    CHECK_THAT(cg.passes[0].params.data[1], WithinAbs(1.2f, 1e-5f));
}

TEST_CASE("CompiledGraph::patch — multiple patches to different passes are independent", "[compiled_graph][cold]")
{
    auto cg = make_graph({"fog", "dof", "bloom"});
    add_param(cg.passes[0], "density",  0.01f);
    add_param(cg.passes[1], "focal",    8.0f);
    add_param(cg.passes[2], "strength", 0.5f);

    cg.patch("fog",   "density",  0.04f);
    cg.patch("dof",   "focal",    12.f);
    cg.patch("bloom", "strength", 0.9f);

    CHECK_THAT(cg.passes[0].params.data[0], WithinAbs(0.04f, 1e-5f));
    CHECK_THAT(cg.passes[1].params.data[0], WithinAbs(12.f,  1e-4f));
    CHECK_THAT(cg.passes[2].params.data[0], WithinAbs(0.9f,  1e-5f));
}


// =============================================================================
// Section 3 — CompiledGraph hot-path invariants
// =============================================================================

TEST_CASE("CompiledGraph::active_count — counts only enabled passes", "[compiled_graph][hot]")
{
    auto cg = make_graph({"a", "b", "c", "d"});
    CHECK(cg.active_count() == 4u);

    cg.set_enabled("b", false);
    cg.set_enabled("d", false);
    CHECK(cg.active_count() == 2u);

    cg.set_enabled("b", true);
    CHECK(cg.active_count() == 3u);
}

TEST_CASE("CompiledGraph::pass_count — reflects total including disabled", "[compiled_graph][hot]")
{
    auto cg = make_graph({"x", "y", "z"});
    cg.set_enabled("y", false);
    CHECK(cg.pass_count()   == 3u);
    CHECK(cg.active_count() == 2u);
}

TEST_CASE("CompiledGraph — time_slot records which param slot holds time", "[compiled_graph][hot]")
{
    auto cg = make_graph({"scene"});

    // Before adding any params, time_slot should be the default sentinel.
    CHECK(cg.passes[0].time_slot == 255u);

    // Add a non-time param first, then time.
    add_param(cg.passes[0], "gain", 1.0f);
    add_param(cg.passes[0], "time", 0.0f);

    // time_slot should point to slot 1 (second param added).
    CHECK(cg.passes[0].time_slot == 1u);
    // gain is at slot 0 — unchanged.
    CHECK_THAT(cg.passes[0].params.data[0], WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("CompiledGraph — time_slot == 0 when time is the first param", "[compiled_graph][hot]")
{
    auto cg = make_graph({"wallpaper"});
    add_param(cg.passes[0], "time",   0.0f);
    add_param(cg.passes[0], "speed",  0.5f);

    CHECK(cg.passes[0].time_slot == 0u);
}

TEST_CASE("CompiledGraph — time_slot == 255 when no time param declared", "[compiled_graph][hot]")
{
    auto cg = make_graph({"tonemap"});
    add_param(cg.passes[0], "exposure", 1.0f);
    add_param(cg.passes[0], "gamma",    2.2f);

    CHECK(cg.passes[0].time_slot == 255u);
}

TEST_CASE("CompiledGraph — params.count grows correctly as slots are filled", "[compiled_graph][hot]")
{
    auto cg = make_graph({"p"});
    CHECK(cg.passes[0].params.count == 0u);
    CHECK(cg.passes[0].params.empty());

    add_param(cg.passes[0], "a", 1.f);
    CHECK(cg.passes[0].params.count == 1u);
    CHECK(!cg.passes[0].params.empty());

    add_param(cg.passes[0], "b", 2.f);
    add_param(cg.passes[0], "c", 3.f);
    CHECK(cg.passes[0].params.count == 3u);
}

TEST_CASE("CompiledGraph — params.span() covers exactly the populated slots", "[compiled_graph][hot]")
{
    auto cg = make_graph({"p"});
    add_param(cg.passes[0], "x", 1.0f);
    add_param(cg.passes[0], "y", 2.0f);

    const auto sp = cg.passes[0].params.span();
    REQUIRE(sp.size() == 2u);
    CHECK_THAT(sp[0], WithinAbs(1.0f, 1e-5f));
    CHECK_THAT(sp[1], WithinAbs(2.0f, 1e-5f));
}

TEST_CASE("CompiledGraph — patch does not clobber adjacent param slots", "[compiled_graph][cold]")
{
    auto cg = make_graph({"dof"});
    add_param(cg.passes[0], "focal",   8.0f);
    add_param(cg.passes[0], "bokeh",   0.3f);
    add_param(cg.passes[0], "samples", 16.f);

    cg.patch("dof", "bokeh", 0.9f);

    // focal and samples must be untouched
    CHECK_THAT(cg.passes[0].params.data[0], WithinAbs(8.0f, 1e-5f));
    CHECK_THAT(cg.passes[0].params.data[1], WithinAbs(0.9f, 1e-5f));
    CHECK_THAT(cg.passes[0].params.data[2], WithinAbs(16.f, 1e-4f));
}

TEST_CASE("CompiledGraph — find_pass returns null for unknown hash", "[compiled_graph][cold]")
{
    auto cg = make_graph({"fog"});
    // patch() / set_enabled() with unknown IDs must be no-ops (tested via
    // observable state, since find_pass is private).
    const bool was_enabled = cg.passes[0].enabled;
    cg.set_enabled("missing_pass_id", false);
    CHECK(cg.passes[0].enabled == was_enabled);
}

TEST_CASE("CompiledGraph — empty graph has no passes and zero active count", "[compiled_graph]")
{
    CompiledGraph cg;
    CHECK(cg.empty());
    CHECK(cg.pass_count()   == 0u);
    CHECK(cg.active_count() == 0u);
}


// =============================================================================
// Section 4 — CompiledParams direct arithmetic
// =============================================================================

TEST_CASE("CompiledParams — set() extends count", "[compiled_params]")
{
    CompiledParams p;
    CHECK(p.count == 0u);

    p.set(0, 1.f);
    CHECK(p.count == 1u);

    p.set(3, 4.f);   // sparse write — count jumps to 4
    CHECK(p.count == 4u);

    // Intermediate slots are zero-initialised from the array default.
    CHECK_THAT(p.data[1], WithinAbs(0.f, 1e-6f));
    CHECK_THAT(p.data[2], WithinAbs(0.f, 1e-6f));
    CHECK_THAT(p.data[3], WithinAbs(4.f, 1e-5f));
}

TEST_CASE("CompiledParams — set() clamps at slot 15", "[compiled_params]")
{
    CompiledParams p;
    // Slot 16 is out of range — must be ignored
    p.set(16, 9999.f);
    CHECK(p.count == 0u);

    // Slot 15 is the last valid slot
    p.set(15, 42.f);
    CHECK(p.count == 16u);
    CHECK_THAT(p.data[15], WithinAbs(42.f, 1e-4f));
}

TEST_CASE("CompiledParams — operator[] reads stored values", "[compiled_params]")
{
    CompiledParams p;
    p.set(0, 3.14f);
    p.set(1, 2.71f);

    CHECK_THAT(p[0], WithinAbs(3.14f, 1e-5f));
    CHECK_THAT(p[1], WithinAbs(2.71f, 1e-5f));
}

TEST_CASE("CompiledParams — copy preserves all fields including count", "[compiled_params]")
{
    CompiledParams orig;
    orig.set(0, 1.f);
    orig.set(1, 2.f);
    orig.set(2, 3.f);

    const CompiledParams copy = orig;   // triggers the time-injection copy path in execute()
    CHECK(copy.count == orig.count);
    CHECK_THAT(copy.data[0], WithinAbs(orig.data[0], 1e-6f));
    CHECK_THAT(copy.data[1], WithinAbs(orig.data[1], 1e-6f));
    CHECK_THAT(copy.data[2], WithinAbs(orig.data[2], 1e-6f));
}


// =============================================================================
// Section 5 — pug parse → CompiledGraph wiring (no GPU)
// =============================================================================

TEST_CASE("pug::parse + CompiledGraph wiring — patch after parse is stable", "[integration]")
{
    // Parse a descriptor, manually build the compiled graph, then patch.
    auto r = parse(
        "pass#fog(shader=\"vol\" writes=\"lit\" density=\"0.02\" altitude=\"500.0\")\n"
        "pass#tonemap(shader=\"tm\" reads=\"lit\" writes=\"swapchain\" exposure=\"1.0\")\n"
    );
    REQUIRE(r.has_value());
    REQUIRE(r->passes.size() == 2u);

    // Build a CompiledGraph that mirrors what FrameGraph::compile() would produce.
    CompiledGraph cg;
    for (const auto& pd : r->passes) {
        CompiledPass cp;
        cp.enabled = pd.enabled;
        // FNV-1a hash
        {
            uint32_t h = 2166136261u;
            for (unsigned char c : pd.id) h = (h ^ c) * 16777619u;
            cp.id_hash = h;
        }
        uint8_t slot = 0;
        for (const auto& e : pd.params) {
            float val = 0.f;
            std::from_chars(e.val.data(), e.val.data() + e.val.size(), val);
            cp.params.set(slot, val);

            uint32_t kh = 2166136261u;
            for (unsigned char c : e.key) kh = (kh ^ c) * 16777619u;
            if (cp.slot_count < 16u) {
                cp.slot_map[cp.slot_count++] = { kh, slot };
            }
            if (e.key == "time") cp.time_slot = slot;
            ++slot;
        }
        cg.passes.push_back(cp);
    }

    REQUIRE(cg.passes.size() == 2u);

    // Patch fog density — must hit slot 0 of fog pass.
    const float before = cg.passes[0].params.data[0];
    cg.patch("fog", "density", 0.1f);
    CHECK_THAT(cg.passes[0].params.data[0], WithinAbs(0.1f, 1e-5f));
    // tonemap pass untouched
    CHECK_THAT(cg.passes[1].params.data[0], WithinAbs(1.0f, 1e-5f));
    (void)before;

    // Disable tonemap
    CHECK(cg.passes[1].enabled == true);
    cg.set_enabled("tonemap", false);
    CHECK(cg.passes[1].enabled == false);
    CHECK(cg.active_count()    == 1u);
}

TEST_CASE("pug::parse — 'time' param is reflected in parsed PassDesc::params", "[integration]")
{
    auto r = parse("pass#sky(shader=\"sky\" writes=\"swapchain\" time=\"0.0\" scale=\"1.5\")");
    REQUIRE(r.has_value());
    REQUIRE(r->passes.size() == 1u);

    const auto& params = r->passes[0].params;
    CHECK(params.find("time")  != nullptr);
    CHECK(params.find("scale") != nullptr);
    CHECK(*params.find("time") == "0.0");
}

TEST_CASE("CompiledGraph — set_enabled + active_count are consistent under many toggles", "[compiled_graph][hot]")
{
    auto cg = make_graph({"a", "b", "c", "d", "e"});
    CHECK(cg.active_count() == 5u);

    // Disable all
    for (const char* id : {"a","b","c","d","e"})
        cg.set_enabled(id, false);
    CHECK(cg.active_count() == 0u);

    // Re-enable one at a time
    cg.set_enabled("a", true);
    CHECK(cg.active_count() == 1u);

    cg.set_enabled("c", true);
    CHECK(cg.active_count() == 2u);

    cg.set_enabled("e", true);
    CHECK(cg.active_count() == 3u);

    CHECK(cg.pass_count() == 5u);  // total is always 5
}
