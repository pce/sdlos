"""
sdlos.commands.interactive
==========================
Interactive prompt flow for ``sdlos create``.

Prompts only for values not already supplied as CLI flags.
``app_dir`` is always optional — if the user hits Enter on an empty field the
default (``examples/apps``) is used.

Invoked automatically when no NAME is provided on the command line, or
explicitly via ``sdlos create --interactive``.  Prompts only for values
that were not already supplied as CLI flags.

Usage (from cli.py)
-------------------
    from .commands.interactive import prompt_missing

    resolved = prompt_missing(
        name=name,           # None  → will prompt
        template=template,   # None  → will prompt
        win_w=win_w,         # None  → will prompt
        win_h=win_h,         # None  → will prompt
        data_dir=data_dir,   # None  → will prompt
        app_dir=app_dir,     # None  → optional prompt (empty = default path)
    )
    # resolved is a dict with every key filled in.
"""
from __future__ import annotations

import re
import sys
from typing import Optional

import questionary
from questionary import Style

# ── Visual style ──────────────────────────────────────────────────────────────
# Stays readable on both dark and light terminals.

_STYLE = Style(
    [
        ("qmark",        "fg:#6366f1 bold"),
        ("question",     "bold"),
        ("answer",       "fg:#6ee7b7 bold"),
        ("pointer",      "fg:#6366f1 bold"),
        ("highlighted",  "fg:#6366f1 bold"),
        ("selected",     "fg:#6ee7b7"),
        ("separator",    "fg:#444444"),
        ("instruction",  "fg:#666666"),
    ]
)


# ── Template choices ──────────────────────────────────────────────────────────

_TEMPLATE_CHOICES = [
    questionary.Choice(
        title="minimal   — empty jade_app_init stub with TODO markers",
        value="minimal",
    ),
    questionary.Choice(
        title="shader    — shader canvas + preset sidebar + param controls",
        value="shader",
    ),
    questionary.Choice(
        title="camera    — live video canvas + filter chips + dragnum inputs",
        value="camera",
    ),
    questionary.Choice(
        title="pug       — FrameGraph pipeline demo (pipeline.pug + CSS + Metal shaders + HUD)",
        value="pug",
    ),
    questionary.Choice(
        title="vfs       — VFS explorer + audio player (MemMount, LocalMount, SDL3 audio)",
        value="vfs",
    ),
    questionary.Choice(
        title="scene3d   — glTF model viewer with orbit camera and floating CSS labels",
        value="scene3d",
    ),
]


# ── Window-size presets ───────────────────────────────────────────────────────
#
# win_w == win_h == None  →  "fullscreen / engine default"
#   cmake snippet omits WIN_W / WIN_H entirely.
#   debug build   → engine default  375 × 667  (SDL_WINDOW_RESIZABLE)
#   release build → SDL_WINDOW_FULLSCREEN  (always, regardless of preset)
#
# Any other (w, h) pair is emitted verbatim as WIN_W / WIN_H compile defs.

class _WinPreset:
    __slots__ = ("label", "w", "h")

    def __init__(self, label: str, w: Optional[int], h: Optional[int]) -> None:
        self.label = label
        self.w = w
        self.h = h


_WIN_PRESETS: list[_WinPreset] = [
    _WinPreset("Fullscreen        — release always, debug uses display size", None,  None),
    _WinPreset("Phone portrait    375 × 667   (sdlos debug default)",          375,   667),
    _WinPreset("Phone landscape   667 × 375",                                  667,   375),
    _WinPreset("Tablet            768 × 1024",                                 768,  1024),
    _WinPreset("Desktop           1280 × 800",                                1280,   800),
    _WinPreset("Wide              1920 × 1080",                               1920,  1080),
    _WinPreset("Square            800 × 800",                                  800,   800),
    _WinPreset("Custom…           enter width and height",                    None,  None),
]

_CUSTOM_LABEL = "Custom…           enter width and height"


def _win_choices() -> list[questionary.Choice]:
    return [
        questionary.Choice(title=p.label, value=p)
        for p in _WIN_PRESETS
    ]


# ── Name validation ───────────────────────────────────────────────────────────

def _validate_name(raw: str) -> bool | str:
    """Return True if valid, or an error string to display inline."""
    if not raw:
        return "Name cannot be empty."
    cleaned = raw.strip().replace("-", "_").lower()
    if not re.fullmatch(r"[a-z][a-z0-9_]*", cleaned):
        return "Use snake_case: start with a letter, then letters / digits / underscores."
    return True


def _normalize_name(raw: str) -> str:
    return raw.strip().replace("-", "_").lower()


# ── Custom window-size entry ──────────────────────────────────────────────────

def _parse_dimension(raw: str, axis: str) -> int:
    """Parse a positive integer from *raw*, re-prompting on bad input."""
    try:
        v = int(raw.strip())
        if v < 1:
            raise ValueError
        return v
    except ValueError:
        return -1  # caller should re-prompt


def _prompt_custom_size() -> tuple[int, int]:
    """Ask for width and height separately with inline validation."""
    def _valid_dim(v: str) -> bool | str:
        try:
            n = int(v.strip())
            return True if n >= 1 else "Must be a positive integer."
        except ValueError:
            return "Must be a positive integer."

    w_str = questionary.text(
        "  Width  (px):",
        default="800",
        validate=_valid_dim,
        style=_STYLE,
    ).ask()
    if w_str is None:
        _abort()

    h_str = questionary.text(
        "  Height (px):",
        default="600",
        validate=_valid_dim,
        style=_STYLE,
    ).ask()
    if h_str is None:
        _abort()

    return int(w_str.strip()), int(h_str.strip())


# ── Abort helper ─────────────────────────────────────────────────────────────

def _abort() -> None:
    """Called when the user hits Ctrl-C / EOF during a prompt."""
    print("\nAborted.", file=sys.stderr)
    sys.exit(1)


# ── Public entry point ────────────────────────────────────────────────────────

def prompt_missing(
    *,
    name: Optional[str],
    template: Optional[str],
    win_w: Optional[int],
    win_h: Optional[int],
    data_dir: Optional[bool],
    app_dir: Optional[str] = None,
) -> dict:
    """Prompt for every argument that is ``None`` (not yet supplied).

    Returns a dict with keys:
        name, template, win_w, win_h, data_dir, app_dir

    All values are guaranteed to be non-None on return (or the process
    exits if the user aborts with Ctrl-C).  ``app_dir`` may be ``None``
    on return when the user accepts the default output location.

    Parameters
    ----------
    name:
        Snake-case app name, or ``None`` to prompt.
    template:
        Template identifier, or ``None`` to prompt.
    win_w / win_h:
        Debug window dimensions, or ``None`` to prompt.
        Both ``None`` on return means "fullscreen / no override".
    data_dir:
        Whether to scaffold a ``data/`` directory, or ``None`` to prompt.
    app_dir:
        Base directory for the generated app folder, or ``None`` to prompt.
        ``None`` on return means use the project-default (``examples/apps``).
    """
    print()  # breathing room after the command line

    # ── Name ─────────────────────────────────────────────────────────────────
    if name is None:
        raw = questionary.text(
            "App name:",
            validate=_validate_name,
            style=_STYLE,
        ).ask()
        if raw is None:
            _abort()
        name = _normalize_name(raw)
    else:
        # Already supplied — print it so the interactive session feels complete.
        print(f"  App name  : {name}")

    # ── Template ──────────────────────────────────────────────────────────────
    if template is None:
        template = questionary.select(
            "Template:",
            choices=_TEMPLATE_CHOICES,
            style=_STYLE,
        ).ask()
        if template is None:
            _abort()
    else:
        print(f"  Template  : {template}")

    # ── Window size ───────────────────────────────────────────────────────────
    if win_w is None and win_h is None:
        preset: _WinPreset = questionary.select(
            "Window size:",
            choices=_win_choices(),
            style=_STYLE,
        ).ask()
        if preset is None:
            _abort()

        if preset.label == _CUSTOM_LABEL:
            win_w, win_h = _prompt_custom_size()
        else:
            # None, None → fullscreen (omit WIN_W/WIN_H in cmake)
            win_w, win_h = preset.w, preset.h
    else:
        # At least one dimension was given on the CLI — keep both as-is and
        # skip the prompt so partial flags work naturally.
        if win_w is not None or win_h is not None:
            _w = str(win_w) if win_w is not None else "default"
            _h = str(win_h) if win_h is not None else "default"
            print(f"  Window    : {_w} × {_h}")

    # ── Data directory ────────────────────────────────────────────────────────
    if data_dir is None:
        # Shader, camera, and pug templates almost always need a data/ folder.
        # For pug, data_dir is forced True in create_app regardless, but we
        # default the prompt to True so the UI stays consistent.
        default_data = template in ("shader", "camera", "pug", "vfs", "scene3d")
        data_dir = questionary.confirm(
            "Scaffold data/ directory?  (shaders · fonts · models · images)",
            default=default_data,
            style=_STYLE,
        ).ask()
        if data_dir is None:
            _abort()
    else:
        if data_dir:
            print("  Data dir  : yes")

    # ── Output directory (optional) ───────────────────────────────────────────
    # Always shown so the user knows where the app will land.
    # An empty answer keeps the project default (examples/apps/<name>).
    if app_dir is None:
        default_label = f"examples/apps/{name}"
        raw_dir = questionary.text(
            "Output directory (base, leave empty for default):",
            default="",
            instruction=f"  default → {default_label}",
            style=_STYLE,
        ).ask()
        if raw_dir is None:
            _abort()
        app_dir = raw_dir.strip() or None
    else:
        print(f"  App dir   : {app_dir}/{name}")

    print()  # blank line before the generator output

    return {
        "name":     name,
        "template": template,
        "win_w":    win_w,
        "win_h":    win_h,
        "data_dir": data_dir,
        "app_dir":  app_dir,
    }
