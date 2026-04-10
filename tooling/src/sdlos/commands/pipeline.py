"""
sdlos.commands.pipeline
=======================
``sdlos pipeline <target>`` — visualise a compiled render pipeline.

Target resolution
-----------------
The *target* argument accepts three forms:

  1. A direct path to a ``pipeline.pug`` file:
       sdlos pipeline examples/apps/viz/data/pipeline.pug

  2. An app name — resolved to ``examples/apps/<name>/data/pipeline.pug``:
       sdlos pipeline viz

  3. ``-`` or omitted inside a directory that contains ``data/pipeline.pug``:
       cd examples/apps/viz && sdlos pipeline

Output (Rich)
-------------
  ╭──────────────── viz/data/pipeline.pug ─────────────────╮
  │  Variants: 1   Resources: 1   Passes: 2                │
  ╰────────────────────────────────────────────────────────╯

  Resources ──────────────────────────────────────────────
   bg_color   rgba16f   swapchain

  Flow ────────────────────────────────────────────────────
   ● bg ──▶ ● grade ──▶ swapchain

   #  Pass    En  Shader   Reads      Writes      Params
  ─────────────────────────────────────────────────────────
   0  bg       ✓  bg       —          bg_color    scale=1.5
                                                  speed=0.4
                                                  time=0.0
   1  grade    ✓  grade    bg_color   swapchain   exposure=1.0
                                                  gamma=2.2
                                                  saturation=1.05

Usage
-----
  sdlos pipeline                          # auto-resolve from cwd
  sdlos pipeline viz                      # by app name
  sdlos pipeline data/pipeline.pug        # explicit path
  sdlos pipeline viz --watch              # re-render on file change
  sdlos pipeline viz --no-params          # compact, no Bucket-C column
"""
from __future__ import annotations

import os
from pathlib import Path
from typing import Optional

import click

from ..core.pipeline_viz import parse_pug_file, show_pipeline


# ── helpers ───────────────────────────────────────────────────────────────────

def _resolve_pug_path(target: Optional[str], cwd: Path) -> Path:
    """Return the absolute path to a ``pipeline.pug`` file.

    Resolution order
    ----------------
    1. *target* is an existing file path → use as-is.
    2. *target* looks like an app name (no ``/``, no ``.``)
       → look in ``<project_root>/examples/apps/<target>/data/pipeline.pug``.
    3. *target* is ``None`` or ``"-"``
       → scan upward from *cwd* for ``data/pipeline.pug`` or ``pipeline.pug``
         stopping at the project root (directory containing CMakeLists.txt).

    Raises
    ------
    click.UsageError
        When no ``pipeline.pug`` can be resolved.
    """
    from ..core.cmake import find_project_root  # local import avoids cycles


    if target and target != "-":
        p = Path(target).expanduser()
        if p.is_file():
            return p.resolve()

        # Treat as app name if it contains no path separators or dots.
        if os.sep not in target and "/" not in target and "." not in target:
            try:
                root = find_project_root(cwd)
                candidate = (
                    root / "examples" / "apps" / target / "data" / "pipeline.pug"
                )
                if candidate.is_file():
                    return candidate.resolve()
            except click.ClickException:
                pass

        raise click.UsageError(
            f"Cannot find pipeline.pug for target {target!r}.\n"
            f"  Tried: {p}\n"
            "  Pass an app name (e.g. 'viz') or an explicit path to pipeline.pug."
        )

    # Auto-detect: check cwd-relative canonical locations first.
    for candidate in (
        cwd / "data" / "pipeline.pug",
        cwd / "pipeline.pug",
    ):
        if candidate.is_file():
            return candidate.resolve()

    # Walk up looking for a data/pipeline.pug, stopping at the project root.
    here = cwd
    for _ in range(8):
        here = here.parent
        if here == here.parent:
            break  # filesystem root
        for rel in ("data/pipeline.pug", "pipeline.pug"):
            c = here / rel
            if c.is_file():
                return c.resolve()
        if (here / "CMakeLists.txt").exists():
            break  # reached project root without finding one

    raise click.UsageError(
        "No pipeline.pug found in the current directory tree.\n"
        "  Pass an app name or explicit path:\n"
        "    sdlos pipeline viz\n"
        "    sdlos pipeline examples/apps/viz/data/pipeline.pug"
    )


# ── command ───────────────────────────────────────────────────────────────────

@click.command("pipeline")
@click.argument("target", metavar="TARGET", required=False, default=None)
@click.option(
    "--watch", "-w",
    is_flag=True, default=False,
    help="Re-render whenever the file changes (Ctrl-C to exit).",
)
@click.option(
    "--no-params",
    is_flag=True, default=False,
    help="Omit the Bucket-C params column (narrower output).",
)
@click.option(
    "--no-flow",
    is_flag=True, default=False,
    help="Omit the pipeline flow diagram.",
)
def cmd_pipeline(
    target:    Optional[str],
    watch:     bool,
    no_params: bool,
    no_flow:   bool,
) -> None:
    """Visualise a pipeline.pug render pipeline.

    TARGET may be an app name (e.g. ``viz``), a path to a ``pipeline.pug``
    file, or omitted to auto-detect from the current directory tree.

    \b
    Examples
    --------
      sdlos pipeline                        auto-detect from cwd
      sdlos pipeline viz                    by app name
      sdlos pipeline data/pipeline.pug      explicit path
      sdlos pipeline viz --watch            live-reload on change
      sdlos pipeline viz --no-params        compact view, no params column
    """
    from rich.console import Console as _RichConsole

    cwd = Path.cwd()

    pug_path = _resolve_pug_path(target, cwd)

    # Shared Rich console — used by show_pipeline for all output.
    _con = _RichConsole()

    def _render_once() -> None:
        desc = parse_pug_file(pug_path)
        if not desc.ok:
            for err in desc.errors:
                _con.print(f"[bold red]error:[/] {err}")
            raise SystemExit(1)

        show_pipeline(
            desc,
            console=_con,
        )

    if not watch:
        _render_once()
        return

    # Watch mode
    # Requires watchfiles (already a tooling dependency).
    try:
        from watchfiles import watch as _watch
    except ImportError:
        raise click.ClickException(
            "watchfiles is required for --watch mode.\n"
            "  Install it with:  uv add watchfiles"
        )

    _con.print(
        f"\n[dim]Watching[/dim] [cyan]{pug_path}[/cyan]  "
        "[dim]─  Ctrl-C to exit[/dim]\n"
    )
    _render_once()

    try:
        for _changes in _watch(str(pug_path)):
            _con.clear()
            _con.print(
                f"\n[dim]── changed ───────────────────────────────────────[/dim]\n"
            )
            _render_once()
    except KeyboardInterrupt:
        _con.print("\n[dim]Stopped.[/dim]")
