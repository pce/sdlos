"""
sdlos.cli
=========
Click-based command-line interface for sdlos tooling.

Entry point (registered in pyproject.toml):
    sdlos = "sdlos.cli:main"

Commands
--------
  sdlos create <name>   — scaffold a new jade app
  sdlos templates       — list available scaffold templates
  sdlos version         — print tooling version

Examples
--------
  sdlos create                          # fully interactive (default on a TTY)
  sdlos create hello                    # name supplied, prompts for the rest
  sdlos create viz  --template shader --win-w 1280 --win-h 800
  sdlos create mycam --template camera --data-dir --with-model assets/Cluster.glb
  sdlos create myapp --config tooling/config/app.yaml --dry-run
  sdlos create myapp --no-interactive   # CI / scripting — no prompts at all
"""
from __future__ import annotations

import sys
from pathlib import Path
from typing import Optional

import click

from . import __version__
from .config.schema import AppConfig
from .core.cmake import find_project_root
from .core.naming import validate_name
from .templates.renderer import list_available


def _is_tty() -> bool:
    """Return True when both stdin and stdout are connected to a terminal."""
    return sys.stdin.isatty() and sys.stdout.isatty()


# ── Root group ────────────────────────────────────────────────────────────────

@click.group(
    context_settings={"help_option_names": ["-h", "--help"]},
    invoke_without_command=True,
)
@click.pass_context
def main(ctx: click.Context) -> None:
    """sdlos jade-app scaffolding and release tooling."""
    if ctx.invoked_subcommand is None:
        click.echo(ctx.get_help())


# ── sdlos version ─────────────────────────────────────────────────────────────

@main.command("version")
def cmd_version() -> None:
    """Print the sdlos tooling version and exit."""
    click.echo(f"sdlos-tooling {__version__}")


# ── sdlos templates ───────────────────────────────────────────────────────────

@main.command("templates")
def cmd_templates() -> None:
    """List available scaffold templates."""
    available = list_available()
    if not available:
        click.echo("No templates found.")
        return
    click.echo("Available templates:")
    for name in available:
        click.echo(f"  {name}")


# ── sdlos create ─────────────────────────────────────────────────────────────

@main.command("create")
@click.argument("name", metavar="NAME", required=False, default=None)
# ── Template ──────────────────────────────────────────────────────────────────
@click.option(
    "--template", "-t",
    type=click.Choice(["minimal", "shader", "camera"]),
    default="minimal",
    show_default=True,
    metavar="TMPL",
    help="Starter template: minimal | shader | camera.",
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
      camera    live video canvas + filter chips + dragnum inputs

    \b
    Examples
    --------
      sdlos create                                  fully interactive on a TTY
      sdlos create hello                            prompts for template + window
      sdlos create viz  --template shader           prompts for window + data
      sdlos create mycam --template camera --data-dir --no-interactive
      sdlos create myapp --config tooling/config/app.yaml --dry-run
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
        )
        name     = resolved["name"]
        _template = resolved["template"]
        win_w    = resolved["win_w"]
        win_h    = resolved["win_h"]
        data_dir = resolved["data_dir"]

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


# ── Standalone entry point ────────────────────────────────────────────────────

if __name__ == "__main__":
    sys.exit(main())
