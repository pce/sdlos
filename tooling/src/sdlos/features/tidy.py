"""
sdlos.features.tidy
===================
Pure libclang lint checker

Two complementary analysis passes run entirely in-process via the
``libclang`` Python bindings:

Pass 1 — Diagnostics
    Parse the file with a comprehensive set of ``-W`` warning flags.
    Clang's own diagnostic engine fires the same checks that clang-tidy
    exposes under ``bugprone-*``, ``modernize-*``, ``readability-*``,
    ``performance-*``, and ``clang-analyzer-*``.  Relevant flags:

        -Wall  -Wextra
        -Wsuggest-override       modernize-use-override
        -Wnon-virtual-dtor       bugprone
        -Wold-style-cast         cppcoreguidelines-pro-type-cstyle-cast
        -Wzero-as-null-pointer-constant   modernize-use-nullptr
        -Wconversion             bugprone-narrowing-conversions
        -Wsign-conversion        signed/unsigned compare
        -Wshadow                 bugprone-shadow
        -Wimplicit-fallthrough   bugprone-implicit-fallthrough
        -Wnull-dereference       clang-analyzer-core
        -Wunused                 readability-unused-*
        -Wdouble-promotion       performance-type-promotion
        -Wdeprecated             modernize-deprecated-*

Pass 2 — AST checks
    Targeted patterns not covered by -W flags:

        cstyle_cast         CSTYLE_CAST_EXPR nodes  (belt-and-braces)
        null_macro          MACRO_EXPANSION of NULL  → use nullptr
        large_val_param     std::string / vector / map by value  → const ref
        endl_perf           stream << std::endl  → prefer '\\n'
        strcmp_eq           strcmp(..) == 0  → std::string operator==
        mutable_global      non-const namespace-scope VAR_DECL

Warning profiles
----------------
    LintProfile.OFF     parse only — no extra -W flags
    LintProfile.SDLOS   curated set for the sdlos / SDL3 C++23 codebase
    LintProfile.STRICT  SDLOS + -Wpedantic + cast-qual + format=2

Compile-commands support
------------------------
When ``compile_commands`` is supplied the relevant compilation flags
(include paths, defines, ``-std``) are extracted from the JSON database
and forwarded to libclang so that header resolution works correctly:

    lint = ClangLint(compile_commands=Path("build/compile_commands.json"))

No subprocess is ever spawned.

Usage
-----
    from sdlos.features.tidy import ClangLint, LintProfile

    # Standalone
    lint   = ClangLint()
    issues = lint.analyze(Path("src/render_tree.cxx"))

    # PostProcessor hook (used by PostProcessPipeline)
    result = lint.process(Path("src/render_tree.cxx"))

    # Strict profile with explicit include dirs
    lint = ClangLint(
        profile=LintProfile.STRICT,
        extra_args=["-Isrc", "-Ideps/SDL3/include"],
    )

Backward compatibility
----------------------
The public names ``ClangTidy`` and ``TidyCheckProfile`` are re-exported
as aliases so existing code that imports them continues to work.
"""
from __future__ import annotations

import dataclasses
import json
import shlex
from enum import Enum, auto
from pathlib import Path
from typing import Optional


# ── Lint profiles ─────────────────────────────────────────────────────────────

class LintProfile(Enum):
    """Warning-flag profiles for :class:`ClangLint`."""

    OFF    = auto()   # parse only — no extra -W flags
    SDLOS  = auto()   # curated profile for sdlos / SDL3 / C++23  (default)
    STRICT = auto()   # SDLOS + pedantic + extra safety flags


# Warning flags for each profile.
# All flags are standard Clang; they do not require clang-tidy.
_WARN_FLAGS: dict[LintProfile, list[str]] = {

    LintProfile.OFF: [],

    LintProfile.SDLOS: [
        # ── Core ──────────────────────────────────────────────────────────────
        "-Wall",
        "-Wextra",

        # ── Null pointer ──────────────────────────────────────────────────────
        "-Wzero-as-null-pointer-constant",   # modernize-use-nullptr
        "-Wnull-dereference",                # clang-analyzer-core.NullDereference

        # ── Casts ─────────────────────────────────────────────────────────────
        "-Wold-style-cast",                  # cppcoreguidelines-pro-type-cstyle-cast

        # ── Override / virtuals ───────────────────────────────────────────────
        "-Wsuggest-override",                # modernize-use-override
        "-Wnon-virtual-dtor",               # bugprone-virtual-near-miss

        # ── Numeric conversions ───────────────────────────────────────────────
        "-Wconversion",                      # bugprone-narrowing-conversions
        "-Wsign-conversion",                 # signed/unsigned mismatch
        "-Wdouble-promotion",               # performance-type-promotion-in-math-fn
        "-Wfloat-equal",                     # comparing floats for equality

        # ── Undefined / unspecified behaviour ─────────────────────────────────
        "-Wimplicit-fallthrough",            # bugprone-implicit-fallthrough
        "-Wuninitialized",                   # cppcoreguidelines-init-variables
        "-Wmissing-field-initializers",

        # ── Unused code ───────────────────────────────────────────────────────
        "-Wunused",
        "-Wunused-parameter",

        # ── Shadow ────────────────────────────────────────────────────────────
        "-Wshadow",                          # bugprone-shadow

        # ── Deprecated ────────────────────────────────────────────────────────
        "-Wdeprecated",

        # ── Suppress SDL3 / platform noise ────────────────────────────────────
        "-Wno-c++98-compat",
        "-Wno-c++98-compat-pedantic",
        "-Wno-padded",
        "-Wno-switch-enum",
        "-Wno-covered-switch-default",
        "-Wno-reserved-identifier",
        "-Wno-reserved-macro-identifier",
        "-Wno-gnu-anonymous-struct",
        "-Wno-nested-anon-types",
    ],

    LintProfile.STRICT: [],   # filled in below
}

_WARN_FLAGS[LintProfile.STRICT] = list(_WARN_FLAGS[LintProfile.SDLOS]) + [
    "-Wpedantic",
    "-Wcast-qual",
    "-Wformat=2",
    "-Wvla",
    "-Warray-bounds-pointer-arithmetic",
    "-Wassign-enum",
    "-Wbad-function-cast",
    "-Wcomma",
    "-Wconsumed",
    "-Wloop-analysis",
    "-Wpointer-arith",
    "-Wshift-sign-overflow",
    "-Wshorten-64-to-32",
    "-Wtautological-compare",
    "-Wthread-safety",
    "-Wunreachable-code",
    "-Wunreachable-code-aggressive",
    "-Wno-c++20-compat",   # suppress C++20 compat noise while targeting C++23
]


# ── TidyIssue ─────────────────────────────────────────────────────────────────

@dataclasses.dataclass(frozen=True)
class TidyIssue:
    """A single lint diagnostic.

    Attributes
    ----------
    file:    Absolute path to the source file.
    line:    1-based line number.
    col:     1-based column number.
    level:   ``"error"``, ``"warning"``, ``"note"``, or ``"remark"``.
    check:   Category name (e.g. ``"Wold-style-cast"``, ``"ast:null_macro"``).
    message: Human-readable diagnostic text.
    fix:     True when an automatic fix could be applied (not yet implemented).
    """

    file:    str
    line:    int
    col:     int
    level:   str
    check:   str
    message: str
    fix:     bool = False


# ── ClangLint ─────────────────────────────────────────────────────────────────

class ClangLint:
    """Pure libclang lint checker — no clang-tidy binary required.

    Parameters
    ----------
    profile:
        Warning-flag profile.  :attr:`LintProfile.SDLOS` (default) uses a
        curated set appropriate for the sdlos / SDL3 / C++23 codebase.
    extra_args:
        Additional compilation flags forwarded to libclang
        (e.g. ``["-Isrc", "-Ideps/SDL3/include"]``).
        ``-std=c++23`` and ``-x c++`` are always included.
    compile_commands:
        Path to a ``compile_commands.json`` produced by CMake with
        ``-DCMAKE_EXPORT_COMPILE_COMMANDS=ON``.  When provided the flags
        for the target file are extracted and merged with *extra_args*,
        giving accurate include-path resolution.
    ast_checks:
        Enable the AST-walk pass for patterns not covered by -W flags.
        True by default.
    skip_system_headers:
        Suppress diagnostics that originate inside system / SDK headers.
        True by default.
    """

    _BASE_ARGS = ["-std=c++23", "-x", "c++"]

    def __init__(
        self,
        profile:             LintProfile          = LintProfile.SDLOS,
        extra_args:          Optional[list[str]]  = None,
        compile_commands:    Optional[Path]       = None,
        ast_checks:          bool                 = True,
        skip_system_headers: bool                 = True,
    ) -> None:
        self.profile             = profile
        self.extra_args          = extra_args or []
        self.compile_commands    = compile_commands
        self.ast_checks          = ast_checks
        self.skip_system_headers = skip_system_headers

    # ── PostProcessor hook ─────────────────────────────────────────────────────

    def process(self, path: Path) -> "PostProcessResult":
        """``PostProcessor`` protocol entry-point.

        Runs both lint passes on *path* and returns a
        :class:`~sdlos.features.post_process.PostProcessResult`.
        The file is **never** modified.
        """
        from .post_process import PostProcessResult

        try:
            from clang import cindex as _  # noqa: F401
        except ImportError:
            return PostProcessResult(
                processor="ClangLint",
                modified=False,
                error=(
                    "libclang Python bindings not installed — "
                    "run 'pip install libclang'.  Lint pass skipped."
                ),
            )

        issues = self.analyze(path)
        return PostProcessResult(
            processor="ClangLint",
            modified=False,
            issues=issues,
        )

    # ── Public API ─────────────────────────────────────────────────────────────

    def analyze(self, path: Path) -> list[TidyIssue]:
        """Run all enabled lint passes on *path*.

        Parameters
        ----------
        path:
            C++ source file to analyse.  Must exist on disk.

        Returns
        -------
        list[TidyIssue]
            All findings, sorted by (line, col).
        """
        try:
            from clang import cindex
        except ImportError:
            return []

        args = self._build_args(path)

        index = cindex.Index.create()
        tu    = index.parse(
            str(path),
            args=args,
            # PARSE_DETAILED_PROCESSING_RECORD is required for macro expansion
            # info (MACRO_INSTANTIATION cursors) and accurate category names.
            # We intentionally do NOT use PARSE_SKIP_FUNCTION_BODIES here:
            # the AST-walk checks (cstyle_cast, null_macro, endl_perf, …) all
            # inspect nodes that live inside function bodies, so we must parse
            # them fully.  This is slower than the SCA / docblocks passes but
            # necessary for correct lint results.
            options=cindex.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD,
        )

        issues: list[TidyIssue] = []

        # Pass 1 — Clang diagnostic engine
        issues.extend(self._collect_diagnostics(tu, path, cindex))

        # Pass 2 — custom AST checks
        if self.ast_checks:
            issues.extend(self._run_ast_checks(tu, path, cindex))

        return sorted(issues, key=lambda i: (i.line, i.col))

    # ── Argument construction ──────────────────────────────────────────────────

    def _build_args(self, path: Path) -> list[str]:
        """Assemble the full argument list for libclang.

        Merge order (later entries win for conflicting flags):
          1. Base args (``-std=c++23 -x c++``)
          2. Warning profile flags
          3. Flags extracted from compile_commands.json (if provided)
          4. Caller-supplied ``extra_args``
        """
        args = list(self._BASE_ARGS)
        args.extend(_WARN_FLAGS.get(self.profile, []))
        args.extend(self._flags_from_compile_commands(path))
        args.extend(self.extra_args)
        return args

    def _flags_from_compile_commands(self, path: Path) -> list[str]:
        """Extract compilation flags for *path* from compile_commands.json.

        Only include flags that affect header resolution or language standard
        (``-I``, ``-D``, ``-std``, ``-isystem``, ``-F``).  Flags that would
        change compilation output (``-o``, ``-c``, the filename itself) are
        dropped.

        Returns an empty list when no compile_commands file was provided or
        when no entry matches *path*.
        """
        if not self.compile_commands or not self.compile_commands.exists():
            return []

        try:
            db    = json.loads(self.compile_commands.read_text(encoding="utf-8"))
            canon = str(path.resolve())

            for entry in db:
                entry_file = str(Path(entry.get("file", "")).resolve())
                if entry_file != canon:
                    continue

                # Prefer 'arguments' (list) over 'command' (shell string).
                if "arguments" in entry:
                    raw_args = list(entry["arguments"])
                elif "command" in entry:
                    raw_args = shlex.split(entry["command"])
                else:
                    continue

                return _filter_compiler_flags(raw_args)

        except Exception:  # noqa: BLE001
            pass

        return []

    # ── Pass 1 — diagnostics ──────────────────────────────────────────────────

    def _collect_diagnostics(
        self,
        tu,
        path: Path,
        cindex,
    ) -> list[TidyIssue]:
        """Collect Clang diagnostic warnings from the parsed translation unit.

        Only diagnostics whose source location maps back to *path* are
        returned; transitive issues from system headers are suppressed when
        :attr:`skip_system_headers` is True.
        """
        issues: list[TidyIssue] = []
        canon  = str(path.resolve())

        _sev_map = {
            cindex.Diagnostic.Note:    "note",
            cindex.Diagnostic.Warning: "warning",
            cindex.Diagnostic.Error:   "error",
            cindex.Diagnostic.Fatal:   "error",
        }

        for diag in tu.diagnostics:
            loc = diag.location
            if not loc.file:
                continue

            diag_file = str(Path(loc.file.name).resolve())

            if self.skip_system_headers and diag_file != canon:
                continue

            level = _sev_map.get(diag.severity, "remark")

            # Derive a short check name from the Clang category.
            # e.g. "Semantic Issue" → "semantic", "Unused Code" → "unused-code"
            category = (diag.category_name or "diagnostic").lower().replace(" ", "-")

            issues.append(TidyIssue(
                file=diag_file,
                line=loc.line,
                col=loc.column,
                level=level,
                check=f"W:{category}",
                message=diag.spelling,
            ))

        return issues

    # ── Pass 2 — AST checks ───────────────────────────────────────────────────

    def _run_ast_checks(
        self,
        tu,
        path: Path,
        cindex,
    ) -> list[TidyIssue]:
        """Walk the AST and apply custom checks not covered by -W flags."""
        issues: list[TidyIssue] = []
        canon  = str(path.resolve())
        CK     = cindex.CursorKind
        TK     = cindex.TypeKind

        def walk(node) -> None:
            # Stay in our file.
            if node.location.file:
                if str(Path(node.location.file.name).resolve()) != canon:
                    return

            k = node.kind

            # ── cstyle_cast ──────────────────────────────────────────────────
            # Belt-and-braces: -Wold-style-cast may not fire for every cast
            # form (e.g. functional-style casts in templates).
            if k == CK.CSTYLE_CAST_EXPR:
                _add(issues, node, "ast:cstyle_cast",
                     "C-style cast — use static_cast, reinterpret_cast, "
                     "or const_cast to make the conversion intent explicit",
                     "warning")

            # ── null_macro ───────────────────────────────────────────────────
            # Detect macro expansion of NULL where nullptr is preferred.
            # The Python bindings expose this as MACRO_INSTANTIATION (the older
            # C API alias); MACRO_EXPANSION may not exist in all binding versions.
            elif k == _macro_instantiation_kind(CK):
                if node.spelling == "NULL":
                    _add(issues, node, "ast:null_macro",
                         "NULL macro used — prefer nullptr (modernize-use-nullptr)",
                         "warning")

            # ── large_val_param ──────────────────────────────────────────────
            # Flag std::string / vector / map / set / unordered_map parameters
            # passed by value when they are not moved.
            elif k == CK.PARM_DECL:
                _check_large_val_param(issues, node, TK)

            # ── endl_perf ────────────────────────────────────────────────────
            # Detect `<< std::endl` — flushes the buffer on every call.
            elif k == CK.CALL_EXPR:
                spelling = node.spelling or ""
                if "endl" in spelling:
                    _add(issues, node, "ast:endl_perf",
                         "'std::endl' flushes the stream buffer on every call — "
                         "use '\\n' unless an explicit flush is required "
                         "(performance-avoid-endl)",
                         "warning")

                # ── strcmp_eq ─────────────────────────────────────────────────
                # strcmp(a, b) == 0  →  a == b  (when both are std::string)
                elif spelling in ("strcmp", "strncmp", "strcasecmp"):
                    _add(issues, node, "ast:strcmp_eq",
                         f"'{spelling}' for equality — consider std::string "
                         "operator== or std::string_view::operator== for "
                         "clearer intent (readability-string-compare)",
                         "note")

            # ── mutable_global ───────────────────────────────────────────────
            # Non-const variable declared at namespace / translation-unit scope.
            elif k == CK.VAR_DECL:
                parent_kind = node.semantic_parent.kind if node.semantic_parent else None
                if parent_kind in (CK.NAMESPACE, CK.TRANSLATION_UNIT):
                    if not node.type.is_const_qualified():
                        # Allow SDL-style extern declarations and static locals.
                        if node.linkage != cindex.LinkageKind.EXTERNAL:
                            _add(issues, node, "ast:mutable_global",
                                 f"Mutable namespace-scope variable '{node.spelling}' — "
                                 "consider making it const, wrapping it in a "
                                 "function-local static (Meyers singleton), or "
                                 "passing it as a parameter "
                                 "(cppcoreguidelines-avoid-non-const-global-variables)",
                                 "note")

            for child in node.get_children():
                walk(child)

        walk(tu.cursor)
        return issues


# ── Rich output ───────────────────────────────────────────────────────────────

def print_issues(
    issues: list[TidyIssue],
    path:   Path,
    console = None,
) -> None:
    """Print a Rich-formatted lint summary for *issues*.

    Parameters
    ----------
    issues:
        Findings from :meth:`ClangLint.analyze`.
    path:
        The source file that was analysed (used in the header).
    console:
        Rich Console instance.  Created if omitted.
    """
    try:
        from rich.console import Console
        from rich.table import Table
        from rich import box
    except ImportError:
        for i in issues:
            print(f"[{i.level}] {i.file}:{i.line}:{i.col} [{i.check}] {i.message}")
        return

    con = console or Console()

    if not issues:
        con.print(
            f"  [green]✓[/]  [dim]ClangLint[/dim]  "
            f"[dim]{path.name}[/dim]  — no issues"
        )
        return

    _sty = {
        "error":   "bold red",
        "warning": "yellow",
        "note":    "cyan",
        "remark":  "dim",
    }

    table = Table(
        box=box.SIMPLE_HEAD,
        show_header=True,
        header_style="bold dim",
        expand=True,
    )
    table.add_column("Line",  style="dim", width=6,  justify="right")
    table.add_column("Col",   style="dim", width=4,  justify="right")
    table.add_column("Level", width=8)
    table.add_column("Check", width=28, no_wrap=True)
    table.add_column("Message")

    for issue in issues:
        st = _sty.get(issue.level, "white")
        table.add_row(
            str(issue.line),
            str(issue.col),
            f"[{st}]{issue.level}[/]",
            f"[dim]{issue.check}[/dim]",
            f"[{st}]{issue.message}[/]",
        )

    counts = {
        lv: sum(1 for i in issues if i.level == lv)
        for lv in ("error", "warning", "note", "remark")
        if any(i.level == lv for i in issues)
    }

    con.print(f"\n[bold]{path.name}[/bold]  — ClangLint")
    con.print(table)
    con.print(
        f"  [bold]{len(issues)}[/bold] issue(s):  "
        + "  ".join(
            f"[{_sty[lv]}]{lv} {n}[/]"
            for lv, n in counts.items()
        )
    )


# ── CursorKind compatibility helpers ─────────────────────────────────────────

def _macro_instantiation_kind(CK):
    """Return the CursorKind value for macro expansions.

    The Python libclang bindings expose the same underlying cursor type under
    two names depending on the binding version:

    * ``CursorKind.MACRO_INSTANTIATION``  — canonical Python-binding name
    * ``CursorKind.MACRO_EXPANSION``      — C API alias, absent in some builds

    We try both and fall back to a sentinel that will never match so that the
    check is silently skipped rather than raising ``AttributeError``.
    """
    for name in ("MACRO_INSTANTIATION", "MACRO_EXPANSION"):
        kind = getattr(CK, name, None)
        if kind is not None:
            return kind
    # Return an object that compares unequal to any real CursorKind so the
    # elif branch is simply never taken.
    return object()


# ── Internal helpers ──────────────────────────────────────────────────────────

def _add(
    issues:  list[TidyIssue],
    node,
    check:   str,
    message: str,
    level:   str = "warning",
) -> None:
    """Append a :class:`TidyIssue` built from *node*'s source location."""
    loc = node.location
    issues.append(TidyIssue(
        file=loc.file.name if loc.file else "",
        line=loc.line,
        col=loc.column,
        level=level,
        check=check,
        message=message,
    ))


def _check_large_val_param(
    issues: list[TidyIssue],
    node,
    TK,
) -> None:
    """Flag known-large standard-library types passed by value.

    Heuristic: if the canonical type spelling contains one of the large
    container / string names and is *not* already a reference or pointer,
    it is likely an unnecessary copy.
    """
    _LARGE_TYPES = (
        "std::basic_string",
        "std::vector",
        "std::map",
        "std::unordered_map",
        "std::set",
        "std::unordered_set",
        "std::deque",
        "std::list",
        "std::multimap",
        "std::multiset",
    )

    try:
        canonical = node.type.get_canonical()
    except Exception:   # noqa: BLE001
        return

    # Already a reference or pointer — not a copy.
    if canonical.kind in (TK.LVALUEREFERENCE, TK.RVALUEREFERENCE, TK.POINTER):
        return

    # Already const-qualified value — still a copy but intentional for move.
    # We only warn on non-const value parameters.
    if canonical.is_const_qualified():
        return

    spelling = canonical.spelling or ""
    for large in _LARGE_TYPES:
        if large in spelling:
            short = spelling.split("<")[0].split("::")[-1]  # e.g. "vector"
            _add(
                issues, node,
                "ast:large_val_param",
                f"Parameter '{node.spelling}' passes std::{short} by value — "
                f"consider 'const {spelling}&' to avoid an unnecessary copy "
                f"(performance-unnecessary-value-param)",
                "warning",
            )
            return


def _filter_compiler_flags(args: list[str]) -> list[str]:
    """Extract only the flags relevant for libclang header resolution.

    Keeps: ``-I``, ``-isystem``, ``-D``, ``-std``, ``-F``.
    Drops: ``-o``, ``-c``, ``-MF``, ``-MT``, ``-MD``, source filenames,
           response files, and all other driver flags.
    """
    result:   list[str] = []
    skip_next: bool     = False

    _KEEP_PREFIXES = ("-I", "-isystem", "-D", "-std=", "-F", "-include")
    _SKIP_FLAGS    = frozenset({"-o", "-c", "-MF", "-MT", "-MQ", "-arch"})
    _SKIP_NEXT     = frozenset({"-o", "-MF", "-MT", "-MQ", "-arch", "-target"})

    for arg in args:
        if skip_next:
            skip_next = False
            continue

        if arg in _SKIP_NEXT:
            skip_next = True
            continue

        if arg in _SKIP_FLAGS:
            continue

        # Drop source / object file arguments.
        if arg.endswith((".c", ".cpp", ".cxx", ".cc", ".o", ".obj")):
            continue

        # Keep known useful prefixes.
        if any(arg.startswith(p) for p in _KEEP_PREFIXES):
            result.append(arg)
            continue

        # Some flags come as two tokens: "-I /path/to/include"
        if arg in ("-I", "-isystem", "-include", "-D", "-F"):
            result.append(arg)
            # The next iteration will add the value.
            continue

    return result


# ── Backward-compatible aliases ───────────────────────────────────────────────

#: Alias so existing code that imports ``ClangTidy`` continues to work.
ClangTidy = ClangLint

#: Alias for the old ``TidyCheckProfile`` enum.
class TidyCheckProfile(Enum):
    """Backward-compatible alias for :class:`LintProfile`.

    Use :class:`LintProfile` in new code.
    """
    OFF    = auto()
    SDLOS  = auto()
    STRICT = auto()
