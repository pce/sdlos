#pragma once

// vfs/uri.hh — lightweight URI type for the sdlos virtual file system.
//
// Supports two surface forms:
//
//   "scheme://path/to/file"    routed to the mount registered as 'scheme'
//   "bare/path/to/file"        no-scheme path; Vfs uses its default scheme
//
// The authority component (the part RFC 3986 places between "//" and the
// first "/") is folded into the path for simplicity:
//
//   "scene://assets/audio/t-rex-roar"
//         ↳ scheme = "scene"
//         ↳ path   = "assets/audio/t-rex-roar"
//
//   "file:///etc/hosts"
//         ↳ scheme = "file"
//         ↳ path   = "/etc/hosts"   ← leading '/' is preserved
//
//   "data/pipeline.pug"
//         ↳ scheme = ""             ← empty → use Vfs default scheme
//         ↳ path   = "data/pipeline.pug"
//
// This departs slightly from RFC 3986 authority handling but keeps Jade
// attribute values natural and readable:
//
//   audio(src="scene://assets/audio/t-rex-roar")
//   img(src="asset://data/textures/hero.png")
//
// All operations are noexcept and allocation-free for the parse path when
// string_view construction is possible.

#include <string>
#include <string_view>

namespace pce::vfs {

// ---------------------------------------------------------------------------
// Uri
// ---------------------------------------------------------------------------

struct Uri {
    std::string scheme;   ///< e.g. "scene", "jade", "asset", "file", "mem"
    std::string path;     ///< path within the scheme's mount (never includes "://")

    // ---- Construction ----------------------------------------------------

    Uri() = default;

    Uri(std::string scheme_, std::string path_)
        : scheme(std::move(scheme_))
        , path(std::move(path_))
    {}

    // ---- Parsing ---------------------------------------------------------

    /// Parse a raw URI string into a Uri.
    ///
    /// "scene://assets/foo"  →  { scheme="scene",  path="assets/foo"     }
    /// "file:///etc/hosts"   →  { scheme="file",   path="/etc/hosts"     }
    /// "mem://scratch/buf"   →  { scheme="mem",    path="scratch/buf"    }
    /// "data/pipeline.pug"   →  { scheme="",       path="data/pipeline.pug" }
    /// ""                    →  { scheme="",        path=""              }
    ///
    /// The function is intentionally forgiving: malformed URIs are treated
    /// as bare paths (empty scheme) so callers never crash on user input.
    [[nodiscard]] static Uri parse(std::string_view raw) noexcept
    {
        if (raw.empty()) return {};

        // Scan for "://" — must appear after at least one scheme character.
        // Scheme chars: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
        // We accept any non-empty prefix before "://" as the scheme to keep
        // the implementation simple and forward-compatible.
        constexpr std::string_view sep{"://"};
        const auto sep_pos = raw.find(sep);

        if (sep_pos == std::string_view::npos || sep_pos == 0) {
            // No "://" or it starts at position 0 → bare path.
            return { "", std::string(raw) };
        }

        // Validate that every character in the scheme candidate is safe
        // (letters, digits, +, -, .).  A Windows drive letter like "C:\"
        // would fail this check (\ after :) which is intentional — such
        // paths should be given as bare strings or via the "file" scheme.
        const auto scheme_sv = raw.substr(0, sep_pos);
        for (const char c : scheme_sv) {
            const bool ok = (c >= 'a' && c <= 'z')
                         || (c >= 'A' && c <= 'Z')
                         || (c >= '0' && c <= '9')
                         || c == '+' || c == '-' || c == '.';
            if (!ok) return { "", std::string(raw) };
        }

        // Normalise scheme to lower-case so "Scene://" and "scene://" are
        // equivalent — matching browser and URL spec behaviour.
        std::string scheme_lc(scheme_sv);
        for (char& c : scheme_lc) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        }

        // Everything after "://" is the path.  For "file:///etc/hosts" that
        // is "/etc/hosts" (preserves the leading slash, which is correct for
        // absolute file paths).
        const auto path_sv = raw.substr(sep_pos + sep.size());

        return { std::move(scheme_lc), std::string(path_sv) };
    }

    // ---- Reconstruction --------------------------------------------------

    /// Reconstruct the canonical URI string.
    ///
    ///   { scheme="scene", path="assets/foo" }  →  "scene://assets/foo"
    ///   { scheme="",      path="data/x.pug" }  →  "data/x.pug"
    [[nodiscard]] std::string str() const
    {
        if (scheme.empty()) return path;
        return scheme + "://" + path;
    }

    // ---- Predicates ------------------------------------------------------

    [[nodiscard]] bool has_scheme() const noexcept { return !scheme.empty(); }
    [[nodiscard]] bool empty()      const noexcept { return scheme.empty() && path.empty(); }

    // ---- Equality --------------------------------------------------------

    [[nodiscard]] bool operator==(const Uri&) const noexcept = default;
    [[nodiscard]] bool operator!=(const Uri&) const noexcept = default;
};

} // namespace pce::vfs
