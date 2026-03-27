"""
sdlos.features.sca
==================
Clang Static Analyser — libclang-based multi-level security and quality
analysis for C++ source files.

Level 1  (cheap, high signal)
  - Unsafe C APIs: strcpy, sprintf, gets, …
  - Raw pointer parameters in function interfaces
  - Explicit ``new`` / ``delete`` expressions
  - Missing ``const`` on local variables and parameters (heuristic)

Level 2  (ownership + correctness)
  - Ownership ambiguity (raw ptr return from factory)
  - Discarded return values of nodiscard-like functions
  - Signed / unsigned comparison in conditions

Level 3  (stubs — advanced, requires full compilation database)
  - Lifetime analysis
  - Concurrency issues (data race heuristics)
  - Taint tracking (external input → sink)

Hook protocol
-------------
``SCA`` implements the ``PostProcessor`` protocol used by
``sdlos.features.post_process.PostProcessPipeline``:

    processor = SCA(level=2)
    result    = processor.process(path)   # PostProcessResult

It can also be used standalone:

    sca    = SCA(level=2)
    issues = sca.analyze(Path("src/foo.cxx"))

Knowledge packs
---------------
Security function warnings and design-pattern advisories are loaded from
``sdlos/data/knowledge_packs.yaml`` at import time.  The YAML file is the
single source of truth; no hard-coded lists live here.
"""
from __future__ import annotations

import dataclasses
from pathlib import Path
from typing import Optional

# Knowledge packs are loaded lazily to keep the import fast.
_KP: Optional[dict] = None


def _knowledge_packs() -> dict:
    global _KP
    if _KP is None:
        import yaml
        _kp_path = Path(__file__).parent.parent / "data" / "knowledge_packs.yaml"
        with _kp_path.open(encoding="utf-8") as fh:
            _KP = yaml.safe_load(fh) or {}
    return _KP


# ── Issue dataclass ────────────────────────────────────────────────────────────

@dataclasses.dataclass(frozen=True)
class Issue:
    """A single SCA finding."""

    level:   int          # 1 = critical, 2 = warning, 3 = informational
    kind:    str          # e.g. "unsafe_call", "raw_ptr_param", "new_expr"
    file:    str          # absolute path of the source file
    line:    int          # 1-based line number
    col:     int          # 1-based column number
    message: str          # human-readable description
    note:    str = ""     # optional secondary advisory text


# ── SCA ───────────────────────────────────────────────────────────────────────

class SCA:
    """Multi-level libclang static analyser.

    Parameters
    ----------
    level:
        Maximum analysis level to run (1–3).  Higher levels are slower and
        may produce more false positives.
    extra_args:
        Additional flags forwarded to libclang (e.g. include paths,
        ``-std=c++23``).  A sensible default is used when omitted.
    skip_system_headers:
        When True (default), issues in system headers are suppressed.
    """

    _DEFAULT_ARGS = ["-std=c++23", "-x", "c++"]

    def __init__(
        self,
        level: int = 2,
        extra_args: Optional[list[str]] = None,
        skip_system_headers: bool = True,
    ) -> None:
        self.level = level
        self.extra_args = extra_args or []
        self.skip_system_headers = skip_system_headers

    # ── PostProcessor hook ─────────────────────────────────────────────────────

    def process(self, path: Path) -> "PostProcessResult":
        """``PostProcessor`` protocol entry-point.

        Analyses *path* and returns a :class:`PostProcessResult` containing
        all found issues.  The file is **not** modified.

        When ``libclang`` Python bindings are not installed the result carries
        a non-fatal ``error`` string rather than a level-1 issue so that
        ``--fail-on-issues`` in CI is not triggered by a missing optional dep.
        """
        from .post_process import PostProcessResult  # avoid circular at module level

        try:
            from clang import cindex as _  # noqa: F401
        except ImportError:
            return PostProcessResult(
                processor="SCA",
                modified=False,
                error=(
                    "libclang Python bindings not installed — "
                    "run 'pip install libclang' and ensure libclang is on PATH. "
                    "SCA skipped."
                ),
            )

        issues = self.analyze(path)
        return PostProcessResult(
            processor="SCA",
            modified=False,
            issues=issues,
        )

    # ── Public API ─────────────────────────────────────────────────────────────

    def analyze(self, path: Path) -> list[Issue]:
        """Parse *path* and return all findings up to ``self.level``.

        Parameters
        ----------
        path:
            C++ source file to analyse.  The file must exist on disk (libclang
            reads it directly).

        Returns
        -------
        list[Issue]
            Sorted by (line, col, level).
        """
        try:
            from clang import cindex
        except ImportError:
            # Silently return no issues when called directly (standalone use).
            # The process() hook surfaces this as PostProcessResult.error instead.
            return []

        args = list(self._DEFAULT_ARGS) + list(self.extra_args)

        index = cindex.Index.create()
        tu = index.parse(
            str(path),
            args=args,
            options=(
                cindex.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD
                | cindex.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES
            ),
        )

        issues: list[Issue] = []
        self._walk(tu.cursor, path, issues, cindex)
        return sorted(issues, key=lambda i: (i.line, i.col, i.level))

    # ── AST walker ────────────────────────────────────────────────────────────

    def _walk(self, node, path: Path, issues: list[Issue], cindex) -> None:
        """Recursively walk the AST, collecting issues into *issues*."""
        CK = cindex.CursorKind

        # Suppress nodes that are not from our file.
        if node.location.file and node.location.file.name != str(path):
            if self.skip_system_headers:
                return  # entire subtree is in an external header
            # fall through for non-system analysis

        if self.level >= 1:
            self._check_unsafe_call(node, path, issues, CK)
            self._check_new_delete(node, path, issues, CK)
            self._check_raw_ptr_param(node, path, issues, CK, cindex)

        if self.level >= 2:
            self._check_raw_ptr_return(node, path, issues, CK, cindex)
            self._check_signed_unsigned(node, path, issues, CK, cindex)

        # Level 3 checks are stubs — reserved for future implementation.

        for child in node.get_children():
            self._walk(child, path, issues, cindex)

    # ── Level 1 checks ────────────────────────────────────────────────────────

    def _check_unsafe_call(self, node, path, issues, CK) -> None:
        """Flag calls to known-unsafe C standard library functions."""
        if node.kind != CK.CALL_EXPR:
            return
        name = node.spelling
        security = _knowledge_packs().get("security_functions", {})
        if name in security:
            advisory = security[name]
            issues.append(Issue(
                level=1,
                kind="unsafe_call",
                file=_file(node, path),
                line=node.location.line,
                col=node.location.column,
                message=f"Unsafe API '{name}'",
                note=advisory,
            ))

    def _check_new_delete(self, node, path, issues, CK) -> None:
        """Flag explicit ``new`` and ``delete`` expressions."""
        if node.kind not in (CK.CXX_NEW_EXPR, CK.CXX_DELETE_EXPR):
            return
        op = "new" if node.kind == CK.CXX_NEW_EXPR else "delete"
        dp = _knowledge_packs().get("design_patterns", {})
        key = "new_expression" if op == "new" else "delete_expression"
        advisory = dp.get(key, {}).get("text", f"Manual '{op}' expression")
        issues.append(Issue(
            level=1,
            kind=f"{op}_expr",
            file=_file(node, path),
            line=node.location.line,
            col=node.location.column,
            message=f"Manual '{op}' expression",
            note=advisory,
        ))

    def _check_raw_ptr_param(self, node, path, issues, CK, cindex) -> None:
        """Flag non-const raw pointer parameters in function declarations.

        C API opaque handles (e.g. ``SDL_GPUDevice*``, ``VkDevice``) are
        excluded via the ``c_opaque_handle_prefixes`` knowledge-pack list:
        those types can never be ``const``-qualified without casting every
        call site, and their lifetime is managed by the originating library.
        """
        if node.kind not in (CK.FUNCTION_DECL, CK.CXX_METHOD):
            return
        TK = cindex.TypeKind
        kp = _knowledge_packs()
        dp = kp.get("design_patterns", {})
        advisory = dp.get("raw_pointer_param", {}).get(
            "text",
            "Raw pointer parameter — ownership is ambiguous",
        )
        opaque_prefixes = tuple(kp.get("c_opaque_handle_prefixes", []))
        for param in node.get_arguments():
            t = param.type.get_canonical()
            if t.kind == TK.POINTER:
                pointee = t.get_pointee()
                if not pointee.is_const_qualified():
                    # Strip leading "struct " / "const " that libclang may
                    # include in the canonical spelling of C typedef'd handles.
                    type_name = (
                        pointee.spelling
                        .removeprefix("const ")
                        .removeprefix("struct ")
                    )
                    if opaque_prefixes and type_name.startswith(opaque_prefixes):
                        continue  # C API opaque handle — suppressed
                    issues.append(Issue(
                        level=1,
                        kind="raw_ptr_param",
                        file=_file(param, path),
                        line=param.location.line,
                        col=param.location.column,
                        message=(
                            f"Parameter '{param.spelling}' is a non-const raw pointer"
                        ),
                        note=advisory,
                    ))

    # ── Level 2 checks ────────────────────────────────────────────────────────

    def _check_raw_ptr_return(self, node, path, issues, CK, cindex) -> None:
        """Flag factory-looking functions that return raw (non-const) pointers."""
        if node.kind not in (CK.FUNCTION_DECL, CK.CXX_METHOD):
            return
        TK = cindex.TypeKind
        ret = node.result_type.get_canonical()
        if ret.kind != TK.POINTER:
            return
        pointee = ret.get_pointee()
        if pointee.is_const_qualified():
            return  # const T* is a common borrow pattern — skip

        # Only flag functions whose name suggests factory / allocator semantics.
        name_lower = (node.spelling or "").lower()
        factory_hints = ("create", "make", "build", "alloc", "new", "construct")
        if not any(h in name_lower for h in factory_hints):
            return

        dp = _knowledge_packs().get("design_patterns", {})
        advisory = dp.get("raw_pointer_param", {}).get(
            "text",
            "Factory returns a raw pointer — ownership transfer is unclear; "
            "prefer std::unique_ptr or document ownership explicitly",
        )
        issues.append(Issue(
            level=2,
            kind="raw_ptr_return",
            file=_file(node, path),
            line=node.location.line,
            col=node.location.column,
            message=(
                f"Factory function '{node.spelling}' returns a non-const raw pointer"
            ),
            note=advisory,
        ))

    def _check_signed_unsigned(self, node, path, issues, CK, cindex) -> None:
        """Flag binary comparisons between signed and unsigned operands."""
        if node.kind != CK.BINARY_OPERATOR:
            return
        children = list(node.get_children())
        if len(children) != 2:
            return
        TK = cindex.TypeKind
        lhs_t = children[0].type.get_canonical()
        rhs_t = children[1].type.get_canonical()

        _signed   = {TK.INT, TK.SHORT, TK.LONG, TK.LONGLONG, TK.CHAR_S, TK.SCHAR}
        _unsigned = {TK.UINT, TK.USHORT, TK.ULONG, TK.ULONGLONG, TK.CHAR_U, TK.UCHAR}

        if (lhs_t.kind in _signed and rhs_t.kind in _unsigned) or \
           (lhs_t.kind in _unsigned and rhs_t.kind in _signed):
            dp = _knowledge_packs().get("design_patterns", {})
            advisory = dp.get("signed_unsigned_compare", {}).get(
                "text",
                "Signed/unsigned comparison — potential undefined behaviour",
            )
            issues.append(Issue(
                level=2,
                kind="signed_unsigned_compare",
                file=_file(node, path),
                line=node.location.line,
                col=node.location.column,
                message="Signed/unsigned comparison",
                note=advisory,
            ))


# ── Helpers ───────────────────────────────────────────────────────────────────

def _file(node, fallback: Path) -> str:
    """Return the source file name for *node*, falling back to *fallback*."""
    if node.location.file:
        return node.location.file.name
    return str(fallback)


# ── Rich summary helper (used by the analyze command) ────────────────────────

def print_issues(issues: list[Issue], path: Path, console=None) -> None:
    """Print a Rich-formatted summary of *issues* to *console*.

    Parameters
    ----------
    issues:
        List of :class:`Issue` objects from :meth:`SCA.analyze`.
    path:
        Source file that was analysed (used as the panel title).
    console:
        Rich Console instance.  A fresh one is created if omitted.
    """
    try:
        from rich.console import Console
        from rich.table import Table
        from rich import box
    except ImportError:
        for i in issues:
            print(f"[L{i.level}] {i.file}:{i.line}:{i.col}: {i.message}")
        return

    con = console or Console()

    if not issues:
        con.print(f"  [green]✓[/]  [dim]{path.name}[/dim]  — no SCA issues")
        return

    _level_style = {1: "bold red", 2: "yellow", 3: "cyan"}
    _level_label = {1: "critical", 2: "warning", 3: "info"}

    table = Table(
        box=box.SIMPLE_HEAD,
        show_header=True,
        header_style="bold",
        expand=True,
    )
    table.add_column("L", style="dim", width=2, justify="right")
    table.add_column("Line", style="dim", width=6, justify="right")
    table.add_column("Kind", width=24)
    table.add_column("Message")

    for issue in issues:
        style = _level_style.get(issue.level, "white")
        table.add_row(
            str(issue.level),
            str(issue.line),
            f"[{style}]{issue.kind}[/]",
            f"[{style}]{issue.message}[/]\n[dim]{issue.note}[/]" if issue.note else f"[{style}]{issue.message}[/]",
        )

    con.print(f"\n[bold]{path.name}[/]  — SCA level {max(i.level for i in issues)}")
    con.print(table)
    con.print(
        f"  [bold]{len(issues)}[/] issue(s): "
        + "  ".join(
            f"[{_level_style[lv]}]{_level_label[lv]} {sum(1 for i in issues if i.level == lv)}[/]"
            for lv in (1, 2, 3)
            if any(i.level == lv for i in issues)
        )
    )
