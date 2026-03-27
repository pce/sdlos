"""
sdlos.features.updater
======================
Updater feature — generates and pre-configures an auto-updater binary stub
that can be bundled alongside a release build.

When applied to a scaffold context the Updater feature:
  1. Writes  data/updater.json  containing the update channel config.
  2. Optionally copies a pre-built updater binary into the output directory.
  3. Emits a CMake snippet that copies the updater resources at build time.

Configuration (AppConfig.extra keys)
-------------------------------------
updater_url      : str   — base URL of the update server.
updater_channel  : str   — release channel  (default: "stable")
updater_binary   : str   — optional path to a pre-built updater binary to copy.

Example app.yaml
----------------
name: my_app
template: minimal
extra:
  updater_url: https://updates.example.com/my_app
  updater_channel: stable
  updater_binary: tools/updater/bin/updater
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, TYPE_CHECKING
import shutil

if TYPE_CHECKING:
    from ..config.schema import AppConfig


# ── Config ────────────────────────────────────────────────────────────────────

@dataclass
class UpdaterConfig:
    """Typed view of the updater-related keys in ``AppConfig.extra``."""

    url: str = ""
    """Base URL of the update server (e.g. https://updates.example.com/myapp)."""

    channel: str = "stable"
    """Release channel label written into updater.json."""

    binary: Optional[str] = None
    """Path to a pre-built updater binary to copy into the app data dir."""

    @classmethod
    def from_extra(cls, extra: dict) -> "UpdaterConfig":
        return cls(
            url=extra.get("updater_url", ""),
            channel=extra.get("updater_channel", "stable"),
            binary=extra.get("updater_binary", None),
        )


# ── Context passed to apply() ─────────────────────────────────────────────────

@dataclass
class UpdaterContext:
    """Minimal context the Updater feature needs from the caller."""

    app_dir: Path
    """Absolute path to examples/apps/<name>/."""

    app_name: str
    """App name in snake_case."""

    dry_run: bool = False
    verbose: bool = True

    cmake_lines: list[str] = field(default_factory=list)
    """Accumulates CMake lines that the caller can append to its snippet."""

    @classmethod
    def from_cfg(cls, cfg: "AppConfig", app_dir: Path) -> "UpdaterContext":
        return cls(
            app_dir=app_dir,
            app_name=cfg.name,
            dry_run=cfg.dry_run,
            verbose=cfg.verbose,
        )


# ── Feature class ─────────────────────────────────────────────────────────────

class Updater:
    """Updater feature — writes updater.json and optionally copies an updater binary.

    Usage
    -----
    ::

        updater = Updater(url="https://updates.example.com/myapp", channel="stable")
        ctx = UpdaterContext.from_cfg(cfg, app_dir)
        updater.apply(ctx)
        # ctx.cmake_lines now contains sdlos_copy_resource lines to add to CMake.

    Or driven entirely from AppConfig.extra via :meth:`from_cfg`::

        updater = Updater.from_cfg(cfg)
        if updater.is_active():
            updater.apply(ctx)
    """

    # ── Construction ──────────────────────────────────────────────────────────

    def __init__(
        self,
        url: str = "",
        channel: str = "stable",
        binary: Optional[str] = None,
    ) -> None:
        self.url     = url
        self.channel = channel
        self.binary  = binary

    @classmethod
    def from_cfg(cls, cfg: "AppConfig") -> "Updater":
        """Build an Updater from the ``extra`` dict in *cfg*."""
        uc = UpdaterConfig.from_extra(cfg.extra)
        return cls(url=uc.url, channel=uc.channel, binary=uc.binary)

    # ── Queries ───────────────────────────────────────────────────────────────

    def is_active(self) -> bool:
        """Return True if this feature has enough configuration to do useful work."""
        return bool(self.url)

    # ── Application ───────────────────────────────────────────────────────────

    def apply(self, ctx: UpdaterContext) -> None:
        """Write updater resources into *ctx.app_dir* and populate cmake lines.

        Steps
        -----
        1. Write ``data/updater.json`` with channel metadata.
        2. Copy the updater binary into ``data/`` if *self.binary* is set.
        3. Emit ``sdlos_copy_resource`` lines into ``ctx.cmake_lines``.
        """
        self._write_json(ctx)
        self._copy_binary(ctx)

    # ── Private ───────────────────────────────────────────────────────────────

    def _write_json(self, ctx: UpdaterContext) -> None:
        dest = ctx.app_dir / "data" / "updater.json"
        payload = {
            "app":     ctx.app_name,
            "url":     self.url,
            "channel": self.channel,
        }
        content = json.dumps(payload, indent=2) + "\n"

        if ctx.verbose:
            print(f"  [updater]   {'(dry-run) ' if ctx.dry_run else ''}write data/updater.json")

        if not ctx.dry_run:
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(content, encoding="utf-8")

        # CMake: copy updater.json to binary data/ at build time.
        base = f"examples/apps/{ctx.app_name}"
        ctx.cmake_lines.append(
            f"sdlos_copy_resource({base}/data/updater.json  data/updater.json)"
        )

    def _copy_binary(self, ctx: UpdaterContext) -> None:
        if not self.binary:
            return

        src = Path(self.binary).expanduser().resolve()
        if not src.exists():
            print(f"  [updater]   warn: updater binary not found: {src}")
            return

        dest = ctx.app_dir / "data" / src.name

        if ctx.verbose:
            print(f"  [updater]   {'(dry-run) ' if ctx.dry_run else ''}copy {src.name} → data/")

        if not ctx.dry_run:
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dest)

        # CMake: copy the binary to the build output as well.
        base = f"examples/apps/{ctx.app_name}"
        ctx.cmake_lines.append(
            f"sdlos_copy_resource({base}/data/{src.name}  data/{src.name})"
        )

    # ── Repr ──────────────────────────────────────────────────────────────────

    def __repr__(self) -> str:
        return (
            f"Updater(url={self.url!r}, channel={self.channel!r}, "
            f"binary={self.binary!r})"
        )
