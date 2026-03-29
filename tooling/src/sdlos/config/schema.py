"""
sdlos.config.schema
===================
AppConfig — the single source of truth for every sdlos tooling command.

A config can come from three sources (in increasing priority order):
  1. Built-in defaults (dataclass field defaults)
  2. A YAML file  (``sdlos create --config app.yaml``)
  3. CLI flags    (always win)

win_w / win_h
-------------
Both are ``Optional[int]``.  ``None`` means "no override" — the cmake snippet
will not emit ``WIN_W`` / ``WIN_H``, so the engine uses its compile-time
defaults (375 × 667 in debug, fullscreen in release).  Any positive integer
overrides the debug window size at compile time.

Example YAML
------------
name: my_app
template: shader
win_w: 1280
win_h: 800
data_dir: true
with_model: assets/Crystal_Cluster.glb
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field, fields
from pathlib import Path
from typing import Optional

import yaml


# ── Template identifiers ──────────────────────────────────────────────────────

TEMPLATES = ("minimal", "shader", "camera", "pug", "vfs", "scene3d")


# ── Config schema ─────────────────────────────────────────────────────────────

@dataclass
class AppConfig:
    # ── Identity ───────────────────────────────────────────────────────────────
    name: str = ""
    """App name in snake_case (e.g. ``my_app``).  Becomes the CMake target."""

    template: str = "minimal"
    """Starter template: ``minimal`` | ``shader`` | ``camera`` | ``pug``."""

    # ── Output directory ──────────────────────────────────────────────────────
    app_dir: Optional[str] = None
    """Base directory for the generated app folder.

    ``None`` (default) → ``<project-root>/examples/apps/<name>``.

    Any other value is treated as the *parent* directory; the app lands at
    ``<app_dir>/<name>``.  The directory (and any missing parents) is created
    automatically.

    Examples::

        # default — lands in examples/apps/my_app/
        app_dir = None

        # custom — lands in /home/alice/projects/apps/my_app/
        app_dir = "/home/alice/projects/apps"

        # per-user convention — repo/<username>/apps/my_app/
        app_dir = "pce/apps"
    """

    # ── Window ───────────────────────────────────────────────────────────────
    win_w: Optional[int] = None
    """Debug window width in logical pixels.  None → engine default (375)."""

    win_h: Optional[int] = None
    """Debug window height in logical pixels.  None → engine default (667)."""

    # ── Scaffold options ──────────────────────────────────────────────────────
    data_dir: bool = False
    """Create a ``data/`` skeleton (shaders/msl, shaders/spirv, img, models)."""

    with_model: Optional[str] = None
    """Path to a .glb/.gltf to copy into ``data/models/`` during scaffold."""

    # ── Safety / UX ───────────────────────────────────────────────────────────
    overwrite: bool = False
    """Re-generate and overwrite existing scaffold files (user regions preserved)."""

    dry_run: bool = False
    """Print what would happen without touching the filesystem."""

    verbose: bool = True
    """Emit per-file log lines during generation."""

    # ── Internal / future ─────────────────────────────────────────────────────
    extra: dict = field(default_factory=dict, repr=False)
    """Unrecognised YAML keys are stored here for forward-compatibility."""

    # ── Validation ────────────────────────────────────────────────────────────

    def validate(self) -> None:
        """Raise ``ValueError`` for any invalid field value."""
        if not self.name:
            raise ValueError("AppConfig.name must not be empty.")
        if not re.fullmatch(r"[a-z][a-z0-9_]*", self.name):
            raise ValueError(
                f"App name {self.name!r} must start with a lowercase letter "
                "and contain only letters, digits, and underscores."
            )
        if self.template not in TEMPLATES:
            raise ValueError(
                f"Unknown template {self.template!r}. "
                f"Choose from: {', '.join(TEMPLATES)}."
            )
        if self.app_dir is not None and not self.app_dir:
            raise ValueError("app_dir must be a non-empty string or None.")
        if self.win_w is not None and self.win_w < 1:
            raise ValueError("win_w must be a positive integer.")
        if self.win_h is not None and self.win_h < 1:
            raise ValueError("win_h must be a positive integer.")

    # ── Constructors ──────────────────────────────────────────────────────────

    @classmethod
    def from_yaml(cls, path: Path) -> "AppConfig":
        """Load an AppConfig from a YAML file.

        Unknown keys are preserved in ``extra`` so they can be inspected
        without causing hard errors on future schema additions.
        """
        with open(path, "r", encoding="utf-8") as fh:
            raw: dict = yaml.safe_load(fh) or {}

        known = {f.name for f in fields(cls) if f.name != "extra"}
        kwargs = {k: v for k, v in raw.items() if k in known}
        extra  = {k: v for k, v in raw.items() if k not in known}

        obj = cls(**kwargs)
        obj.extra = extra
        return obj

    @classmethod
    def from_cli(
        cls,
        *,
        name: str,
        template: Optional[str] = None,
        win_w: Optional[int] = None,
        win_h: Optional[int] = None,
        data_dir: bool = False,
        overwrite: bool = False,
        dry_run: bool = False,
        with_model: Optional[str] = None,
        app_dir: Optional[str] = None,
        config_path: Optional[Path] = None,
    ) -> "AppConfig":
        """Build an AppConfig from CLI arguments, optionally seeded from a
        YAML file first.  CLI arguments always take priority over the file.

        ``None`` is the universal "not supplied" sentinel for all optional
        parameters — Click uses ``default=None`` for flags and options that
        were not passed on the command line, so we never have to guess
        whether a value came from the user or from a hardcoded default.
        """
        if config_path is not None:
            base = cls.from_yaml(config_path)
        else:
            base = cls()

        # Scalars: only override when the caller explicitly provided a value.
        if name:
            base.name = name
        if template is not None:
            base.template = template
        if win_w is not None:
            base.win_w = win_w
        if win_h is not None:
            base.win_h = win_h

        # Flags: always apply — the CLI has already resolved the default.
        base.overwrite = overwrite
        base.dry_run   = dry_run

        if data_dir:
            base.data_dir = data_dir
        if with_model is not None:
            base.with_model = with_model
        if app_dir is not None:
            base.app_dir = app_dir

        return base

    def to_yaml(self) -> str:
        """Serialise this config back to a YAML string (useful for ``--save-config``)."""
        known = {f.name: getattr(self, f.name) for f in fields(self) if f.name != "extra"}
        return yaml.dump(known, default_flow_style=False, sort_keys=False)
