"""
sdlos.templates.renderer
========================
Jinja2 environment and render helpers for sdlos code-generation templates.

Delimiter choice
----------------
To avoid conflicts with C++ syntax the Jinja2 delimiters are remapped:

  {$ expr $}          variable / expression   (default: {{ }})
  <% block %>         control flow            (default: {% %})
  <# comment #>       template comment        (default: {# #})

This means that:
  - All C++ braces  { }  {{ }}  pass through as literals.
  - C++ template arguments  std::shared_ptr<MyState>  are safe.
  - C++ stream operators  <<  and  >>  are safe.
  - C++ aggregate initialisers  { "a", 1.f }  are safe.
  - The sequence  std::make_shared<MyState>()  is safe (no delimiter collision).

Template lookup
---------------
Templates live under  src/sdlos/templates/<template_name>/<kind>.j2
where <kind> is one of:  jade  css  behavior_cxx

The renderer is usable without installing the package: it resolves the
templates directory relative to this file via FileSystemLoader.
"""
from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING

from jinja2 import (
    Environment,
    FileSystemLoader,
    StrictUndefined,
    TemplateNotFound,
)

if TYPE_CHECKING:
    from ..config.schema import AppConfig

from ..core.naming import pascal as _pascal


# ── Template kinds ────────────────────────────────────────────────────────────

KINDS = ("jade", "css", "behavior_cxx")

# Maps kind → output filename template  (receives *name* as a format arg)
_FILENAME_MAP: dict[str, str] = {
    "jade":         "{name}.jade",
    "css":          "{name}.css",
    "behavior_cxx": "{name}_behavior.cxx",
}

_TEMPLATES_DIR = Path(__file__).parent


# ── Jinja2 environment ────────────────────────────────────────────────────────

def _make_env() -> Environment:
    """Build and return a configured Jinja2 :class:`Environment`.

    The environment is created fresh each call so callers that need to
    add custom filters or globals can start from a clean state.
    """
    return Environment(
        loader=FileSystemLoader(str(_TEMPLATES_DIR)),
        # Remapped delimiters — no C++ brace or template-argument conflicts.
        #
        # {$ expr $}   variable / expression   (default: {{ }})
        # <% block %>  control flow            (default: {% %})
        # <# text #>   template comment        (default: {# #})
        #
        # Rationale: the default {{ }} conflicts with C++ aggregate-init and
        # lambda captures.  << >> (previous choice) conflicted with the
        # sequence <<<pascal_name>> which Jinja2 parsed as << + <expr.
        # {$ $} never appears in C++, CSS, or Jade source.
        variable_start_string="{$",
        variable_end_string="$}",
        block_start_string="<%",
        block_end_string="%>",
        comment_start_string="<#",
        comment_end_string="#>",
        # Fail loudly on typos instead of silently emitting empty strings.
        undefined=StrictUndefined,
        # Preserve the trailing newline that editors add to source files.
        keep_trailing_newline=True,
        # Do not auto-escape — output is source code, not HTML.
        autoescape=False,
    )


# ── Public API ────────────────────────────────────────────────────────────────

def render_template(template_name: str, kind: str, cfg: "AppConfig") -> str:
    """Render ``templates/<template_name>/<kind>.j2`` with *cfg* as context.

    Parameters
    ----------
    template_name:
        One of ``minimal``, ``shader``, ``camera`` (matches a subdirectory
        under ``src/sdlos/templates/``).
    kind:
        One of ``jade``, ``css``, ``behavior_cxx``.
    cfg:
        The :class:`~sdlos.config.schema.AppConfig` that drives generation.

    Returns
    -------
    str
        Rendered template text, ready to be written to disk.

    Raises
    ------
    click.ClickException
        If the template file cannot be found.
    ValueError
        If *kind* is not a recognised template kind.
    """
    import click  # local import keeps the module importable without click

    if kind not in KINDS:
        raise ValueError(
            f"Unknown template kind {kind!r}. Choose from: {', '.join(KINDS)}."
        )

    template_path = f"{template_name}/{kind}.j2"

    env = _make_env()
    try:
        tmpl = env.get_template(template_path)
    except TemplateNotFound:
        raise click.ClickException(
            f"Template not found: {_TEMPLATES_DIR / template_path}\n"
            f"Available templates: {list_available()}"
        )

    return tmpl.render(**_build_context(cfg))


def output_filename(kind: str, name: str) -> str:
    """Return the output filename for *kind* and app *name*.

    >>> output_filename("jade", "my_app")
    'my_app.jade'
    >>> output_filename("behavior_cxx", "my_app")
    'my_app_behavior.cxx'
    """
    if kind not in _FILENAME_MAP:
        raise ValueError(f"Unknown kind {kind!r}")
    return _FILENAME_MAP[kind].format(name=name)


def list_available() -> list[str]:
    """Return a list of template names that have all required kinds present."""
    available = []
    for subdir in sorted(_TEMPLATES_DIR.iterdir()):
        if not subdir.is_dir():
            continue
        if all((subdir / f"{k}.j2").exists() for k in KINDS):
            available.append(subdir.name)
    return available


# ── Context builder ───────────────────────────────────────────────────────────

def _build_context(cfg: "AppConfig") -> dict:
    """Build the Jinja2 template context dict from *cfg*."""
    return {
        "name":        cfg.name,
        "pascal_name": _pascal(cfg.name),
        "win_w":       cfg.win_w,
        "win_h":       cfg.win_h,
        "data_dir":    cfg.data_dir,
        "with_model":  cfg.with_model,
    }
