"""
sdlos.commands.run
==================
``run_app(cfg, project_root)`` — build, launch, and optionally watch a jade app.

Workflow
--------
  Normal (one-shot)::

      sdlos run calc
        → cmake --build build --target calc
        → ./build/calc

  Watch (live-reload loop)::

      sdlos run calc --watch
        → build + launch calc
        → on any source change inside examples/apps/calc/:
              kill current process
              cmake --build build --target calc
              re-launch

  Clean recompile (no dep re-fetch)::

      sdlos run calc --clean
        → cmake --build build --target calc --clean-first
        → ./build/calc

  Auto-configure (first run, or after ``sdlos create`` on an existing build)::

      sdlos run calc
        # build dir absent or unconfigured → cmake configure first, then build

      sdlos run calc --reconfigure
        # force re-configure before building (use after adding a new app)

      sdlos run calc --reconfigure --preset macos-debug
        # re-configure using a named CMakePresets.json entry

  The cmake cache is never deleted so FetchContent / vcpkg deps are not
  re-fetched.  ``--clean-first`` only wipes object files for the named
  target, keeping the engine and vendored libs intact.

Why auto-configure matters
--------------------------
The root CMakeLists.txt discovers jade app targets through a glob::

    file(GLOB _app_cmake_files "examples/apps/*/*.cmake")

This glob runs at cmake *configure* time, not at ``cmake --build`` time.
A freshly scaffolded app (``sdlos create <name>``) is invisible to the build
system until cmake has been re-run.  ``sdlos run`` handles this automatically:

- No build dir at all          → auto-configure (``-B build -S .`` or preset).
- Build dir lacks CMakeCache   → auto-configure (same).
- Build dir fully configured   → go straight to ``cmake --build``.
- ``--reconfigure`` flag       → always re-configure first, then build.

Notes
-----
- Executable lookup order (all without an extension on POSIX, ``.exe`` on
  Windows):

    1. <build>/<name>                — Ninja / Makefile (most common)
    2. <build>/Debug/<name>          — Xcode / MSVC Debug
    3. <build>/Release/<name>        — Xcode / MSVC Release
    4. <build>/RelWithDebInfo/<name>  — Xcode / MSVC RelWithDebInfo
    5. <build>/MinSizeRel/<name>

- Watch mode requires ``watchfiles`` (declared in pyproject.toml).
  It uses OS-native file events (FSEvents on macOS, inotify on Linux,
  ReadDirectoryChangesW on Windows) so CPU usage is negligible.
"""
from __future__ import annotations

import os
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional



@dataclass
class RunConfig:
    """Resolved options for a single ``sdlos run`` invocation."""

    name: str
    """The cmake target name (== app name)."""

    build_dir: Path
    """Absolute path to the cmake binary directory."""

    clean: bool = False
    """Pass ``--clean-first`` to cmake (full rebuild, no dep re-fetch)."""

    watch: bool = False
    """Watch source files and rebuild + relaunch on every change."""

    jobs: Optional[int] = None
    """Parallel build jobs.  ``None`` → let cmake decide (usually cpu count)."""

    no_build: bool = False
    """Skip the cmake build step and jump straight to launching."""

    quiet: bool = False
    """Suppress sdlos-tooling output (cmake itself still prints normally)."""

    reconfigure: bool = False
    """Re-run cmake configure before building.

    Use this after ``sdlos create`` on an existing (already-configured) build
    directory so the new ``<name>.cmake`` is picked up by the root glob.
    When the build directory does not exist or has no ``CMakeCache.txt``,
    configure runs automatically even without this flag.
    """

    preset: Optional[str] = None
    """cmake preset name to use when configuring (e.g. ``"macos-debug"``).

    Only used during the configure step.  ``None`` means plain
    ``cmake -B <build> -S <root>`` with no preset.  Ignored when the build
    directory is already configured and ``--reconfigure`` is not set.
    """

    # Derived / internal
    _watch_dir: Optional[Path] = field(default=None, init=False, repr=False)



_CONFIGS = ("", "Debug", "Release", "RelWithDebInfo", "MinSizeRel")
_EXE_SUFFIX = ".exe" if sys.platform == "win32" else ""


def _find_executable(name: str, build_dir: Path) -> Optional[Path]:
    """Return the first existing executable for *name* under *build_dir*.

    Checks single-config generator layout first (``<build>/<name>``), then
    multi-config sub-directories (``<build>/Debug/<name>``, etc.).
    """
    for cfg_subdir in _CONFIGS:
        candidate = (
            build_dir / cfg_subdir / (name + _EXE_SUFFIX)
            if cfg_subdir
            else build_dir / (name + _EXE_SUFFIX)
        )
        if candidate.is_file():
            return candidate
    return None



def _needs_configure(build_dir: Path) -> bool:
    """Return True when cmake has not yet been configured in *build_dir*.

    ``CMakeCache.txt`` is always written by cmake during configuration,
    regardless of the generator used (Ninja, Makefile, Xcode, …).  Its
    absence is therefore a reliable signal that the configure step has never
    been run in this directory.
    """
    return not (build_dir / "CMakeCache.txt").exists()


def _configure(
    project_root: Path,
    build_dir: Path,
    preset: Optional[str],
    quiet: bool,
) -> bool:
    """Run the cmake configure step.  Returns ``True`` on success.

    With *preset*:   ``cmake --preset <preset>``
    Without preset:  ``cmake -B <build_dir> -S <project_root>``

    Note: ``cmake --preset`` uses the ``binaryDir`` defined in the preset
    (all sdlos presets point to ``build/``).  ``cmake -B / -S`` writes to
    the explicit *build_dir*.
    """
    if preset:
        cmd: list[str] = ["cmake", "--preset", preset]
    else:
        cmd = ["cmake", "-B", str(build_dir), "-S", str(project_root)]

    if not quiet:
        _echo(f"  [configure] {' '.join(cmd)}")

    result = subprocess.run(cmd, cwd=str(project_root))
    return result.returncode == 0



def _build(run_cfg: RunConfig, project_root: Path) -> bool:
    """Run ``cmake --build`` for the target.  Returns ``True`` on success."""
    cmd: list[str] = [
        "cmake", "--build", str(run_cfg.build_dir),
        "--target", run_cfg.name,
    ]
    if run_cfg.clean:
        cmd.append("--clean-first")
    if run_cfg.jobs is not None:
        cmd.extend(["-j", str(run_cfg.jobs)])

    if not run_cfg.quiet:
        _echo(f"  [build]  cmake --build {run_cfg.build_dir.name} --target {run_cfg.name}"
              + (" --clean-first" if run_cfg.clean else "")
              + (f" -j {run_cfg.jobs}" if run_cfg.jobs else ""))

    result = subprocess.run(cmd, cwd=str(project_root))
    return result.returncode == 0



def _launch(exe: Path, quiet: bool) -> subprocess.Popen:
    """Start *exe* in a child process and return the Popen handle."""
    if not quiet:
        _echo(f"  [launch] {exe}")
    # On macOS / Linux run the binary directly.
    # Pass through current env so SDL_VIDEODRIVER, display env vars etc. work.
    return subprocess.Popen([str(exe)], env=os.environ.copy())


def _wait_or_kill(proc: subprocess.Popen, timeout: float = 5.0) -> None:
    """Politely terminate *proc*, escalating to SIGKILL after *timeout* s."""
    if proc.poll() is not None:
        return  # already exited
    proc.terminate()
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        _echo("  [warn]   process did not terminate; sending SIGKILL")
        proc.kill()
        proc.wait()



def _watch_loop(run_cfg: RunConfig, project_root: Path, exe: Path) -> None:
    """Build → launch → watch → rebuild → relaunch.  Runs until Ctrl-C."""
    watch_dir = run_cfg._watch_dir or (project_root / "examples" / "apps" / run_cfg.name)

    try:
        from watchfiles import watch as _wf_watch
    except ImportError:
        _echo(
            "  [error]  watchfiles is not installed.\n"
            "           Run:  pip install watchfiles  (or reinstall sdlos-tooling)"
        )
        sys.exit(1)

    proc = _launch(exe, run_cfg.quiet)
    _echo(f"  [watch]  {watch_dir}  (Ctrl-C to stop)")

    try:
        for _changes in _wf_watch(str(watch_dir)):
            changed_names = sorted({Path(p).name for _, p in _changes})
            _echo(f"  [change] {', '.join(changed_names)}")

            _echo("  [stop]   stopping current process …")
            _wait_or_kill(proc)

            _echo("  [build]  rebuilding …")
            if not _build(run_cfg, project_root):
                _echo("  [warn]   build failed — keeping previous binary, relaunching")

            # Rediscover exe in case the binary moved (e.g. cmake config change)
            new_exe = _find_executable(run_cfg.name, run_cfg.build_dir) or exe
            proc = _launch(new_exe, run_cfg.quiet)

    except KeyboardInterrupt:
        _echo("\n  [stop]   Ctrl-C received — shutting down")
        _wait_or_kill(proc)


# Ensure .cmake registration file exists

def _find_app_dir(name: str, project_root: Path) -> Optional[Path]:
    """Return the app directory if it exists, or None."""
    apps_root = project_root / "examples" / "apps"
    candidate = apps_root / name
    if candidate.is_dir():
        return candidate
    return None


def _find_cmake_file(name: str, project_root: Path) -> Optional[Path]:
    """Return the .cmake registration file for *name*, or None if missing."""
    app_dir = _find_app_dir(name, project_root)
    if app_dir is None:
        return None
    cmake_file = app_dir / f"{name}.cmake"
    return cmake_file if cmake_file.exists() else None


def _cmake_registered_after_configure(
    name: str,
    project_root: Path,
    build_dir: Path,
) -> bool:
    """Return True when the app's .cmake file is newer than CMakeCache.txt.

    This means the app was registered (by ``sdlos create``) *after* the last
    cmake configure pass, so the target is not yet known to the build system.
    Auto-reconfiguring once will fix it.
    """
    cache = build_dir / "CMakeCache.txt"
    cmake_file = project_root / "examples" / "apps" / name / f"{name}.cmake"
    if not cache.exists() or not cmake_file.exists():
        return False
    return cmake_file.stat().st_mtime > cache.stat().st_mtime


def _infer_cmake_snippet(name: str, app_dir: Path) -> str:
    """Build a minimal sdlos_jade_app() cmake snippet by inspecting what files
    actually exist in the app directory."""
    src_base = f"examples/apps/{name}"

    # Find jade file
    jade_file = f"{src_base}/{name}.jade"

    # Find behavior file — try common extensions
    behavior = None
    for ext in (".cc", ".cxx", ".cpp"):
        candidate_name = f"{name}_behavior{ext}"
        if (app_dir / candidate_name).exists():
            behavior = f"{src_base}/{candidate_name}"
            break
        # Also try plain name (e.g. shade.cc)
        candidate_name2 = f"{name}{ext}"
        if (app_dir / candidate_name2).exists():
            behavior = f"{src_base}/{candidate_name2}"
            break

    # Check for data/ directory
    has_data = (app_dir / "data").is_dir()

    lines = [f"sdlos_jade_app({name}", f"    {jade_file}"]
    if behavior:
        lines.append(f"    BEHAVIOR {behavior}")
    if has_data:
        lines.append(f"    DATA_DIR {src_base}/data")
    lines.append(")")
    return "\n".join(lines) + "\n"


def _ensure_app_cmake(
    name: str,
    project_root: Path,
    quiet: bool,
    run_cfg: "RunConfig",
) -> None:
    """Check that a .cmake registration file exists for the app.

    If the app directory exists but the .cmake file is missing:
      - In an interactive terminal: prompt the user to auto-create it.
      - Non-interactive: print a helpful error and exit.

    If the .cmake was created, force reconfigure so cmake picks it up.
    """
    app_dir = _find_app_dir(name, project_root)

    if app_dir is None:
        # No app directory at all — nothing we can do here; cmake will fail
        # later with a clear error message.
        return

    cmake_file = app_dir / f"{name}.cmake"
    if cmake_file.exists():
        return  # all good

    # The app directory exists but the .cmake file is missing.
    if not quiet:
        _echo(f"  [warn]   {name}.cmake not found in {app_dir.relative_to(project_root)}")

    # Check if this is an interactive TTY
    is_interactive = sys.stdin.isatty() and sys.stdout.isatty()

    if is_interactive:
        snippet = _infer_cmake_snippet(name, app_dir)
        _echo(f"\n  The app directory exists but {name}.cmake is missing.")
        _echo(f"  I can create it for you:\n")
        for line in snippet.splitlines():
            _echo(f"    {line}")
        _echo("")
        try:
            answer = input("  Create this file? [Y/n] ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            _echo("")
            answer = "n"

        if answer in ("", "y", "yes"):
            cmake_file.write_text(snippet, encoding="utf-8")
            _echo(f"  [create] wrote {cmake_file.relative_to(project_root)}")
            # Force reconfigure so cmake picks up the new file
            run_cfg.reconfigure = True
        else:
            _die(
                f"{name}.cmake is missing — the cmake build system cannot find this target.\n"
                f"Create it manually or run:  sdlos create {name} --overwrite"
            )
    else:
        _die(
            f"{name}.cmake not found in {app_dir.relative_to(project_root)}.\n"
            f"The cmake glob (examples/apps/*/*.cmake) cannot discover this app.\n"
            f"Fix:\n"
            f"  sdlos create {name} --overwrite     # re-scaffold with .cmake\n"
            f"  sdlos run {name} --reconfigure      # in a terminal (interactive prompt)"
        )



def run_app(
    name: str,
    project_root: Path,
    *,
    build_dir: Optional[Path] = None,
    clean: bool = False,
    watch: bool = False,
    jobs: Optional[int] = None,
    no_build: bool = False,
    quiet: bool = False,
    reconfigure: bool = False,
    preset: Optional[str] = None,
) -> None:
    """Build and launch the jade app *name*.

    Parameters
    ----------
    name:
        CMake target name (== app name in snake_case).
    project_root:
        Absolute path to the sdlos project root (contains CMakeLists.txt).
    build_dir:
        Path to the cmake binary directory.  Defaults to
        ``<project_root>/build`` (matches all CMakePresets).
    clean:
        Pass ``--clean-first`` to cmake.  Fully recompiles the target
        without deleting the cmake cache → no FetchContent re-fetches.
    watch:
        After launching, watch ``examples/apps/<name>/`` for file changes
        and auto-rebuild + relaunch on every change.
    jobs:
        Parallel build jobs (``-j N``).  ``None`` → cmake default.
    no_build:
        Skip the cmake build step entirely (just find + launch).
    quiet:
        Suppress sdlos-tooling status lines.
    reconfigure:
        Force a cmake configure pass before building.  Use this after
        ``sdlos create <name>`` on an existing build to register the new
        target (the auto-discovery glob runs at configure time, not build
        time).  Configure also runs automatically when the build directory
        does not exist or contains no ``CMakeCache.txt``.
    preset:
        cmake preset name to pass to ``cmake --preset`` during the configure
        step (e.g. ``"macos-debug"``).  ``None`` → plain
        ``cmake -B <build> -S <root>``.
    """
    # ── Resolve build directory ───────────────────────────────────────────────
    resolved_build = (build_dir or project_root / "build").resolve()

    run_cfg = RunConfig(
        name=name,
        build_dir=resolved_build,
        clean=clean,
        watch=watch,
        jobs=jobs,
        no_build=no_build,
        quiet=quiet,
        reconfigure=reconfigure,
        preset=preset,
    )

    if not quiet:
        _echo(f"\nsdlos run  {name}")
        _echo(f"  build dir: {resolved_build}")

    # ── Ensure the app has a .cmake registration file ─────────────────────────
    # The root CMakeLists.txt discovers app targets via:
    #   file(GLOB _app_cmake_files "examples/apps/*/*.cmake")
    # If the .cmake file is missing (deleted, never created, etc.) the target
    # is invisible to cmake even after --reconfigure.  Detect this early and
    # offer to create a minimal one when running interactively.
    if not no_build:
        _ensure_app_cmake(name, project_root, quiet, run_cfg)

    # ── Configure (auto or forced) ────────────────────────────────────────────
    # Handled cases:
    #   1. Build dir absent          → create dir + auto-configure.
    #   2. CMakeCache.txt missing    → auto-configure (dir exists but blank).
    #   3. --reconfigure flag        → always re-configure (new app added to
    #                                  an already-configured build).
    #   4. --no-build                → skip; just validate the dir exists.
    if not no_build:
        if not resolved_build.exists():
            if not quiet:
                _echo(
                    f"  [configure] build dir not found — running cmake configure\n"
                    f"              Tip: use --preset PRESET for a specific configuration\n"
                    f"              e.g.  sdlos run {name} --preset macos-debug"
                )
            resolved_build.mkdir(parents=True, exist_ok=True)
            if not _configure(project_root, resolved_build, preset, quiet):
                _die(
                    "cmake configure failed.\n"
                    "Specify a preset explicitly:\n"
                    f"  sdlos run {name} --preset macos-debug"
                )
        elif reconfigure or _needs_configure(resolved_build):
            reason = (
                "--reconfigure requested"
                if reconfigure
                else "CMakeCache.txt not found — build dir was never configured"
            )
            if not quiet:
                _echo(f"  [configure] {reason}")
            if not _configure(project_root, resolved_build, preset, quiet):
                _die(
                    "cmake configure failed.\n"
                    "Specify a preset explicitly:\n"
                    f"  sdlos run {name} --reconfigure --preset macos-debug"
                )
    elif not resolved_build.exists():
        _die(
            f"Build directory not found: {resolved_build}\n"
            f"Drop --no-build so sdlos run can configure and build first:\n"
            f"  sdlos run {name}"
        )

    # ── Build ─────────────────────────────────────────────────────────────────
    if not no_build:
        ok = _build(run_cfg, project_root)
        if not ok and not reconfigure:
            # Auto-detect: .cmake registered after last configure → reconfigure once.
            if _cmake_registered_after_configure(name, project_root, resolved_build):
                if not quiet:
                    _echo(
                        f"  [auto-reconfigure] {name}.cmake is newer than cmake cache\n"
                        f"                     re-running configure to register the target …"
                    )
                if _configure(project_root, resolved_build, preset, quiet):
                    ok = _build(run_cfg, project_root)
        if not ok:
            _die(
                f"cmake --build failed for target '{name}'.\n"
                f"If '{name}' was just created with `sdlos create`, the build\n"
                f"system may not know about it yet — re-configure first:\n"
                f"  sdlos run {name} --reconfigure"
            )
    else:
        if not quiet:
            _echo("  [build]  skipped (--no-build)")

    # ── Locate executable ─────────────────────────────────────────────────────
    exe = _find_executable(name, resolved_build)
    if exe is None:
        _die(
            f"Could not find built executable for '{name}' under {resolved_build}.\n"
            "Checked: <build>/<name>, <build>/Debug/<name>, <build>/Release/<name>, …\n"
            "Is the target name correct?  Run:  sdlos templates  to check."
        )

    if not quiet:
        _echo(f"  exe:      {exe}")

    # ── Launch / watch ────────────────────────────────────────────────────────
    if watch:
        _watch_loop(run_cfg, project_root, exe)
    else:
        proc = _launch(exe, quiet)
        rc = proc.wait()
        if rc != 0 and not quiet:
            _echo(f"  [exit]   process exited with code {rc}")



def _echo(msg: str) -> None:
    print(msg, flush=True)


def _die(msg: str) -> None:
    print(f"\nsdlos run: {msg}", file=sys.stderr, flush=True)
    sys.exit(1)
