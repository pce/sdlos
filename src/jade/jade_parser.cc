#include "jade_parser.h"

#include "../style_applier.h"
#include "jade_lexer.h"
#include "jade_token.h"

#include <cassert>
#include <vector>

// JadeLite — Parser implementation
// Single-pass: lexer tokens → RenderTree nodes, no intermediate representation.
//
// Indent/Dedent handling
// The token stream emits Indent *before* the first token on a child line:
//
//   div           → Tag:"div"  Newline
//     p text      → Indent  Tag:"p"  Text:"text"  Newline  Dedent
//
// So when we see Indent, the node finalized on the *previous* Newline
// (tracked as `last`) becomes the new attachment parent.
//
// Parent stack
//   parent_stack.back() = where new nodes are appended as children.
//   Indent  → push `last` (the just-finalized parent candidate).
//   Dedent  → pop one level.
//
// Node lifecycle
//   current  — node handle being assembled from the current line's tokens.
//              k_null_handle between lines.
//   finalize — applies StyleApplier, attaches to parent, clears `current`.

namespace pce::sdlos::jade {

NodeHandle parse(std::string_view source, RenderTree &tree) {
    const auto tokens = Lexer{source}.tokenize();

    // Virtual root — transparent FlexColumn container for all top-level nodes.
    const NodeHandle root = tree.alloc();
    {
        RenderNode *r   = tree.node(root);
        r->layout_kind  = LayoutKind::FlexColumn;
        r->dirty_render = false;
        r->setStyle("tag", "_root");
    }

    // Parent stack: back() is the node new children attach to.
    std::vector<NodeHandle> parent_stack;
    parent_stack.reserve(16);
    parent_stack.push_back(root);

    // last  — most recently finalized node; becomes a parent on Indent.
    // current — node accumulating tokens for the current line.
    NodeHandle last    = root;
    NodeHandle current = k_null_handle;

    // finalize: apply styles → attach to parent → update bookkeeping.
    const auto finalize = [&]() noexcept {
        if (!current.valid())
            return;
        RenderNode *n = tree.node(current);
        if (n)
            if (!n->style_attrs.empty()) {
                StyleApplier::apply(*n);
            }
        tree.appendChild(parent_stack.back(), current);
        last    = current;
        current = k_null_handle;
    };

    for (std::size_t i = 0; i < tokens.size();) {
        const Token &tok = tokens[i++];

        switch (tok.type) {
            // Stream control

        case TokenType::End:
            finalize();
            goto done;

        case TokenType::Newline:
            finalize();
            break;

        case TokenType::Indent:
            // The last finalized node is the parent of the upcoming children.
            if (last.valid() && last != root)
                parent_stack.push_back(last);
            break;

        case TokenType::Dedent:
            if (parent_stack.size() > 1)
                parent_stack.pop_back();
            break;

            //  Node assembly

        case TokenType::Tag:
            if (!current.valid())
                current = tree.alloc();
            tree.node(current)->setStyle("tag", tok.value);
            break;

        case TokenType::Id:
            if (!current.valid())
                current = tree.alloc();
            tree.node(current)->setStyle("id", tok.value);
            break;

        case TokenType::Class: {
            if (!current.valid())
                current = tree.alloc();
            RenderNode *n  = tree.node(current);
            const auto cls = n->style("class");
            if (cls.empty())
                n->setStyle("class", tok.value);
            else
                n->setStyle("class", std::string(cls) + ' ' + tok.value);
            break;
        }

        case TokenType::Text:
            if (!current.valid())
                current = tree.alloc();
            tree.node(current)->setStyle("text", tok.value);
            break;

        case TokenType::AttrKey: {
            if (!current.valid())
                current = tree.alloc();
            // Always paired: AttrKey is immediately followed by AttrValue.
            std::string val;
            if (i < tokens.size() && tokens[i].type == TokenType::AttrValue)
                val = tokens[i++].value;
            tree.node(current)->setStyle(tok.value, std::move(val));
            break;
        }

        case TokenType::AttrValue:
            // Consumed above by AttrKey — orphaned AttrValue is a no-op.
            break;

        }  // switch
    }  // for

done:
    return root;
}

}  // namespace pce::sdlos::jade
