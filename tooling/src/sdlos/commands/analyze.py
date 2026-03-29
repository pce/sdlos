"""
sdlos.commands.analyze
======================
``sdlos analyze <path>`` — run SCA, clang-tidy, docblocks, clang-format,
and/or code-metrics on a file or directory tree of C++ / Python source files.

Target resolution
-----------------
  sdlos analyze src/render_tree.cxx          single file
  sdlos analyze src/                         all C++ files under src/
  sdlos analyze .                            entire project tree
  sdlos analyze src/ --engine-root .         use project root for include paths

Safety model (--safe, default on when --write)
----------------------------------------------
When ``--safe`` is active every file is processed inside a transaction:

  1. Snapshot  — SHA-256 the file before touching it.
  2. Run       — execute all processors in order.
  3. Verify    — parse the result with libclang (bracket-balance fallback).
  4. Idempotency — run clang-format a second time; confirm no further changes.
  5. Revert    — if verification fails, restore the snapshot and report why.

Without ``--safe`` (or in dry-run mode) the pipeline runs but will not
automatically revert broken output.

Options
-------
  --sca LEVEL      libclang static analysis (0 = off, 1–3)
  --format         run clang-format
  --tidy           run clang-tidy
  --docblocks      insert Doxygen docblocks (AST reflection)
  --metrics        collect lizard + radon code metrics
  --write          apply modifications in place
  --safe/--no-safe enable/disable snapshot-verify-revert (default: on with --write)
  --fix-tidy       pass --fix to clang-tidy (implies --write)

Examples
--------
  sdlos analyze userspace/src/                      full tree, dry-run
  sdlos analyze src/ --format --write               reformat in place (safe)
  sdlos analyze src/ --tidy --fix-tidy --write      apply tidy fixes (safe)
  sdlos analyze src/ --sca 1 --no-tidy --metrics    SCA + metrics only
  sdlos analyze src/ --no-safe --write              write without revert guard
"""
from __future__ import annotations

import sys
from pathlib import Path
from typing import Optional

import click


# ── C++ file extensions recognised by the analyser ───────────────────────────

_CXX_EXTENSIONS = frozenset({
    ".cxx", ".cpp", ".cc", ".c",
    ".hxx", ".hpp", ".hh", ".h",
})

_PY_EXTENSIONS = frozenset({".py"})

_ALL_EXTENSIONS = _CXX_EXTENSIONS | _PY_EXTENSIONS

# Directory names to skip when walking a tree.
_SKIP_DIRS = frozenset({
    "build", ".build", "_build",
    "deps", "third_party", "vendor", "external",
    ".git", ".cache", "__pycache__",
    "CMakeFiles", ".cmake",
})


# ── File collection ───────────────────────────────────────────────────────────

def _collect_files(path: Path, include_python: bool = False) -> list[Path]:
    """Return all C++ (and optionally Python) source files under *path*.

    Parameters
    ----------
    path:
        A single file or a directory root to walk recursively.
    include_python:
        When True, ``.py`` files are included alongside C++ files.

    Returns
    -------
    list[Path]
        Absolute, sorted paths.
    """
    exts = _ALL_EXTENSIONS if include_python else _CXX_EXTENSIONS

    if path.is_file():
        return [path.resolve()]

    files: list[Path] = []
    for child in sorted(path.rglob("*")):
        if any(part in _SKIP_DIRS for part in child.parts):
            continue
        if child.is_file() and child.suffix.lower() in exts:
            files.append(child.resolve())
    return files


# ── Path resolution ───────────────────────────────────────────────────────────

def _resolve_path(given: Path) -> Optional[Path]:
    """Try to locate *given* even when the caller is in a sub-directory.

    Resolution order
    ----------------
    1. The path as-is (relative to cwd — the normal case).
    2. Each ancestor of cwd up to 6 levels: ``ancestor / given``.
       This handles the common pattern of running ``sdlos analyze src/foo.cxx``
       from ``tooling/`` where the real file lives at ``../src/foo.cxx``.
    3. Project root / given  (via :func:`~sdlos.core.cmake.find_project_root`).

    Returns ``None`` when all candidates are exhausted.
    """
    if given.is_absolute():
        return given if given.exists() else None

    # 1. Relative to cwd
    cwd = Path.cwd()
    if (cwd / given).exists():
        return (cwd / given).resolve()

    # 2. Walk up ancestors
    cur = cwd
    for _ in range(6):
        parent = cur.parent
        if parent == cur:
            break
        candidate = parent / given
        if candidate.exists():
            return candidate.resolve()
        cur = parent

    # 3. Project root
    try:
        from ..core.cmake import find_project_root
        root = find_project_root(cwd)
        candidate = root / given
        if candidate.exists():
            return candidate.resolve()
    except Exception:  # noqa: BLE001
        pass

    return None


# ── Pipeline factory ──────────────────────────────────────────────────────────

def _make_pipeline(
    *,
    sca_level:        int,
    run_format:       bool,
    run_tidy:         bool,
    run_docblocks:    bool,
    run_metrics:      bool,
    apply_tidy_fixes: bool,
    write:            bool,
    compile_commands: Optional[Path],
    include_dirs:     list[Path],
    console,
):
    """Build a :class:`~sdlos.features.post_process.PostProcessPipeline`.

    Parameters
    ----------
    sca_level:
        SCA depth (0 = disabled).
    run_format:
        Include :class:`ClangFormatProcessor`.
    run_tidy:
        Include :class:`ClangTidyProcessor`.
    run_docblocks:
        Include :class:`DocblocksProcessor`.
    run_metrics:
        Include :class:`MetricsProcessor`.
    apply_tidy_fixes:
        Forward ``--fix`` to clang-tidy.
    write:
        When False processors that would modify files run in dry-run mode.
    compile_commands:
        Path to ``compile_commands.json``.
    include_dirs:
        Extra include directories for libclang / clang-tidy.
    console:
        Rich Console for per-processor progress lines.
    """
    from ..features.post_process import (
        ClangFormatProcessor,
        ClangTidyProcessor,
        DocblocksProcessor,
        MetricsProcessor,
        PostProcessPipeline,
        SCAProcessor,
    )

    extra_args: list[str] = ["-std=c++23"]
    for d in include_dirs:
        extra_args.append(f"-I{d}")

    processors = []

    if sca_level > 0:
        processors.append(SCAProcessor(level=sca_level, extra_args=extra_args))

    if run_tidy:
        processors.append(
            ClangTidyProcessor(
                extra_args=extra_args,
                compile_commands=compile_commands,
            )
        )

    if run_format:
        processors.append(
            ClangFormatProcessor(
                style="file",
                dry_run=not write,
            )
        )

    if run_docblocks:
        processors.append(
            DocblocksProcessor(
                extra_args=extra_args,
                dry_run=not write,
            )
        )

    if run_metrics:
        processors.append(MetricsProcessor())

    return PostProcessPipeline(
        processors,
        share_sca_issues=True,
        console=console,
    )


# ── Summary printer ───────────────────────────────────────────────────────────

def _print_transaction_reports(
    reports: dict,   # Path → TransactionReport
    console,
) -> None:
    """Print a section for every file that was reverted.

    Parameters
    ----------
    reports:
        Mapping returned by :meth:`PostProcessPipeline.run_all_safe`.
    console:
        Rich Console.
    """
    if not console:
        return

    reverted = {
        p: (_, rpt)
        for p, (_, rpt) in reports.items()
        if rpt.reverted
    }
    if not reverted:
        return

    try:
        console.print(
            f"\n[bold red]↩  Reverted {len(reverted)} file(s)[/bold red]"
            "  — processors introduced syntax errors; originals restored.\n"
        )
        for path, (_, rpt) in sorted(reverted.items()):
            console.print(
                f"  [bold]{path.name}[/bold]  "
                f"[dim]{path}[/dim]"
            )
            console.print(f"    [red]Reason:[/red] {rpt.revert_reason}")
            if rpt.verification and rpt.verification.errors:
                for err in rpt.verification.errors[:5]:
                    console.print(f"    [dim red]  {err}[/dim red]")
            console.print(
                f"    [dim]before={rpt.before.sha256}  "
                f"after (reverted)={rpt.before.sha256}[/dim]"
            )
            console.print()
    except Exception:  # noqa: BLE001
        pass


def _print_idempotency_issues(
    reports: dict,
    console,
) -> None:
    """Print idempotency warnings from transaction reports.

    Parameters
    ----------
    reports:
        Mapping returned by :meth:`PostProcessPipeline.run_all_safe`.
    console:
        Rich Console.
    """
    if not console:
        return

    non_idempotent = {
        p: (_, rpt)
        for p, (_, rpt) in reports.items()
        if rpt.idempotency_ok is False
    }
    if not non_idempotent:
        return

    try:
        console.print(
            f"\n[yellow]⚠  {len(non_idempotent)} file(s) have unstable formatting[/yellow]\n"
        )
        for path, (_, rpt) in sorted(non_idempotent.items()):
            console.print(f"  [dim]{path.name}[/dim]")
            for w in rpt.idempotency_warnings:
                console.print(f"    [yellow]{w}[/yellow]")
        console.print()
    except Exception:  # noqa: BLE001
        pass


def _print_metrics_section(
    all_results: dict,   # Path → list[PostProcessResult]
    console,
) -> None:
    """Print the aggregated metrics summary from all MetricsProcessor results.

    Parameters
    ----------
    all_results:
        ``{path: [PostProcessResult, ...]}`` mapping.
    console:
        Rich Console.
    """
    if not console:
        return

    try:
        from ..features.metrics import print_metrics_summary
    except ImportError:
        return

    collected = []
    for results in all_results.values():
        for r in results:
            m = r.metadata.get("metrics")
            if m is not None:
                collected.append(m)

    if collected:
        print_metrics_summary(collected, console=console)


def _print_issues_summary(
    all_results: dict,   # Path → list[PostProcessResult]
    reports: Optional[dict],  # Path → (results, TransactionReport), or None
    write: bool,
    safe: bool,
    console,
) -> int:
    """Print per-file issues and a summary footer; return exit code.

    Parameters
    ----------
    all_results:
        ``{path: [PostProcessResult, ...]}`` mapping.
    reports:
        Transaction reports from ``run_all_safe()``, or None when not in
        safe/transactional mode.
    write:
        Whether modifications were (or would be) applied.
    safe:
        Whether transactional safety was active.
    console:
        Rich Console.

    Returns
    -------
    int
        0 when no critical issues or processor errors exist, 1 otherwise.
    """
    try:
        from rich.console import Console
    except ImportError:
        _print_issues_plain(all_results)
        return 0

    con = console or Console()

    # ── Per-file issue details ─────────────────────────────────────────────────
    for path, results in sorted(all_results.items()):
        # Determine whether this file was reverted (transaction mode).
        reverted = (
            reports is not None
            and path in reports
            and reports[path][1].reverted
        )

        has_content = any(
            r.issues or r.error or r.metadata.get("metrics")
            for r in results
        )
        if not has_content and not reverted:
            continue

        con.print(f"\n[bold]{path.name}[/bold]  [dim]{path}[/dim]")

        if reverted:
            rpt = reports[path][1]
            con.print(
                f"  [bold red]↩ reverted[/bold red]  "
                f"[dim]{rpt.revert_reason}[/dim]"
            )

        for r in results:
            if r.metadata.get("metrics"):
                # Inline metrics in the per-file block.
                m = r.metadata["metrics"]
                g = m.health_grade
                try:
                    from ..features.metrics import _grade_style, print_metrics
                    gs = _grade_style(g)
                except ImportError:
                    gs = "white"
                con.print(
                    f"  [dim]Metrics[/dim]  "
                    f"NLOC={m.nloc}  funcs={m.function_count}  "
                    f"avg_CC={m.avg_complexity:.1f}  max_CC={m.max_complexity}  "
                    f"grade=[{gs}]{g}[/{gs}]"
                )
                continue

            if r.error:
                con.print(
                    f"  [bold red]✗[/bold red]  [dim]{r.processor}[/dim]  "
                    f"[red]{r.error}[/red]"
                )
                continue

            if not r.issues:
                continue

            con.print(f"  [bold dim]{r.processor}[/bold dim]")

            for issue in r.issues:
                lvl  = getattr(issue, "level",   "?")
                line = getattr(issue, "line",    "?")
                kind = getattr(issue, "kind",    getattr(issue, "check", "?"))
                msg  = getattr(issue, "message", str(issue))
                note = getattr(issue, "note",    "")

                if isinstance(lvl, int):
                    sty   = {1: "bold red", 2: "yellow", 3: "cyan"}.get(lvl, "white")
                    label = f"L{lvl}"
                else:
                    sty   = {
                        "error": "bold red", "warning": "yellow",
                        "note": "cyan", "remark": "dim",
                    }.get(str(lvl), "white")
                    label = str(lvl)

                con.print(
                    f"    [{sty}]{label}[/]  "
                    f"[dim]:{line}[/dim]  "
                    f"[{sty}]{kind}[/]  {msg}"
                )
                if note:
                    con.print(f"         [dim]{note}[/dim]")

    # ── Metrics summary (multi-file) ───────────────────────────────────────────
    _print_metrics_section(all_results, con)

    # ── Transaction reports ────────────────────────────────────────────────────
    if reports:
        _print_transaction_reports(reports, con)
        _print_idempotency_issues(reports, con)

    # ── Aggregated footer ──────────────────────────────────────────────────────
    total_files    = len(all_results)
    total_issues   = sum(
        len(r.issues)
        for rs in all_results.values()
        for r in rs
    )
    total_errors   = sum(
        1
        for rs in all_results.values()
        for r in rs
        if r.error
    )
    total_modified = sum(
        1 for rs in all_results.values()
        if any(r.modified for r in rs)
    )
    reverted_count = sum(
        1 for _, rpt in (reports or {}).values()
        if rpt.reverted
    )
    idempotency_failures = sum(
        1 for _, rpt in (reports or {}).values()
        if rpt.idempotency_ok is False
    )
    critical_count = sum(
        1
        for rs in all_results.values()
        for r in rs
        for issue in r.issues
        if getattr(issue, "level", 0) == 1
    )

    mode_label = "[cyan]write[/cyan]" if write else "[dim]dry-run[/dim]"
    safe_label = " [dim]+safe[/dim]" if safe else ""
    con.print()
    con.print(
        f"  [bold]Files scanned[/bold]    {total_files}"
        f"   {mode_label}{safe_label}"
    )
    if total_modified:
        con.print(f"  [cyan]Modified[/cyan]         {total_modified}")
    if reverted_count:
        con.print(f"  [bold red]Reverted[/bold red]         {reverted_count}  (syntax errors introduced by processors)")
    if idempotency_failures:
        con.print(f"  [yellow]Unstable fmt[/yellow]     {idempotency_failures}  (clang-format not a fixed point)")
    if total_issues:
        crit_tag = (
            f"  [bold red]{critical_count} critical[/bold red]"
            if critical_count else ""
        )
        con.print(f"  [yellow]Issues found[/yellow]     {total_issues}{crit_tag}")
    if total_errors:
        con.print(f"  [bold red]Processor errors[/bold red] {total_errors}")
    if not (total_issues or total_errors or reverted_count):
        con.print("  [green]✓ No issues found[/green]")

    con.print()

    # Exit 1 when there are critical SCA findings, revert events, or hard errors.
    return 1 if (critical_count or total_errors or reverted_count) else 0


def _print_issues_plain(all_results: dict) -> None:
    """Minimal plain-text fallback when Rich is not available."""
    for path, results in all_results.items():
        for r in results:
            if r.error:
                print(f"ERROR [{r.processor}] {path.name}: {r.error}")
            for issue in r.issues:
                print(
                    f"  L{getattr(issue, 'level', '?')} "
                    f"{path.name}:{getattr(issue, 'line', '?')} "
                    f"{getattr(issue, 'message', str(issue))}"
                )


# ── Error helpers ─────────────────────────────────────────────────────────────

def _path_error(con: object, message: str, hint: str = "") -> None:
    """Print a Rich error panel, or fall back to plain text."""
    try:
        from rich.console import Console as _C
        from rich.panel import Panel
        from rich.text import Text

        _con: object = con if con is not None else _C(stderr=True)
        body = Text()
        body.append("✖  ", style="bold red")
        body.append(message, style="bold white")
        if hint:
            body.append(f"\n\n{hint}", style="dim white")
        _con.print(  # type: ignore[union-attr]
            Panel(body, border_style="red", expand=False, padding=(0, 2))
        )
    except Exception:  # noqa: BLE001
        click.echo(f"Error: {message}", err=True)
        if hint:
            click.echo(f"  {hint}", err=True)


# ── Click command ─────────────────────────────────────────────────────────────

@click.command("analyze")
@click.argument(
    "path",
    metavar="PATH",
    required=False,
    default=None,
    type=click.Path(path_type=Path),
)
@click.option(
    "--sca",
    "sca_level",
    type=click.IntRange(0, 3),
    default=2,
    show_default=True,
    metavar="LEVEL",
    help=(
        "SCA depth: 0=off, 1=critical (unsafe calls/raw ptrs), "
        "2=+ownership/signedness, 3=+lifetime stubs."
    ),
)
@click.option(
    "--format / --no-format",
    "run_format",
    default=False,
    help=(
        "Run clang-format.  Off by default to avoid unexpected rewrites; "
        "always combine with --write so you can review changes."
    ),
)
@click.option(
    "--tidy / --no-tidy",
    "run_tidy",
    default=True,
    show_default=True,
    help="Run clang-tidy with the sdlos check profile.",
)
@click.option(
    "--docblocks / --no-docblocks",
    "run_docblocks",
    default=True,
    show_default=True,
    help="Insert Doxygen docblocks for undocumented functions (AST reflection).",
)
@click.option(
    "--metrics / --no-metrics",
    "run_metrics",
    default=False,
    help=(
        "Collect code metrics via lizard (C++/Python) and radon (Python). "
        "Reports NLOC, cyclomatic complexity, token count, bad patterns, and "
        "an A–F health grade per file."
    ),
)
@click.option(
    "--write", "-w",
    is_flag=True,
    default=False,
    help=(
        "Apply all modifications in place.  "
        "Without this flag the pipeline runs in dry-run / report-only mode."
    ),
)
@click.option(
    "--safe / --no-safe",
    "safe",
    default=None,
    help=(
        "Enable transactional safety: snapshot → process → verify → revert on "
        "broken output.  Enabled automatically when --write is active.  "
        "Pass --no-safe to disable even when writing."
    ),
)
@click.option(
    "--fix-tidy",
    is_flag=True,
    default=False,
    help=(
        "Reserved for future use — the pure libclang lint pass does not "
        "apply fixes yet.  Currently implies --write for compatibility."
    ),
)
@click.option(
    "--compile-commands",
    "compile_commands",
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
    default=None,
    metavar="JSON",
    help=(
        "Path to compile_commands.json from CMake "
        "(-DCMAKE_EXPORT_COMPILE_COMMANDS=ON).  "
        "Enables accurate include-path resolution for clang-tidy."
    ),
)
@click.option(
    "--include-dir", "-I",
    "include_dirs",
    multiple=True,
    type=click.Path(exists=True, file_okay=False, path_type=Path),
    metavar="DIR",
    help=(
        "Extra include directory forwarded to libclang and clang-tidy.  "
        "May be specified multiple times."
    ),
)
@click.option(
    "--engine-root",
    "engine_root",
    type=click.Path(exists=True, file_okay=False, path_type=Path),
    default=None,
    metavar="DIR",
    help=(
        "Project root.  Auto-resolved from PATH when omitted.  "
        "Used to locate engine src/ include paths for libclang."
    ),
)
@click.option(
    "--quiet", "-q",
    is_flag=True,
    default=False,
    help="Suppress per-processor progress lines; only print the summary.",
)
@click.option(
    "--fail-on-issues",
    is_flag=True,
    default=False,
    help=(
        "Exit with code 1 when any level-1 SCA issue, revert event, or "
        "processor error is found.  Useful for CI gating."
    ),
)
@click.option(
    "--python / --no-python",
    "include_python",
    default=False,
    help=(
        "Include Python (.py) files in the analysis.  "
        "SCA and clang-tidy are skipped for Python files; "
        "metrics (lizard + radon) run on all supported languages."
    ),
)
@click.pass_context
def cmd_analyze(
    ctx:              click.Context,
    path:             Optional[Path],
    sca_level:        int,
    run_format:       bool,
    run_tidy:         bool,
    run_docblocks:    bool,
    run_metrics:      bool,
    write:            bool,
    safe:             Optional[bool],
    fix_tidy:         bool,
    compile_commands: Optional[Path],
    include_dirs:     tuple[Path, ...],
    engine_root:      Optional[Path],
    quiet:            bool,
    fail_on_issues:   bool,
    include_python:   bool,
) -> None:
    """Analyse C++ source files with SCA, clang-tidy, docblocks, and metrics.

    PATH may be a single file or a directory tree.  When PATH is omitted the
    command tries to resolve the engine ``src/`` directory automatically.

    \b
    Safety model
    ------------
    With --write (default) or --safe, every file is processed inside a
    transaction: a snapshot is taken before processing, the result is
    verified with libclang (or a bracket-balance fallback), and the file is
    automatically restored if the processors introduced syntax errors.

    \b
    Examples
    --------
      sdlos analyze userspace/src/                      dry-run, report only
      sdlos analyze src/render_tree.cxx                 single file
      sdlos analyze src/ --format --write               reformat in place
      sdlos analyze src/ --tidy --fix-tidy --write      apply tidy fixes
      sdlos analyze src/ --sca 1 --no-tidy --metrics    SCA + metrics
      sdlos analyze src/ --metrics --no-tidy            lizard metrics only
      sdlos analyze src/ --no-safe --write              write, skip revert guard
    """
    try:
        from rich.console import Console as _RichConsole
    except ImportError:
        _RichConsole = None  # type: ignore[misc, assignment]

    con = _RichConsole() if not quiet and _RichConsole else None

    # ── Resolve implied flags ─────────────────────────────────────────────────
    if fix_tidy:
        write = True

    # safe defaults to True whenever we are actually writing files.
    if safe is None:
        safe = write

    # ── Resolve target path ───────────────────────────────────────────────────
    if path is None:
        from ..core.cmake import find_project_root
        try:
            root = find_project_root(Path.cwd())
            path = root / "src"
            if not path.exists():
                _path_error(
                    con,
                    "Could not auto-detect src/ directory.",
                    hint="Pass a PATH explicitly:\n  sdlos analyze userspace/src/",
                )
                raise SystemExit(1)
        except SystemExit:
            raise
        except Exception:  # noqa: BLE001
            _path_error(
                con,
                "PATH is required outside a sdlos project tree.",
                hint=(
                    "sdlos analyze userspace/src/\n"
                    "  sdlos analyze src/render_tree.cxx"
                ),
            )
            raise SystemExit(1)
    else:
        resolved = _resolve_path(path)
        if resolved is None:
            _path_error(
                con,
                f"Path not found: {path}",
                hint=(
                    "Tried the given path relative to the current directory and "
                    "its ancestors.\n"
                    f"  cwd : {Path.cwd()}\n"
                    f"  Try : {Path.cwd().parent / path}"
                ),
            )
            raise SystemExit(1)
        path = resolved

    # ── Collect files ─────────────────────────────────────────────────────────
    files = _collect_files(path, include_python=include_python)
    if not files:
        if con:
            con.print(f"[yellow]No source files found under[/yellow] {path}")
        else:
            click.echo(f"No source files found under {path}")
        return

    # ── Engine include dirs ───────────────────────────────────────────────────
    resolved_include_dirs = list(include_dirs)

    if engine_root is None:
        try:
            from ..core.cmake import find_project_root
            engine_root = find_project_root(
                path if path.is_dir() else path.parent
            )
        except Exception:  # noqa: BLE001
            engine_root = None

    if engine_root is not None:
        for candidate in (
            engine_root / "src",
            engine_root / "libs",
            engine_root / "deps" / "SDL3" / "include",
        ):
            if candidate.exists() and candidate not in resolved_include_dirs:
                resolved_include_dirs.append(candidate)

    # ── Print banner ──────────────────────────────────────────────────────────
    if con:
        from ..core.console import print_banner

        parts = []
        if sca_level:      parts.append(f"SCA L{sca_level}")
        if run_tidy:       parts.append("tidy")
        if run_format:     parts.append("format")
        if run_docblocks:  parts.append("docblocks")
        if run_metrics:    parts.append("metrics")

        mode_note = "write" + (" +safe" if safe else "") if write else "dry-run"
        subtitle  = "  ".join(parts) + f"  — {len(files)} file(s)  [{mode_note}]"
        print_banner("sdlos analyze", subtitle=subtitle.strip())

        if not write:
            con.print(
                "  [dim]Dry-run mode — pass [bold]--write[/bold] to apply changes."
                "  Pass [bold]--no-safe[/bold] to disable revert guard.[/dim]\n"
            )
        elif safe:
            con.print(
                "  [dim]Safe mode — files are snapshotted and automatically "
                "reverted if processors introduce syntax errors.[/dim]\n"
            )

    # ── Build pipeline ────────────────────────────────────────────────────────
    pipeline = _make_pipeline(
        sca_level=sca_level,
        run_format=run_format,
        run_tidy=run_tidy,
        run_docblocks=run_docblocks,
        run_metrics=run_metrics,
        apply_tidy_fixes=fix_tidy,
        write=write,
        compile_commands=compile_commands,
        include_dirs=resolved_include_dirs,
        console=con if not quiet else None,
    )

    if not pipeline.processors:
        raise click.UsageError(
            "No processors selected.  Enable at least one of:\n"
            "  --sca, --tidy, --format, --docblocks, --metrics"
        )

    # ── Run ───────────────────────────────────────────────────────────────────
    # Choose transactional vs. standard mode.
    reports: Optional[dict] = None

    if safe and write:
        # Transactional: snapshot → run → verify → revert on failure.
        safe_results = pipeline.run_all_safe(
            files,
            verify=True,
            idempotency_check=run_format,
        )
        # Unpack into the all_results shape the summary printer expects.
        all_results = {p: rs for p, (rs, _) in safe_results.items()}
        reports     = safe_results
    else:
        # Non-transactional: standard run (dry-run or explicitly --no-safe).
        all_results = pipeline.run_all(files)

    # ── Summary ───────────────────────────────────────────────────────────────
    exit_code = _print_issues_summary(
        all_results,
        reports=reports,
        write=write,
        safe=safe,
        console=con,
    )

    if fail_on_issues and exit_code != 0:
        sys.exit(exit_code)
