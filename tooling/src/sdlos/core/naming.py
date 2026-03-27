"""
sdlos.core.naming
=================
Name conversion and validation helpers shared across all tooling commands.
"""
from __future__ import annotations

import re
import click


def pascal(snake: str) -> str:
    """Convert snake_case (or kebab-case) to PascalCase.

    Examples
    --------
    >>> pascal("my_app")
    'MyApp'
    >>> pascal("head-volley")
    'HeadVolley'
    >>> pascal("calc")
    'Calc'
    """
    return "".join(w.capitalize() for w in re.split(r"[_\-]+", snake))


def validate_name(value: str) -> str:
    """Normalise and validate an app name.

    Rules
    -----
    - Lower-cased.
    - Hyphens converted to underscores.
    - Must match ``[a-z][a-z0-9_]*``.

    Raises :class:`click.BadParameter` on failure so it integrates cleanly
    with Click option/argument callbacks.
    """
    name = value.lower().replace("-", "_")
    if not re.fullmatch(r"[a-z][a-z0-9_]*", name):
        raise click.BadParameter(
            f"'{value}' must start with a letter and contain only "
            "letters, digits, and underscores (hyphens are converted automatically)."
        )
    return name
