"""
sdlos.commands.create
=====================
``create_app(cfg, root_dir)`` — scaffold a new sdlos jade app from config.

Steps
-----
1. Validate the AppConfig.
2. Create the app directory (examples/apps/<name>/).
3. Optionally scaffold the data/ skeleton and copy a model file.
4. Render and write jade / css / behavior_cc from the chosen template.
   On --overwrite, existing user regions are spliced into the new output.
5. Write <name>.cmake into the app directory — picked up automatically by
   the root CMakeLists.txt glob, no manual edits needed.
6. Print a summary with next steps.
"""
from __future__ import annotations

import shutil
from pathlib import Path
from typing import Optional

from ..assets import gitignore as _gi
from ..assets import pipeline_templates as _pt
from ..assets import png as _png
from ..assets import shaders as _shaders
from ..config.schema import AppConfig
from ..core.cmake import cmake_snippet, write_app_cmake
from ..core.console import (
    print_banner,
    print_done,
    print_dryrun_notice,
    print_item,
    print_kv_table,
    print_warn,
)
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

# Maps data/ subdirectory path → gitignore content to write there.
# Applied to every template that requests a data directory.
_SUBDIR_GITIGNORES: dict[str, str] = {
    ".":              _gi.DATA_ROOT,
    "img":            _gi.IMG,
    "models":         _gi.MODELS,
    "shaders":        _gi.SHADERS,
    "shaders/spirv":  _gi.SPIRV,
}


def _write_text(path: Path, content: str, cfg: AppConfig, label: str) -> None:
    """Write *content* to *path*, respecting dry_run and overwrite flags."""
    if cfg.dry_run:
        if cfg.verbose:
            print_item("dry-run", label)
        return
    if path.exists() and not cfg.overwrite:
        if cfg.verbose:
            print_item("skip", label, detail="(exists)", dim=True)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    if cfg.verbose:
        print_item("create", label)


def _write_bytes(path: Path, data: bytes, cfg: AppConfig, label: str) -> None:
    """Write binary *data* to *path*, respecting dry_run and overwrite flags."""
    if cfg.dry_run:
        if cfg.verbose:
            print_item("dry-run", label)
        return
    if path.exists() and not cfg.overwrite:
        if cfg.verbose:
            print_item("skip", label, detail="(exists)", dim=True)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    if cfg.verbose:
        print_item("create", label)


def _scaffold_data_dir(data_dir: Path, cfg: AppConfig) -> None:
    """Create examples/apps/<name>/data/ skeleton with bootstrap content.

    The data directory is *colocated* with the app source directory so that
    every app is a self-contained, independently buildable unit::

        examples/apps/<name>/         — source files + <name>.cmake
        examples/apps/<name>/data/    — shaders, images, models …

    Bootstrap content written per template
    ---------------------------------------
    All templates
        .gitignore files in data/, img/, models/, shaders/, shaders/spirv/
        (no .gitkeep — every directory has real content or a .gitignore)

    shader template
        data/img/canvas.png             — soft half-opaque white dot (256 × 256)
        data/shaders/msl/preset_a.frag.metal
        data/shaders/msl/preset_b.frag.metal

    camera template
        data/shaders/msl/cinematic.frag.metal
    """
    subdirs_label = ", ".join(_DATA_SUBDIRS)
    data_rel = f"examples/apps/{cfg.name}/data"

    if cfg.dry_run:
        if cfg.verbose:
            print_item("dry-run", f"{data_rel}/", detail=f"({subdirs_label})")
    else:
        for sub in _DATA_SUBDIRS:
            (data_dir / sub).mkdir(parents=True, exist_ok=True)
        if cfg.verbose:
            print_item("mkdir", f"{data_rel}/", detail=f"({subdirs_label})")

    # ── .gitignore files (replace .gitkeep) ───────────────────────────────────
    for subdir, content in _SUBDIR_GITIGNORES.items():
        target_dir = data_dir if subdir == "." else data_dir / subdir
        _write_text(
            target_dir / ".gitignore",
            content,
            cfg,
            f"{data_rel}/{subdir}/.gitignore" if subdir != "." else f"{data_rel}/.gitignore",
        )

    # ── Template-specific bootstrap assets ────────────────────────────────────

    # pipeline.pug + pipeline.css — FrameGraph descriptor for the pug template.
    # These live at data/ (not data/shaders/) so the renderer auto-loads them.
    if cfg.template == "pug":
        _write_text(
            data_dir / "pipeline.pug",
            _pt.starter_pipeline_pug(cfg.name),
            cfg,
            f"{data_rel}/pipeline.pug",
        )
        _write_text(
            data_dir / "pipeline.css",
            _pt.STARTER_PIPELINE_CSS,
            cfg,
            f"{data_rel}/pipeline.css",
        )

    # canvas.png — soft half-opaque white dot used as src= on the shader canvas
    if cfg.template == "shader":
        canvas_png = _png.dot(size=256, peak_alpha=128, color=(255, 255, 255))
        _write_bytes(
            data_dir / "img" / "canvas.png",
            canvas_png,
            cfg,
            f"{data_rel}/img/canvas.png",
        )

    # Metal fragment shaders — preset_a / preset_b (shader) or cinematic (camera)
    msl_sources = _shaders.starter_msl(cfg.template)
    for filename, source in msl_sources.items():
        _write_text(
            data_dir / "shaders" / "msl" / filename,
            source,
            cfg,
            f"{data_rel}/shaders/msl/{filename}",
        )

    # ── Optional: copy a model file ───────────────────────────────────────────
    if cfg.with_model:
        src = Path(cfg.with_model).expanduser().resolve()
        if not src.exists():
            print_warn(f"--with-model path not found: {src}")
            return
        dest = data_dir / "models" / src.name
        if dest.exists() and not cfg.overwrite:
            if cfg.verbose:
                print_item("skip", dest.name, detail="(model already exists)", dim=True)
            return
        if cfg.dry_run:
            if cfg.verbose:
                print_item("dry-run", f"{src.name} → {data_rel}/models/")
        else:
            shutil.copy2(src, dest)
            if cfg.verbose:
                print_item("copy", f"{src.name} → {data_rel}/models/")


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


# ── Post-processing hook ───────────────────────────────────────────────────────

def _post_process_generated(app_dir: Path, cfg: AppConfig) -> None:
    """Run the template post-processing pipeline on the generated C++ file.

    Applies (in order):
      1. clang-format — normalise whitespace / brace style.
      2. SCA (level 2) — flag unsafe APIs and raw pointer interfaces.
      3. Docblocks — insert Doxygen docblocks for un-documented functions.

    The pipeline is intentionally lenient: if any processor is unavailable
    (libclang not installed, clang-format not on PATH, …) it reports the
    error as a warning and continues.  Scaffold output is never blocked.

    Only the ``behavior_cc`` file is processed — the jade and CSS files
    are not C++ and would confuse clang-format / libclang.
    """
    cxx_file = app_dir / output_filename("behavior_cc", cfg.name)
    if not cxx_file.exists():
        return

    try:
        from ..features.post_process import make_template_pipeline
    except ImportError:
        if cfg.verbose:
            print_warn("post-process skipped: sdlos.features.post_process not available")
        return

    # Build a lightweight console so the pipeline can print progress lines.
    _console = None
    if cfg.verbose:
        try:
            from rich.console import Console as _RichConsole
            _console = _RichConsole()
        except ImportError:
            pass

    pipeline = make_template_pipeline(
        sca_level=2,
        format=True,
        docblocks=True,
        console=_console,
    )

    results = pipeline.run(cxx_file)

    if cfg.verbose:
        for result in results:
            if result.error:
                print_warn(
                    f"  [{result.processor}] {result.error}"
                )
            elif result.issues:
                n = len(result.issues)
                print_item(
                    "sca",
                    cxx_file.name,
                    detail=f"({n} issue(s) — run 'sdlos analyze' for details)",
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

    Source path is relative to the project root (examples/apps/<name>/data/…).
    Destination path is relative to the binary directory (data/models/…).
    """
    if not (cfg.data_dir and cfg.with_model):
        return None
    model_name = Path(cfg.with_model).name
    data_base = f"examples/apps/{cfg.name}/data"
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

    # The pug template always requires a data/ directory — pipeline.pug,
    # pipeline.css, and compiled shader binaries all live there.
    # Force data_dir=True so _scaffold_data_dir() always runs for this template.
    if cfg.template == "pug":
        cfg.data_dir = True

    if cfg.app_dir:
        app_dir = Path(cfg.app_dir).expanduser().resolve() / cfg.name
    else:
        app_dir = root_dir / "examples" / "apps" / cfg.name
    data_dir = app_dir / "data"

    _print_header(cfg, app_dir)

    # ── Directories ────────────────────────────────────────────────────────────
    if not cfg.dry_run:
        app_dir.mkdir(parents=True, exist_ok=True)
    elif cfg.verbose:
        print_item("dry-run", f"would mkdir {app_dir}")

    if cfg.data_dir:
        _scaffold_data_dir(data_dir, cfg)

    # ── Source files ──────────────────────────────────────────────────────────
    _render_and_write(app_dir, cfg)

    # ── Post-process the generated .cxx behavior file ─────────────────────────
    # Runs clang-format → SCA → docblocks on the freshly written file.
    # Skipped in dry_run mode (nothing was written to disk yet).
    if not cfg.dry_run:
        _post_process_generated(app_dir, cfg)

    # ── CMake — write <name>.cmake alongside the source files ─────────────────
    _write_app_cmake(app_dir, cfg)

    # ── Done ──────────────────────────────────────────────────────────────────
    _print_footer(cfg, app_dir)


# ── Pretty printing ───────────────────────────────────────────────────────────

def _print_header(cfg: AppConfig, app_dir: Path) -> None:
    data_dir = app_dir / "data"

    if cfg.win_w is not None and cfg.win_h is not None:
        win_label = f"{cfg.win_w} × {cfg.win_h}"
    else:
        win_label = "fullscreen / engine default"

    subtitle = f"template: {cfg.template}  ·  {win_label}"
    print_banner("sdlos create", subtitle=subtitle)

    rows: list[tuple[str, str]] = [
        ("name",     f"{cfg.name}  ({pascal(cfg.name)})"),
        ("template", cfg.template),
        ("window",   win_label),
        ("source",   str(app_dir)),
    ]
    if cfg.app_dir:
        rows.append(("app-dir", str(app_dir.parent)))
    if cfg.data_dir:
        rows.append(("data", str(data_dir)))
    print_kv_table(rows)

    if cfg.dry_run:
        print_dryrun_notice()


def _print_footer(cfg: AppConfig, app_dir: Path) -> None:
    if cfg.dry_run:
        return

    next_steps: list[str] = []
    if cfg.data_dir:
        next_steps.append(f"Add shaders/assets to  {app_dir / 'data'}")
    next_steps.append(f"cmake --build build --target {cfg.name}")
    next_steps.append(f"sdlos run {cfg.name}")
    print_done(cfg.name, next_steps=next_steps)
