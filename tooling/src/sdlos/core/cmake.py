"""
sdlos.core.cmake
================
CMakeLists.txt helpers used by sdlos tooling commands.

  find_project_root(start)               — walk upward to the sdlos project root
  cmake_snippet(name, ww, wh, …)        — render an sdlos_jade_app() block
  write_app_cmake(app_dir, snippet, cfg) — write <name>.cmake into the app dir
"""
from __future__ import annotations

from pathlib import Path
from typing import Optional


# ── Project-root detection ────────────────────────────────────────────────────

def find_project_root(start: Path, *, max_ascent: int = 12) -> Path:
    """Walk upward from *start* until a CMakeLists.txt containing
    ``project(sdlos`` is found.

    Falls back to the grandparent of this file (…/tooling/src/sdlos/core →
    …/userspace) if no matching root is found.
    """
    p = start.resolve()
    for _ in range(max_ascent):
        cml = p / "CMakeLists.txt"
        if cml.exists():
            try:
                if "project(sdlos" in cml.read_text(encoding="utf-8"):
                    return p
            except OSError:
                pass
        parent = p.parent
        if parent == p:
            break
        p = parent

    # Fallback: this file lives at src/sdlos/core/cmake.py inside tooling/
    return Path(__file__).resolve().parents[4]


# ── Snippet builder ───────────────────────────────────────────────────────────

def cmake_snippet(
    name: str,
    win_w: Optional[int],
    win_h: Optional[int],
    has_data: bool,
    *,
    extra_resources: Optional[list[str]] = None,
) -> str:
    """Return a formatted ``sdlos_jade_app()`` CMake call for app *name*.

    Parameters
    ----------
    name:            app name in snake_case.
    win_w / win_h:   debug window dimensions, or ``None`` to omit the lines
                     entirely (engine uses its compile-time defaults:
                     375 × 667 in debug, fullscreen in release).
    has_data:        if True, include a ``DATA_DIR`` line.
    extra_resources: optional list of extra ``sdlos_copy_resource`` lines
                     to append after the app registration block.
    """
    src_base = f"examples/apps/{name}"
    data_base = f"examples/apps/{name}/data"
    lines = [
        f"sdlos_jade_app({name}",
        f"    {src_base}/{name}.jade",
        f"    BEHAVIOR {src_base}/{name}_behavior.cxx",
    ]
    if has_data:
        lines.append(f"    DATA_DIR {data_base}")
    if win_w is not None:
        lines.append(f"    WIN_W {win_w}")
    if win_h is not None:
        lines.append(f"    WIN_H {win_h}")
    lines.append(")")

    block = "\n".join(lines)

    if extra_resources:
        block += "\n" + "\n".join(extra_resources)

    return block


# ── App cmake file writer ─────────────────────────────────────────────────────

def write_app_cmake(app_dir: Path, snippet: str, cfg: "AppConfig") -> None:  # type: ignore[name-defined]
    """Write ``<name>.cmake`` into *app_dir*.

    The file is picked up automatically by the root CMakeLists.txt glob::

        file(GLOB _app_cmake_files "examples/apps/*/*.cmake")
        foreach(_f ${_app_cmake_files})
            include(${_f})
        endforeach()

    This means an app registers itself in the build simply by existing —
    no manual CMakeLists.txt edits needed.
    """
    from .fs import write_file_safe  # local import avoids circular at module level

    name = app_dir.name
    cmake_file = app_dir / f"{name}.cmake"
    write_file_safe(
        cmake_file,
        snippet + "\n",
        overwrite=cfg.overwrite,
        dry_run=cfg.dry_run,
        verbose=cfg.verbose,
    )


# ── Snippet builder ───────────────────────────────────────────────────────────
