"""
sdlos.core.fs
=============
File-system abstractions used by all sdlos tooling commands.

  write_atomic(path, data)          — POSIX-atomic write via rename
  backup_file(path) -> Path | None  — unique .bak copy before overwrite
  write_file_safe(path, data, …)    — skip / backup / atomic write with logging
  extract_user_regions(text)        — return list of user-region bodies
  splice_user_regions(new, old)     — preserve user regions on regen
"""
from __future__ import annotations

import os
import shutil
import tempfile
from pathlib import Path
from typing import Optional
import re


# ── User-region markers ───────────────────────────────────────────────────────
# Place these verbatim inside generated files.  The tooling will preserve
# everything between them when a file is regenerated.

USER_BEGIN = "--- enter the forrest ---"
USER_END   = "--- back to the sea ---"

_RE_REGION = re.compile(
    rf"{re.escape(USER_BEGIN)}(.*?){re.escape(USER_END)}",
    re.DOTALL,
)


# ── Atomic write ─────────────────────────────────────────────────────────────

def write_atomic(path: Path, data: str) -> None:
    """Write *data* to *path* atomically (write-to-temp then rename).

    The parent directory is created if it does not exist.
    Safe against partial writes and concurrent readers on POSIX.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=path.name, dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, str(path))   # atomic on POSIX (same filesystem)
    finally:
        # In the error path the tmp file might still exist.
        if os.path.exists(tmp):
            os.unlink(tmp)


# ── Backup ───────────────────────────────────────────────────────────────────

def backup_file(path: Path) -> Optional[Path]:
    """Copy *path* to a unique <name>.bak (or .bak1, .bak2 …) sibling.

    Returns the backup path, or *None* if the source does not exist.
    """
    if not path.exists():
        return None
    candidate = path.with_suffix(path.suffix + ".bak")
    i = 0
    while candidate.exists():
        i += 1
        candidate = path.with_suffix(path.suffix + f".bak{i}")
    shutil.copy2(path, candidate)
    return candidate


# ── Safe file writer ─────────────────────────────────────────────────────────

def write_file_safe(
    path: Path,
    content: str,
    overwrite: bool = False,
    dry_run: bool = False,
    *,
    verbose: bool = True,
) -> bool:
    """Write *content* to *path* with logging, skip/backup/dry-run support.

    Returns True if the file was written (or would be in dry-run mode).

    Behaviour:
      - If *path* does not exist  → atomic write.
      - If *path* exists and not *overwrite* → skip (return False).
      - If *path* exists and *overwrite* → backup first, then atomic write.
      - If *dry_run* is True → only print what would happen, never touch disk.
    """
    exists = path.exists()
    tag = path.name

    if dry_run:
        action = "overwrite" if exists else "create"
        if exists and not overwrite:
            action = "skip (exists)"
        _log(verbose, f"[dry-run] {action:10s}  {tag}")
        return not (exists and not overwrite)

    if exists and not overwrite:
        _log(verbose, f"[skip]      {tag}  (exists — pass --overwrite to replace)")
        return False

    if exists:
        bak = backup_file(path)
        _log(verbose, f"[backup]    {tag} → {bak.name if bak else '?'}")

    write_atomic(path, content)
    verb = "overwrite" if exists else "create"
    _log(verbose, f"[{verb}]  {tag}")
    return True


# ── User-region helpers ───────────────────────────────────────────────────────

def extract_user_regions(text: str) -> list[str]:
    """Return the body of every user region in *text*, in document order."""
    return [m.group(1) for m in _RE_REGION.finditer(text)]


def splice_user_regions(generated: str, existing: str) -> str:
    """Graft user-region bodies from *existing* into *generated*.

    Matches regions by position (index 0, 1, 2 …).  If the counts differ
    the minimum of the two is used; remaining generated regions are kept as-is.

    Typical use-case: re-running ``sdlos create --overwrite`` should preserve
    any code the developer placed between the USER_BEGIN / USER_END markers.
    """
    gen_matches = list(_RE_REGION.finditer(generated))
    ex_matches  = list(_RE_REGION.finditer(existing))

    if not gen_matches or not ex_matches:
        return generated

    out: list[str] = []
    cursor = 0

    for i in range(min(len(gen_matches), len(ex_matches))):
        gm = gen_matches[i]
        em = ex_matches[i]

        # Everything in *generated* up to (but not including) this region.
        out.append(generated[cursor : gm.start()])
        # Re-emit the opening marker.
        out.append(USER_BEGIN)
        # Preserve the user's body verbatim from the *existing* file.
        out.append(em.group(1))
        # Re-emit the closing marker.
        out.append(USER_END)
        cursor = gm.end()

    out.append(generated[cursor:])
    return "".join(out)


# ── Internal ─────────────────────────────────────────────────────────────────

def _log(verbose: bool, msg: str) -> None:
    if verbose:
        print(f"  {msg}")
