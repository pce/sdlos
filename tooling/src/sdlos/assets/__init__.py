"""
sdlos.assets
============
Bootstrap data helpers for scaffolded app directories.

Sub-modules
-----------
png         — pure-Python RGBA PNG encoder (dot, solid, gradient_h, encode)
shaders     — starter Metal fragment shader sources keyed by template / preset
gitignore   — per-directory .gitignore content for the data/ skeleton

Typical usage (from commands/create.py)
----------------------------------------
    from sdlos.assets import png, shaders, gitignore

    canvas_bytes = png.dot(size=256, peak_alpha=128)
    msl_sources  = shaders.starter_msl("shader")   # {"preset_a.frag.metal": ...}
    root_ignore  = gitignore.DATA_ROOT
"""

from . import gitignore, png, shaders

__all__ = ["png", "shaders", "gitignore"]
