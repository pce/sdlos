"""
sdlos.commands.create
=====================
``create_app(cfg, root_dir)`` — scaffold a new sdlos jade app from config.

Steps
-----
1. Validate the AppConfig.
2. Create the app directory (examples/apps/<name>/).
3. Optionally scaffold the data/ skeleton and copy a model file.
4. Render and write jade / css / behavior_cxx from the chosen template.
   On --overwrite, existing user regions are spliced into the new output.
5. Write <name>.cmake into the app directory — picked up automatically by
   the root CMakeLists.txt glob, no manual edits needed.
6. Print a summary with next steps.
"""
from __future__ import annotations

import shutil
from pathlib import Path
from typing import Optional

from ..config.schema import AppConfig
from ..core.cmake import cmake_snippet, write_app_cmake
from ..core.fs import USER_BEGIN, USER_END, splice_user_regions, write_file_safe
from ..core.naming import pascal
from ..templates.renderer import KINDS, output_filename, render_template


# ── Data-directory skeleton ───────────────────────────────────────────────────

_DATA_SUBDIRS = (
    "shaders/msl",
    "shaders/spirv",
    "img",
    "models",
)


def _scaffold_data_dir(data_dir: Path, cfg: AppConfig) -> None:
    """Create examples/data/<name>/ skeleton and optionally copy a model file.

    The data directory is *separate* from the app source directory so that
    source code (jade/css/cxx) and runtime assets live in distinct trees::

        examples/apps/<name>/   — source files + <name>.cmake
        examples/data/<name>/   — shaders, fonts, models, images …
    """
    subdirs_label = ", ".join(_DATA_SUBDIRS)

    if cfg.dry_run:
        _log(cfg.verbose, f"[dry-run] mkdir      examples/data/{cfg.name}/  ({subdirs_label})")
    else:
        for sub in _DATA_SUBDIRS:
            (data_dir / sub).mkdir(parents=True, exist_ok=True)
        gitkeep = data_dir / ".gitkeep"
        if not gitkeep.exists():
            gitkeep.touch()
        _log(cfg.verbose, f"[mkdir]     examples/data/{cfg.name}/  ({subdirs_label})")

    if cfg.with_model:
        src = Path(cfg.with_model).expanduser().resolve()
        if not src.exists():
            _log(True, f"[warn]      --with-model path not found: {src}")
            return
        dest = data_dir / "models" / src.name
        if dest.exists() and not cfg.overwrite:
            _log(cfg.verbose, f"[skip]      {dest.name}  (model already exists)")
            return
        if cfg.dry_run:
            _log(cfg.verbose, f"[dry-run] copy      {src.name} → examples/data/{cfg.name}/models/")
        else:
            shutil.copy2(src, dest)
            _log(cfg.verbose, f"[copy]      {src.name} → examples/data/{cfg.name}/models/")


# ── File generation ───────────────────────────────────────────────────────────

def _render_and_write(app_dir: Path, cfg: AppConfig) -> None:
    """Render all template kinds and write them with safe overwrite handling."""
    for kind in KINDS:
        filename = output_filename(kind, cfg.name)
        dest = app_dir / filename

        rendered = render_template(cfg.template, kind, cfg)

        # On overwrite: splice user regions from existing file into the
        # freshly rendered template so the developer's code is preserved.
        if dest.exists() and cfg.overwrite:
            existing = dest.read_text(encoding="utf-8")
            rendered = splice_user_regions(rendered, existing)

        write_file_safe(
            dest,
            rendered,
            overwrite=cfg.overwrite,
            dry_run=cfg.dry_run,
            verbose=cfg.verbose,
        )


# ── CMake ─────────────────────────────────────────────────────────────────────

def _write_app_cmake(app_dir: Path, cfg: AppConfig) -> None:
    """Write ``<name>.cmake`` into *app_dir*.

    The root CMakeLists.txt discovers it automatically via::

        file(GLOB _app_cmake_files "examples/apps/*/*.cmake")
        foreach(_f ${_app_cmake_files})
            include(${_f})
        endforeach()

    No manual CMakeLists.txt edits are ever needed.
    """
    snippet = cmake_snippet(
        cfg.name,
        cfg.win_w,
        cfg.win_h,
        cfg.data_dir,
        extra_resources=_extra_cmake_resources(cfg),
    )
    write_app_cmake(app_dir, snippet, cfg)


def _extra_cmake_resources(cfg: AppConfig) -> Optional[list[str]]:
    """Return extra sdlos_copy_resource_to lines if a model was provided.

    Source path is relative to the project root (examples/data/<name>/…).
    Destination path is relative to the binary directory (data/models/…).
    """
    if not (cfg.data_dir and cfg.with_model):
        return None
    model_name = Path(cfg.with_model).name
    data_base = f"examples/data/{cfg.name}"
    return [
        f"sdlos_copy_resource_to({cfg.name} "
        f'"{data_base}/models/{model_name}" '
        f'"data/models/{model_name}")',
    ]


# ── Public entry point ────────────────────────────────────────────────────────

def create_app(cfg: AppConfig, root_dir: Path) -> None:
    """Scaffold a new sdlos jade app according to *cfg* under *root_dir*.

    Parameters
    ----------
    cfg:
        Validated :class:`~sdlos.config.schema.AppConfig`.
    root_dir:
        Absolute path to the sdlos project root (contains CMakeLists.txt).
    """
    cfg.validate()

    app_dir  = root_dir / "examples" / "apps" / cfg.name
    data_dir = root_dir / "examples" / "data" / cfg.name

    _print_header(cfg, app_dir)

    # ── Directories ────────────────────────────────────────────────────────────
    if not cfg.dry_run:
        app_dir.mkdir(parents=True, exist_ok=True)
    else:
        _log(cfg.verbose, f"[dry-run]   would mkdir {app_dir}")

    if cfg.data_dir:
        _scaffold_data_dir(data_dir, cfg)

    # ── Source files ──────────────────────────────────────────────────────────
    _render_and_write(app_dir, cfg)

    # ── CMake — write <name>.cmake alongside the source files ─────────────────
    _write_app_cmake(app_dir, cfg)

    # ── Done ──────────────────────────────────────────────────────────────────
    _print_footer(cfg)


# ── Pretty printing ───────────────────────────────────────────────────────────

def _print_header(cfg: AppConfig, app_dir: Path) -> None:
    data_dir = app_dir.parent.parent / "data" / cfg.name
    print()
    print("sdlos create")
    print(f"  name      : {cfg.name}  ({pascal(cfg.name)})")
    print(f"  template  : {cfg.template}")
    if cfg.win_w is not None and cfg.win_h is not None:
        print(f"  window    : {cfg.win_w} × {cfg.win_h}")
    else:
        print( "  window    : fullscreen / engine default")
    print(f"  source    : {app_dir}")
    if cfg.data_dir:
        print(f"  data      : {data_dir}")
    if cfg.dry_run:
        print("  mode      : DRY RUN — no files will be written")
    print()


def _print_footer(cfg: AppConfig) -> None:
    print()
    if not cfg.dry_run:
        print("Done.  Next steps:")
        if cfg.data_dir:
            print(f"  Add shaders/assets to  examples/data/{cfg.name}/")
        print(f"  cmake --build build --target {cfg.name}")
        print()


def _log(verbose: bool, msg: str) -> None:
    if verbose:
        print(f"  {msg}")
