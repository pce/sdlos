"""
sdlos.core.console
==================
Shared Rich console instance and themed print helpers used by every sdlos
command (create, run, pipeline, …).

All terminal output in sdlos tooling goes through this module so the look and
feel is consistent across commands and can be toggled off in one place
(e.g. redirect to a file, suppress in CI, swap theme colours).

Usage
-----
::

    from sdlos.core.console import console, print_banner, print_section
    from sdlos.core.console import print_item, print_done, print_warn, print_error

    print_banner("sdlos create", subtitle="shader · 375 × 667")
    print_section("Writing files")
    print_item("create", "examples/apps/viz/viz.jade")
    print_item("skip",   "examples/apps/viz/viz.css", dim=True)
    print_done("viz", next_steps=["cmake --build build --target viz", "sdlos run viz"])

Design
------
- One ``rich.console.Console`` instance at module level — cheap, thread-safe.
- A custom ``Theme`` maps semantic token names (``ok``, ``warn``, ``path``, …)
  to concrete styles so callers never hard-code ANSI colour codes.
- Helper functions accept plain Python strings; Rich markup is only used
  internally so callers stay free of Rich-specific syntax.
"""
from __future__ import annotations

from typing import Sequence

from rich.console import Console
from rich.padding import Padding
from rich.panel import Panel
from rich.rule import Rule
from rich.table import Table
from rich.text import Text
from rich.theme import Theme

# ── Theme ─────────────────────────────────────────────────────────────────────
#
# Tokens are intentionally semantic ("ok" not "green") so a dark/light theme
# swap only requires changing this dict.

_THEME = Theme(
    {
        # Status tokens
        "ok":     "bold green",
        "warn":   "bold yellow",
        "error":  "bold red",
        "info":   "bold cyan",
        "dim":    "dim white",
        # Structural tokens
        "header": "bold white",
        "rule":   "bright_black",
        # Code / data tokens
        "path":   "cyan",
        "key":    "bold magenta",
        "val":    "white",
        "tag":    "bold blue",
        "hash":   "bright_black",
        # Action tokens  (used in file-log lines)
        "create": "bold green",
        "skip":   "dim white",
        "copy":   "bold cyan",
        "mkdir":  "bold blue",
        "dryrun": "bold yellow",
    }
)

# ── Shared console ────────────────────────────────────────────────────────────

#: Module-level console used by all sdlos commands.
#: Callers may pass this to Rich objects that accept a ``console=`` argument.
console = Console(theme=_THEME, highlight=False)


# ── Banner ────────────────────────────────────────────────────────────────────

def print_banner(title: str, subtitle: str = "") -> None:
    """Print a top-of-command decorative banner panel.

    Parameters
    ----------
    title:
        Primary heading, e.g. ``"sdlos create"``.
    subtitle:
        Optional secondary line rendered inside the panel,
        e.g. ``"template: shader  ·  375 × 667"``.

    Example output::

        ╭─────────────────────────────────╮
        │         sdlos  create           │
        │   template: shader · 375 × 667  │
        ╰─────────────────────────────────╯
    """
    body = Text(justify="center")
    body.append(title, style="bold white")
    if subtitle:
        body.append(f"\n{subtitle}", style="dim white")

    console.print(
        Panel(
            Padding(body, (0, 4)),
            border_style="bright_black",
            expand=False,
            padding=(0, 2),
        )
    )
    console.print()


# ── Section headings ──────────────────────────────────────────────────────────

def print_section(label: str) -> None:
    """Print a horizontal rule with a centred section label.

    Example output::

        ─────────────────── Writing files ───────────────────
    """
    console.print(Rule(f"[rule]{label}[/rule]", style="rule"))


# ── Per-file / per-action log lines ──────────────────────────────────────────

#: Maps action keyword → (left-pad width, style token).
_ACTION_STYLES: dict[str, tuple[int, str]] = {
    "create":   (8, "create"),
    "skip":     (8, "skip"),
    "copy":     (8, "copy"),
    "mkdir":    (8, "mkdir"),
    "dry-run":  (8, "dryrun"),
    "warn":     (8, "warn"),
    "error":    (8, "error"),
    "info":     (8, "info"),
    "ok":       (8, "ok"),
}


def print_item(
    action: str,
    target: str,
    detail: str = "",
    dim: bool = False,
) -> None:
    """Print a single file-action log line.

    Parameters
    ----------
    action:
        Short verb: ``"create"``, ``"skip"``, ``"copy"``, ``"mkdir"``,
        ``"dry-run"``, ``"warn"``, ``"error"``.
    target:
        File path or label to display after the action badge.
    detail:
        Optional extra note appended in dim style.
    dim:
        When *True* the entire line is dimmed (used for "skip" lines that
        would otherwise clutter the output).

    Example output::

          [create]    examples/apps/viz/viz.jade
          [skip]      examples/apps/viz/viz.css  (exists)
    """
    _, style = _ACTION_STYLES.get(action, (8, "info"))
    action_badge = Text()
    action_badge.append(f"  [{action}]", style="dim" if dim else style)
    action_badge.append("  ")
    action_badge.append(target, style="dim" if dim else "path")
    if detail:
        action_badge.append(f"  {detail}", style="dim")
    console.print(action_badge)


# ── Key-value property rows ───────────────────────────────────────────────────

def print_kv_table(rows: Sequence[tuple[str, str]], indent: int = 2) -> None:
    """Print a compact key → value table, left-aligned, no borders.

    Useful for displaying resolved AppConfig fields before scaffolding.

    Parameters
    ----------
    rows:
        Sequence of ``(key, value)`` pairs.
    indent:
        Number of leading spaces.

    Example output::

      name       : viz
      template   : shader
      window     : 375 × 667
    """
    if not rows:
        return
    pad = " " * indent
    key_width = max(len(k) for k, _ in rows)
    for key, val in rows:
        console.print(
            f"{pad}[key]{key:<{key_width}}[/key]  [dim]:[/dim]  [val]{val}[/val]"
        )


# ── Done / footer ─────────────────────────────────────────────────────────────

def print_done(app_name: str, next_steps: Sequence[str] = ()) -> None:
    """Print the post-scaffold success panel with next-step commands.

    Parameters
    ----------
    app_name:
        The scaffolded app's snake_case name.
    next_steps:
        Shell commands the developer should run next.

    Example output::

        ╭──────────────── Done ─────────────────╮
        │  ✓  viz scaffolded successfully        │
        │                                        │
        │  Next steps                            │
        │    cmake --build build --target viz    │
        │    sdlos run viz                       │
        ╰────────────────────────────────────────╯
    """
    body = Text()
    body.append(f"✓  ", style="ok")
    body.append(f"{app_name}", style="bold white")
    body.append("  scaffolded successfully", style="white")

    if next_steps:
        body.append("\n\n")
        body.append("Next steps\n", style="bold white")
        for cmd in next_steps:
            body.append(f"  {cmd}\n", style="path")

    console.print()
    console.print(
        Panel(
            Padding(body, (0, 2)),
            title="[ok]Done[/ok]",
            border_style="green",
            expand=False,
        )
    )
    console.print()


# ── Warning / error helpers ───────────────────────────────────────────────────

def print_warn(message: str) -> None:
    """Print a single-line warning with a ⚠  prefix."""
    console.print(f"  [warn]⚠[/warn]  {message}")


def print_error(message: str) -> None:
    """Print a single-line error with a ✖  prefix."""
    console.print(f"  [error]✖[/error]  {message}")


def print_info(message: str) -> None:
    """Print a single-line informational line with a ℹ  prefix."""
    console.print(f"  [info]ℹ[/info]  {message}")


# ── Dry-run notice ────────────────────────────────────────────────────────────

def print_dryrun_notice() -> None:
    """Print a prominent dry-run banner so the developer knows nothing was written."""
    console.print()
    console.print(
        Panel(
            "[dryrun]DRY RUN[/dryrun] — no files will be written to disk",
            border_style="yellow",
            expand=False,
            padding=(0, 2),
        )
    )
    console.print()
