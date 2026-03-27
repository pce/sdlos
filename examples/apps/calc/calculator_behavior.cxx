#include <climits>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {


using Fixed = int64_t;

static constexpr Fixed kScale     = 1'000'000LL;   // 10^6
static constexpr int   kScaleDigs = 6;             // fractional digits in display


std::string fixed_fmt(Fixed v)
{
    if (v == 0) return "0";

    const bool  neg  = (v < 0);
    const Fixed absv = neg ? -v : v;  // safe: our range never reaches INT64_MIN

    const Fixed int_part  = absv / kScale;
    const Fixed frac_part = absv % kScale;

    std::string s;
    if (neg) s += '-';
    s += std::to_string(int_part);

    if (frac_part != 0) {
        s += '.';
        std::string frac = std::to_string(frac_part);
        // Left-pad with zeros to exactly kScaleDigs digits.
        while (static_cast<int>(frac.size()) < kScaleDigs)
            frac = '0' + frac;
        // Strip trailing zeros.
        while (!frac.empty() && frac.back() == '0')
            frac.pop_back();
        s += frac;
    }

    return s;
}

std::optional<Fixed> fixed_parse(std::string_view s) noexcept
{
    if (s.empty() || s == "Error") return std::nullopt;

    const bool neg = (!s.empty() && s[0] == '-');
    if (neg) s.remove_prefix(1);
    if (s.empty()) return std::nullopt;

    Fixed int_part  = 0;
    Fixed frac_part = 0;
    int   frac_digs = 0;
    bool  in_frac   = false;

    for (const char c : s) {
        if (c == '.') { in_frac = true; continue; }
        if (c < '0' || c > '9') return std::nullopt;

        if (!in_frac) {
            // Guard against overflow of the integer part before scaling.
            if (int_part > (INT64_MAX / 10)) return std::nullopt;
            int_part = int_part * 10 + (c - '0');
        } else {
            if (frac_digs < kScaleDigs) {
                frac_part = frac_part * 10 + (c - '0');
                ++frac_digs;
            }
            // Digits beyond kScaleDigs are truncated (not rounded), KISS.
        }
    }

    // Scale the fractional part up to kScale.
    for (int i = frac_digs; i < kScaleDigs; ++i)
        frac_part *= 10;

    // Combine integer and fractional parts, checking for overflow.
    if (int_part > INT64_MAX / kScale) return std::nullopt;
    const Fixed result = int_part * kScale + frac_part;

    return neg ? -result : result;
}


std::optional<Fixed> fixed_apply(Fixed a, Fixed b, std::string_view op) noexcept
{
    if (op == "+") {
        const __int128 r = static_cast<__int128>(a) + b;
        if (r > INT64_MAX || r < INT64_MIN) return std::nullopt;
        return static_cast<Fixed>(r);
    }

    if (op == "−") {                                    // U+2212 MINUS SIGN
        const __int128 r = static_cast<__int128>(a) - b;
        if (r > INT64_MAX || r < INT64_MIN) return std::nullopt;
        return static_cast<Fixed>(r);
    }

    if (op == "×") {                                    // U+00D7 MULTIPLICATION SIGN
        const __int128 r = static_cast<__int128>(a) * b / kScale;
        if (r > INT64_MAX || r < INT64_MIN) return std::nullopt;
        return static_cast<Fixed>(r);
    }

    if (op == "÷") {                                    // U+00F7 DIVISION SIGN
        if (b == 0) return std::nullopt;                // division by zero
        const __int128 r = static_cast<__int128>(a) * kScale / b;
        if (r > INT64_MAX || r < INT64_MIN) return std::nullopt;
        return static_cast<Fixed>(r);
    }

    return std::nullopt;
}


struct RpnState {

    std::vector<Fixed> stack;
    std::string        input;               // decimal string currently being typed
    std::string        display_str{ "0" };  // text on the display node
    bool               error = false;       // set on ÷0 / overflow; cleared by AC only

    // Commit the input string to the stack.
    // Sets the error flag when the string cannot be parsed (should not happen
    // in normal use since we control every character that enters `input`).
    void commitInput()
    {
        if (input.empty()) return;
        const auto v = fixed_parse(input);
        if (!v) {
            error       = true;
            display_str = "Error";
            input.clear();
            return;
        }
        stack.push_back(*v);
        input.clear();
    }

    // Refresh display_str from the top of the stack.
    // When the error flag is set the display always shows "Error".
    void syncDisplay()
    {
        if (error) { display_str = "Error"; return; }
        display_str = stack.empty() ? "0" : fixed_fmt(stack.back());
    }
};

} // anonymous namespace


void jade_app_init(pce::sdlos::RenderTree&  tree,
                   pce::sdlos::NodeHandle   root,
                   pce::sdlos::IEventBus&   bus,
                   pce::sdlos::SDLRenderer& /*renderer*/)
{
    const pce::sdlos::NodeHandle display_h = tree.findById(root, "display");
    if (!display_h.valid()) return;

    auto state = std::make_shared<RpnState>();

    auto show = [&tree, display_h, state]() {
        if (pce::sdlos::RenderNode* dn = tree.node(display_h)) {
            dn->setStyle("text", state->display_str);
            dn->dirty_render = true;
        }
    };

    bus.subscribe("calc:digit",
        [state, show](const std::string& key) {
            if (state->error) { show(); return; }

            if (key == ".") {
                if (state->input.find('.') == std::string::npos) {
                    if (state->input.empty()) state->input = "0";
                    if (state->input.size() < 9) state->input += '.';
                }
            } else {
                // Count significant chars (excluding a leading '-').
                const std::size_t sig = state->input.size()
                    - ((!state->input.empty() && state->input[0] == '-') ? 1 : 0);

                if      (state->input == "0") state->input  = key;  // replace lone zero
                else if (sig < 9)             state->input += key;
            }

            state->display_str = state->input.empty() ? "0" : state->input;
            show();
        });


    bus.subscribe("calc:push",
        [state, show](const std::string& /*key*/) {
            if (state->error) { show(); return; }
            state->commitInput();
            state->syncDisplay();
            show();
        });

    bus.subscribe("calc:op",
        [state, show](const std::string& op) {
            if (state->error) { show(); return; }

            state->commitInput();
            if (state->error) { show(); return; }   // commitInput may have failed

            if (state->stack.size() >= 2) {
                const Fixed b = state->stack.back(); state->stack.pop_back();
                const Fixed a = state->stack.back(); state->stack.pop_back();

                const auto result = fixed_apply(a, b, op);

                if (!result) {
                    // Bad result: restore operands, latch error.
                    state->stack.push_back(a);
                    state->stack.push_back(b);
                    state->error       = true;
                    state->display_str = "Error";
                } else {
                    state->stack.push_back(*result);
                    state->syncDisplay();
                }
            }
            show();
        });


    bus.subscribe("calc:fn",
        [state, show](const std::string& key) {

            if (key == "AC") {
                state->stack.clear();
                state->input.clear();
                state->display_str = "0";
                state->error       = false;         // only AC escapes the error state

            } else if (!state->error) {

                if (key == "±") {
                    if (!state->input.empty() && state->input != "0") {
                        // Toggle leading '-' directly on the string.
                        if (state->input[0] == '-') state->input.erase(0, 1);
                        else                        state->input.insert(0, "-");
                        state->display_str = state->input;
                    } else if (!state->stack.empty()) {
                        state->stack.back() = -state->stack.back();
                        state->syncDisplay();
                    }

                } else if (key == "%") {
                    if (!state->input.empty()) {
                        const auto v = fixed_parse(state->input);
                        if (v) {
                            state->input       = fixed_fmt(*v / 100);
                            state->display_str = state->input;
                        }
                    } else if (!state->stack.empty()) {
                        state->stack.back() /= 100;
                        state->syncDisplay();
                    }
                }
            }

            show();
        });
}
