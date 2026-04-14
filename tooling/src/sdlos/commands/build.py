"""
sdlos.commands.build
====================
``sdlos build`` — configure and build sdlos for non-desktop targets.

Subcommands
-----------
  sdlos build ios       Configure an Xcode project for iOS / iPadOS
  sdlos build android   Prepare and build the Android APK via Gradle
  sdlos build web       Configure and optionally build the Emscripten / WASM target

Each subcommand is a direct Python replacement for the shell scripts that
previously lived in ``tooling/config/config-*.sh``.  All cmake invocations
are run in-process via ``subprocess`` so the output streams through normally.

Examples
--------
  # iOS — generate Xcode project for a specific app
  sdlos build ios --app clima                    # configure + open clima scheme hint
  sdlos build ios --app clima --open             # configure then open in Xcode
  sdlos build ios --app clima --build            # cmake + xcodebuild clima scheme
  sdlos build ios --build-type Release --team ABCDE12345 --app clima
  sdlos build ios --list-apps                    # list discoverable apps
  SDLOS_IOS_TEAM=ABCDE12345 sdlos build ios --app clima

  # Android — debug APK (gradle assembleDebug)
  sdlos build android
  sdlos build android --variant release          # gradle assembleRelease
  sdlos build android --install                  # adb install after build

  # Web / WASM — configure + build + serve
  sdlos build web
  sdlos build web --build-type Debug
  sdlos build web --build                        # cmake --build build/web -j
  sdlos build web --build --serve                # build then python3 -m http.server
  sdlos build web --serve                        # skip build, just serve
  sdlos build web --port 9000 --app styleguide
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

import click

from ..core.cmake import find_project_root
from ..core.console import console


# ── helpers ───────────────────────────────────────────────────────────────────

def _run(cmd: list[str], cwd: Optional[Path] = None) -> int:
    """Run *cmd*, streaming output.  Returns the exit code."""
    click.echo(f"  $ {' '.join(str(c) for c in cmd)}")
    result = subprocess.run(cmd, cwd=cwd)
    return result.returncode


def _require(program: str, install_hint: str) -> str:
    """Return the full path to *program* or raise UsageError with install hint."""
    found = shutil.which(program)
    if not found:
        raise click.UsageError(
            f"'{program}' not found in PATH.\n\n{install_hint}"
        )
    return found


def _list_apps(root: Path) -> list[str]:
    """Return sorted app names discovered from examples/apps/*/*.cmake."""
    apps_dir = root / "examples" / "apps"
    if not apps_dir.is_dir():
        return []
    names: list[str] = []
    for cmake_file in sorted(apps_dir.glob("*/*.cmake")):
        name = cmake_file.parent.name
        if name not in names:
            names.append(name)
    return sorted(names)


# ── sdlos build ───────────────────────────────────────────────────────────────

@click.group("build")
def cmd_build() -> None:
    """Configure and build sdlos for non-desktop targets.

    \b
    Subcommands
    -----------
      ios       Generate an Xcode project for iOS / iPadOS
      android   Build the Android APK via Gradle
      web       Configure the Emscripten / WASM build
    """


# ── sdlos build ios ───────────────────────────────────────────────────────────

@cmd_build.command("ios")
@click.option(
    "--app", "app_name",
    default=None, metavar="NAME",
    help=(
        "Jade app to target (e.g. clima, styleguide).  "
        "Must match a directory under examples/apps/.  "
        "Omit to configure all apps (pick the scheme in Xcode)."
    ),
)
@click.option(
    "--list-apps",
    is_flag=True, default=False,
    help="List discoverable apps and exit.",
)
@click.option(
    "--build-type",
    type=click.Choice(["Debug", "Release", "RelWithDebInfo", "MinSizeRel"]),
    default="Debug", show_default=True,
    help="CMake build type.",
)
@click.option(
    "--team",
    default=None, metavar="TEAM_ID", envvar="SDLOS_IOS_TEAM",
    help=(
        "Apple Developer Team ID (10-char string, e.g. ABCDE12345).  "
        "Can also be set via SDLOS_IOS_TEAM environment variable.  "
        "Omit to generate the project without signing — set it in Xcode later."
    ),
)
@click.option(
    "--bundle-prefix",
    default="dev.sdlos", show_default=True, metavar="PREFIX",
    envvar="SDLOS_IOS_BUNDLE_PREFIX",
    help="Reverse-DNS bundle ID prefix (e.g. com.example).",
)
@click.option(
    "--deployment",
    default="16.0", show_default=True, metavar="VER",
    envvar="SDLOS_IOS_DEPLOYMENT",
    help="Minimum iOS deployment target.",
)
@click.option(
    "--build-dir",
    type=click.Path(path_type=Path),
    default=None, metavar="DIR",
    help="cmake binary directory [default: <project-root>/build/ios].",
)
@click.option(
    "--fresh/--no-fresh",
    default=True, show_default=True,
    help="Pass --fresh to cmake (clears CMakeCache before configuring).",
)
@click.option(
    "--open", "open_project",
    is_flag=True, default=False,
    help="Open the generated .xcodeproj in Xcode after cmake completes.",
)
@click.option(
    "--build", "run_build",
    is_flag=True, default=False,
    help=(
        "Run xcodebuild after cmake (builds the selected --app scheme).  "
        "Useful for CI.  For device builds with signing, open the project in Xcode."
    ),
)
def cmd_build_ios(
    app_name: Optional[str],
    list_apps: bool,
    build_type: str,
    team: Optional[str],
    bundle_prefix: str,
    deployment: str,
    build_dir: Optional[Path],
    fresh: bool,
    open_project: bool,
    run_build: bool,
) -> None:
    """Configure an Xcode project for iOS / iPadOS.

    All apps under examples/apps/ are included as separate Xcode schemes.
    Use --app to specify which scheme to build with --build, or to highlight
    which target to select when opening in Xcode.

    \b
    Examples
    --------
      sdlos build ios                                    # Debug, all apps, no signing
      sdlos build ios --list-apps                        # show available app names
      sdlos build ios --app clima                        # configure (clima highlighted)
      sdlos build ios --app clima --open                 # configure + open Xcode
      sdlos build ios --app clima --build                # cmake + xcodebuild clima
      sdlos build ios --app clima --build-type Release --team ABCDE12345
      SDLOS_IOS_TEAM=ABCDE12345 sdlos build ios --app clima
    """
    root = find_project_root(Path.cwd())

    # ── --list-apps ───────────────────────────────────────────────────────────
    if list_apps:
        apps = _list_apps(root)
        if not apps:
            console.print("[yellow]No apps found under examples/apps/[/]")
        else:
            console.print("\n[bold]Available apps[/] (examples/apps/*)\n")
            for a in apps:
                marker = " [bold cyan]←[/]" if a == app_name else ""
                console.print(f"  [bold]{a}[/]{marker}")
            console.print()
            console.print(
                "Use [bold cyan]sdlos build ios --app NAME[/] to target a specific app.\n"
            )
        return

    _require(
        "cmake",
        "Install CMake: https://cmake.org/download/  or  brew install cmake",
    )

    out_dir = build_dir or (root / "build" / "ios")

    cmake_args: list[str] = [
        "cmake",
        *(["--fresh"] if fresh else []),
        "-G", "Xcode",
        "-DCMAKE_SYSTEM_NAME=iOS",
        "-DCMAKE_OSX_ARCHITECTURES=arm64",
        f"-DCMAKE_OSX_DEPLOYMENT_TARGET={deployment}",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        # App Store: no user-space shared libraries
        "-DBUILD_SHARED_LIBS=OFF",
        "-DSDL_SHARED=OFF",
        "-DSDL_STATIC=ON",
        "-DSDL_TEST_LIBRARY=OFF",
        # SDL3_ttf — static + all vendored deps
        "-DSDLTTF_SHARED=OFF",
        "-DSDLTTF_STATIC=ON",
        "-DSDLTTF_VENDORED=ON",
        "-DSDLTTF_FREETYPE_VENDORED=ON",
        "-DSDLTTF_HARFBUZZ_VENDORED=ON",
        # plutosvg/plutovg = SVG color emoji — unused; sdlos uses COLR/Twemoji
        "-DSDLTTF_PLUTOSVG=OFF",
        # SDL3_image / SDL3_mixer — static
        "-DSDLIMAGE_SHARED=OFF",
        "-DSDLIMAGE_STATIC=ON",
        "-DSDLMIXER_SHARED=OFF",
        "-DSDLMIXER_STATIC=ON",
        # no desktop-only features
        "-DSDLOS_ENABLE_TESTS=OFF",
        f"-DSDLOS_IOS_BUNDLE_PREFIX={bundle_prefix}",
        "-B", str(out_dir),
        "-S", str(root),
    ]

    if team:
        cmake_args.append(f"-DSDLOS_IOS_TEAM={team}")

    # Resolve scheme: explicit --app, or default ALL_BUILD for xcodebuild.
    scheme = app_name or "ALL_BUILD"

    console.print(f"\n[bold]sdlos build ios[/]  ({build_type})")
    if app_name:
        console.print(f"  app / scheme: [bold cyan]{app_name}[/]")
    else:
        console.print(f"  app / scheme: [dim]all apps (pick scheme in Xcode)[/]")
    if team:
        console.print(f"  team:         {team}")
    console.print(f"  bundle:       {bundle_prefix}.*")
    console.print(f"  deployment:   iOS {deployment}+")
    console.print(f"  output:       {out_dir}\n")

    rc = _run(cmake_args, cwd=root)
    if rc != 0:
        raise click.ClickException(f"cmake configure failed (exit {rc})")

    xcodeproj = out_dir / "sdlos.xcodeproj"
    console.print(f"\n[green]✓[/] Xcode project: [bold]{xcodeproj}[/]")
    if app_name:
        console.print(
            f"  Open:   [cyan]open {xcodeproj}[/]\n"
            f"  Scheme: select [bold]{app_name}[/] in the scheme dropdown"
        )
    else:
        console.print(f"  Open:   [cyan]open {xcodeproj}[/]")

    if not team:
        console.print(
            "\n[yellow]NOTE[/] No signing team provided.  "
            "Set [bold]Signing & Capabilities → Team[/] in Xcode,\n"
            "      or re-run with [bold]--team YOUR_TEAM_ID[/] "
            f"(or [bold]SDLOS_IOS_TEAM=... sdlos build ios[/])."
        )

    if open_project and xcodeproj.exists():
        _run(["open", str(xcodeproj)])

    if run_build:
        console.print(f"\n[bold]xcodebuild[/]  scheme=[cyan]{scheme}[/] …")
        rc = _run([
            "xcodebuild",
            "-project", str(xcodeproj),
            "-scheme", scheme,
            "-configuration", build_type,
            "-sdk", "iphoneos",
            "CODE_SIGNING_ALLOWED=NO",
            "build",
        ], cwd=root)
        if rc != 0:
            raise click.ClickException(f"xcodebuild failed (exit {rc})")
        console.print(f"[green]✓[/] xcodebuild succeeded (scheme: {scheme}).")


# ── sdlos build android ───────────────────────────────────────────────────────

@cmd_build.command("android")
@click.option(
    "--variant",
    type=click.Choice(["debug", "release"]),
    default="debug", show_default=True,
    help="Gradle build variant.",
)
@click.option(
    "--build-dir",
    type=click.Path(path_type=Path),
    default=None, metavar="DIR",
    help=(
        "SDL3 android-project root "
        "[default: <project-root>/deps/SDL3/android-project]."
    ),
)
@click.option(
    "--install", "run_install",
    is_flag=True, default=False,
    help="Run 'adb install -r <apk>' after a successful build.",
)
@click.option(
    "--configure-only",
    is_flag=True, default=False,
    help="Copy files into android-project but do not run Gradle.",
)
def cmd_build_android(
    variant: str,
    build_dir: Optional[Path],
    run_install: bool,
    configure_only: bool,
) -> None:
    """Prepare and build the Android APK via Gradle.

    \b
    Prerequisites
    -------------
      - deps/SDL3 checked out with android-project/ (run pre_cmake.sh first)
      - Android SDK / NDK  (ANDROID_HOME or via Android Studio)
      - Java 17+           (brew install --cask temurin@17)

    \b
    Examples
    --------
      sdlos build android                     debug APK
      sdlos build android --variant release   release APK
      sdlos build android --install           build + adb install
      sdlos build android --configure-only    copy files, skip gradle
    """
    import re

    root = find_project_root(Path.cwd())
    sdl_android = build_dir or (root / "deps" / "SDL3" / "android-project")
    our_android  = root / "android"

    # ── Preflight ─────────────────────────────────────────────────────────────
    if not sdl_android.is_dir():
        raise click.ClickException(
            f"SDL3 android-project not found at: {sdl_android}\n\n"
            "Run ./pre_cmake.sh first to clone SDL3 with the android-project template."
        )

    java = _require(
        "java",
        "Install Java 17+:\n"
        "  macOS: brew install --cask temurin@17\n"
        "  Linux: sdk install java 17-tem",
    )

    # Check Java version
    java_ver_out = subprocess.run(
        [java, "-version"], capture_output=True, text=True
    ).stderr
    match = re.search(r'version "(\d+)', java_ver_out)
    java_major = int(match.group(1)) if match else 0
    if java_major < 17:
        raise click.ClickException(
            f"Java 17+ required, found version {java_major or '?'}.\n"
            "  macOS: brew install --cask temurin@17"
        )

    # ── Inject our files into SDL3's android-project ──────────────────────────
    console.print(f"\n[bold]sdlos build android[/]  ({variant})")
    console.print(f"  android-project: {sdl_android}\n")

    build_gradle_src = our_android / "app" / "build.gradle"
    if build_gradle_src.exists():
        shutil.copy2(build_gradle_src, sdl_android / "app" / "build.gradle")
        console.print(f"  [green]copied[/] build.gradle")
    else:
        console.print(f"  [yellow]warn[/] android/app/build.gradle not found — skipping")

    manifest_src = our_android / "app" / "src" / "main" / "AndroidManifest.xml"
    if manifest_src.exists():
        manifest_dst = sdl_android / "app" / "src" / "main"
        manifest_dst.mkdir(parents=True, exist_ok=True)
        shutil.copy2(manifest_src, manifest_dst / "AndroidManifest.xml")
        console.print(f"  [green]copied[/] AndroidManifest.xml")

    java_src = our_android / "app" / "src" / "main" / "java" / "dev" / "sdlos" / "app"
    java_dst = sdl_android / "app" / "src" / "main" / "java" / "dev" / "sdlos" / "app"
    if java_src.is_dir():
        java_dst.mkdir(parents=True, exist_ok=True)
        for f in java_src.rglob("*"):
            if f.is_file():
                rel = f.relative_to(java_src)
                dest = java_dst / rel
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(f, dest)
        console.print(f"  [green]copied[/] Java sources")

    if configure_only:
        console.print("\n[green]✓[/] android-project configured (--configure-only; gradle skipped).")
        return

    # ── Gradle build ──────────────────────────────────────────────────────────
    gradle_task = f"assemble{variant.capitalize()}"
    gradlew = sdl_android / "gradlew"
    if not gradlew.exists():
        raise click.ClickException(f"gradlew not found at {gradlew}")

    gradlew.chmod(gradlew.stat().st_mode | 0o111)   # ensure executable
    rc = _run([str(gradlew), gradle_task], cwd=sdl_android)
    if rc != 0:
        raise click.ClickException(f"Gradle {gradle_task} failed (exit {rc})")

    apk = sdl_android / "app" / "build" / "outputs" / "apk" / variant / f"app-{variant}.apk"
    if apk.exists():
        console.print(f"\n[green]✓[/] APK ready: [bold]{apk}[/]")
        if run_install:
            adb = _require("adb", "Install Android platform-tools: https://developer.android.com/tools/releases/platform-tools")
            rc = _run([adb, "install", "-r", str(apk)])
            if rc != 0:
                raise click.ClickException(f"adb install failed (exit {rc})")
            console.print("[green]✓[/] Installed on connected device.")
        else:
            console.print(f"  Install:  [cyan]adb install -r {apk}[/]")
    else:
        console.print(f"[yellow]warn[/] APK not found at expected path: {apk}")


# ── sdlos build web ───────────────────────────────────────────────────────────

@cmd_build.command("web")
@click.option(
    "--build-type",
    type=click.Choice(["Debug", "Release", "RelWithDebInfo", "MinSizeRel"]),
    default="Release", show_default=True,
    help="CMake build type.",
)
@click.option(
    "--app",
    default="styleguide", show_default=True, metavar="NAME",
    help="Jade app to compile for the web (SDLOS_WEB_APP cmake variable).",
)
@click.option(
    "--build-dir",
    type=click.Path(path_type=Path),
    default=None, metavar="DIR",
    help="cmake binary directory [default: <project-root>/build/web].",
)
@click.option(
    "--emsdk",
    default=None, metavar="DIR", envvar="EMSDK",
    help=(
        "Path to the emsdk root directory.  "
        "Auto-detected from EMSDK env var or PATH (emcc location)."
    ),
)
@click.option(
    "--build", "run_build",
    is_flag=True, default=False,
    help="Run 'cmake --build build/web -j' after configure.",
)
@click.option(
    "--jobs", "-j",
    type=int, default=None, metavar="N",
    help="Parallel build jobs (only used with --build).",
)
@click.option(
    "--serve",
    is_flag=True, default=False,
    help="Start 'python3 -m http.server' in build/web after building.",
)
@click.option(
    "--port",
    type=int, default=8080, show_default=True,
    help="Port for the local dev server (only used with --serve).",
)
def cmd_build_web(
    build_type: str,
    app: str,
    build_dir: Optional[Path],
    emsdk: Optional[str],
    run_build: bool,
    jobs: Optional[int],
    serve: bool,
    port: int,
) -> None:
    """Configure and build sdlos for WebAssembly (Emscripten).

    \b
    Prerequisites
    -------------
      Install emsdk:
        git clone https://github.com/emscripten-core/emsdk.git
        cd emsdk && ./emsdk install latest && ./emsdk activate latest
        source ./emsdk_env.sh

    \b
    Examples
    --------
      sdlos build web                          configure only
      sdlos build web --build                  configure + build
      sdlos build web --build --serve          configure + build + serve
      sdlos build web --serve                  serve build/web/ (already built)
      sdlos build web --app mycalc --build     build a different jade app
      sdlos build web --build-type Debug --build
    """
    root    = find_project_root(Path.cwd())
    out_dir = build_dir or (root / "build" / "web")

    # ── Locate Emscripten toolchain ───────────────────────────────────────────
    toolchain: Optional[Path] = None

    if emsdk:
        candidate = Path(emsdk) / "upstream" / "emscripten" / "cmake" / "Modules" / "Platform" / "Emscripten.cmake"
        if candidate.exists():
            toolchain = candidate

    if toolchain is None:
        emcc = shutil.which("emcc")
        if emcc:
            candidate = Path(emcc).parent / "cmake" / "Modules" / "Platform" / "Emscripten.cmake"
            if candidate.exists():
                toolchain = candidate

    if toolchain is None:
        raise click.ClickException(
            "Emscripten not found in PATH and EMSDK is not set.\n\n"
            "Install emsdk:\n"
            "  git clone https://github.com/emscripten-core/emsdk.git\n"
            "  cd emsdk && ./emsdk install latest && ./emsdk activate latest\n"
            "  source ./emsdk_env.sh\n\n"
            "Or pass --emsdk /path/to/emsdk"
        )

    _require("cmake", "Install CMake: https://cmake.org/download/  or  brew install cmake")

    # ── Configure ─────────────────────────────────────────────────────────────
    console.print(f"\n[bold]sdlos build web[/]  ({build_type}  app={app})")
    console.print(f"  toolchain: {toolchain}")
    console.print(f"  output:    {out_dir}\n")

    cmake_args = [
        "cmake",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        f"-DSDLOS_WEB_APP={app}",
        "-DSDLTTF_VENDORED=ON",
        "-DSDLTTF_FREETYPE_VENDORED=ON",
        "-DSDLTTF_HARFBUZZ_VENDORED=ON",
        "-DSDLTTF_PLUTOSVG=OFF",
        "-DSDLOS_ENABLE_TESTS=OFF",
        "-B", str(out_dir),
        "-S", str(root),
    ]

    rc = _run(cmake_args, cwd=root)
    if rc != 0:
        raise click.ClickException(f"cmake configure failed (exit {rc})")

    console.print(f"\n[green]✓[/] Web build configured in [bold]{out_dir}[/]")

    # ── Build ─────────────────────────────────────────────────────────────────
    if run_build:
        build_cmd = ["cmake", "--build", str(out_dir)]
        if jobs:
            build_cmd += ["-j", str(jobs)]
        rc = _run(build_cmd, cwd=root)
        if rc != 0:
            raise click.ClickException(f"cmake --build failed (exit {rc})")
        html = out_dir / f"{app}.html"
        console.print(f"\n[green]✓[/] Built: [bold]{html}[/]")

    # ── Serve ─────────────────────────────────────────────────────────────────
    if serve:
        console.print(f"\n[bold]Serving[/] {out_dir}  on http://localhost:{port}")
        console.print(f"  Open: [cyan]http://localhost:{port}/{app}.html[/]")
        console.print("  Stop: Ctrl-C\n")
        try:
            subprocess.run(
                [sys.executable, "-m", "http.server", str(port)],
                cwd=out_dir,
            )
        except KeyboardInterrupt:
            console.print("\n[yellow]server stopped[/]")
    elif not run_build:
        console.print(f"  Build:  [cyan]sdlos build web --build[/]")
        console.print(f"  Serve:  [cyan]sdlos build web --serve[/]")
        console.print(f"  Open:   [cyan]http://localhost:{port}/{app}.html[/]")

