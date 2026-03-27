// =============================================================================
// Extra StyleApplier tests: app tag mapping + display:none
// Catch2 v3 — no GPU context required.
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include "style_applier.h"
#include "render_tree.h"

using namespace pce::sdlos;

TEST_CASE("StyleApplier: app tag → FlexColumn", "[applier]")
{
    RenderNode n;
    n.setStyle("tag", "app");
    StyleApplier::apply(n);
    REQUIRE(n.layout_kind == LayoutKind::FlexColumn);
}

TEST_CASE("StyleApplier: display:none marks hidden", "[applier]")
{
    RenderNode n;
    n.setStyle("display", "none");
    StyleApplier::apply(n);
    REQUIRE(n.hidden == true);
}

