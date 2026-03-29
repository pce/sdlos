"""sdlos.features — optional feature blocks applied during app scaffolding.

Each feature is a self-contained class with an ``apply(context)`` method.
The ``create`` command collects the active features from AppConfig and
calls them in order after the core scaffold files have been written.

Post-processing pipeline
------------------------
The pipeline is applied automatically after each C++ file is rendered by
``sdlos create``.  It can also be invoked directly via ``sdlos analyze``.

Available features
------------------
Bundler           — packs build output into a distributable archive.
Updater           — generates and pre-configures an auto-updater binary stub.

Post-processors (used by PostProcessPipeline and sdlos analyze)
---------------------------------------------------------------
SCA               — libclang multi-level static security analyser.
ClangLint         — pure libclang lint checker (no clang-tidy binary required).
                    Two passes: Clang -W diagnostic engine + custom AST checks.
ClangTidy         — backward-compatible alias for ClangLint.
Docblocks         — AST-reflection Doxygen docblock generator.
MetricsProcessor  — lizard + radon code metrics collector.

Transactional safety
--------------------
``PostProcessPipeline.run_safe()`` wraps every run in a snapshot / verify /
revert transaction.  Use ``run_all_safe()`` for batch processing.

Key types:

    FileSnapshot        — immutable before/after content snapshot
    VerificationResult  — result of post-processing syntax check
    TransactionReport   — full record of what happened (reverted?, why?)

Verification helpers:

    verify_cxx_file()          — libclang parse check (bracket-balance fallback)
    check_format_idempotency() — confirm clang-format is a fixed point

Metrics
-------
    MetricsCollector   — collect FileMetrics from a source file
    FileMetrics        — per-file aggregated metrics
    FunctionMetrics    — per-function CCN, NLOC, token count, grade
    HalsteadMetrics    — Halstead volume/difficulty/effort/bugs (Python, radon)
    RawLineMetrics     — total/code/comment/blank line breakdown

Pipeline helpers
----------------
    PostProcessPipeline   — chains processors; SCA issues flow into Docblocks/Metrics.
    make_template_pipeline — lightweight pipeline for generated scaffold files.
    make_engine_pipeline   — full pipeline for the engine source tree.
"""

from .bundler import Bundler
from .updater import Updater

# ── Static analysis ────────────────────────────────────────────────────────────
from .sca import SCA, Issue
from .tidy import (
    ClangLint,
    LintProfile,
    TidyIssue,
    # backward-compatible aliases
    ClangTidy,
    TidyCheckProfile,
)

# ── Docblock generation ────────────────────────────────────────────────────────
from .docblocks import Docblocks, GeneratedDocblock

# ── Code metrics ───────────────────────────────────────────────────────────────
from .metrics import (
    MetricsCollector,
    MetricsProcessor,
    FileMetrics,
    FunctionMetrics,
    HalsteadMetrics,
    RawLineMetrics,
    print_metrics,
    print_metrics_summary,
)

# ── Post-processing pipeline ───────────────────────────────────────────────────
from .post_process import (
    # Result types
    PostProcessResult,
    # Transactional safety
    FileSnapshot,
    VerificationResult,
    TransactionReport,
    verify_cxx_file,
    check_format_idempotency,
    # Processors
    PostProcessPipeline,
    ClangFormatProcessor,
    SCAProcessor,
    ClangTidyProcessor,
    DocblocksProcessor,
    MetricsProcessor as _MetricsProcessorPP,   # same class, re-exported via metrics
    # Factory helpers
    make_template_pipeline,
    make_engine_pipeline,
)

__all__ = [
    # Scaffold features
    "Bundler",
    "Updater",

    # Static analysis
    "SCA",
    "Issue",
    # Lint (pure libclang — no binary required)
    "ClangLint",
    "LintProfile",
    "TidyIssue",
    # backward-compatible aliases
    "ClangTidy",
    "TidyCheckProfile",

    # Docblock generation
    "Docblocks",
    "GeneratedDocblock",

    # Code metrics
    "MetricsCollector",
    "MetricsProcessor",
    "FileMetrics",
    "FunctionMetrics",
    "HalsteadMetrics",
    "RawLineMetrics",
    "print_metrics",
    "print_metrics_summary",

    # Post-processing pipeline — result types
    "PostProcessResult",

    # Transactional safety
    "FileSnapshot",
    "VerificationResult",
    "TransactionReport",
    "verify_cxx_file",
    "check_format_idempotency",

    # Processors
    "PostProcessPipeline",
    "ClangFormatProcessor",
    "SCAProcessor",
    "ClangTidyProcessor",
    "DocblocksProcessor",

    # Factory helpers
    "make_template_pipeline",
    "make_engine_pipeline",
]
