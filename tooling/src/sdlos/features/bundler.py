"""
sdlos.features.bundler
======================
Bundler feature — packs a built sdlos app into a distributable archive.

Context protocol
----------------
The ``apply(context)`` method receives a mutable dict with at minimum:

    context["app_dir"]    : Path  — examples/apps/<name>/
    context["build_dir"]  : Path  — project build output directory
    context["cfg"]        : AppConfig
    context["root_dir"]   : Path  — project root

The Bundler adds / mutates the following keys:

    context["bundle_path"] : Path  — path to the produced archive

Usage
-----
    from sdlos.features import Bundler

    bundler = Bundler(format="zip")
    bundler.apply(context)
"""
from __future__ import annotations

import shutil
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Literal

if TYPE_CHECKING:
    from ..config.schema import AppConfig


# ── Bundle format ─────────────────────────────────────────────────────────────

BundleFormat = Literal["zip", "tar.gz", "dir"]

_ARCHIVE_SUFFIXES: dict[str, str] = {
    "zip":    ".zip",
    "tar.gz": ".tar.gz",
    "dir":    "",        # no archive — just a directory copy
}


# ── Context helpers ───────────────────────────────────────────────────────────

def _require(ctx: dict, key: str) -> object:
    if key not in ctx:
        raise KeyError(
            f"Bundler.apply(): context is missing required key '{key}'. "
            "Make sure create_app() has populated the context before calling features."
        )
    return ctx[key]


# ── Bundler ───────────────────────────────────────────────────────────────────

@dataclass
class Bundler:
    """Pack a built sdlos app into a distributable archive.

    Parameters
    ----------
    format:
        Output format: ``zip``, ``tar.gz``, or ``dir`` (plain directory copy).
    include_data:
        If True (default), include the app's ``data/`` directory in the bundle.
    include_binary:
        If True (default), include the compiled app binary from the build dir.
    out_dir:
        Where to write the bundle.  Defaults to ``<build_dir>/dist/``.
    extra_paths:
        Additional files or directories to include, relative to ``root_dir``.
    """

    format: BundleFormat = "zip"
    include_data: bool = True
    include_binary: bool = True
    out_dir: Path | None = None
    extra_paths: list[str] = field(default_factory=list)

    # ── Public API ─────────────────────────────────────────────────────────────

    def apply(self, context: dict) -> None:
        """Pack the built app and write the result into *context['bundle_path']*."""
        cfg: AppConfig = _require(context, "cfg")         # type: ignore[assignment]
        app_dir: Path  = _require(context, "app_dir")     # type: ignore[assignment]
        build_dir: Path = _require(context, "build_dir")  # type: ignore[assignment]
        root_dir: Path  = _require(context, "root_dir")   # type: ignore[assignment]

        out_dir = self.out_dir or (build_dir / "dist")
        out_dir.mkdir(parents=True, exist_ok=True)

        bundle_name = f"{cfg.name}-{_version_tag(build_dir, cfg.name)}"
        suffix = _ARCHIVE_SUFFIXES[self.format]
        bundle_path = out_dir / (bundle_name + suffix)

        sources = self._collect_sources(cfg, app_dir, build_dir, root_dir)

        if self.format == "dir":
            self._write_dir(bundle_path, sources)
        elif self.format == "zip":
            self._write_zip(bundle_path, sources)
        elif self.format == "tar.gz":
            self._write_targz(bundle_path, sources)
        else:
            raise ValueError(f"Unknown bundle format: {self.format!r}")

        context["bundle_path"] = bundle_path
        print(f"  [bundle]    {bundle_path.name}  ({_human_size(bundle_path)})")

    # ── Source collection ──────────────────────────────────────────────────────

    def _collect_sources(
        self,
        cfg: "AppConfig",
        app_dir: Path,
        build_dir: Path,
        root_dir: Path,
    ) -> list[tuple[Path, str]]:
        """Return a list of (absolute_path, archive_name) pairs to bundle."""
        sources: list[tuple[Path, str]] = []

        if self.include_binary:
            binary = _find_binary(build_dir, cfg.name)
            if binary and binary.exists():
                sources.append((binary, binary.name))
            else:
                print(f"  [warn]      binary not found for '{cfg.name}' in {build_dir}")

        if self.include_data:
            data_dir = app_dir / "data"
            if data_dir.exists():
                for child in sorted(data_dir.rglob("*")):
                    if child.is_file():
                        rel = child.relative_to(app_dir)
                        sources.append((child, str(rel)))
            else:
                print(f"  [warn]      data/ directory not found: {data_dir}")

        for extra in self.extra_paths:
            p = (root_dir / extra).resolve()
            if p.exists():
                if p.is_dir():
                    for child in sorted(p.rglob("*")):
                        if child.is_file():
                            sources.append((child, child.name))
                else:
                    sources.append((p, p.name))
            else:
                print(f"  [warn]      extra path not found: {p}")

        return sources

    # ── Writers ────────────────────────────────────────────────────────────────

    @staticmethod
    def _write_zip(dest: Path, sources: list[tuple[Path, str]]) -> None:
        with zipfile.ZipFile(dest, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            for src, arc_name in sources:
                zf.write(src, arc_name)

    @staticmethod
    def _write_targz(dest: Path, sources: list[tuple[Path, str]]) -> None:
        import tarfile
        with tarfile.open(dest, "w:gz") as tf:
            for src, arc_name in sources:
                tf.add(src, arcname=arc_name)

    @staticmethod
    def _write_dir(dest: Path, sources: list[tuple[Path, str]]) -> None:
        if dest.exists():
            shutil.rmtree(dest)
        dest.mkdir(parents=True)
        for src, arc_name in sources:
            out = dest / arc_name
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, out)


# ── Utilities ─────────────────────────────────────────────────────────────────

def _find_binary(build_dir: Path, name: str) -> Path | None:
    """Heuristically locate the compiled binary for *name* inside *build_dir*."""
    # Common CMake output locations — adjust if your project uses a custom layout.
    candidates = [
        build_dir / name,
        build_dir / f"{name}.app" / "Contents" / "MacOS" / name,  # macOS bundle
        build_dir / "bin" / name,
        build_dir / "Debug" / name,
        build_dir / "Release" / name,
    ]
    for c in candidates:
        if c.exists():
            return c
    # Fallback: recursive search (slower, only if heuristics fail).
    found = list(build_dir.rglob(name))
    return found[0] if found else None


def _version_tag(build_dir: Path, name: str) -> str:
    """Return a short version/date tag for the bundle filename.

    Tries to read a VERSION file from the build dir; falls back to a date stamp.
    """
    version_file = build_dir / "VERSION"
    if version_file.exists():
        return version_file.read_text(encoding="utf-8").strip()
    from datetime import date
    return date.today().strftime("%Y%m%d")


def _human_size(path: Path) -> str:
    """Return a human-readable file size string."""
    size = path.stat().st_size
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024:
            return f"{size:.1f} {unit}"
        size /= 1024
    return f"{size:.1f} TB"
