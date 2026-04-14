"""
sdlos.cli
=========
Click-based command-line interface for sdlos tooling.

Entry point (registered in pyproject.toml):
    sdlos = "sdlos.cli:main"

Commands
--------
  sdlos create   <name>    — scaffold a new jade app from a template
  sdlos run      <name>    — build and launch a jade app (with optional watch)
  sdlos pipeline [target]  — visualise a pipeline.pug render pipeline
  sdlos mesh     generate  — generate a 3D region mesh from OpenStreetMap data
  sdlos templates          — list available scaffold templates
  sdlos version            — print tooling version

Examples
--------
  sdlos create                          # fully interactive (default on a TTY)
  sdlos create hello                    # name supplied, prompts for the rest
  sdlos create viz  --template shader --win-w 1280 --win-h 800
  sdlos create mycam --template camera --data-dir --with-model assets/Cluster.glb
  sdlos create myapp --config tooling/config/app.yaml --dry-run
  sdlos create myapp --no-interactive   # CI / scripting — no prompts at all

  sdlos run calc                        # build + launch (incremental)
  sdlos run calc --watch                # build + launch + auto-rebuild on change
  sdlos run calc --clean                # --clean-first rebuild, then launch
  sdlos run calc --build-dir ../mybuild # explicit cmake binary dir
  sdlos run calc --no-build             # skip build, just launch

  sdlos mesh generate --place "Mainz, Germany" --name mainz
  sdlos mesh generate --bbox "50.05,49.90,8.35,8.15" --name mainz_bbox
  sdlos mesh generate --place "Altstadt, Mainz" --name mainz_lp \
          --lod lowpoly --face-count 3000 --dem --app flatshader
  sdlos mesh info output/mainz.gltf

  sdlos pipeline                        # auto-detect pipeline.pug from cwd
  sdlos pipeline viz                    # by app name
  sdlos pipeline data/pipeline.pug      # explicit path
  sdlos pipeline viz --watch            # live-reload on file change
  sdlos pipeline viz --no-params        # compact view, omit Bucket-C params
"""
from __future__ import annotations

import sys
from pathlib import Path
from typing import Optional

import click

from . import __version__
from .config.schema import AppConfig
from .core.cmake import find_project_root
from .core.console import console, print_banner
from .core.naming import validate_name
from .templates.renderer import list_available
from .commands.run import run_app
from .commands.pipeline import cmd_pipeline
from .commands.mesh import cmd_mesh
from .commands.build import cmd_build


def _is_tty() -> bool:
    """Return True when both stdin and stdout are connected to a terminal."""
    return sys.stdin.isatty() and sys.stdout.isatty()


# ── Shared helpers ────────────────────────────────────────────────────────────

def _resolve_build_dir(build_dir: Optional[Path], project_root: Path) -> Path:
    """Return the cmake binary directory, defaulting to <project_root>/build."""
    return (build_dir or project_root / "build").resolve()


# ── Root group ────────────────────────────────────────────────────────────────

@click.group(
    context_settings={"help_option_names": ["-h", "--help"]},
    invoke_without_command=True,
)
@click.pass_context
def main(ctx: click.Context) -> None:
    """sdlos jade-app scaffolding and release tooling."""
    if ctx.invoked_subcommand is None:
        print_banner(
            "sdlos tooling",
            subtitle=f"v{__version__}  ·  create · run · build · pipeline · mesh · templates",
        )
        click.echo(ctx.get_help())


# ── sdlos version ─────────────────────────────────────────────────────────────

@main.command("version")
def cmd_version() -> None:
    """Print the sdlos tooling version and exit."""
    console.print(
        f"[bold white]sdlos-tooling[/]  [cyan]{__version__}[/]"
    )


# ── sdlos templates ───────────────────────────────────────────────────────────

@main.command("templates")
def cmd_templates() -> None:
    """List available scaffold templates."""
    available = list_available()
    if not available:
        console.print("[yellow]No templates found.[/]")
        return
    console.print("\n[bold]Available templates[/]")
    for name in available:
        console.print(f"  [bold cyan]{name}[/]")
    console.print()


# ── sdlos pipeline / mesh / build ────────────────────────────────────────────

main.add_command(cmd_pipeline)
main.add_command(cmd_mesh)
main.add_command(cmd_build)

# ── sdlos run ─────────────────────────────────────────────────────────────────

@main.command("run")
@click.argument("name", metavar="NAME")
@click.option(
    "--build-dir", "-b",
    type=click.Path(path_type=Path),
    default=None, metavar="DIR",
    help="cmake binary directory [default: <project-root>/build].",
)
@click.option(
    "--clean",
    is_flag=True, default=False,
    help="Pass --clean-first to cmake (full recompile, no dep re-fetch).",
)
@click.option(
    "--watch", "-w",
    is_flag=True, default=False,
    help="Watch app sources and rebuild + relaunch on every file change.",
)
@click.option(
    "--jobs", "-j",
    type=int, default=None, metavar="N",
    help="Parallel build jobs (-j N). Default: cmake decides.",
)
@click.option(
    "--no-build",
    is_flag=True, default=False,
    help="Skip the cmake build step and jump straight to launching.",
)
@click.option(
    "--quiet", "-q",
    is_flag=True, default=False,
    help="Suppress sdlos-tooling status lines (cmake output is unaffected).",
)
@click.option(
    "--reconfigure", "-r",
    is_flag=True, default=False,
    help=(
        "Re-run cmake configure before building.  "
        "Use this after `sdlos create` on an already-configured build "
        "so the new <name>.cmake is picked up by the root glob.  "
        "Configure also runs automatically when the build dir is absent "
        "or contains no CMakeCache.txt."
    ),
)
@click.option(
    "--preset",
    default=None, metavar="PRESET",
    help=(
        "cmake preset name to use during configure "
        "(e.g. macos-debug, macos-release).  "
        "Runs `cmake --preset PRESET` instead of `cmake -B <dir> -S .`.  "
        "Only used when a configure step actually runs."
    ),
)
def cmd_run(
    name: str,
    build_dir: Optional[Path],
    clean: bool,
    watch: bool,
    jobs: Optional[int],
    no_build: bool,
    quiet: bool,
    reconfigure: bool,
    preset: Optional[str],
) -> None:
    """Build and launch the jade app NAME.

    NAME must be the snake_case app name used when the app was created.

    \b
    Examples
    --------
      sdlos run calc                           incremental build + launch
      sdlos run calc --watch                   rebuild + relaunch on source change
      sdlos run calc --clean                   cmake --clean-first, then launch
      sdlos run calc --reconfigure             re-configure then build (after sdlos create)
      sdlos run calc --reconfigure --preset macos-debug
      sdlos run calc --build-dir ../out        use an explicit cmake binary dir
      sdlos run calc --no-build                skip build, just launch
      sdlos run calc -j 8 --watch              parallel build in watch loop
    """
    root_dir = find_project_root(Path.cwd())

    try:
        run_app(
            name=name,
            project_root=root_dir,
            build_dir=_resolve_build_dir(build_dir, root_dir),
            clean=clean,
            watch=watch,
            jobs=jobs,
            no_build=no_build,
            quiet=quiet,
            reconfigure=reconfigure,
            preset=preset,
        )
    except SystemExit:
        raise
    except Exception as exc:  # noqa: BLE001
        raise click.ClickException(str(exc)) from exc


# ── sdlos create ─────────────────────────────────────────────────────────────

@main.command("create")
@click.argument("name", metavar="NAME", required=False, default=None)
# ── Template ──────────────────────────────────────────────────────────────────
@click.option(
    "--template", "-t",
    type=click.Choice(["minimal", "shader", "camera", "pug", "vfs", "scene3d"]),
    default="minimal",
    show_default=True,
    metavar="TMPL",
    help="Starter template: minimal | shader | camera | pug | vfs | scene3d.",
)
# ── Window ────────────────────────────────────────────────────────────────────
@click.option(
    "--win-w",
    type=int, default=None,
    metavar="W",
    help="Debug window width in pixels (omit for engine default / fullscreen).",
)
@click.option(
    "--win-h",
    type=int, default=None,
    metavar="H",
    help="Debug window height in pixels (omit for engine default / fullscreen).",
)
# ── Scaffold ──────────────────────────────────────────────────────────────────
@click.option(
    "--data-dir",
    is_flag=True, default=False,
    help="Create a data/ skeleton (shaders/msl, shaders/spirv, img, models).",
)
@click.option(
    "--with-model",
    default=None, metavar="PATH",
    help="Copy a .glb/.gltf file into data/models/ during scaffold.",
)
@click.option(
    "--app-dir",
    default=None, metavar="DIR",
    help=(
        "Base directory for the generated app folder.  "
        "The app lands at <DIR>/<name>.  "
        "Default: examples/apps/<name> inside the project root."
    ),
)
# ── Safety ───────────────────────────────────────────────────────────────────
@click.option(
    "--overwrite",
    is_flag=True, default=False,
    help="Re-generate scaffold files (user regions between markers are preserved).",
)
@click.option(
    "--dry-run",
    is_flag=True, default=False,
    help="Print what would happen without touching the filesystem.",
)
@click.option(
    "--interactive/--no-interactive", "-i",
    default=None,
    help=(
        "Run interactive prompts for any value not already supplied.  "
        "Default: interactive when running in a terminal, "
        "--no-interactive otherwise (CI / pipes)."
    ),
)
@click.option(
    "--quiet", "-q",
    is_flag=True, default=False,
    help="Suppress per-file log lines.",
)
# ── Config file ───────────────────────────────────────────────────────────────
@click.option(
    "--config",
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
    default=None, metavar="YAML",
    help="Seed options from a YAML config file (CLI flags take priority).",
)
# ── Save config ───────────────────────────────────────────────────────────────
@click.option(
    "--save-config",
    type=click.Path(dir_okay=False, path_type=Path),
    default=None, metavar="YAML",
    help="After merging CLI + file options, write the resolved config to YAML.",
)
def cmd_create(
    name: Optional[str],
    template: Optional[str],
    win_w: Optional[int],
    win_h: Optional[int],
    data_dir: bool,
    with_model: Optional[str],
    app_dir: Optional[str],
    overwrite: bool,
    dry_run: bool,
    interactive: Optional[bool],
    quiet: bool,
    config: Optional[Path],
    save_config: Optional[Path],
) -> None:
    """Scaffold a new sdlos jade app named NAME.

    NAME must be snake_case (hyphens are converted automatically).
    If --config is given and NAME is omitted, the name is read from the file.

    \b
    Templates
    ---------
      minimal   empty jade_app_init stub with TODO markers
      shader    shader canvas + preset sidebar + +/- param controls
      camera    live video canvas + filter chips + xnum inputs
      pug       FrameGraph pipeline demo (pipeline.pug + CSS + Metal shaders + HUD)
      vfs       VFS explorer + audio player (MemMount, LocalMount, SDL3 audio)
      scene3d   glTF model viewer + floating labels + orbit camera

    \b
    Examples
    --------
      sdlos create                                  fully interactive on a TTY
      sdlos create hello                            prompts for template + window
      sdlos create viz  --template shader           prompts for window + data
      sdlos create mycam --template camera --data-dir --no-interactive
      sdlos create myapp --config tooling/config/app.yaml --dry-run
      sdlos create myvis --template pug             FrameGraph demo, data/ forced
      sdlos create myapp --app-dir ~/projects/apps  custom output directory
    """
    # ── Decide whether to run interactive prompts ─────────────────────────────
    # Explicit --interactive / --no-interactive always wins.
    # Otherwise: interactive when attached to a real terminal.
    run_interactive: bool = interactive if interactive is not None else _is_tty()

    # ── Seed defaults from YAML config file if supplied ───────────────────────
    # We do this early so interactive prompts can show YAML values as defaults.
    _config_base = AppConfig.from_yaml(config) if config else AppConfig()

    # Resolved template from CLI or config (None = still unknown)
    _template = template or (_config_base.template if config else None)

    # ── Interactive prompts — fill in anything not already supplied ───────────
    if run_interactive and not dry_run:
        # Determine which values are already "pinned" by the user.
        _pinned_data_dir: Optional[bool] = True if data_dir else (
            _config_base.data_dir if config else None
        )

        from .commands.interactive import prompt_missing
        resolved = prompt_missing(
            name=validate_name(name) if name else None,
            template=_template,
            win_w=win_w,
            win_h=win_h,
            data_dir=_pinned_data_dir,
            app_dir=app_dir,
        )
        name      = resolved["name"]
        _template = resolved["template"]
        win_w     = resolved["win_w"]
        win_h     = resolved["win_h"]
        data_dir  = resolved["data_dir"]
        app_dir   = resolved["app_dir"]

    elif not name:
        # Non-interactive and no name → must have it from --config or fail.
        if not _config_base.name:
            raise click.UsageError(
                "App name is required.  Pass it as an argument, use "
                "--interactive, or set 'name:' in your --config YAML file."
            )

    # ── Build final AppConfig ─────────────────────────────────────────────────
    try:
        cfg = AppConfig.from_cli(
            name=validate_name(name) if name else _config_base.name,
            template=_template,
            win_w=win_w,
            win_h=win_h,
            data_dir=data_dir,
            overwrite=overwrite,
            dry_run=dry_run,
            with_model=with_model,
            app_dir=app_dir,
            config_path=config,
        )
    except (ValueError, KeyError) as exc:
        raise click.UsageError(str(exc)) from exc

    cfg.verbose = not quiet

    # ── Final name sanity check ───────────────────────────────────────────────
    if not cfg.name:
        raise click.UsageError(
            "App name is required.  Pass it as an argument or set 'name:' "
            "in your --config YAML file."
        )
    try:
        cfg.name = validate_name(cfg.name)
    except click.BadParameter as exc:
        raise click.UsageError(str(exc)) from exc

    # ── Optionally save resolved config ───────────────────────────────────────
    if save_config is not None:
        save_config.parent.mkdir(parents=True, exist_ok=True)
        save_config.write_text(cfg.to_yaml(), encoding="utf-8")
        if cfg.verbose:
            click.echo(f"  [config]    saved → {save_config}")

    # ── Locate project root ───────────────────────────────────────────────────
    root_dir = find_project_root(Path.cwd())

    # ── Run create ────────────────────────────────────────────────────────────
    from .commands.create import create_app

    try:
        create_app(cfg, root_dir)
    except Exception as exc:  # noqa: BLE001
        raise click.ClickException(str(exc)) from exc

    # ── Optional: reconfigure so the new target is immediately known ──────────
    # When the build directory is already configured (CMakeCache.txt present)
    # the cmake glob that discovers app .cmake files will NOT pick up the new
    # file until cmake re-runs.  Offer a one-time reconfigure on interactive
    # terminals so the user can go straight to `sdlos run <name>` without the
    # extra --reconfigure flag.
    if not cfg.dry_run and _is_tty():
        build_dir = _resolve_build_dir(None, root_dir)
        if (build_dir / "CMakeCache.txt").exists():
            try:
                import questionary as _q
                from .commands.interactive import _STYLE
                should_reconfigure = _q.confirm(
                    f"Build system already configured — reconfigure now to register '{cfg.name}'?",
                    default=True,
                    style=_STYLE,
                ).ask()
            except ImportError:
                # questionary not available (e.g. minimal install) — fall back to input()
                try:
                    raw = input(
                        f"\n  Reconfigure cmake now to register '{cfg.name}'? [Y/n] "
                    ).strip().lower()
                    should_reconfigure = raw in ("", "y", "yes")
                except (EOFError, KeyboardInterrupt):
                    should_reconfigure = False

            if should_reconfigure:
                from .commands.run import _configure
                _configure(root_dir, build_dir, preset=None, quiet=False)
