"""
sdlos.features.metrics
======================
Code metrics for C++ and Python source files.

Primary backend — ``lizard`` (https://github.com/terryyin/lizard)
  Supports 27+ languages including C/C++, Python, Java, Go, Rust.
  Measures per-function and per-file:
    - NLOC           non-comment, non-blank lines of code
    - CCN            cyclomatic complexity number
    - token_count    raw lexical token count
    - parameter_count  number of declared parameters

Secondary backend — ``radon`` (Python source files only, optional)
  Provides richer Python-specific metrics:
    - Raw LOC breakdown  (total, code, comment, blank, multi-line)
    - Halstead metrics   (volume, difficulty, effort, bugs estimate)
    - Maintainability Index (MI, A–F graded)

Both backends degrade gracefully when not installed: a best-effort
line-count fallback is used so ``MetricsProcessor`` never blocks the
pipeline.

Bad-pattern integration
-----------------------
SCA issues can be forwarded to :meth:`MetricsCollector.collect` so that
the ``bad_pattern_count`` and ``security_issue_count`` fields are
populated from real findings rather than estimated from heuristics.

PostProcessor hook
------------------
``MetricsProcessor`` satisfies the ``PostProcessor`` protocol and stores
its results in ``PostProcessResult.metadata["metrics"]`` so the pipeline
and ``sdlos analyze`` can surface them without modifying the file::

    collector  = MetricsProcessor()
    result     = collector.process(Path("src/render_tree.cxx"))
    metrics    = result.metadata["metrics"]   # FileMetrics

Standalone usage::

    from sdlos.features.metrics import MetricsCollector

    mc      = MetricsCollector()
    metrics = mc.collect(Path("src/render_tree.cxx"))
    print(metrics.summary())
"""
from __future__ import annotations

import dataclasses
import re
from pathlib import Path
from typing import Optional


# ── Thresholds (used for grading and warnings) ────────────────────────────────

CCN_THRESHOLD_WARN     = 10   # above → "complex"
CCN_THRESHOLD_CRITICAL = 20   # above → "very complex"
NLOC_THRESHOLD_LONG    = 50   # above → "long function"
PARAM_THRESHOLD        = 5    # above → "over-parameterised"
FILE_NLOC_THRESHOLD    = 500  # above → "large file"


# ── FunctionMetrics ───────────────────────────────────────────────────────────

@dataclasses.dataclass
class FunctionMetrics:
    """Per-function metrics collected by lizard.

    Attributes
    ----------
    name:
        Short function name as reported by lizard.
    long_name:
        Full signature including parameter types (may be truncated).
    start_line:
        First line of the function body (1-based).
    end_line:
        Last line of the function body (1-based).
    nloc:
        Non-comment, non-blank lines inside the function body.
    cyclomatic_complexity:
        McCabe cyclomatic complexity (CCN).  Counts decision points + 1.
        Guideline: ≤5 trivial, 6–10 moderate, 11–20 complex, >20 very complex.
    token_count:
        Raw lexical token count inside the function.
    parameter_count:
        Number of declared parameters.
    """

    name:                  str
    long_name:             str
    start_line:            int
    end_line:              int
    nloc:                  int
    cyclomatic_complexity: int
    token_count:           int
    parameter_count:       int

    # ── Derived properties ─────────────────────────────────────────────────────

    @property
    def line_count(self) -> int:
        """Total lines spanned (end − start + 1), including comments/blanks."""
        return max(0, self.end_line - self.start_line + 1)

    @property
    def complexity_grade(self) -> str:
        """A / B / C / D / F grade based on CCN.

        Follows the same scale used by lizard's ``--warnings-only`` mode:
        A (≤5), B (6–10), C (11–20), D (21–30), F (>30).
        """
        cc = self.cyclomatic_complexity
        if cc <= 5:   return "A"
        if cc <= 10:  return "B"
        if cc <= 20:  return "C"
        if cc <= 30:  return "D"
        return "F"

    @property
    def is_complex(self) -> bool:
        """True when CCN exceeds the warning threshold."""
        return self.cyclomatic_complexity > CCN_THRESHOLD_WARN

    @property
    def is_very_complex(self) -> bool:
        """True when CCN exceeds the critical threshold."""
        return self.cyclomatic_complexity > CCN_THRESHOLD_CRITICAL

    @property
    def is_long(self) -> bool:
        """True when NLOC exceeds the long-function threshold."""
        return self.nloc > NLOC_THRESHOLD_LONG

    @property
    def is_over_parameterised(self) -> bool:
        """True when the function declares more parameters than the threshold."""
        return self.parameter_count > PARAM_THRESHOLD

    @property
    def bad_pattern_flags(self) -> list[str]:
        """Return a list of active bad-pattern labels for this function."""
        flags: list[str] = []
        if self.is_very_complex:
            flags.append(f"very-complex CC={self.cyclomatic_complexity}")
        elif self.is_complex:
            flags.append(f"complex CC={self.cyclomatic_complexity}")
        if self.is_long:
            flags.append(f"long NLOC={self.nloc}")
        if self.is_over_parameterised:
            flags.append(f"over-parameterised params={self.parameter_count}")
        return flags


# ── HalsteadMetrics (Python only, from radon) ─────────────────────────────────

@dataclasses.dataclass
class HalsteadMetrics:
    """Halstead software science metrics (radon, Python files only).

    Attributes
    ----------
    h1:   distinct operators
    h2:   distinct operands
    N1:   total operators
    N2:   total operands
    vocabulary:  h1 + h2
    length:      N1 + N2
    volume:      length × log2(vocabulary)
    difficulty:  (h1/2) × (N2/h2)
    effort:      difficulty × volume
    bugs:        estimated bug count (volume / 3000)
    time:        estimated coding time in seconds (effort / 18)
    """

    h1:         int
    h2:         int
    N1:         int
    N2:         int
    vocabulary: int
    length:     int
    volume:     float
    difficulty: float
    effort:     float
    bugs:       float
    time:       float


# ── RawLineMetrics ─────────────────────────────────────────────────────────────

@dataclasses.dataclass
class RawLineMetrics:
    """Raw line-count breakdown.

    For Python files these come from ``radon.raw``.
    For C++ files they are estimated from the source text.

    Attributes
    ----------
    total:    total line count (including blank lines)
    code:     non-blank, non-comment lines
    comment:  lines that are purely comments (// … or /* … */)
    blank:    blank / whitespace-only lines
    multi:    multi-line string / docstring lines (Python only; 0 for C++)
    """

    total:   int
    code:    int
    comment: int
    blank:   int
    multi:   int = 0


# ── FileMetrics ───────────────────────────────────────────────────────────────

@dataclasses.dataclass
class FileMetrics:
    """Aggregated metrics for one source file.

    Attributes
    ----------
    path:
        Absolute path to the source file.
    language:
        Language detected by lizard (e.g. ``"C++"``, ``"Python"``).
    lines:
        Raw line-count breakdown.
    nloc:
        Total non-comment, non-blank lines (sum across all functions + top-level).
    token_count:
        Total lexical token count.
    function_count:
        Number of functions / methods found.
    functions:
        Per-function detail objects.
    avg_complexity:
        Mean CCN across all functions.  0.0 when there are none.
    max_complexity:
        Maximum CCN of any single function.  0 when there are none.
    bad_pattern_count:
        Number of structural bad patterns (complex + long + over-parameterised
        functions, combined).
    security_issue_count:
        SCA level-1 issue count, if SCA results were forwarded.
    halstead:
        Halstead metrics (Python files only; None for C++).
    maintainability_index:
        Maintainability Index 0–100 (Python files only; radon).
    error:
        Non-empty when a backend could not process the file.
    """

    path:                   Path
    language:               str
    lines:                  RawLineMetrics
    nloc:                   int
    token_count:            int
    function_count:         int
    functions:              list[FunctionMetrics]
    avg_complexity:         float
    max_complexity:         int
    bad_pattern_count:      int
    security_issue_count:   int = 0
    halstead:               Optional[HalsteadMetrics] = None
    maintainability_index:  Optional[float] = None
    error:                  Optional[str] = None

    # ── Derived properties ─────────────────────────────────────────────────────

    @property
    def complex_functions(self) -> list[FunctionMetrics]:
        """Functions with CCN above the warning threshold."""
        return [f for f in self.functions if f.is_complex]

    @property
    def very_complex_functions(self) -> list[FunctionMetrics]:
        """Functions with CCN above the critical threshold."""
        return [f for f in self.functions if f.is_very_complex]

    @property
    def long_functions(self) -> list[FunctionMetrics]:
        """Functions with NLOC above the long-function threshold."""
        return [f for f in self.functions if f.is_long]

    @property
    def over_parameterised_functions(self) -> list[FunctionMetrics]:
        """Functions with more parameters than the threshold."""
        return [f for f in self.functions if f.is_over_parameterised]

    @property
    def code_density(self) -> float:
        """Fraction of total lines that are actual code (0–1)."""
        return self.lines.code / max(self.lines.total, 1)

    @property
    def comment_density(self) -> float:
        """Fraction of total lines that are comments (0–1)."""
        return self.lines.comment / max(self.lines.total, 1)

    @property
    def health_grade(self) -> str:
        """Overall A–F health grade for the file.

        Scoring (100 → grade):
          - 10 pts deducted per very-complex function
          - 5  pts deducted per complex function
          - 3  pts deducted per long function
          - 4  pts deducted per over-parameterised function
          - 8  pts deducted per security issue (SCA level-1)
          - 10 pts deducted when comment density < 5 %
          - 5  pts deducted when file NLOC > FILE_NLOC_THRESHOLD
        """
        score = 100
        score -= len(self.very_complex_functions) * 10
        score -= len(self.complex_functions) * 5
        score -= len(self.long_functions) * 3
        score -= len(self.over_parameterised_functions) * 4
        score -= self.security_issue_count * 8
        if self.comment_density < 0.05:
            score -= 10
        if self.nloc > FILE_NLOC_THRESHOLD:
            score -= 5
        score = max(0, score)

        if score >= 80: return "A"
        if score >= 60: return "B"
        if score >= 40: return "C"
        if score >= 20: return "D"
        return "F"

    @property
    def maintainability_grade(self) -> Optional[str]:
        """A–F grade derived from the radon Maintainability Index.

        Only available for Python files when ``radon`` is installed.
        radon grades: A (100–20), B (19–10), C (9–0).
        We map those to A/B/C with D/F for very low values.
        """
        mi = self.maintainability_index
        if mi is None:
            return None
        if mi >= 80: return "A"
        if mi >= 60: return "B"
        if mi >= 40: return "C"
        if mi >= 20: return "D"
        return "F"

    def summary(self) -> str:
        """Return a compact one-line summary string."""
        grade = self.health_grade
        return (
            f"{self.path.name}  [{self.language}]  "
            f"NLOC={self.nloc}  funcs={self.function_count}  "
            f"avg_cc={self.avg_complexity:.1f}  max_cc={self.max_complexity}  "
            f"grade={grade}"
        )


# ── MetricsCollector ──────────────────────────────────────────────────────────

class MetricsCollector:
    """Collect code metrics from a source file.

    Parameters
    ----------
    use_radon:
        Enable ``radon`` for Python files (Halstead, MI, raw lines).
        True by default; silently disabled when radon is not installed.
    extra_lizard_args:
        Additional arguments forwarded to :func:`lizard.analyze_file`.
        Currently unused (lizard has no per-call extra args), reserved for
        future use.
    """

    def __init__(
        self,
        use_radon: bool = True,
        extra_lizard_args: Optional[list[str]] = None,
    ) -> None:
        self.use_radon = use_radon
        self.extra_lizard_args = extra_lizard_args or []

    # ── Public API ─────────────────────────────────────────────────────────────

    def collect(
        self,
        path: Path,
        sca_issues: Optional[list] = None,
    ) -> FileMetrics:
        """Collect metrics for the file at *path*.

        Parameters
        ----------
        path:
            Source file to analyse.
        sca_issues:
            Optional pre-computed SCA findings (from
            :class:`~sdlos.features.sca.SCA`).  Level-1 issues are counted
            in ``FileMetrics.security_issue_count`` and merged into the
            health grade.

        Returns
        -------
        FileMetrics
            Fully populated metrics object.  On error the object is returned
            with ``error`` set and sensible zero-value fallbacks.
        """
        sca_issues = sca_issues or []
        security_count = sum(
            1 for i in sca_issues if getattr(i, "level", 0) == 1
        )

        # ── lizard pass ───────────────────────────────────────────────────────
        try:
            fm = self._collect_lizard(path)
        except Exception as exc:  # noqa: BLE001
            fm = self._fallback_metrics(path, str(exc))

        fm.security_issue_count = security_count
        fm.bad_pattern_count = (
            len(fm.complex_functions)
            + len(fm.long_functions)
            + len(fm.over_parameterised_functions)
        )

        # ── radon pass (Python only) ───────────────────────────────────────────
        if self.use_radon and path.suffix == ".py":
            self._enrich_with_radon(fm, path)

        return fm

    # ── lizard backend ────────────────────────────────────────────────────────

    def _collect_lizard(self, path: Path) -> FileMetrics:
        """Run ``lizard`` and return a populated :class:`FileMetrics`.

        Raises
        ------
        ImportError
            When lizard is not installed.
        Exception
            On any other lizard failure.
        """
        try:
            import lizard
        except ImportError as exc:
            raise ImportError(
                "lizard is not installed — run 'pip install lizard>=1.17'"
            ) from exc

        result = lizard.analyze_file(str(path))
        if result is None:
            raise ValueError(f"lizard returned None for {path}")

        functions: list[FunctionMetrics] = []
        for fn in (result.function_list or []):
            functions.append(FunctionMetrics(
                name=fn.name,
                long_name=getattr(fn, "long_name", fn.name),
                start_line=fn.start_line,
                end_line=fn.end_line,
                nloc=fn.nloc,
                cyclomatic_complexity=fn.cyclomatic_complexity,
                token_count=fn.token_count,
                parameter_count=len(fn.parameters),
            ))

        language = _detect_language(path, result)
        raw_lines = _count_lines_cxx(path)  # baseline; radon overwrites for .py

        avg_cc = (
            sum(f.cyclomatic_complexity for f in functions) / len(functions)
            if functions else 0.0
        )
        max_cc = max((f.cyclomatic_complexity for f in functions), default=0)

        return FileMetrics(
            path=path,
            language=language,
            lines=raw_lines,
            nloc=result.nloc,
            token_count=result.token_count,
            function_count=len(functions),
            functions=functions,
            avg_complexity=round(avg_cc, 2),
            max_complexity=max_cc,
            bad_pattern_count=0,   # populated after return
            security_issue_count=0,
        )

    # ── radon backend ─────────────────────────────────────────────────────────

    def _enrich_with_radon(self, fm: FileMetrics, path: Path) -> None:
        """Enrich *fm* with radon raw/Halstead/MI metrics (Python files only).

        Modifies *fm* in place.  Silently returns when radon is not installed
        or the file cannot be parsed.
        """
        try:
            source = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return

        # ── Raw metrics ───────────────────────────────────────────────────────
        try:
            from radon.raw import analyze as raw_analyze
            raw = raw_analyze(source)
            fm.lines = RawLineMetrics(
                total=raw.loc,
                code=raw.sloc,
                comment=raw.comments,
                blank=raw.blank,
                multi=raw.multi,
            )
        except Exception:  # noqa: BLE001
            pass  # keep the line-count fallback

        # ── Halstead metrics ──────────────────────────────────────────────────
        try:
            from radon.metrics import h_visit
            h_results = h_visit(source)
            # h_visit returns a list of HalsteadReport; aggregate the totals.
            if h_results:
                agg = h_results[0]  # module-level aggregate
                fm.halstead = HalsteadMetrics(
                    h1=agg.h1,
                    h2=agg.h2,
                    N1=agg.N1,
                    N2=agg.N2,
                    vocabulary=agg.vocabulary,
                    length=agg.length,
                    volume=agg.volume,
                    difficulty=agg.difficulty,
                    effort=agg.effort,
                    bugs=agg.bugs,
                    time=agg.time,
                )
        except Exception:  # noqa: BLE001
            pass

        # ── Maintainability Index ─────────────────────────────────────────────
        try:
            from radon.metrics import mi_visit
            mi = mi_visit(source, multi=True)
            fm.maintainability_index = round(float(mi), 2)
        except Exception:  # noqa: BLE001
            pass

    # ── Fallback (no lizard) ──────────────────────────────────────────────────

    def _fallback_metrics(self, path: Path, error_msg: str) -> FileMetrics:
        """Return a minimal :class:`FileMetrics` computed from raw line counts."""
        raw = _count_lines_cxx(path)
        return FileMetrics(
            path=path,
            language=_guess_language(path),
            lines=raw,
            nloc=raw.code,
            token_count=0,
            function_count=0,
            functions=[],
            avg_complexity=0.0,
            max_complexity=0,
            bad_pattern_count=0,
            error=error_msg,
        )


# ── MetricsProcessor ──────────────────────────────────────────────────────────

class MetricsProcessor:
    """Collect code metrics as a pipeline ``PostProcessor``.

    Does **not** modify the file.  Results are stored in
    ``PostProcessResult.metadata["metrics"]`` as a :class:`FileMetrics`
    instance.

    Parameters
    ----------
    use_radon:
        Enable radon for Python-file enrichment (Halstead, MI).
    """

    def __init__(self, use_radon: bool = True) -> None:
        self.use_radon = use_radon
        self._collector = MetricsCollector(use_radon=use_radon)

    def process(self, path: Path) -> "PostProcessResult":
        """Collect metrics for *path* and return a :class:`PostProcessResult`.

        Parameters
        ----------
        path:
            Source file to measure.

        Returns
        -------
        PostProcessResult
            ``modified=False``.  The :class:`FileMetrics` object is stored
            in ``result.metadata["metrics"]``.
        """
        from .post_process import PostProcessResult

        # Forward any SCA issues that were stashed on the path (by the
        # pipeline's share_sca_issues mechanism).
        sca_issues: list = getattr(path, "_sca_issues", [])

        try:
            metrics = self._collector.collect(path, sca_issues=sca_issues)
            return PostProcessResult(
                processor="Metrics",
                modified=False,
                error=metrics.error,
                metadata={"metrics": metrics},
            )
        except Exception as exc:  # noqa: BLE001
            return PostProcessResult(
                processor="Metrics",
                modified=False,
                error=str(exc),
            )


# ── Rich output ───────────────────────────────────────────────────────────────

def print_metrics(metrics: FileMetrics, console=None) -> None:
    """Print a Rich-formatted metrics report for *metrics*.

    Parameters
    ----------
    metrics:
        :class:`FileMetrics` produced by :class:`MetricsCollector`.
    console:
        Rich Console.  Created if omitted.
    """
    try:
        from rich.console import Console
        from rich.table import Table
        from rich.panel import Panel
        from rich import box
    except ImportError:
        _print_metrics_plain(metrics)
        return

    con = console or Console()

    if metrics.error and not metrics.function_count:
        con.print(
            f"  [yellow]⚠[/]  [dim]Metrics[/dim]  "
            f"[dim]{metrics.path.name}[/dim]  {metrics.error}"
        )
        return

    grade = metrics.health_grade
    grade_style = _grade_style(grade)

    # ── Header bar ────────────────────────────────────────────────────────────
    mi_str = (
        f"  MI={metrics.maintainability_index:.0f}"
        if metrics.maintainability_index is not None else ""
    )
    mi_grade = metrics.maintainability_grade
    mi_grade_str = (
        f" [{_grade_style(mi_grade)}]{mi_grade}[/]"
        if mi_grade else ""
    )

    header = (
        f"[bold]{metrics.path.name}[/bold]  "
        f"[dim]{metrics.language}[/dim]  "
        f"NLOC [cyan]{metrics.nloc}[/cyan]  "
        f"funcs [cyan]{metrics.function_count}[/cyan]  "
        f"avg_CC [cyan]{metrics.avg_complexity:.1f}[/cyan]  "
        f"max_CC [cyan]{metrics.max_complexity}[/cyan]"
        f"{mi_str}{mi_grade_str}  "
        f"grade [{grade_style}]{grade}[/]"
    )
    con.print(f"\n{header}")

    # ── Line counts ───────────────────────────────────────────────────────────
    l = metrics.lines
    con.print(
        f"  [dim]lines  total={l.total}  code={l.code}  "
        f"comment={l.comment} ({metrics.comment_density:.0%})  "
        f"blank={l.blank}[/dim]"
    )

    # ── Halstead (Python only) ────────────────────────────────────────────────
    if metrics.halstead:
        h = metrics.halstead
        con.print(
            f"  [dim]halstead  vol={h.volume:.0f}  diff={h.difficulty:.1f}  "
            f"effort={h.effort:.0f}  bugs≈{h.bugs:.2f}[/dim]"
        )

    # ── Function table ────────────────────────────────────────────────────────
    if metrics.functions:
        table = Table(
            box=box.SIMPLE_HEAD,
            show_header=True,
            header_style="bold dim",
            expand=True,
        )
        table.add_column("Function",   no_wrap=True)
        table.add_column("Lines",      justify="right", width=6)
        table.add_column("NLOC",       justify="right", width=6)
        table.add_column("CC",         justify="right", width=5)
        table.add_column("Grade",      justify="center", width=6)
        table.add_column("Tokens",     justify="right", width=7)
        table.add_column("Params",     justify="right", width=7)
        table.add_column("Flags",      style="dim")

        for fn in sorted(metrics.functions, key=lambda f: -f.cyclomatic_complexity):
            cc_style = (
                "bold red"   if fn.is_very_complex else
                "yellow"     if fn.is_complex      else
                "green"
            )
            g = fn.complexity_grade
            flags = ", ".join(fn.bad_pattern_flags) if fn.bad_pattern_flags else ""
            table.add_row(
                _truncate(fn.name, 40),
                str(fn.line_count),
                str(fn.nloc),
                f"[{cc_style}]{fn.cyclomatic_complexity}[/]",
                f"[{_grade_style(g)}]{g}[/]",
                str(fn.token_count),
                str(fn.parameter_count),
                f"[yellow]{flags}[/]" if flags else "",
            )

        con.print(table)

    # ── Bad-pattern summary ───────────────────────────────────────────────────
    if metrics.bad_pattern_count or metrics.security_issue_count:
        parts: list[str] = []
        if metrics.very_complex_functions:
            parts.append(
                f"[bold red]{len(metrics.very_complex_functions)} very-complex[/bold red]"
            )
        if metrics.complex_functions:
            parts.append(
                f"[yellow]{len(metrics.complex_functions)} complex[/yellow]"
            )
        if metrics.long_functions:
            parts.append(f"[yellow]{len(metrics.long_functions)} long[/yellow]")
        if metrics.over_parameterised_functions:
            parts.append(
                f"[yellow]{len(metrics.over_parameterised_functions)} over-param[/yellow]"
            )
        if metrics.security_issue_count:
            parts.append(
                f"[bold red]{metrics.security_issue_count} security[/bold red]"
            )
        con.print("  Bad patterns: " + "  ".join(parts))


def print_metrics_summary(
    all_metrics: list[FileMetrics],
    console=None,
) -> None:
    """Print an aggregated summary table across multiple files.

    Parameters
    ----------
    all_metrics:
        One :class:`FileMetrics` per analysed file.
    console:
        Rich Console.
    """
    try:
        from rich.console import Console
        from rich.table import Table
        from rich import box
    except ImportError:
        for m in all_metrics:
            print(m.summary())
        return

    con = console or Console()

    if not all_metrics:
        return

    # ── Aggregates ────────────────────────────────────────────────────────────
    total_nloc       = sum(m.nloc for m in all_metrics)
    total_funcs      = sum(m.function_count for m in all_metrics)
    total_bad        = sum(m.bad_pattern_count for m in all_metrics)
    total_security   = sum(m.security_issue_count for m in all_metrics)
    avg_cc           = (
        sum(m.avg_complexity for m in all_metrics if m.function_count) /
        max(sum(1 for m in all_metrics if m.function_count), 1)
    )
    max_cc_file      = max(all_metrics, key=lambda m: m.max_complexity)
    grade_counts: dict[str, int] = {}
    for m in all_metrics:
        grade_counts[m.health_grade] = grade_counts.get(m.health_grade, 0) + 1

    con.print(f"\n[bold]Metrics summary[/bold]  — {len(all_metrics)} file(s)")
    con.print(
        f"  NLOC [cyan]{total_nloc}[/cyan]  "
        f"functions [cyan]{total_funcs}[/cyan]  "
        f"avg_CC [cyan]{avg_cc:.1f}[/cyan]  "
        f"bad-patterns [yellow]{total_bad}[/yellow]  "
        f"security [bold red]{total_security}[/bold red]"
    )

    # Grade distribution
    grade_str = "  ".join(
        f"[{_grade_style(g)}]{g}={n}[/]"
        for g, n in sorted(grade_counts.items())
    )
    con.print(f"  Grades  {grade_str}")

    if max_cc_file.max_complexity > CCN_THRESHOLD_WARN:
        con.print(
            f"  Highest CC  [bold red]{max_cc_file.max_complexity}[/bold red]  "
            f"in [dim]{max_cc_file.path.name}[/dim]"
        )

    # ── Per-file table (sorted worst-first) ───────────────────────────────────
    table = Table(
        box=box.SIMPLE_HEAD,
        show_header=True,
        header_style="bold dim",
        expand=True,
    )
    table.add_column("File",         no_wrap=True)
    table.add_column("Lang",         width=6)
    table.add_column("NLOC",         justify="right", width=7)
    table.add_column("Funcs",        justify="right", width=6)
    table.add_column("avg CC",       justify="right", width=7)
    table.add_column("max CC",       justify="right", width=7)
    table.add_column("bad",          justify="right", width=5)
    table.add_column("sec",          justify="right", width=5)
    table.add_column("grade",        justify="center", width=6)

    sorted_metrics = sorted(
        all_metrics,
        key=lambda m: (
            -m.max_complexity,
            -m.bad_pattern_count,
            -m.security_issue_count,
        ),
    )

    for m in sorted_metrics:
        g = m.health_grade
        cc_style = "bold red" if m.max_complexity > CCN_THRESHOLD_CRITICAL else (
            "yellow" if m.max_complexity > CCN_THRESHOLD_WARN else "dim"
        )
        table.add_row(
            _truncate(m.path.name, 36),
            m.language,
            str(m.nloc),
            str(m.function_count),
            f"{m.avg_complexity:.1f}",
            f"[{cc_style}]{m.max_complexity}[/]",
            str(m.bad_pattern_count) if m.bad_pattern_count else "[dim]0[/dim]",
            (f"[bold red]{m.security_issue_count}[/bold red]"
             if m.security_issue_count else "[dim]0[/dim]"),
            f"[{_grade_style(g)}]{g}[/]",
        )

    con.print(table)


# ── Line-count helpers ────────────────────────────────────────────────────────

def _count_lines_cxx(path: Path) -> RawLineMetrics:
    """Count lines in a C/C++ source file without requiring any backend.

    This is a best-effort heuristic: it counts ``//`` and ``/* … */`` style
    comments, blank lines, and everything else as code.
    """
    try:
        source = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return RawLineMetrics(0, 0, 0, 0)

    lines = source.splitlines()
    total   = len(lines)
    blank   = 0
    comment = 0
    in_block = False

    for raw_line in lines:
        stripped = raw_line.strip()
        if not stripped:
            blank += 1
            continue
        if in_block:
            comment += 1
            if "*/" in stripped:
                in_block = False
            continue
        if stripped.startswith("//"):
            comment += 1
            continue
        if stripped.startswith("/*"):
            comment += 1
            if "*/" not in stripped[2:]:
                in_block = True
            continue

    code = total - blank - comment
    return RawLineMetrics(total=total, code=max(code, 0), comment=comment, blank=blank)


def _detect_language(path: Path, lizard_result) -> str:
    """Return the language string for the file.

    Prefers the value set by lizard; falls back to extension heuristics.
    """
    lang = getattr(lizard_result, "language", None)
    if lang:
        return str(lang)
    return _guess_language(path)


def _guess_language(path: Path) -> str:
    """Guess language from file extension."""
    ext = path.suffix.lower()
    _MAP = {
        ".cxx": "C++", ".cpp": "C++", ".cc": "C++", ".c": "C",
        ".hxx": "C++", ".hpp": "C++", ".hh": "C++", ".h": "C/C++",
        ".py":  "Python",
        ".rs":  "Rust",
        ".go":  "Go",
        ".ts":  "TypeScript", ".tsx": "TypeScript",
        ".js":  "JavaScript", ".jsx": "JavaScript",
    }
    return _MAP.get(ext, ext.lstrip(".").upper() or "Unknown")


# ── Misc helpers ──────────────────────────────────────────────────────────────

def _grade_style(grade: Optional[str]) -> str:
    """Return a Rich style string for a letter grade."""
    _STYLES = {
        "A": "bold green",
        "B": "green",
        "C": "yellow",
        "D": "bold yellow",
        "F": "bold red",
    }
    return _STYLES.get(grade or "", "white")


def _truncate(s: str, max_len: int) -> str:
    """Truncate *s* to *max_len* characters, adding '…' when truncated."""
    if len(s) <= max_len:
        return s
    return s[:max_len - 1] + "…"


def _print_metrics_plain(metrics: FileMetrics) -> None:
    """Plain-text fallback when Rich is not available."""
    print(metrics.summary())
    for fn in metrics.functions:
        flags = ", ".join(fn.bad_pattern_flags)
        flag_str = f"  [{flags}]" if flags else ""
        print(
            f"  {fn.name}  CC={fn.cyclomatic_complexity}  "
            f"NLOC={fn.nloc}  params={fn.parameter_count}{flag_str}"
        )
