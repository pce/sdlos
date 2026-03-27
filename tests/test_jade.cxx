// =============================================================================
// JadeLite — unit tests
// Catch2 v3 — no GPU context required.
//
// Build:  see CMakeLists.txt  (test_jade target)
// Run:    ./test_jade  or  ctest --test-dir build -R jade
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include "jade/jade_token.hh"
#include "jade/jade_lexer.hh"
#include "jade/jade_parser.hh"
#include "style_applier.hh"
#include "hit_test.hh"
#include "node_events.hh"
#include "render_tree.hh"

using namespace pce::sdlos;
using namespace pce::sdlos::jade;

// ─── helpers ──────────────────────────────────────────────────────────────────

// Extract only the TokenType values from a stream — makes REQUIRE lines short.
static std::vector<TokenType> types(const std::vector<Token>& toks)
{
    std::vector<TokenType> out;
    out.reserve(toks.size());
    for (const auto& t : toks) out.push_back(t.type);
    return out;
}

// Return the first child of the virtual parse root.
// Every parse() call wraps results in a _root FlexColumn node.
static const RenderNode* firstChild(const RenderTree& tree, NodeHandle root)
{
    const RenderNode* r = tree.node(root);
    if (!r || !r->child.valid()) return nullptr;
    return tree.node(r->child);
}

// =============================================================================
// Lexer
// =============================================================================

TEST_CASE("Lexer: empty source → only End", "[lexer]")
{
    const auto toks = Lexer{""}.tokenize();
    REQUIRE(toks.size() == 1);
    REQUIRE(toks[0].type == TokenType::End);
}

TEST_CASE("Lexer: whitespace-only source → only End", "[lexer]")
{
    const auto toks = Lexer{"   \n\n\t\n"}.tokenize();
    REQUIRE(toks.size() == 1);
    REQUIRE(toks[0].type == TokenType::End);
}

TEST_CASE("Lexer: simple tag", "[lexer]")
{
    const auto toks = Lexer{"div"}.tokenize();
    REQUIRE(toks[0].type  == TokenType::Tag);
    REQUIRE(toks[0].value == "div");
    REQUIRE(toks[1].type  == TokenType::Newline);
    REQUIRE(toks[2].type  == TokenType::End);
}

TEST_CASE("Lexer: tag with single class", "[lexer]")
{
    const auto toks = Lexer{"div.card"}.tokenize();
    REQUIRE(toks[0].type  == TokenType::Tag);
    REQUIRE(toks[0].value == "div");
    REQUIRE(toks[1].type  == TokenType::Class);
    REQUIRE(toks[1].value == "card");
}

TEST_CASE("Lexer: tag with single id", "[lexer]")
{
    const auto toks = Lexer{"div#main"}.tokenize();
    REQUIRE(toks[0].type  == TokenType::Tag);
    REQUIRE(toks[0].value == "div");
    REQUIRE(toks[1].type  == TokenType::Id);
    REQUIRE(toks[1].value == "main");
}

TEST_CASE("Lexer: implicit div from bare .class", "[lexer]")
{
    const auto toks = Lexer{".card"}.tokenize();
    REQUIRE(toks[0].type  == TokenType::Tag);
    REQUIRE(toks[0].value == "div");
    REQUIRE(toks[1].type  == TokenType::Class);
    REQUIRE(toks[1].value == "card");
}

TEST_CASE("Lexer: implicit div from bare #id", "[lexer]")
{
    const auto toks = Lexer{"#app"}.tokenize();
    REQUIRE(toks[0].type  == TokenType::Tag);
    REQUIRE(toks[0].value == "div");
    REQUIRE(toks[1].type  == TokenType::Id);
    REQUIRE(toks[1].value == "app");
}

TEST_CASE("Lexer: multiple classes and id combined", "[lexer]")
{
    const auto toks = Lexer{"div.foo.bar#baz"}.tokenize();
    REQUIRE(toks[0].type  == TokenType::Tag);   REQUIRE(toks[0].value == "div");
    REQUIRE(toks[1].type  == TokenType::Class); REQUIRE(toks[1].value == "foo");
    REQUIRE(toks[2].type  == TokenType::Class); REQUIRE(toks[2].value == "bar");
    REQUIRE(toks[3].type  == TokenType::Id);    REQUIRE(toks[3].value == "baz");
}

TEST_CASE("Lexer: inline text after tag", "[lexer]")
{
    const auto toks = Lexer{"h1 Hello World"}.tokenize();
    REQUIRE(toks[0].type  == TokenType::Tag);  REQUIRE(toks[0].value == "h1");
    REQUIRE(toks[1].type  == TokenType::Text); REQUIRE(toks[1].value == "Hello World");
}

TEST_CASE("Lexer: text with class shorthand", "[lexer]")
{
    const auto toks = Lexer{"h1.title Hello"}.tokenize();
    REQUIRE(toks[0].type  == TokenType::Tag);   REQUIRE(toks[0].value == "h1");
    REQUIRE(toks[1].type  == TokenType::Class); REQUIRE(toks[1].value == "title");
    REQUIRE(toks[2].type  == TokenType::Text);  REQUIRE(toks[2].value == "Hello");
}

TEST_CASE("Lexer: unquoted attribute", "[lexer]")
{
    const auto toks = Lexer{"div(width=100)"}.tokenize();
    REQUIRE(toks[1].type  == TokenType::AttrKey);   REQUIRE(toks[1].value == "width");
    REQUIRE(toks[2].type  == TokenType::AttrValue); REQUIRE(toks[2].value == "100");
}

TEST_CASE("Lexer: double-quoted attribute", "[lexer]")
{
    const auto toks = Lexer{"div(id=\"app\")"}.tokenize();
    REQUIRE(toks[1].type  == TokenType::AttrKey);   REQUIRE(toks[1].value == "id");
    REQUIRE(toks[2].type  == TokenType::AttrValue); REQUIRE(toks[2].value == "app");
}

TEST_CASE("Lexer: single-quoted attribute", "[lexer]")
{
    const auto toks = Lexer{"div(class='card')"}.tokenize();
    REQUIRE(toks[1].type  == TokenType::AttrKey);   REQUIRE(toks[1].value == "class");
    REQUIRE(toks[2].type  == TokenType::AttrValue); REQUIRE(toks[2].value == "card");
}

TEST_CASE("Lexer: multiple attributes", "[lexer]")
{
    const auto toks = Lexer{"div(width=320 height=480)"}.tokenize();
    REQUIRE(toks[1].type  == TokenType::AttrKey);   REQUIRE(toks[1].value == "width");
    REQUIRE(toks[2].type  == TokenType::AttrValue); REQUIRE(toks[2].value == "320");
    REQUIRE(toks[3].type  == TokenType::AttrKey);   REQUIRE(toks[3].value == "height");
    REQUIRE(toks[4].type  == TokenType::AttrValue); REQUIRE(toks[4].value == "480");
}

TEST_CASE("Lexer: attribute with no value (boolean attr)", "[lexer]")
{
    const auto toks = Lexer{"input(disabled)"}.tokenize();
    REQUIRE(toks[1].type  == TokenType::AttrKey);   REQUIRE(toks[1].value == "disabled");
    REQUIRE(toks[2].type  == TokenType::AttrValue); REQUIRE(toks[2].value == "");
}

TEST_CASE("Lexer: comment line is skipped entirely", "[lexer]")
{
    const auto toks = Lexer{"// this is a comment"}.tokenize();
    REQUIRE(toks.size() == 1);
    REQUIRE(toks[0].type == TokenType::End);
}

TEST_CASE("Lexer: comment between nodes leaves no trace", "[lexer]")
{
    const auto toks = Lexer{"div\n// comment\np"}.tokenize();
    const auto tt   = types(toks);
    // Tag:"div"  Newline  Tag:"p"  Newline  End — no comment token
    for (auto t : tt)
        REQUIRE(t != TokenType::Indent); // no indentation change
    bool found_p = false;
    for (const auto& tok : toks)
        if (tok.type == TokenType::Tag && tok.value == "p") { found_p = true; break; }
    REQUIRE(found_p);
}

TEST_CASE("Lexer: pipe text", "[lexer]")
{
    const auto toks = Lexer{"| Hello Pipe"}.tokenize();
    REQUIRE(toks[0].type  == TokenType::Text);
    REQUIRE(toks[0].value == "Hello Pipe");
}

TEST_CASE("Lexer: indentation emits Indent then Dedent", "[lexer]")
{
    const auto toks = Lexer{"div\n  p"}.tokenize();
    const auto tt   = types(toks);
    // Tag Newline Indent Tag Newline Dedent End
    REQUIRE(tt[0] == TokenType::Tag);
    REQUIRE(tt[1] == TokenType::Newline);
    REQUIRE(tt[2] == TokenType::Indent);
    REQUIRE(tt[3] == TokenType::Tag);
    REQUIRE(tt[4] == TokenType::Newline);
    REQUIRE(tt[5] == TokenType::Dedent);
    REQUIRE(tt[6] == TokenType::End);
}

TEST_CASE("Lexer: two levels of indentation", "[lexer]")
{
    const auto toks = Lexer{"div\n  section\n    p"}.tokenize();
    const auto tt   = types(toks);
    // Tag Newline  Indent Tag Newline  Indent Tag Newline  Dedent Dedent End
    REQUIRE(tt[2] == TokenType::Indent);
    REQUIRE(tt[5] == TokenType::Indent);
    REQUIRE(tt[8] == TokenType::Dedent);
    REQUIRE(tt[9] == TokenType::Dedent);
}

TEST_CASE("Lexer: siblings at same indent level", "[lexer]")
{
    const auto toks = Lexer{"div\n  p\n  span"}.tokenize();
    const auto tt   = types(toks);
    // Tag Newline  Indent Tag Newline  Tag Newline  Dedent End
    // Only ONE Indent (before first child) and ONE Dedent (after last child).
    int indent_count = 0, dedent_count = 0;
    for (auto t : tt) {
        if (t == TokenType::Indent) ++indent_count;
        if (t == TokenType::Dedent) ++dedent_count;
    }
    REQUIRE(indent_count == 1);
    REQUIRE(dedent_count == 1);
}

TEST_CASE("Lexer: Windows CRLF line endings handled", "[lexer]")
{
    const auto toks = Lexer{"div\r\n  p\r\n"}.tokenize();
    REQUIRE(toks[0].type  == TokenType::Tag);
    REQUIRE(toks[0].value == "div");
    REQUIRE(toks[2].type  == TokenType::Indent);
    REQUIRE(toks[3].type  == TokenType::Tag);
    REQUIRE(toks[3].value == "p");
}

TEST_CASE("Lexer: full shorthand combined with attrs and text", "[lexer]")
{
    const auto toks = Lexer{"div.card#app(width=320 height=480) Title"}.tokenize();
    REQUIRE(toks[0].type  == TokenType::Tag);       REQUIRE(toks[0].value == "div");
    REQUIRE(toks[1].type  == TokenType::Class);     REQUIRE(toks[1].value == "card");
    REQUIRE(toks[2].type  == TokenType::Id);        REQUIRE(toks[2].value == "app");
    REQUIRE(toks[3].type  == TokenType::AttrKey);   REQUIRE(toks[3].value == "width");
    REQUIRE(toks[4].type  == TokenType::AttrValue); REQUIRE(toks[4].value == "320");
    REQUIRE(toks[5].type  == TokenType::AttrKey);   REQUIRE(toks[5].value == "height");
    REQUIRE(toks[6].type  == TokenType::AttrValue); REQUIRE(toks[6].value == "480");
    REQUIRE(toks[7].type  == TokenType::Text);      REQUIRE(toks[7].value == "Title");
}

// =============================================================================
// Parser
// =============================================================================

TEST_CASE("Parser: single node attached to virtual root", "[parser]")
{
    RenderTree tree;
    const auto root = parse("div", tree);
    const auto* r   = tree.node(root);
    REQUIRE(r != nullptr);
    REQUIRE(r->child.valid());

    const auto* div = tree.node(r->child);
    REQUIRE(div != nullptr);
    REQUIRE(div->style("tag") == "div");
    // No sibling — only one top-level element
    REQUIRE(!div->sibling.valid());
}

TEST_CASE("Parser: virtual root has FlexColumn layout", "[parser]")
{
    RenderTree tree;
    const auto root = parse("div", tree);
    REQUIRE(tree.node(root)->layout_kind == LayoutKind::FlexColumn);
}

TEST_CASE("Parser: id and class stored in styles", "[parser]")
{
    RenderTree tree;
    const auto  root = parse("div.card#app", tree);
    const auto* div  = firstChild(tree, root);
    REQUIRE(div != nullptr);
    REQUIRE(div->style("id")    == "app");
    REQUIRE(div->style("class") == "card");
}

TEST_CASE("Parser: multiple classes joined with space", "[parser]")
{
    RenderTree tree;
    const auto  root = parse("div.a.b.c", tree);
    const auto* div  = firstChild(tree, root);
    REQUIRE(div != nullptr);
    REQUIRE(div->style("class") == "a b c");
}

TEST_CASE("Parser: attributes stored in styles", "[parser]")
{
    RenderTree tree;
    const auto  root = parse("div(width=320 height=480)", tree);
    const auto* div  = firstChild(tree, root);
    REQUIRE(div != nullptr);
    REQUIRE(div->style("width")  == "320");
    REQUIRE(div->style("height") == "480");
}

TEST_CASE("Parser: inline text stored in styles", "[parser]")
{
    RenderTree tree;
    const auto  root = parse("h1 Hello World", tree);
    const auto* h1   = firstChild(tree, root);
    REQUIRE(h1 != nullptr);
    REQUIRE(h1->style("text") == "Hello World");
}

TEST_CASE("Parser: nested child attached to parent", "[parser]")
{
    RenderTree tree;
    const auto  root    = parse("div\n  p", tree);
    const auto* div     = firstChild(tree, root);
    REQUIRE(div != nullptr);
    REQUIRE(div->child.valid());

    const auto* p = tree.node(div->child);
    REQUIRE(p != nullptr);
    REQUIRE(p->style("tag") == "p");
}

TEST_CASE("Parser: two siblings share same parent", "[parser]")
{
    RenderTree tree;
    const auto  root = parse("div\n  p\n  span", tree);
    const auto* div  = firstChild(tree, root);
    REQUIRE(div != nullptr);

    const auto* p = tree.node(div->child);
    REQUIRE(p != nullptr);
    REQUIRE(p->style("tag") == "p");

    const auto* span = tree.node(p->sibling);
    REQUIRE(span != nullptr);
    REQUIRE(span->style("tag") == "span");

    // span has no further sibling
    REQUIRE(!span->sibling.valid());
}

TEST_CASE("Parser: three siblings in order", "[parser]")
{
    RenderTree tree;
    const auto root = parse("div\n  h1\n  p\n  footer", tree);
    const auto* div = firstChild(tree, root);

    const auto* h1     = tree.node(div->child);
    const auto* p      = tree.node(h1->sibling);
    const auto* footer = tree.node(p->sibling);

    REQUIRE(h1->style("tag")     == "h1");
    REQUIRE(p->style("tag")      == "p");
    REQUIRE(footer->style("tag") == "footer");
}

TEST_CASE("Parser: two levels of nesting", "[parser]")
{
    RenderTree tree;
    const auto  root    = parse("div\n  section\n    p text", tree);
    const auto* div     = firstChild(tree, root);
    const auto* section = tree.node(div->child);
    const auto* p       = tree.node(section->child);

    REQUIRE(div     != nullptr);
    REQUIRE(section != nullptr); REQUIRE(section->style("tag") == "section");
    REQUIRE(p       != nullptr); REQUIRE(p->style("tag")       == "p");
    REQUIRE(p->style("text") == "text");
}

TEST_CASE("Parser: comment lines produce no nodes", "[parser]")
{
    RenderTree tree;
    const auto  root = parse("// only a comment", tree);
    const auto* r    = tree.node(root);
    // No children — comment produced nothing
    REQUIRE(!r->child.valid());
}

TEST_CASE("Parser: comment mixed with real nodes", "[parser]")
{
    RenderTree tree;
    const auto  root = parse("div\n// ignored\np", tree);
    const auto* r    = tree.node(root);

    // Two top-level children: div, p
    const auto* div = tree.node(r->child);
    const auto* p   = tree.node(div->sibling);
    REQUIRE(div->style("tag") == "div");
    REQUIRE(p->style("tag")   == "p");
}

TEST_CASE("Parser: implicit div from .class shorthand", "[parser]")
{
    RenderTree tree;
    const auto  root = parse(".container", tree);
    const auto* div  = firstChild(tree, root);
    REQUIRE(div != nullptr);
    REQUIRE(div->style("tag")   == "div");
    REQUIRE(div->style("class") == "container");
}

TEST_CASE("Parser: node count matches source", "[parser]")
{
    RenderTree tree;
    parse("div\n  p\n  span\n  button", tree);
    // root + div + p + span + button = 5
    REQUIRE(tree.nodeCount() == 5);
}

TEST_CASE("Parser: empty source produces only virtual root", "[parser]")
{
    RenderTree tree;
    const auto  root = parse("", tree);
    const auto* r    = tree.node(root);
    REQUIRE(r != nullptr);
    REQUIRE(!r->child.valid());
}

// =============================================================================
// StyleApplier
// =============================================================================

// helper — build a bare RenderNode with one style set
[[maybe_unused]] static RenderNode makeNode(std::string_view key, std::string_view val)
{
    RenderNode n;
    n.setStyle(key, std::string(val));
    return n;
}

TEST_CASE("StyleApplier: empty styles → no-op", "[applier]")
{
    RenderNode n;
    REQUIRE(n.layout_kind == LayoutKind::None); // default
    StyleApplier::apply(n);
    REQUIRE(n.layout_kind == LayoutKind::None);
}

TEST_CASE("StyleApplier: div → Block", "[applier]")
{
    auto n = makeNode("tag", "div");
    StyleApplier::apply(n);
    REQUIRE(n.layout_kind == LayoutKind::Block);
}

TEST_CASE("StyleApplier: block-container tags all → Block", "[applier]")
{
    for (const char* tag : {"section", "main", "header", "footer",
                            "article", "aside", "nav", "layout",
                            "panel", "calculator", "keypad"})
    {
        auto n = makeNode("tag", tag);
        StyleApplier::apply(n);
        INFO("tag = " << tag);
        REQUIRE(n.layout_kind == LayoutKind::Block);
    }
}

TEST_CASE("StyleApplier: row / hbox → FlexRow", "[applier]")
{
    for (const char* tag : {"row", "hbox"}) {
        auto n = makeNode("tag", tag);
        StyleApplier::apply(n);
        INFO("tag = " << tag);
        REQUIRE(n.layout_kind == LayoutKind::FlexRow);
    }
}

TEST_CASE("StyleApplier: col / column / vbox → FlexColumn", "[applier]")
{
    for (const char* tag : {"col", "column", "vbox", "stack"}) {
        auto n = makeNode("tag", tag);
        StyleApplier::apply(n);
        INFO("tag = " << tag);
        REQUIRE(n.layout_kind == LayoutKind::FlexColumn);
    }
}

TEST_CASE("StyleApplier: leaf tags stay None", "[applier]")
{
    for (const char* tag : {"span", "p", "text", "label", "button",
                            "h1", "h2", "h3", "img", "image", "a"})
    {
        auto n = makeNode("tag", tag);
        StyleApplier::apply(n);
        INFO("tag = " << tag);
        REQUIRE(n.layout_kind == LayoutKind::None);
    }
}

TEST_CASE("StyleApplier: flexDirection=row overrides Block tag", "[applier]")
{
    RenderNode n;
    n.setStyle("tag",           "div");    // would → Block
    n.setStyle("flexDirection", "row");    // overrides → FlexRow
    StyleApplier::apply(n);
    REQUIRE(n.layout_kind == LayoutKind::FlexRow);
}

TEST_CASE("StyleApplier: flexDirection=column overrides Block tag", "[applier]")
{
    RenderNode n;
    n.setStyle("tag",           "header");
    n.setStyle("flexDirection", "column");
    StyleApplier::apply(n);
    REQUIRE(n.layout_kind == LayoutKind::FlexColumn);
}

TEST_CASE("StyleApplier: width and height → layout_props + w/h", "[applier]")
{
    RenderNode n;
    n.setStyle("width",  "320");
    n.setStyle("height", "480");
    StyleApplier::apply(n);
    REQUIRE(n.layout_props.width  == 320.f);
    REQUIRE(n.layout_props.height == 480.f);
    REQUIRE(n.w == 320.f);
    REQUIRE(n.h == 480.f);
}

TEST_CASE("StyleApplier: x and y set geometry", "[applier]")
{
    RenderNode n;
    n.setStyle("x", "10");
    n.setStyle("y", "25");
    StyleApplier::apply(n);
    REQUIRE(n.x == 10.f);
    REQUIRE(n.y == 25.f);
}

TEST_CASE("StyleApplier: flexGrow applied", "[applier]")
{
    RenderNode n;
    n.setStyle("flexGrow", "2");
    StyleApplier::apply(n);
    REQUIRE(n.layout_props.flex_grow == 2.f);
}

TEST_CASE("StyleApplier: gap applied", "[applier]")
{
    RenderNode n;
    n.setStyle("gap", "8");
    StyleApplier::apply(n);
    REQUIRE(n.layout_props.gap == 8.f);
}

TEST_CASE("StyleApplier: non-numeric width is silently ignored", "[applier]")
{
    RenderNode n;
    n.setStyle("width", "auto");   // not parseable as float
    StyleApplier::apply(n);
    REQUIRE(n.layout_props.width == -1.f); // unchanged default
}

TEST_CASE("StyleApplier: unknown keys stay in styles, not applied", "[applier]")
{
    RenderNode n;
    n.setStyle("backgroundColor", "0.2,0.2,0.2");
    n.setStyle("fontSize",        "24");
    StyleApplier::apply(n);
    // Visual keys are preserved verbatim — not layout-applied
    REQUIRE(n.style("backgroundColor") == "0.2,0.2,0.2");
    REQUIRE(n.style("fontSize")        == "24");
}

// =============================================================================
// Integration — Lexer + Parser + StyleApplier end-to-end
// =============================================================================

TEST_CASE("Integration: calculator skeleton layout", "[integration]")
{
    constexpr std::string_view src =
        "calculator(width=320 height=480)\n"
        "  .display(width=320 height=80)\n"
        "    span#result 0\n"
        "  .keypad\n"
        "    row\n"
        "      button.key AC\n"
        "      button.key +/-\n";

    RenderTree  tree;
    const auto  root = parse(src, tree);
    const auto* calc = firstChild(tree, root);

    REQUIRE(calc != nullptr);
    REQUIRE(calc->style("tag")    == "calculator");
    REQUIRE(calc->layout_kind     == LayoutKind::Block);
    REQUIRE(calc->layout_props.width  == 320.f);
    REQUIRE(calc->layout_props.height == 480.f);

    // display is first child of calculator
    const auto* display = tree.node(calc->child);
    REQUIRE(display != nullptr);
    REQUIRE(display->style("class") == "display");
    REQUIRE(display->layout_kind    == LayoutKind::Block);

    // result span inside display
    const auto* result = tree.node(display->child);
    REQUIRE(result != nullptr);
    REQUIRE(result->style("tag") == "span");
    REQUIRE(result->style("id")  == "result");
    REQUIRE(result->style("text") == "0");

    // keypad is sibling of display
    const auto* keypad = tree.node(display->sibling);
    REQUIRE(keypad != nullptr);
    REQUIRE(keypad->style("class") == "keypad");

    // row inside keypad
    const auto* row = tree.node(keypad->child);
    REQUIRE(row != nullptr);
    REQUIRE(row->style("tag")  == "row");
    REQUIRE(row->layout_kind   == LayoutKind::FlexRow);

    // two buttons inside row
    const auto* ac  = tree.node(row->child);
    const auto* pm  = tree.node(ac->sibling);
    REQUIRE(ac->style("tag")   == "button");
    REQUIRE(ac->style("class") == "key");
    REQUIRE(ac->style("text")  == "AC");
    REQUIRE(pm->style("text")  == "+/-");
}

// =============================================================================
// HitTest
// =============================================================================

// Build a simple tree:  root → div(x=10,y=10,w=100,h=100) → button(x=20,y=20,w=40,h=40)
static NodeHandle buildHitTree(RenderTree& tree)
{
    const NodeHandle root = tree.alloc();
    tree.node(root)->layout_kind = LayoutKind::FlexColumn;

    const NodeHandle div = tree.alloc();
    RenderNode* d = tree.node(div);
    d->x = 10.f; d->y = 10.f; d->w = 100.f; d->h = 100.f;

    const NodeHandle btn = tree.alloc();
    RenderNode* b = tree.node(btn);
    b->x = 20.f; b->y = 20.f; b->w = 40.f; b->h = 40.f;
    b->setStyle("tag", "button");
    b->setStyle("onclick", "btn:press");
    b->setStyle("data-value", "ok");

    tree.appendChild(root, div);
    tree.appendChild(div, btn);
    return root;
}

TEST_CASE("HitTest: point inside inner node returns deepest handle", "[hittest]")
{
    RenderTree tree;
    const auto root = buildHitTree(tree);
    const auto* r   = tree.node(root);
    const auto* div = tree.node(r->child);
    const auto* btn = tree.node(div->child);

    // button absolute rect: x=10+20=30, y=10+20=30, w=40, h=40
    const NodeHandle hit = hitTest(tree, root, 35.f, 35.f);
    REQUIRE(hit.valid());
    // Should hit the button (deepest), not the div
    REQUIRE(tree.node(hit)->style("tag") == "button");

    (void)btn; // suppress unused warning
}

TEST_CASE("HitTest: point inside outer node but outside inner returns outer", "[hittest]")
{
    RenderTree tree;
    const auto root = buildHitTree(tree);
    const auto* r   = tree.node(root);
    const auto* div = tree.node(r->child);

    // div absolute rect: x=10, y=10, w=100, h=100
    // button absolute rect: x=30, y=30, w=40, h=40
    // Point (15, 15) is inside div but outside button
    const NodeHandle hit = hitTest(tree, root, 15.f, 15.f);
    REQUIRE(hit.valid());
    REQUIRE(hit == r->child);

    (void)div; // suppress unused warning
}

TEST_CASE("HitTest: point outside all nodes returns invalid", "[hittest]")
{
    RenderTree tree;
    const auto root = buildHitTree(tree);

    // div is at x=10,y=10,w=100,h=100 — point (5,5) is outside
    const NodeHandle hit = hitTest(tree, root, 5.f, 5.f);
    REQUIRE(!hit.valid());
}

TEST_CASE("HitTest: zero-area node is never hit", "[hittest]")
{
    RenderTree tree;
    const auto root = tree.alloc();

    const NodeHandle n = tree.alloc();
    tree.node(n)->x = 0.f; tree.node(n)->y = 0.f;
    tree.node(n)->w = 0.f; tree.node(n)->h = 0.f;  // zero area
    tree.appendChild(root, n);

    const NodeHandle hit = hitTest(tree, root, 0.f, 0.f);
    REQUIRE(!hit.valid());
}

TEST_CASE("HitTest: last sibling wins when siblings overlap", "[hittest]")
{
    RenderTree tree;
    const auto root = tree.alloc();

    // Two overlapping nodes at the same position — last one should win.
    const NodeHandle first  = tree.alloc();
    const NodeHandle second = tree.alloc();
    tree.node(first)->x  = 0.f; tree.node(first)->y  = 0.f;
    tree.node(first)->w  = 50.f; tree.node(first)->h = 50.f;
    tree.node(first)->setStyle("tag", "first");

    tree.node(second)->x = 0.f; tree.node(second)->y = 0.f;
    tree.node(second)->w = 50.f; tree.node(second)->h = 50.f;
    tree.node(second)->setStyle("tag", "second");

    tree.appendChild(root, first);
    tree.appendChild(root, second);

    const NodeHandle hit = hitTest(tree, root, 10.f, 10.f);
    REQUIRE(hit.valid());
    REQUIRE(tree.node(hit)->style("tag") == "second");
}

// =============================================================================
// dispatchClick + dispatchSelect
// =============================================================================

// Minimal IEventBus stub — records published events for assertions.
struct RecordingBus : IEventBus {
    struct Entry { std::string topic; std::string data; };
    std::vector<Entry> log;

    void subscribe(const std::string&,
                   std::function<void(const std::string&)>) override {}

    void publish(const std::string& topic,
                 const std::string& data = "") override
    {
        log.push_back({topic, data});
    }
};

TEST_CASE("dispatchClick: onclick topic is published", "[events]")
{
    RenderTree   tree;
    RecordingBus bus;
    const auto   root = buildHitTree(tree);

    // Point (35, 35) hits the button which has onclick="btn:press"
    const NodeHandle hit = dispatchClick(tree, root, 35.f, 35.f, bus);
    REQUIRE(hit.valid());
    REQUIRE(bus.log.size() == 1);
    REQUIRE(bus.log[0].topic == "btn:press");
    REQUIRE(bus.log[0].data  == "ok");    // data-value="ok"
}

TEST_CASE("dispatchClick: miss publishes nothing", "[events]")
{
    RenderTree   tree;
    RecordingBus bus;
    const auto   root = buildHitTree(tree);

    // Point (5, 5) hits nothing
    const NodeHandle hit = dispatchClick(tree, root, 5.f, 5.f, bus);
    REQUIRE(!hit.valid());
    REQUIRE(bus.log.empty());
}

TEST_CASE("dispatchClick: hit with no onclick publishes nothing", "[events]")
{
    RenderTree tree;
    const auto root = tree.alloc();
    const auto div  = tree.alloc();
    tree.node(div)->x = 0.f; tree.node(div)->y = 0.f;
    tree.node(div)->w = 100.f; tree.node(div)->h = 100.f;
    // No onclick style
    tree.appendChild(root, div);

    RecordingBus bus;
    dispatchClick(tree, root, 10.f, 10.f, bus);
    REQUIRE(bus.log.empty());
}



TEST_CASE("dispatchSelect: onselect topic is published", "[events]")
{
    RenderTree tree;
    const auto root = tree.alloc();
    const auto item = tree.alloc();
    tree.node(item)->setStyle("onselect",   "list:pick");
    tree.node(item)->setStyle("data-value", "item-3");
    tree.appendChild(root, item);

    RecordingBus bus;
    dispatchSelect(tree, tree.node(root)->child, bus);
    REQUIRE(bus.log.size() == 1);
    REQUIRE(bus.log[0].topic == "list:pick");
    REQUIRE(bus.log[0].data  == "item-3");
}

// =============================================================================
// Integration — search overlay structure
// =============================================================================

TEST_CASE("Integration: search overlay structure", "[integration]")
{
    constexpr std::string_view src =
        "div.overlay(width=800 height=600)\n"
        "  div.panel(width=600 height=68)\n"
        "    input#search(placeholder=Search... width=560 height=44)\n";

    RenderTree  tree;
    const auto  root    = parse(src, tree);
    const auto* overlay = firstChild(tree, root);

    REQUIRE(overlay->style("class")       == "overlay");
    REQUIRE(overlay->layout_props.width   == 800.f);
    REQUIRE(overlay->layout_props.height  == 600.f);

    const auto* panel = tree.node(overlay->child);
    REQUIRE(panel->style("class")         == "panel");
    REQUIRE(panel->layout_props.width     == 600.f);

    const auto* input = tree.node(panel->child);
    REQUIRE(input->style("id")            == "search");
    REQUIRE(input->style("placeholder")   == "Search...");
    REQUIRE(input->layout_props.width     == 560.f);
    REQUIRE(input->layout_props.height    == 44.f);
}

TEST_CASE("Integration: JadeLite parse + dispatchClick end-to-end", "[integration]")
{
    constexpr std::string_view src =
        "div(width=320 height=480)\n"
        "  button.key(onclick=calc:ac data-value=AC width=80 height=80) AC\n"
        "  button.key(onclick=calc:num data-value=7  width=80 height=80) 7\n";

    RenderTree   tree;
    RecordingBus bus;
    const auto   root = parse(src, tree);

    // button "7" is the second child of div.
    // div absolute: x=0,y=0 (virtual root, children start at 0).
    // After parse, x/y/w/h are set from style attrs via StyleApplier.
    // div: x=0, y=0, w=320, h=480  (StyleApplier sets w/h from attrs)
    // btn AC: x=0, y=0, w=80, h=80
    // btn 7 : x=0, y=0, w=80, h=80  (sibling, same relative position)
    //
    // Both buttons are at x=0 relative to div. The last sibling wins on overlap.
    // Click at (10, 10) inside the div — should hit the last button ("7") since
    // siblings share the same position before layout resolves them.

    const NodeHandle hit = dispatchClick(tree, root, 10.f, 10.f, bus);
    REQUIRE(hit.valid());
    // At least one publish happened
    REQUIRE(!bus.log.empty());
    // The published topic must be a calc: topic
    REQUIRE(bus.log[0].topic.substr(0, 5) == "calc:");
}
