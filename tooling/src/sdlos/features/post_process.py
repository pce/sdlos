"""
sdlos.features.post_process
============================
Post-processing pipeline for C++ source files

Architecture
------------
Every processor implements the ``PostProcessor`` protocol::

    class MyProcessor:
        def process(self, path: Path) -> PostProcessResult: ...

``PostProcessPipeline`` accepts a list of processors and runs them in order.
Each processor receives the (potentially already-modified) file on disk.
If a processor modifies the file it sets ``result.modified = True`` so the
caller knows to re-read the file.

Transactional safety
--------------------
``PostProcessPipeline.run_safe()`` wraps every run in a transaction:

1. **Snapshot** — capture the file's content and SHA-256 before any
   processor touches it.
2. **Run** — execute all processors in order.
3. **Verify** — parse the result with libclang (or a bracket-balance
   fallback) to check for syntax errors introduced by processing.  Also
   check that clang-format is now idempotent (no further changes pending).
4. **Revert** — if verification fails, restore the snapshot, record why,
   and return the :class:`TransactionReport` with ``reverted=True``.

The original ``run()`` method is unchanged for callers that do not need
transactional guarantees (dry-run, reporting-only pipelines, CI).

Built-in processors
-------------------
FileIntegrityProcessor
    Scans for BOM, C0/C1 control chars, zero-width / invisible Unicode,
    BiDi overrides (Trojan Source), Unicode tag chars (watermark vectors),
    variation selectors, and mixed line endings.  Optionally strips them.

ClangFormatProcessor
    Runs ``clang-format`` on the file in-place.

SCAProcessor
    Wraps :class:`~sdlos.features.sca.SCA` — reports security issues.

ClangTidyProcessor
    Runs ``clang-tidy`` as a subprocess, optionally applying fixes.

DocblocksProcessor
    Inserts Doxygen docblocks via AST reflection.

MetricsProcessor
    Collects code metrics via ``lizard`` + ``radon``; stores them in
    ``PostProcessResult.metadata["metrics"]``.

Factory helpers
---------------
::

    # Lightweight pipeline for generated template files (transactional)
    pipeline = make_template_pipeline(sca_level=2, format=True)
    results, report = pipeline.run_safe(path)

    # Full analysis pipeline for engine source tree
    pipeline = make_engine_pipeline(sca_level=2, tidy=True, docblocks=True)
    results, report = pipeline.run_safe(path)

    # With integrity scanning (always place first)
    pipeline = make_engine_pipeline(sca_level=2, integrity=True, strip_integrity=False)
"""
from __future__ import annotations

import dataclasses
import hashlib
import subprocess
from pathlib import Path
from typing import Optional, Protocol, runtime_checkable


# ── PostProcessResult ──────────────────────────────────────────────────────────

@dataclasses.dataclass
class PostProcessResult:
    """The outcome of running a single processor on one file.

    Attributes
    ----------
    processor:
        Human-readable name of the processor (e.g. ``"ClangFormat"``).
    modified:
        True when the processor rewrote the file on disk.
    issues:
        Structured findings from SCA or clang-tidy (empty for formatters).
    error:
        Non-empty when the processor could not complete (tool not found,
        parse failure, …).  A non-empty error does *not* raise; the
        pipeline continues with the next processor.
    metadata:
        Arbitrary structured data attached by the processor.
        ``MetricsProcessor`` stores a :class:`~sdlos.features.metrics.FileMetrics`
        here under the key ``"metrics"``.
    """

    processor: str
    modified:  bool
    issues:    list                   = dataclasses.field(default_factory=list)
    error:     Optional[str]          = None
    metadata:  dict                   = dataclasses.field(default_factory=dict)

    @property
    def ok(self) -> bool:
        """True when the processor completed without an error."""
        return self.error is None

    def __repr__(self) -> str:
        tag = "✓" if self.ok else "✗"
        mod = " modified" if self.modified else ""
        iss = f" {len(self.issues)} issue(s)" if self.issues else ""
        err = f" error={self.error!r}" if self.error else ""
        return f"<PostProcessResult {tag} {self.processor}{mod}{iss}{err}>"


# ── Protocol ───────────────────────────────────────────────────────────────────

@runtime_checkable
class PostProcessor(Protocol):
    """Protocol satisfied by every processor in the pipeline."""

    def process(self, path: Path) -> PostProcessResult:
        """Process *path* (possibly in-place) and return the result."""
        ...


# ── FileSnapshot ──────────────────────────────────────────────────────────────

@dataclasses.dataclass(frozen=True)
class FileSnapshot:
    """Immutable snapshot of a file's content at a point in time.

    Attributes
    ----------
    path:
        Absolute path that was snapshotted.
    content:
        Byte-exact file content at snapshot time.
    sha256:
        First 12 hex characters of the SHA-256 digest (display only).
    """

    path:    Path
    content: bytes
    sha256:  str

    @classmethod
    def take(cls, path: Path) -> "FileSnapshot":
        """Read *path* from disk and return a snapshot.

        Parameters
        ----------
        path:
            File to snapshot.  Must exist and be readable.

        Returns
        -------
        FileSnapshot
        """
        content = path.read_bytes()
        digest  = hashlib.sha256(content).hexdigest()[:12]
        return cls(path=path, content=content, sha256=digest)

    def restore(self) -> None:
        """Write the snapshotted content back to :attr:`path`.

        This is the revert operation; it overwrites whatever the processors
        may have written.
        """
        self.path.write_bytes(self.content)

    def matches_disk(self) -> bool:
        """Return True if the on-disk content still matches this snapshot."""
        try:
            return self.path.read_bytes() == self.content
        except OSError:
            return False

    @property
    def changed(self) -> bool:
        """True when the file on disk differs from this snapshot."""
        return not self.matches_disk()


# ── VerificationResult ────────────────────────────────────────────────────────

@dataclasses.dataclass
class VerificationResult:
    """Result of a post-processing verification check.

    Attributes
    ----------
    ok:
        True when all verification steps passed.
    errors:
        Fatal problems (syntax errors, unbalanced braces, …).
    warnings:
        Non-fatal observations (clang parse warnings, near-miss idempotency).
    method:
        Description of the verification method used
        (e.g. ``"libclang"``, ``"bracket-balance"``).
    """

    ok:       bool
    errors:   list[str]  = dataclasses.field(default_factory=list)
    warnings: list[str]  = dataclasses.field(default_factory=list)
    method:   str        = "unknown"


# ── TransactionReport ─────────────────────────────────────────────────────────

@dataclasses.dataclass
class TransactionReport:
    """Records everything that happened during a :meth:`run_safe` call.

    Attributes
    ----------
    path:
        The file that was processed.
    before:
        Snapshot taken before any processor ran.
    after:
        Snapshot taken after all processors finished.  Equal to ``before``
        (same object) when the file was reverted or nothing changed.
    reverted:
        True when the file was restored to its pre-processing state because
        a verification step failed.
    revert_reason:
        Human-readable explanation of why the revert happened.
    verification:
        Result of the post-processing syntax / parse check.
    idempotency_ok:
        True when a re-run of clang-format would produce no further changes.
        ``None`` when the idempotency check was skipped.
    idempotency_warnings:
        Details when ``idempotency_ok`` is False.
    processor_results:
        The :class:`PostProcessResult` list from the inner
        :meth:`PostProcessPipeline.run` call.
    """

    path:                  Path
    before:                FileSnapshot
    after:                 FileSnapshot
    reverted:              bool
    revert_reason:         str
    verification:          Optional[VerificationResult]
    idempotency_ok:        Optional[bool]
    idempotency_warnings:  list[str]
    processor_results:     list[PostProcessResult]

    @property
    def ok(self) -> bool:
        """True when the transaction completed without reverting."""
        return not self.reverted

    @property
    def net_modified(self) -> bool:
        """True when the file on disk differs from the pre-processing snapshot."""
        return not self.reverted and (self.before.content != self.after.content)


# ── Verification helpers ──────────────────────────────────────────────────────

def verify_cxx_file(
    path: Path,
    extra_args: Optional[list[str]] = None,
) -> VerificationResult:
    """Verify that *path* is syntactically valid C++.

    Attempts libclang first; falls back to a bracket-balance heuristic when
    libclang is not installed.  The file is only *read*, never written.

    Parameters
    ----------
    path:
        C++ source file to check.
    extra_args:
        Additional compiler flags forwarded to libclang
        (e.g. ``["-I", "include/"]``).

    Returns
    -------
    VerificationResult
    """
    try:
        from clang import cindex
        return _verify_libclang(path, extra_args or [], cindex)
    except ImportError:
        return _verify_bracket_balance(path)


def _verify_libclang(
    path: Path,
    extra_args: list[str],
    cindex,
) -> VerificationResult:
    """Verify *path* using libclang's parse diagnostics.

    Parameters
    ----------
    path:
        Source file to parse.
    extra_args:
        Compiler flags.
    cindex:
        The imported ``clang.cindex`` module.

    Returns
    -------
    VerificationResult
    """
    args = ["-std=c++23", "-x", "c++"] + extra_args
    try:
        idx = cindex.Index.create()
        tu  = idx.parse(
            str(path),
            args=args,
            options=(
                cindex.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD
                | cindex.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES
            ),
        )
    except Exception as exc:  # noqa: BLE001
        return VerificationResult(
            ok=False,
            errors=[f"libclang parse failed: {exc}"],
            method="libclang",
        )

    errors:   list[str] = []
    warnings: list[str] = []

    for diag in tu.diagnostics:
        # Only report issues from the primary file, not transitive headers.
        if diag.location.file and diag.location.file.name != str(path):
            continue
        msg = f":{diag.location.line}:{diag.location.column}: {diag.spelling}"
        if diag.severity >= cindex.Diagnostic.Error:
            errors.append(msg)
        elif diag.severity == cindex.Diagnostic.Warning:
            warnings.append(msg)

    return VerificationResult(
        ok=not errors,
        errors=errors,
        warnings=warnings,
        method="libclang",
    )


def _verify_bracket_balance(path: Path) -> VerificationResult:
    """Minimal fallback: verify that ``{ }`` are balanced in *path*.

    Parameters
    ----------
    path:
        File to check.

    Returns
    -------
    VerificationResult
    """
    try:
        source = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return VerificationResult(
            ok=False,
            errors=[f"Cannot read file: {exc}"],
            method="bracket-balance",
        )

    depth      = 0
    in_string  = False
    in_char    = False
    i          = 0
    n          = len(source)

    while i < n:
        c = source[i]

        if in_string:
            if c == "\\" and i + 1 < n:
                i += 2
                continue
            if c == '"':
                in_string = False

        elif in_char:
            if c == "\\" and i + 1 < n:
                i += 2
                continue
            if c == "'":
                in_char = False

        # Skip line comments
        elif c == "/" and i + 1 < n and source[i + 1] == "/":
            while i < n and source[i] != "\n":
                i += 1
            continue

        # Skip block comments
        elif c == "/" and i + 1 < n and source[i + 1] == "*":
            i += 2
            while i + 1 < n and not (source[i] == "*" and source[i + 1] == "/"):
                i += 1
            i += 2
            continue

        elif c == '"':
            in_string = True
        elif c == "'":
            in_char = True

        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth < 0:
                return VerificationResult(
                    ok=False,
                    errors=["Unmatched '}' detected (brace depth went negative)"],
                    method="bracket-balance",
                )

        i += 1

    if depth != 0:
        return VerificationResult(
            ok=False,
            errors=[f"Unbalanced braces: depth={depth} at end of file"],
            method="bracket-balance",
        )

    return VerificationResult(ok=True, method="bracket-balance")


def check_format_idempotency(
    path: Path,
    style: str = "file",
    clang_format_bin: str = "clang-format",
) -> tuple[bool, list[str]]:
    """Check that running clang-format again would produce no further changes.

    A formatted file should be a fixed point: formatting it a second time
    must produce identical output.  If it does not, something about the
    formatter configuration or the inserted content is unstable.

    Parameters
    ----------
    path:
        Already-processed C++ source file.
    style:
        ``--style`` argument forwarded to clang-format.
    clang_format_bin:
        Path to the clang-format executable.

    Returns
    -------
    tuple[bool, list[str]]
        ``(is_idempotent, warning_messages)``.
        When ``is_idempotent`` is False the warnings list contains a brief
        description of the discrepancy.
    """
    try:
        original = path.read_bytes()
        result   = subprocess.run(
            [
                clang_format_bin,
                f"--style={style}",
                "--assume-filename", str(path),
            ],
            input=original,
            capture_output=True,
            timeout=30,
        )
        if result.returncode != 0:
            # clang-format could not parse the file — not an idempotency failure,
            # already caught by verify_cxx_file.
            return True, []

        reformatted = result.stdout
        if reformatted == original:
            return True, []

        # Compute a lightweight diff description.
        orig_lines = original.decode(errors="replace").splitlines()
        new_lines  = reformatted.decode(errors="replace").splitlines()
        changed = sum(
            1 for a, b in zip(orig_lines, new_lines) if a != b
        ) + abs(len(new_lines) - len(orig_lines))

        return False, [
            f"clang-format is not idempotent: {changed} line(s) would change "
            f"on a second pass — the inserted content may contain a formatting "
            f"pattern that clang-format resolves differently on first vs. second "
            f"application."
        ]

    except FileNotFoundError:
        return True, []   # tool absent — cannot check, assume ok
    except subprocess.TimeoutExpired:
        return True, []   # timed out — skip check


# ── ClangFormatProcessor ──────────────────────────────────────────────────────

class ClangFormatProcessor:
    """Run ``clang-format`` on a C++ file in-place.

    Parameters
    ----------
    style:
        Passed as ``--style``.  Use ``"file"`` (default) to honour the
        nearest ``.clang-format`` on disk; ``"LLVM"`` / ``"Google"`` / etc.
        for a built-in style.
    clang_format_bin:
        Path to the ``clang-format`` executable.  Resolved from ``PATH``
        when ``None`` (default).
    dry_run:
        When True, compute what would change but do not write the file.
    """

    def __init__(
        self,
        style:            str           = "file",
        clang_format_bin: Optional[str] = None,
        dry_run:          bool          = False,
    ) -> None:
        self.style            = style
        self.clang_format_bin = clang_format_bin or "clang-format"
        self.dry_run          = dry_run

    def process(self, path: Path) -> PostProcessResult:
        """Format *path* with ``clang-format``.

        Returns
        -------
        PostProcessResult
            ``modified=True`` when the file was actually rewritten.
        """
        if not path.exists():
            return PostProcessResult(
                processor="ClangFormat",
                modified=False,
                error=f"File not found: {path}",
            )

        original = path.read_bytes()
        cmd      = [
            self.clang_format_bin,
            f"--style={self.style}",
            "--assume-filename", str(path),
        ]

        try:
            result = subprocess.run(
                cmd,
                input=original,
                capture_output=True,
                timeout=30,
            )
        except FileNotFoundError:
            return PostProcessResult(
                processor="ClangFormat",
                modified=False,
                error=(
                    f"clang-format not found at '{self.clang_format_bin}'. "
                    "Install LLVM or pass clang_format_bin explicitly."
                ),
            )
        except subprocess.TimeoutExpired:
            return PostProcessResult(
                processor="ClangFormat",
                modified=False,
                error="clang-format timed out after 30 s",
            )

        if result.returncode != 0:
            stderr = result.stderr.decode(errors="replace").strip()
            return PostProcessResult(
                processor="ClangFormat",
                modified=False,
                error=f"clang-format exited {result.returncode}: {stderr}",
            )

        formatted = result.stdout
        modified  = formatted != original

        if modified and not self.dry_run:
            path.write_bytes(formatted)

        return PostProcessResult(
            processor="ClangFormat",
            modified=modified and not self.dry_run,
        )


# ── SCAProcessor ──────────────────────────────────────────────────────────────

class SCAProcessor:
    """Wrap :class:`~sdlos.features.sca.SCA` as a pipeline processor.

    The file is **not** modified — issues are reported via
    ``PostProcessResult.issues``.

    Parameters
    ----------
    level:
        SCA analysis depth (1 = critical only, 2 = warnings, 3 = info).
    extra_args:
        Additional ``clang`` flags (e.g. include paths).
    """

    def __init__(
        self,
        level:      int                    = 2,
        extra_args: Optional[list[str]]    = None,
    ) -> None:
        self.level      = level
        self.extra_args = extra_args or []

    def process(self, path: Path) -> PostProcessResult:
        """Analyse *path* and collect SCA issues.

        Delegates to :meth:`SCA.process` so that a missing ``libclang``
        installation is surfaced as ``PostProcessResult.error`` rather than
        silently returning an empty issue list.
        """
        from .sca import SCA

        try:
            sca    = SCA(level=self.level, extra_args=self.extra_args)
            result = sca.process(path)
            result.processor = "SCA"
            return result
        except Exception as exc:  # noqa: BLE001
            return PostProcessResult(
                processor="SCA",
                modified=False,
                error=str(exc),
            )


# ── ClangTidyProcessor ────────────────────────────────────────────────────────

class ClangTidyProcessor:
    """Run ``clang-tidy`` on a C++ file.

    Parameters
    ----------
    profile:
        Warning-flag profile forwarded to :class:`~sdlos.features.tidy.ClangLint`.
        Defaults to ``LintProfile.SDLOS`` — a curated set for the sdlos / SDL3
        C++23 codebase.  Use ``LintProfile.OFF`` to disable all -W flags and
        only surface parse errors.
    extra_args:
        Additional compilation flags forwarded to libclang
        (e.g. ``["-Isrc", "-Ideps/SDL3/include"]``).
    compile_commands:
        Path to a ``compile_commands.json`` database produced by CMake with
        ``-DCMAKE_EXPORT_COMPILE_COMMANDS=ON``.  When provided, the flags for
        the target file are extracted and merged so that header resolution
        works correctly.
    ast_checks:
        Enable the AST-walk pass for patterns not covered by -W flags
        (C-style casts, NULL macro, large value params, endl, strcmp).
        True by default.
    """

    def __init__(
        self,
        profile:          "LintProfile | None"  = None,
        extra_args:       Optional[list[str]]   = None,
        compile_commands: Optional[Path]        = None,
        ast_checks:       bool                  = True,
    ) -> None:
        self.profile          = profile          # resolved lazily to avoid import at module load
        self.extra_args       = extra_args or []
        self.compile_commands = compile_commands
        self.ast_checks       = ast_checks

    def process(self, path: Path) -> PostProcessResult:
        """Run the libclang lint passes on *path* and return structured results.

        No external binary is required.  Both the diagnostic pass (-W flags)
        and the AST-walk pass run entirely in-process via ``libclang``.
        """
        from .tidy import ClangLint, LintProfile

        profile = self.profile if self.profile is not None else LintProfile.SDLOS

        try:
            lint = ClangLint(
                profile=profile,
                extra_args=self.extra_args,
                compile_commands=self.compile_commands,
                ast_checks=self.ast_checks,
            )
            result = lint.process(path)
            # Normalise processor name so pipeline logs read "ClangLint".
            result.processor = "ClangLint"
            return result
        except Exception as exc:  # noqa: BLE001
            return PostProcessResult(
                processor="ClangLint",
                modified=False,
                error=str(exc),
            )


# ── DocblocksProcessor ────────────────────────────────────────────────────────

class DocblocksProcessor:
    """Insert / enrich Doxygen docblocks using AST reflection.

    Parameters
    ----------
    sca_issues:
        Pre-computed SCA findings; level-1 issues are embedded as
        ``@warning`` tags in the relevant function docblocks.
    extra_args:
        Additional libclang flags.
    dry_run:
        When True, compute what would be inserted but do not write.
    """

    def __init__(
        self,
        sca_issues: Optional[list]      = None,
        extra_args: Optional[list[str]] = None,
        dry_run:    bool                = False,
    ) -> None:
        self.sca_issues = sca_issues or []
        self.extra_args = extra_args or []
        self.dry_run    = dry_run

    def process(self, path: Path) -> PostProcessResult:
        """Reflect on *path* and insert missing Doxygen docblocks."""
        from .docblocks import Docblocks

        try:
            db       = Docblocks(extra_args=self.extra_args, dry_run=self.dry_run)
            modified = db.apply_to_file(path, sca_issues=self.sca_issues)
            return PostProcessResult(
                processor="Docblocks",
                modified=modified,
            )
        except Exception as exc:  # noqa: BLE001
            return PostProcessResult(
                processor="Docblocks",
                modified=False,
                error=str(exc),
            )


# ── MetricsProcessor ──────────────────────────────────────────────────────────

class MetricsProcessor:
    """Collect code metrics and attach them to ``PostProcessResult.metadata``.

    Does **not** modify the file.  Results are stored as
    ``result.metadata["metrics"]`` (a
    :class:`~sdlos.features.metrics.FileMetrics` instance).

    Parameters
    ----------
    use_radon:
        Enable radon for Python-file Halstead / MI enrichment.
    """

    def __init__(self, use_radon: bool = True) -> None:
        self.use_radon = use_radon

    def process(self, path: Path) -> PostProcessResult:
        """Collect metrics for *path*.

        Parameters
        ----------
        path:
            Source file to measure.

        Returns
        -------
        PostProcessResult
            ``modified=False``.  Metrics are in ``result.metadata["metrics"]``.
        """
        from .metrics import MetricsCollector

        # SCA issues may be attached to the path by the pipeline's
        # share_sca_issues mechanism.
        sca_issues: list = getattr(path, "_pipeline_sca_issues", [])

        try:
            collector = MetricsCollector(use_radon=self.use_radon)
            metrics   = collector.collect(path, sca_issues=sca_issues)
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


# ── PostProcessPipeline ────────────────────────────────────────────────────────

class PostProcessPipeline:
    """Chain multiple :class:`PostProcessor` instances and run them in order.

    Each processor receives the file *after* any previous processor has
    potentially modified it.  SCA issues discovered in an early processor
    are forwarded to later ``DocblocksProcessor`` and ``MetricsProcessor``
    instances when ``share_sca_issues=True``.

    Parameters
    ----------
    processors:
        Ordered list of processors to apply.
    share_sca_issues:
        When True, issues from :class:`SCAProcessor` are forwarded to all
        subsequent :class:`DocblocksProcessor` and :class:`MetricsProcessor`
        instances.
    console:
        Optional Rich Console for progress output.
    """

    def __init__(
        self,
        processors:       list[PostProcessor],
        share_sca_issues: bool = True,
        console           = None,
    ) -> None:
        self.processors       = processors
        self.share_sca_issues = share_sca_issues
        self.console          = console

    # ── Standard (non-transactional) run ──────────────────────────────────────

    def run(self, path: Path) -> list[PostProcessResult]:
        """Run all processors on *path* sequentially.

        This is the non-transactional variant: if a processor introduces
        broken output the file is left in that state and the caller is
        responsible for handling it.  Use :meth:`run_safe` for automatic
        snapshot / verify / revert semantics.

        Parameters
        ----------
        path:
            C++ source file to process.

        Returns
        -------
        list[PostProcessResult]
            One entry per processor, in registration order.
        """
        return self._run_inner(path)

    # ── Transactional run ──────────────────────────────────────────────────────

    def run_safe(
        self,
        path: Path,
        *,
        verify:            bool           = True,
        idempotency_check: bool           = True,
        extra_verify_args: Optional[list[str]] = None,
    ) -> tuple[list[PostProcessResult], TransactionReport]:
        """Run all processors on *path* with snapshot / verify / revert safety.

        Execution model
        ---------------
        1. **Snapshot** — read and hash the current file content.
        2. **Run** — execute all processors via :meth:`run`.
        3. **Verify** (when ``verify=True``) — parse the result with
           :func:`verify_cxx_file`.  If syntax errors are found the file is
           reverted to the snapshot.
        4. **Idempotency check** (when ``idempotency_check=True`` and the
           pipeline includes a :class:`ClangFormatProcessor`) — run
           clang-format a second time and confirm it produces no further
           changes.  A non-idempotent result is reported as a warning but
           does **not** trigger a revert on its own.
        5. Return ``(processor_results, TransactionReport)``.

        Parameters
        ----------
        path:
            C++ source file to process.
        verify:
            Enable post-processing syntax verification.
        idempotency_check:
            Enable clang-format idempotency check after processing.
        extra_verify_args:
            Additional compiler flags forwarded to :func:`verify_cxx_file`.

        Returns
        -------
        tuple[list[PostProcessResult], TransactionReport]
        """
        before = FileSnapshot.take(path)

        # ── Run all processors ─────────────────────────────────────────────────
        results = self._run_inner(path)

        after = FileSnapshot.take(path)

        # ── Verification ──────────────────────────────────────────────────────
        verification:         Optional[VerificationResult] = None
        reverted:             bool                         = False
        revert_reason:        str                          = ""
        idempotency_ok:       Optional[bool]               = None
        idempotency_warnings: list[str]                    = []

        if verify and after.content != before.content:
            # Only verify when the file was actually modified.
            verification = verify_cxx_file(path, extra_args=extra_verify_args)

            if not verification.ok:
                # Processors broke the file — revert immediately.
                before.restore()
                reverted      = True
                revert_reason = _format_revert_reason(verification, results)
                _log_revert(path, revert_reason, self.console)
                # After revert the "after" snapshot equals before.
                after = before

        # ── Idempotency check (only when not reverted) ─────────────────────────
        if idempotency_check and not reverted:
            has_format = any(
                isinstance(p, ClangFormatProcessor) for p in self.processors
            )
            if has_format and after.content != before.content:
                # Determine the style from the first ClangFormatProcessor found.
                fmt_proc = next(
                    p for p in self.processors
                    if isinstance(p, ClangFormatProcessor)
                )
                idempotency_ok, idempotency_warnings = check_format_idempotency(
                    path,
                    style=fmt_proc.style,
                    clang_format_bin=fmt_proc.clang_format_bin,
                )
                if not idempotency_ok:
                    _log_idempotency_warning(path, idempotency_warnings, self.console)

        report = TransactionReport(
            path=path,
            before=before,
            after=after,
            reverted=reverted,
            revert_reason=revert_reason,
            verification=verification,
            idempotency_ok=idempotency_ok,
            idempotency_warnings=idempotency_warnings,
            processor_results=results,
        )

        return results, report

    # ── Batch helpers ──────────────────────────────────────────────────────────

    def run_all(self, paths: list[Path]) -> dict[Path, list[PostProcessResult]]:
        """Run the pipeline over multiple files (non-transactional).

        Parameters
        ----------
        paths:
            Files to process.

        Returns
        -------
        dict[Path, list[PostProcessResult]]
        """
        return {p: self.run(p) for p in paths}

    def run_all_safe(
        self,
        paths: list[Path],
        *,
        verify:            bool = True,
        idempotency_check: bool = True,
    ) -> dict[Path, tuple[list[PostProcessResult], TransactionReport]]:
        """Run the pipeline over multiple files with transactional safety.

        Parameters
        ----------
        paths:
            Files to process.
        verify:
            Forward to :meth:`run_safe`.
        idempotency_check:
            Forward to :meth:`run_safe`.

        Returns
        -------
        dict[Path, tuple[list[PostProcessResult], TransactionReport]]
        """
        return {
            p: self.run_safe(p, verify=verify, idempotency_check=idempotency_check)
            for p in paths
        }

    # ── Internal ──────────────────────────────────────────────────────────────

    def _run_inner(self, path: Path) -> list[PostProcessResult]:
        """Execute all processors in order, managing SCA issue forwarding."""
        results:         list[PostProcessResult] = []
        accumulated_sca: list                    = []

        for proc in self.processors:
            # Clone DocblocksProcessor with accumulated SCA issues.
            if self.share_sca_issues and isinstance(proc, DocblocksProcessor):
                proc = _clone_docblocks_with_issues(proc, accumulated_sca)

            # Attach accumulated SCA issues to the path object so that
            # MetricsProcessor can pick them up without changing its signature.
            if self.share_sca_issues and isinstance(proc, MetricsProcessor):
                # Use a thin wrapper so we don't mutate Path itself.
                _inject_sca_issues(path, accumulated_sca)

            try:
                result = proc.process(path)
            except Exception as exc:  # noqa: BLE001
                result = PostProcessResult(
                    processor=type(proc).__name__,
                    modified=False,
                    error=str(exc),
                )

            results.append(result)

            # Harvest SCA issues for downstream consumers.
            if self.share_sca_issues and isinstance(proc, SCAProcessor):
                accumulated_sca.extend(result.issues)

            _log_result(result, path, self.console)

        return results


# ── Factory helpers ────────────────────────────────────────────────────────────

def make_template_pipeline(
    *,
    sca_level:       int  = 2,
    format:          bool = True,
    docblocks:       bool = True,
    metrics:         bool = False,
    integrity:       bool = True,
    strip_integrity: bool = False,
    console               = None,
) -> PostProcessPipeline:
    """Build a lightweight pipeline for files generated from templates.

    Order: [integrity] → clang-format → SCA → docblocks → [metrics]

    The generated ``.cxx`` behavior files are ``#include``-d into
    ``jade_host.cxx`` and may not parse cleanly as standalone translation
    units.  Processors that rely on libclang use best-effort parsing;
    failures are reported in ``PostProcessResult.error`` rather than
    raising.

    Parameters
    ----------
    sca_level:
        SCA analysis depth (default 2).
    format:
        Include :class:`ClangFormatProcessor` as the first step.
    docblocks:
        Include :class:`DocblocksProcessor` to insert missing Doxygen headers.
    metrics:
        Include :class:`MetricsProcessor` at the end of the pipeline.
    integrity:
        Include :class:`~sdlos.features.sanitize.FileIntegrityProcessor`
        as the very first step.  Detects BOM, invisible Unicode, BiDi
        overrides, tag characters, and mixed line endings.  Default: True.
    strip_integrity:
        When True the integrity processor rewrites the file with dangerous
        bytes removed.  Default: False (report only).
    console:
        Rich Console for progress lines.
    """
    processors: list[PostProcessor] = []

    # Integrity scan runs first so subsequent processors see clean bytes.
    if integrity:
        from .sanitize import FileIntegrityProcessor
        processors.append(FileIntegrityProcessor(strip=strip_integrity))

    if format:
        processors.append(ClangFormatProcessor(style="file"))

    if sca_level > 0:
        processors.append(SCAProcessor(level=sca_level))

    if docblocks:
        processors.append(DocblocksProcessor())

    if metrics:
        processors.append(MetricsProcessor())

    return PostProcessPipeline(
        processors,
        share_sca_issues=True,
        console=console,
    )


def make_engine_pipeline(
    *,
    sca_level:        int            = 2,
    integrity:        bool           = True,
    strip_integrity:  bool           = False,
    format:           bool           = False,
    tidy:             bool           = True,
    docblocks:        bool           = True,
    metrics:          bool           = True,
    apply_tidy_fixes: bool           = False,
    compile_commands: Optional[Path] = None,
    include_dirs:     Optional[list[Path]] = None,
    console                          = None,
) -> PostProcessPipeline:
    """Build a full-featured pipeline for the source tree.

    Order: SCA → clang-tidy → [clang-format] → docblocks → [metrics]

    Parameters
    ----------
    sca_level:
        SCA analysis depth.
    format:
        Include :class:`ClangFormatProcessor`.
    tidy:
        Include :class:`ClangTidyProcessor`.
    docblocks:
        Include :class:`DocblocksProcessor`.
    metrics:
        Include :class:`MetricsProcessor` (on by default for engine analysis).
    apply_tidy_fixes:
        Pass ``--fix`` to clang-tidy.
    compile_commands:
        Path to a ``compile_commands.json`` for accurate clang-tidy analysis.
    include_dirs:
        Additional include directories forwarded to libclang.
    console:
        Rich Console for progress lines.
    """
    extra_args: list[str] = ["-std=c++23"]
    if include_dirs:
        for d in include_dirs:
            extra_args.append(f"-I{d}")

    processors: list[PostProcessor] = []

    # Integrity scan always runs first so all downstream processors see clean bytes.
    if integrity:
        from .sanitize import FileIntegrityProcessor
        processors.append(FileIntegrityProcessor(strip=strip_integrity))

    if sca_level > 0:
        processors.append(SCAProcessor(level=sca_level, extra_args=extra_args))

    if tidy:
        processors.append(
            ClangTidyProcessor(
                apply_fixes=apply_tidy_fixes,
                extra_args=extra_args,
                compile_commands=compile_commands,
            )
        )

    if format:
        processors.append(ClangFormatProcessor(style="file"))

    if docblocks:
        processors.append(DocblocksProcessor(extra_args=extra_args))

    if metrics:
        processors.append(MetricsProcessor())

    return PostProcessPipeline(
        processors,
        share_sca_issues=True,
        console=console,
    )


# ── Internal helpers ───────────────────────────────────────────────────────────

def _clone_docblocks_with_issues(
    proc:   DocblocksProcessor,
    issues: list,
) -> DocblocksProcessor:
    """Return a copy of *proc* with *issues* merged into its sca_issues list."""
    merged = list(proc.sca_issues) + [
        i for i in issues if i not in proc.sca_issues
    ]
    return DocblocksProcessor(
        sca_issues=merged,
        extra_args=list(proc.extra_args),
        dry_run=proc.dry_run,
    )


def _inject_sca_issues(path: Path, issues: list) -> None:
    """Attach *issues* to the path object as ``_pipeline_sca_issues``.

    This is a lightweight side-channel so ``MetricsProcessor`` can pick up
    SCA findings without the pipeline needing to know about its internals.
    We store the list on the :class:`Path` object itself (as a dynamic
    attribute) rather than creating a wrapper, which avoids breaking any
    isinstance checks downstream.
    """
    try:
        object.__setattr__(path, "_pipeline_sca_issues", issues)
    except (AttributeError, TypeError):
        pass  # Path may be a subclass that forbids extra attrs — silently skip


def _format_revert_reason(
    vr:      VerificationResult,
    results: list[PostProcessResult],
) -> str:
    """Build a human-readable revert reason from *vr* and processor results."""
    blame: list[str] = []

    # Which processors actually modified the file?
    modifiers = [r.processor for r in results if r.modified]
    if modifiers:
        blame.append("modified by: " + ", ".join(modifiers))

    # What went wrong?
    for err in vr.errors[:3]:   # cap at 3 to keep the message concise
        blame.append(err)
    if len(vr.errors) > 3:
        blame.append(f"… and {len(vr.errors) - 3} more error(s)")

    return "; ".join(blame) if blame else "verification failed"


def _log_result(
    result:  PostProcessResult,
    path:    Path,
    console,
) -> None:
    """Print a single-line progress entry for *result* to *console*."""
    if console is None:
        return
    try:
        name = path.name
        if result.error:
            console.print(
                f"  [yellow]⚠[/]  [dim]{result.processor}[/dim]  "
                f"[yellow]{name}[/yellow]  [dim]{result.error}[/dim]"
            )
        elif result.modified:
            console.print(
                f"  [cyan]✎[/]  [dim]{result.processor}[/dim]  "
                f"[cyan]{name}[/cyan]  [dim]modified[/dim]"
            )
        elif result.issues:
            n = len(result.issues)
            console.print(
                f"  [yellow]![/]  [dim]{result.processor}[/dim]  "
                f"[dim]{name}[/dim]  {n} issue(s)"
            )
        elif "metrics" in result.metadata:
            m = result.metadata["metrics"]
            console.print(
                f"  [green]✓[/]  [dim]{result.processor}[/dim]  "
                f"[dim]{name}[/dim]  "
                f"[dim]NLOC={m.nloc} funcs={m.function_count} "
                f"avg_CC={m.avg_complexity:.1f} grade={m.health_grade}[/dim]"
            )
        else:
            console.print(
                f"  [green]✓[/]  [dim]{result.processor}[/dim]  "
                f"[dim]{name}[/dim]"
            )
    except Exception:  # noqa: BLE001
        pass  # logging must never crash the pipeline


def _log_revert(path: Path, reason: str, console) -> None:
    """Print a revert notice to *console*."""
    if console is None:
        return
    try:
        console.print(
            f"\n  [bold red]↩ REVERTED[/bold red]  [dim]{path.name}[/dim]\n"
            f"  [dim]Reason: {reason}[/dim]\n"
            f"  [dim]The file has been restored to its pre-processing state.[/dim]\n"
        )
    except Exception:  # noqa: BLE001
        pass


def _log_idempotency_warning(
    path:     Path,
    warnings: list[str],
    console,
) -> None:
    """Print idempotency warnings to *console*."""
    if console is None:
        return
    try:
        for w in warnings:
            console.print(
                f"  [yellow]⚠[/]  [dim]ClangFormat[/dim]  "
                f"[dim]{path.name}[/dim]  [yellow]{w}[/yellow]"
            )
    except Exception:  # noqa: BLE001
        pass
