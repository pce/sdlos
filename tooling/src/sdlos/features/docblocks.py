"""
sdlos.features.docblocks
========================
AST-reflection-based Doxygen docblock generator for C++ source files.

How it works
------------
1. Parse the source file with libclang (``-std=c++23``, best-effort).
2. Walk every ``FUNCTION_DECL`` and ``CXX_METHOD`` cursor.
3. Read the raw source lines immediately preceding each declaration.
4. If no ``/**`` block is present:
     a. Generate a ``@brief`` from the function name using ``naming_patterns``
        in the knowledge pack.
     b. Generate ``@param`` lines from parameter names + types using
        ``param_taxonomy`` and ``type_taxonomy``.
     c. Generate a ``@return`` line from the return type using
        ``return_taxonomy``.
     d. Optionally inject ``@warning`` tags from pre-computed SCA issues.
5. Insert the generated docblock at the correct line in the source.
6. Write the patched file back to disk (unless ``dry_run=True``).

Knowledge-pack integration
--------------------------
All human-readable strings (brief verbs, param hints, type descriptions,
return sentences, security warnings) come from
``sdlos/data/knowledge_packs.yaml``.  No taxonomy strings are hard-coded
here.

Hook API
--------
``Docblocks`` satisfies the ``PostProcessor`` protocol::

    db = Docblocks()
    result = db.process(path)           # PostProcessResult

It can also be used standalone::

    db       = Docblocks(dry_run=True)
    modified = db.apply_to_file(path)   # bool

Or on a source string (for testing / preview without touching the disk)::

    db      = Docblocks()
    patched = db.apply_to_source(source, path=Path("foo.cxx"))
"""
from __future__ import annotations

import re
import textwrap
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

# Knowledge packs are loaded lazily.
_KP: Optional[dict] = None


def _knowledge_packs() -> dict:
    global _KP
    if _KP is None:
        import yaml
        _kp_path = Path(__file__).parent.parent / "data" / "knowledge_packs.yaml"
        with _kp_path.open(encoding="utf-8") as fh:
            _KP = yaml.safe_load(fh) or {}
    return _KP


# ── Regex helpers ──────────────────────────────────────────────────────────────

_DOXYGEN_OPEN_RE   = re.compile(r"/\*\*")
_DOXYGEN_CLOSE_RE  = re.compile(r"\*/")
_LINE_COMMENT_RE   = re.compile(r"^\s*///")

# Matches identifier characters (for word splitting of function names).
_WORD_SPLIT_RE     = re.compile(r"[_\s]+|(?<=[a-z])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])")


# ── GeneratedDocblock ──────────────────────────────────────────────────────────

@dataclass
class GeneratedDocblock:
    """A Doxygen docblock that has been generated for one function."""

    insert_before_line: int   # 1-based line number; docblock is inserted here
    function_name: str
    brief: str
    params: list[tuple[str, str, str]]  # (name, type_spelling, description)
    return_type: str
    return_desc: str
    warnings: list[str] = field(default_factory=list)
    notes: list[str]    = field(default_factory=list)

    def render(self, indent: str = "") -> str:
        """Render the docblock as a formatted C++ ``/** … */`` comment.

        Parameters
        ----------
        indent:
            Whitespace prefix matching the indentation of the function
            declaration (e.g. ``"    "`` for a method inside a class).

        Returns
        -------
        str
            The complete docblock string, not including a trailing newline.
        """
        lines: list[str] = [f"{indent}/**"]

        # @brief
        brief_text = self.brief or _fallback_brief(self.function_name)
        for i, segment in enumerate(textwrap.wrap(brief_text, width=76 - len(indent))):
            prefix = " * @brief " if i == 0 else " *        "
            lines.append(f"{indent}{prefix}{segment}")

        # @param — only when the function has parameters
        if self.params:
            lines.append(f"{indent} *")
            # Compute alignment width for parameter names.
            max_name = max(len(p[0]) for p in self.params)
            for pname, _ptype, pdesc in self.params:
                pad = " " * (max_name - len(pname))
                pdesc = pdesc or f"{_ptype} parameter"
                for j, segment in enumerate(
                    textwrap.wrap(pdesc, width=72 - len(indent) - max_name)
                ):
                    if j == 0:
                        lines.append(f"{indent} * @param {pname}{pad}  {segment}")
                    else:
                        lines.append(
                            f"{indent} *        {' ' * (max_name + 2)}{segment}"
                        )

        # @return
        if self.return_desc:
            lines.append(f"{indent} *")
            for j, segment in enumerate(
                textwrap.wrap(self.return_desc, width=76 - len(indent))
            ):
                prefix = " * @return " if j == 0 else " *         "
                lines.append(f"{indent}{prefix}{segment}")

        # @warning
        if self.warnings:
            lines.append(f"{indent} *")
            for w in self.warnings:
                for j, segment in enumerate(
                    textwrap.wrap(w, width=76 - len(indent))
                ):
                    prefix = " * @warning " if j == 0 else " *          "
                    lines.append(f"{indent}{prefix}{segment}")

        # @note
        if self.notes:
            lines.append(f"{indent} *")
            for n in self.notes:
                for j, segment in enumerate(
                    textwrap.wrap(n, width=76 - len(indent))
                ):
                    prefix = " * @note " if j == 0 else " *       "
                    lines.append(f"{indent}{prefix}{segment}")

        lines.append(f"{indent} */")
        return "\n".join(lines)


# ── Docblocks ──────────────────────────────────────────────────────────────────

class Docblocks:
    """Generate and insert Doxygen docblocks by reflecting on a C++ AST.

    Parameters
    ----------
    extra_args:
        Additional libclang compilation flags (e.g. ``["-I", "include/"]``).
        ``-std=c++23`` and ``-x c++`` are always included.
    skip_anonymous:
        When True (default), skip functions/methods in anonymous namespaces
        (they are implementation details not meant for public documentation).
    skip_operators:
        When True (default), skip ``operator`` overloads.
    min_params:
        Minimum parameter count to trigger docblock generation.  Functions
        with fewer params than this get a brief-only docblock regardless.
        Default: 0 (always generate).
    dry_run:
        When True, compute changes but do not write the file to disk.
    """

    _DEFAULT_ARGS = ["-std=c++23", "-x", "c++"]

    def __init__(
        self,
        extra_args: Optional[list[str]] = None,
        skip_anonymous: bool = True,
        skip_operators: bool = True,
        min_params: int = 0,
        dry_run: bool = False,
    ) -> None:
        self.extra_args       = extra_args or []
        self.skip_anonymous   = skip_anonymous
        self.skip_operators   = skip_operators
        self.min_params       = min_params
        self.dry_run          = dry_run

    # ── PostProcessor hook ─────────────────────────────────────────────────────

    def process(self, path: Path) -> "PostProcessResult":
        """``PostProcessor`` protocol entry-point.

        Inserts docblocks into *path* and returns a :class:`PostProcessResult`.
        """
        from .post_process import PostProcessResult  # avoid circular at module level

        try:
            modified = self.apply_to_file(path)
            return PostProcessResult(
                processor="Docblocks",
                modified=modified,
            )
        except Exception as exc:  # noqa: BLE001
            return PostProcessResult(
                processor="Docblocks",
                modified=False,
                error=str(exc),
            )

    # ── Public API ─────────────────────────────────────────────────────────────

    def apply_to_file(
        self,
        path: Path,
        sca_issues: Optional[list] = None,
    ) -> bool:
        """Reflect on *path* and insert missing Doxygen docblocks in-place.

        Parameters
        ----------
        path:
            C++ source file to augment.
        sca_issues:
            Optional SCA findings from :class:`~sdlos.features.sca.SCA`.
            Any issue whose ``line`` falls within a function body will be
            merged as a ``@warning`` tag into that function's docblock.

        Returns
        -------
        bool
            True when the file was modified and written to disk.
        """
        if not path.exists():
            raise FileNotFoundError(f"Docblocks: file not found: {path}")

        source = path.read_text(encoding="utf-8")
        patched = self.apply_to_source(source, path=path, sca_issues=sca_issues)

        if patched == source:
            return False

        if not self.dry_run:
            path.write_text(patched, encoding="utf-8")

        return True

    def apply_to_source(
        self,
        source: str,
        path: Optional[Path] = None,
        sca_issues: Optional[list] = None,
    ) -> str:
        """Reflect on *source* and return the patched text with inserted docblocks.

        Parameters
        ----------
        source:
            Raw C++ source text.
        path:
            Filesystem path used for libclang parsing; a temp file is created
            when ``None``.
        sca_issues:
            SCA findings to merge as ``@warning`` tags.

        Returns
        -------
        str
            Source text with docblocks inserted.  Unchanged when nothing was
            added.
        """
        import tempfile, os

        if path is None:
            tmp = tempfile.NamedTemporaryFile(suffix=".cxx", delete=False, mode="w", encoding="utf-8")
            tmp.write(source)
            tmp.close()
            work_path = Path(tmp.name)
            _cleanup = True
        else:
            work_path = path
            _cleanup = False

        try:
            docblocks = self._reflect(work_path, sca_issues or [])
        finally:
            if _cleanup:
                try:
                    os.unlink(work_path)
                except OSError:
                    pass

        if not docblocks:
            return source

        return _insert_docblocks(source, docblocks)

    # ── Reflection ─────────────────────────────────────────────────────────────

    def _reflect(
        self,
        path: Path,
        sca_issues: list,
    ) -> list[GeneratedDocblock]:
        """Parse *path* with libclang and generate :class:`GeneratedDocblock` objects.

        Only functions / methods that currently lack a preceding ``/**`` block
        are included in the result.

        Parameters
        ----------
        path:
            C++ source file (must exist on disk).
        sca_issues:
            Pre-computed SCA findings used to inject ``@warning`` tags.

        Returns
        -------
        list[GeneratedDocblock]
            Sorted by ``insert_before_line`` in descending order so that
            inserting them from the bottom of the file upward keeps line
            numbers consistent.
        """
        try:
            from clang import cindex
        except ImportError:
            return []  # libclang not installed — silently skip

        args = list(self._DEFAULT_ARGS) + list(self.extra_args)

        idx = cindex.Index.create()
        tu  = idx.parse(
            str(path),
            args=args,
            options=(
                cindex.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD
                | cindex.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES
            ),
        )

        source_lines = path.read_text(encoding="utf-8").splitlines()

        # Map sca_issues by line for quick lookup: line → list[issue]
        sca_by_line: dict[int, list] = {}
        for issue in sca_issues:
            sca_by_line.setdefault(issue.line, []).append(issue)

        results: list[GeneratedDocblock] = []
        seen_lines: set[int] = set()  # avoid duplicate entries for the same decl

        self._walk_for_docblocks(
            tu.cursor, path, source_lines, sca_by_line, results, seen_lines, cindex
        )

        # Sort descending by line so insertions from bottom upward keep offsets valid.
        results.sort(key=lambda d: d.insert_before_line, reverse=True)
        return results

    def _walk_for_docblocks(
        self,
        node,
        path: Path,
        source_lines: list[str],
        sca_by_line: dict,
        results: list,
        seen_lines: set,
        cindex,
    ) -> None:
        """Recursively walk the AST and collect functions needing docblocks."""
        CK = cindex.CursorKind

        # Only process nodes from our own file.
        if node.location.file and node.location.file.name != str(path):
            return

        interesting = {
            CK.FUNCTION_DECL,
            CK.CXX_METHOD,
            CK.CONSTRUCTOR,
            CK.DESTRUCTOR,
            CK.FUNCTION_TEMPLATE,
        }

        if node.kind in interesting:
            self._maybe_add_docblock(
                node, path, source_lines, sca_by_line, results, seen_lines, cindex
            )

        for child in node.get_children():
            self._walk_for_docblocks(
                child, path, source_lines, sca_by_line, results, seen_lines, cindex
            )

    def _maybe_add_docblock(
        self,
        node,
        path: Path,
        source_lines: list[str],
        sca_by_line: dict,
        results: list,
        seen_lines: set,
        cindex,
    ) -> None:
        """Add a :class:`GeneratedDocblock` for *node* if it lacks one."""
        decl_line = node.location.line  # 1-based
        if not decl_line or decl_line in seen_lines:
            return

        spelling = node.spelling or ""

        # Optionally skip anonymous-namespace functions (impl details).
        if self.skip_anonymous and _is_in_anonymous_namespace(node):
            return

        # Skip operator overloads by default.
        if self.skip_operators and spelling.startswith("operator"):
            return

        # Skip compiler-generated boilerplate (empty spelling = implicit).
        if not spelling:
            return

        # Check whether a docblock already exists immediately above.
        if _has_preceding_docblock(source_lines, decl_line):
            return

        seen_lines.add(decl_line)

        # Determine insertion line (before any template<> line above decl).
        insert_line = _find_insert_line(source_lines, decl_line)

        # Collect indentation from the declaration line.
        decl_text = source_lines[decl_line - 1] if decl_line <= len(source_lines) else ""
        indent = _leading_whitespace(decl_text)

        # --- Brief ---
        brief = _generate_brief(spelling)

        # --- Parameters ---
        params = _generate_params(node, cindex)

        # --- Return ---
        ret_type_spelling, ret_desc = _generate_return(node, cindex)

        # --- Warnings from SCA ---
        warnings: list[str] = []
        # Collect SCA issues whose line falls within ±10 lines of the declaration
        # (a rough heuristic for function-level issues).
        for offset in range(-2, 15):
            check_line = decl_line + offset
            for issue in sca_by_line.get(check_line, []):
                w = _sca_issue_to_warning(issue)
                if w and w not in warnings:
                    warnings.append(w)

        results.append(GeneratedDocblock(
            insert_before_line=insert_line,
            function_name=spelling,
            brief=brief,
            params=params,
            return_type=ret_type_spelling,
            return_desc=ret_desc,
            warnings=warnings,
        ))


# ── Knowledge-pack helpers ─────────────────────────────────────────────────────

def _generate_brief(spelling: str) -> str:
    """Derive a ``@brief`` line from a function name using naming_patterns.

    Matches the function spelling against every ``prefix_:`` entry in
    ``naming_patterns`` (longest match wins).

    Parameters
    ----------
    spelling:
        The raw function name as reported by libclang (e.g. ``get_node_by_id``).

    Returns
    -------
    str
        A brief sentence fragment, e.g. ``"Returns the node by id"``.
    """
    kp = _knowledge_packs()
    naming: dict[str, str] = kp.get("naming_patterns", {})
    lower = spelling.lower()

    # Try prefixes from longest to shortest for the best match.
    best_prefix = ""
    best_verb   = ""
    for pattern, verb in naming.items():
        key = pattern.rstrip("_").rstrip("*")
        if lower.startswith(key) and len(key) > len(best_prefix):
            best_prefix = key
            best_verb   = verb

    if best_verb:
        # Remainder of the name after the matched prefix.
        remainder = spelling[len(best_prefix):]
        if remainder.startswith("_"):
            remainder = remainder[1:]
        human_remainder = _humanise(remainder)
        return f"{best_verb} {human_remainder}".rstrip()

    return _fallback_brief(spelling)


def _fallback_brief(spelling: str) -> str:
    """Generate a minimal @brief from the function name when no prefix matches.

    Parameters
    ----------
    spelling:
        Raw C++ function name.

    Returns
    -------
    str
        Capitalised human-readable phrase derived from the name.
    """
    return _humanise(spelling).capitalize()


def _humanise(name: str) -> str:
    """Convert a ``snake_case`` or ``camelCase`` identifier to a phrase.

    Parameters
    ----------
    name:
        Identifier string.

    Returns
    -------
    str
        Space-separated lower-case words.
    """
    if not name:
        return name
    # Insert spaces at camelCase boundaries.
    spaced = re.sub(r"([a-z])([A-Z])", r"\1 \2", name)
    spaced = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1 \2", spaced)
    # Replace underscores / multiple spaces.
    words = re.split(r"[_\s]+", spaced)
    return " ".join(w for w in words if w).lower()


def _generate_params(node, cindex) -> list[tuple[str, str, str]]:
    """Generate ``(name, type_spelling, description)`` triples for all params.

    Parameters
    ----------
    node:
        libclang ``FUNCTION_DECL`` or ``CXX_METHOD`` cursor.
    cindex:
        The imported ``clang.cindex`` module.

    Returns
    -------
    list[tuple[str, str, str]]
        One entry per parameter in declaration order.
    """
    kp           = _knowledge_packs()
    param_tax: dict[str, str] = kp.get("param_taxonomy", {})
    type_tax:  dict[str, str] = kp.get("type_taxonomy",  {})

    result: list[tuple[str, str, str]] = []
    for param in node.get_arguments():
        pname      = param.spelling or f"param{len(result)}"
        type_spell = param.type.spelling if param.type else ""

        desc = _lookup_param(pname.lower(), param_tax)
        if not desc:
            desc = _lookup_type(type_spell, type_tax)
        if not desc:
            desc = f"{type_spell} value" if type_spell else "parameter"

        result.append((pname, type_spell, desc))

    return result


def _generate_return(node, cindex) -> tuple[str, str]:
    """Generate ``(type_spelling, @return description)`` for the return type.

    Parameters
    ----------
    node:
        libclang function cursor.
    cindex:
        The imported ``clang.cindex`` module.

    Returns
    -------
    tuple[str, str]
        ``(canonical_type_spelling, return_sentence)``.  The return sentence
        is an empty string when the return type is ``void``.
    """
    kp          = _knowledge_packs()
    return_tax: dict[str, str] = kp.get("return_taxonomy", {})

    try:
        ret = node.result_type
        type_spell = ret.spelling if ret else "void"
    except Exception:  # noqa: BLE001
        type_spell = "void"

    desc = _lookup_return(type_spell, return_tax)
    return type_spell, desc


def _lookup_param(name_lower: str, taxonomy: dict[str, str]) -> str:
    """Longest-key substring match for a parameter name in *taxonomy*.

    Parameters
    ----------
    name_lower:
        Lower-cased parameter name.
    taxonomy:
        ``param_taxonomy`` dict from the knowledge pack.

    Returns
    -------
    str
        Matched description or an empty string when no match is found.
    """
    best_key  = ""
    best_desc = ""
    for key, desc in taxonomy.items():
        k = key.lower()
        if k in name_lower and len(k) > len(best_key):
            best_key  = k
            best_desc = desc
    return best_desc


def _lookup_type(type_spell: str, taxonomy: dict[str, str]) -> str:
    """Substring match for a C++ type spelling in *taxonomy*.

    Parameters
    ----------
    type_spell:
        Full canonical type spelling (e.g. ``"std::vector<int>"``).
    taxonomy:
        ``type_taxonomy`` dict from the knowledge pack.

    Returns
    -------
    str
        Human-readable type description or an empty string.
    """
    best_key  = ""
    best_desc = ""
    for key, desc in taxonomy.items():
        if key in type_spell and len(key) > len(best_key):
            best_key  = key
            best_desc = desc
    return best_desc


def _lookup_return(type_spell: str, taxonomy: dict[str, str]) -> str:
    """Match a return type spelling against *taxonomy*.

    Handles ``void`` explicitly (returns ``""`` to suppress ``@return``).

    Parameters
    ----------
    type_spell:
        Return type spelling.
    taxonomy:
        ``return_taxonomy`` dict from the knowledge pack.

    Returns
    -------
    str
        ``@return`` sentence, or ``""`` for void.
    """
    if "void" in type_spell:
        return ""

    best_key  = ""
    best_desc = ""
    for key, desc in taxonomy.items():
        if key == "void":
            continue
        if key in type_spell and len(key) > len(best_key):
            best_key  = key
            best_desc = desc

    if best_desc:
        return best_desc

    # Generic fallback based on pointer / reference presence.
    if "*" in type_spell:
        return "Pointer to the result, or nullptr on failure"
    if "&" in type_spell:
        return "Reference to the result"
    return f"{type_spell} result"


def _sca_issue_to_warning(issue) -> str:
    """Convert an SCA :class:`~sdlos.features.sca.Issue` to a ``@warning`` string.

    Parameters
    ----------
    issue:
        An :class:`~sdlos.features.sca.Issue` dataclass instance.

    Returns
    -------
    str
        A concise warning sentence suitable for a Doxygen ``@warning`` tag.
    """
    msg = issue.message or ""
    note = issue.note or ""
    if note:
        return f"{msg} — {note}"
    return msg


# ── Source-level helpers ───────────────────────────────────────────────────────

def _has_preceding_docblock(lines: list[str], decl_line: int) -> bool:
    """Check whether a ``/** … */`` block exists immediately above *decl_line*.

    Scans upward from ``decl_line - 1``, skipping blank lines, to see if the
    nearest non-blank content is a doxygen comment block.

    Parameters
    ----------
    lines:
        All source lines (0-indexed internally).
    decl_line:
        1-based declaration line number.

    Returns
    -------
    bool
        True when a docblock is already present.
    """
    idx = decl_line - 2  # convert to 0-based, then step one above
    # Skip blank lines above the declaration.
    while idx >= 0 and not lines[idx].strip():
        idx -= 1
    if idx < 0:
        return False
    line = lines[idx]
    stripped = line.strip()
    # Docblock close marker or a triple-slash comment.
    if stripped.endswith("*/") or stripped.startswith("///") or stripped.startswith("/**"):
        return True
    return False


def _find_insert_line(source_lines: list[str], decl_line: int) -> int:
    """Return the 1-based line number at which to insert the docblock.

    Walks upward from *decl_line* to skip ``template<…>`` lines and
    ``[[nodiscard]]`` / ``[[maybe_unused]]`` attribute lines that belong to
    the same declaration.

    Parameters
    ----------
    source_lines:
        All source lines (0-indexed).
    decl_line:
        1-based declaration line.

    Returns
    -------
    int
        1-based line number to insert *before* (i.e. the docblock is placed
        starting at this line in the output).
    """
    idx = decl_line - 1  # 0-based
    # Walk upward through template<> / attribute lines.
    while idx > 0:
        prev = source_lines[idx - 1].strip()
        if (
            prev.startswith("template")
            or prev.startswith("[[")
            or prev == "inline"
            or prev == "virtual"
            or prev == "explicit"
            or prev == "static"
            or prev == "constexpr"
            or prev == "consteval"
            or prev == "constinit"
        ):
            idx -= 1
        else:
            break
    return idx + 1  # convert back to 1-based


def _leading_whitespace(line: str) -> str:
    """Return the leading whitespace of *line*.

    Parameters
    ----------
    line:
        Source line.

    Returns
    -------
    str
        The prefix of spaces / tabs before the first non-whitespace character.
    """
    return line[: len(line) - len(line.lstrip())]


def _is_in_anonymous_namespace(node) -> bool:
    """Return True if *node* is declared inside an anonymous namespace.

    Parameters
    ----------
    node:
        libclang cursor.

    Returns
    -------
    bool
    """
    parent = node.semantic_parent
    while parent is not None:
        try:
            from clang.cindex import CursorKind
            if parent.kind == CursorKind.NAMESPACE and not parent.spelling:
                return True
            if parent.kind == CursorKind.TRANSLATION_UNIT:
                break
        except Exception:  # noqa: BLE001
            break
        parent = parent.semantic_parent
    return False


# ── Source patcher ─────────────────────────────────────────────────────────────

def _insert_docblocks(source: str, docblocks: list[GeneratedDocblock]) -> str:
    """Insert all generated docblocks into *source*.

    *docblocks* must be sorted in **descending** line order so that inserting
    from the bottom of the file upward keeps all earlier line numbers valid.

    Parameters
    ----------
    source:
        Original C++ source text.
    docblocks:
        Docblocks to insert, sorted descending by ``insert_before_line``.

    Returns
    -------
    str
        The patched source text.
    """
    lines = source.splitlines(keepends=True)

    for db in docblocks:
        insert_idx = db.insert_before_line - 1  # 0-based

        # Determine indentation from the declaration line.
        if insert_idx < len(lines):
            indent = _leading_whitespace(lines[insert_idx])
        else:
            indent = ""

        rendered = db.render(indent=indent)
        # Make sure the rendered block ends with a newline before the declaration.
        block_lines = [l + "\n" for l in rendered.split("\n")]

        lines[insert_idx:insert_idx] = block_lines

    return "".join(lines)
