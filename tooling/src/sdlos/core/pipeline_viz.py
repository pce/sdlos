"""
sdlos.core.pipeline_viz
=======================
Python-side pipeline.pug parser and Rich terminal visualiser.

Two public entry points
-----------------------
parse_pug(source)          → PipelineDesc  (pure data, no I/O)
show_pipeline(path, console) → None         (renders to terminal)

The parser intentionally mirrors the C++ pug::parse() logic so that
tooling output matches what the engine actually executes at runtime.

Grammar (line-oriented, same as the C++ parser):
-------------------------------------------------
    file        := line*
    line        := blank | comment | declaration
    blank       := whitespace* newline
    comment     := '//' .* newline
    declaration := tag '#' id '(' attrs? ')' newline
    tag         := 'variant' | 'resource' | 'pass'
    attr        := key '=' '"' value '"'

Rich output example
-------------------
    ╭──────────────────── viz/data/pipeline.pug ─────────────────────╮
    │  Variants 1   Resources 1   Passes 2                           │
    ╰────────────────────────────────────────────────────────────────╯

    Resources ─────────────────────────────────────────────────────────
      bg_color    rgba16f    swapchain-sized

    Flow ───────────────────────────────────────────────────────────────
      ● bg  ──▶  bg_color  ──▶  ● grade  ──▶  swapchain

     #   Pass    En  Shader   Reads        Writes      Params
    ─────────────────────────────────────────────────────────
     0   bg      ✓   bg       —            bg_color    scale=1.5
                                                       speed=0.4
                                                       time=0.0
     1   grade   ✓   grade    bg_color     swapchain   exposure=1.0
                                                       gamma=2.2
                                                       saturation=1.05

Usage
-----
    from sdlos.core.pipeline_viz import parse_pug, show_pipeline
    from rich.console import Console

    desc = parse_pug(Path("data/pipeline.pug").read_text())
    show_pipeline(Path("data/pipeline.pug"), Console())
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
# Data model  (mirrors C++ fg:: descriptor types)
# ---------------------------------------------------------------------------

@dataclass
class VariantDesc:
    """Bucket-A compile-time shader variant (brdf, shadow algorithm …)."""
    id:      str
    defines: dict[str, str] = field(default_factory=dict)


@dataclass
class ResourceDesc:
    """Named transient GPU texture declared in pipeline.pug."""
    id:     str
    format: str = "rgba8"     # rgba8 | rgba16f | depth32
    size:   str = "swapchain" # swapchain | fixed
    w:      int = 0
    h:      int = 0


@dataclass
class PassDesc:
    """One render pass in the pipeline."""
    id:         str
    shader_key: str              = ""
    reads:      list[str]        = field(default_factory=list)
    writes:     str              = "swapchain"
    params:     dict[str, str]   = field(default_factory=dict)
    enabled:    bool             = True


@dataclass
class PipelineDesc:
    """Complete parsed pipeline — output of :func:`parse_pug`."""
    source_path: Optional[Path]       = None
    variants:    list[VariantDesc]    = field(default_factory=list)
    resources:   list[ResourceDesc]   = field(default_factory=list)
    passes:      list[PassDesc]       = field(default_factory=list)
    errors:      list[str]            = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.errors

    @property
    def active_passes(self) -> list[PassDesc]:
        return [p for p in self.passes if p.enabled]


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------

# Matches:  tag#id(attrs)   or   tag#id
_DECL_RE = re.compile(
    r"^(?P<tag>variant|resource|pass)"
    r"#(?P<id>[a-zA-Z_][a-zA-Z0-9_-]*)"
    r"(?:\((?P<attrs>[^)]*)\))?\s*$"
)

# Matches:  key="value"
_ATTR_RE = re.compile(r'(?P<key>[a-zA-Z_][a-zA-Z0-9_-]*)="(?P<val>[^"]*)"')

_META_KEYS = frozenset({"shader", "reads", "writes", "enabled"})


def _parse_attrs(block: str) -> dict[str, str]:
    """Extract all key="value" pairs from an attribute block string."""
    return {m.group("key"): m.group("val") for m in _ATTR_RE.finditer(block)}


def _parse_bool(s: str) -> bool:
    return s.lower() not in {"false", "0", "no"}


def parse_pug(source: str, path: Optional[Path] = None) -> PipelineDesc:
    """Parse a *pipeline.pug* source string into a :class:`PipelineDesc`.

    Handles multi-line declarations where attributes wrap across lines:

        pass#bg(shader="bg"
                writes="bg_color"
                scale="1.5")

    Continuation lines are joined until the closing ``)`` is found.
    The parser is lenient: unknown tags are silently skipped (matching the
    C++ engine behaviour) so that forward-compatible pipeline descriptors
    don't break older tooling.

    Parameters
    ----------
    source:
        Full text of the pipeline.pug file.
    path:
        Optional filesystem path recorded in :attr:`PipelineDesc.source_path`
        for display purposes only.

    Returns
    -------
    PipelineDesc
        Always returns a valid object.  Check ``desc.errors`` for issues.
    """
    desc = PipelineDesc(source_path=path)

    # ── Pre-pass: join multi-line declarations ────────────────────────────────
    # A declaration that opens a ``(`` without a matching ``)`` on the same
    # line is continued on subsequent lines until ``)`` is found.
    # We collapse those into a single logical line before parsing.
    logical_lines: list[str] = []
    pending: list[str] = []

    for raw in source.splitlines():
        stripped = raw.strip()

        if pending:
            # We are inside an open paren — accumulate until we see ')'
            pending.append(stripped)
            if ")" in stripped:
                logical_lines.append(" ".join(pending))
                pending = []
        else:
            # Start of a new logical line
            if stripped.startswith("//") or not stripped:
                logical_lines.append(stripped)
            elif "(" in stripped and ")" not in stripped:
                # Opens a paren without closing it — start accumulation
                pending = [stripped]
            else:
                logical_lines.append(stripped)

    # Flush any unclosed paren block (malformed pug — add as-is)
    if pending:
        logical_lines.append(" ".join(pending))

    # ── Main parse loop ───────────────────────────────────────────────────────
    for lineno, line in enumerate(logical_lines, start=1):
        # Skip blank lines and comments.
        if not line or line.startswith("//"):
            continue

        m = _DECL_RE.match(line)
        if not m:
            # Unknown declaration — lenient, skip with no error.
            continue

        tag   = m.group("tag")
        id_   = m.group("id")
        attrs = _parse_attrs(m.group("attrs") or "")

        if tag == "variant":
            defines = {k: v for k, v in attrs.items()}
            desc.variants.append(VariantDesc(id=id_, defines=defines))

        elif tag == "resource":
            desc.resources.append(ResourceDesc(
                id     = id_,
                format = attrs.get("format", "rgba8"),
                size   = attrs.get("size",   "swapchain"),
                w      = int(attrs.get("w", "0") or "0"),
                h      = int(attrs.get("h", "0") or "0"),
            ))

        elif tag == "pass":
            reads_raw = attrs.get("reads", "")
            reads     = [r for r in reads_raw.split() if r] if reads_raw else []
            params    = {k: v for k, v in attrs.items() if k not in _META_KEYS}
            desc.passes.append(PassDesc(
                id         = id_,
                shader_key = attrs.get("shader",  ""),
                reads      = reads,
                writes     = attrs.get("writes",  "swapchain"),
                params     = params,
                enabled    = _parse_bool(attrs.get("enabled", "true")),
            ))

    return desc


def parse_pug_file(path: Path) -> PipelineDesc:
    """Load and parse a pipeline.pug file from disk."""
    try:
        source = path.read_text(encoding="utf-8")
    except OSError as exc:
        desc = PipelineDesc(source_path=path)
        desc.errors.append(str(exc))
        return desc
    return parse_pug(source, path=path)


# ---------------------------------------------------------------------------
# Rich renderer  (imported lazily so the module is usable without rich)
# ---------------------------------------------------------------------------

def show_pipeline(
    path_or_desc: "Path | PipelineDesc",
    console:      "Console | None" = None,  # type: ignore[name-defined]
) -> None:
    """Render a pipeline.pug file (or pre-parsed :class:`PipelineDesc`) to the
    terminal using Rich panels, tables, and colour markup.

    Parameters
    ----------
    path_or_desc:
        Either a :class:`pathlib.Path` pointing at a ``pipeline.pug`` file,
        or an already-parsed :class:`PipelineDesc`.
    console:
        A :class:`rich.console.Console` instance.  A default console is
        created when *None* is passed.
    """
    from rich.console import Console as _Console
    from rich.panel import Panel
    from rich.table import Table, box
    from rich.text import Text
    from rich.padding import Padding
    from rich import print as rprint  # noqa: F401

    con: _Console = console or _Console()

    # ── Resolve descriptor ────────────────────────────────────────────────────
    if isinstance(path_or_desc, Path):
        desc = parse_pug_file(path_or_desc)
    else:
        desc = path_or_desc

    if not desc.ok:
        con.print(f"[bold red]pipeline.pug errors:[/]")
        for e in desc.errors:
            con.print(f"  [red]{e}[/]")
        return

    if not desc.passes:
        con.print("[yellow]pipeline.pug: no passes declared.[/]")
        return

    # ── Header panel ──────────────────────────────────────────────────────────
    # Use a short relative path when possible — long absolute paths overflow.
    if desc.source_path:
        try:
            title_path = str(desc.source_path.relative_to(Path.cwd()))
        except ValueError:
            # source_path not under cwd — fall back to the last three components
            parts = desc.source_path.parts
            title_path = str(Path(*parts[max(0, len(parts) - 3):]))
    else:
        title_path = "pipeline.pug"
    summary = (
        f"[dim]Variants[/] [bold]{len(desc.variants)}[/]   "
        f"[dim]Resources[/] [bold]{len(desc.resources)}[/]   "
        f"[dim]Passes[/] [bold]{len(desc.passes)}[/]   "
        f"[dim]Active[/] [bold]{len(desc.active_passes)}[/]"
    )
    con.print(Panel(summary, title=f"[bold cyan]{title_path}[/]", expand=False))
    con.print()

    # ── Resources table ───────────────────────────────────────────────────────
    if desc.resources:
        con.rule("[bold]Resources[/]", style="dim")
        rtable = Table(box=box.SIMPLE, show_header=False, padding=(0, 2))
        rtable.add_column(style="bold magenta")
        rtable.add_column(style="cyan")
        rtable.add_column(style="dim")
        for r in desc.resources:
            size_str = "swapchain" if r.size == "swapchain" else f"{r.w}×{r.h}"
            rtable.add_row(r.id, r.format, size_str)
        con.print(Padding(rtable, (0, 2)))

    # ── Flow diagram ──────────────────────────────────────────────────────────
    con.rule("[bold]Flow[/]", style="dim")
    flow_parts: list[str] = []
    for pass_ in desc.passes:
        enabled_marker = "●" if pass_.enabled else "○"
        style = "bold green" if pass_.enabled else "dim"
        flow_parts.append(f"[{style}]{enabled_marker} {pass_.id}[/]")
    flow_arrow = "  [dim]──▶[/]  "
    flow_line  = flow_arrow.join(flow_parts)
    # Append final target
    last_writes = desc.passes[-1].writes if desc.passes else "swapchain"
    flow_line += f"  [dim]──▶[/]  [bold yellow]{last_writes}[/]"
    con.print(Padding(Text.from_markup(flow_line), (0, 4)))
    con.print()

    # ── Passes table ──────────────────────────────────────────────────────────
    con.rule("[bold]Passes[/]", style="dim")

    ptable = Table(
        box=box.SIMPLE_HEAD,
        show_header=True,
        header_style="bold dim",
        padding=(0, 1),
        expand=False,
    )
    ptable.add_column("#",       style="dim",         width=3,  no_wrap=True)
    # Enabled marker (✓/✗) is folded into the Pass column as a prefix so the
    # table fits a standard 80-column terminal without overflow.
    ptable.add_column("Pass",    style="",            width=14, no_wrap=True)
    ptable.add_column("Shader",  style="magenta",     width=12, no_wrap=True)
    ptable.add_column("Reads",   style="yellow",      width=14, no_wrap=True)
    ptable.add_column("Writes",  style="green",       width=12, no_wrap=True)
    ptable.add_column("Params",  style="white",       no_wrap=True)

    for idx, pass_ in enumerate(desc.passes):
        # Fold enabled state into the pass name: "✓ bg" or "○ bg (disabled)"
        if pass_.enabled:
            pass_cell = Text.from_markup(f"[green]✓[/] [bold cyan]{pass_.id}[/]")
        else:
            pass_cell = Text.from_markup(f"[dim]○ {pass_.id}[/]")

        reads_str = ", ".join(pass_.reads) if pass_.reads else "[dim]—[/]"

        writes_style = "bold green" if pass_.writes == "swapchain" else "green"
        writes_str   = f"[{writes_style}]{pass_.writes}[/]"

        # Render params — first on the same row, rest spill into continuation rows
        param_items = [
            f"[dim]{k}[/]=[cyan]{v}[/]"
            for k, v in pass_.params.items()
        ]
        first_param = param_items[0] if param_items else ""

        ptable.add_row(
            str(idx),
            pass_cell,
            pass_.shader_key or "[dim]—[/]",
            Text.from_markup(reads_str),
            Text.from_markup(writes_str),
            Text.from_markup(first_param),
        )
        # Continuation rows: blank leading columns, only the Params column filled
        for extra in param_items[1:]:
            ptable.add_row("", "", "", "", "", Text.from_markup(extra))

    con.print(Padding(ptable, (0, 2)))

    # ── Variants (if any) ─────────────────────────────────────────────────────
    if desc.variants:
        con.rule("[bold]Variants (Bucket A)[/]", style="dim")
        vtable = Table(box=box.SIMPLE, show_header=False, padding=(0, 2))
        vtable.add_column(style="bold magenta")
        vtable.add_column(style="dim")
        for v in desc.variants:
            defines_str = "  ".join(f"{k}={val}" for k, val in v.defines.items())
            vtable.add_row(v.id, defines_str)
        con.print(Padding(vtable, (0, 2)))

    # ── Resource-flow sanity checks ───────────────────────────────────────────
    _check_resource_flow(desc, con)


def _check_resource_flow(desc: PipelineDesc, con: "Console") -> None:  # type: ignore[name-defined]
    """Emit Rich warnings for common pipeline wiring mistakes."""
    from rich.padding import Padding

    declared  = {r.id for r in desc.resources}
    written:  set[str] = set()
    warnings: list[str] = []

    for pass_ in desc.passes:
        for r in pass_.reads:
            if r not in declared:
                warnings.append(
                    f"[yellow]⚠[/]  pass [bold]{pass_.id}[/] reads "
                    f"[bold]{r}[/] which is not declared as a resource"
                )
            elif r not in written:
                warnings.append(
                    f"[yellow]⚠[/]  pass [bold]{pass_.id}[/] reads "
                    f"[bold]{r}[/] before any pass writes it"
                )
        if pass_.writes and pass_.writes != "swapchain":
            written.add(pass_.writes)

    # Resources declared but never written
    for r in desc.resources:
        if r.id not in written:
            warnings.append(
                f"[dim]ℹ[/]  resource [bold]{r.id}[/] is declared "
                f"but never written by any pass"
            )

    if warnings:
        con.print()
        con.rule("[bold yellow]Warnings[/]", style="yellow dim")
        for w in warnings:
            con.print(Padding(w, (0, 4)))

    con.print()
