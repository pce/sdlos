"""
sdlos.features.sanitize
File-integrity processor — scans source files for hidden, invisible, or
suspicious byte sequences that have no place in source code and that can
serve as watermarks, covert-channel carriers, or tracking tokens.

Categories detected
BOM
    UTF-8  (0xEF 0xBB 0xBF), UTF-16 LE/BE, UTF-32 LE/BE at the start of
    a file.  Also flags a bare U+FEFF (ZERO WIDTH NO-BREAK SPACE) anywhere
    else in the file — it is invisible and useless outside a BOM position.

CONTROL_CHAR
    ASCII C0 controls (U+0000–U+001F) other than TAB (U+0009), LF (U+000A),
    and CR (U+000D), plus the DEL character (U+007F).  Also the C1 control
    range U+0080–U+009F.  None of these have a legitimate role in C++, CSS,
    Jade, or Python source.

ZERO_WIDTH
    Invisible Unicode spacing characters that can encode bit-streams through
    their presence or absence pattern:
        U+00AD  SOFT HYPHEN
        U+200B  ZERO WIDTH SPACE
        U+200C  ZERO WIDTH NON-JOINER
        U+200D  ZERO WIDTH JOINER
        U+2060  WORD JOINER
        U+2061–U+2064  invisible mathematical operators
        U+206A–U+206F  inhibit/activate symmetric/Arabic form shaping
        U+FEFF  ZERO WIDTH NO-BREAK SPACE (non-BOM position)

BIDI_OVERRIDE
    Unicode bidirectional control characters.  CVE-2021-42574 ("Trojan
    Source") demonstrated that these can make malicious code visually
    indistinguishable from benign code in editors and code-review tools:
        U+202A–U+202E  LRE, RLE, PDF, LRO, RLO
        U+2066–U+2069  LRI, RLI, FSI, PDI

VARIATION_SEL
    Unicode variation selectors U+FE00–U+FE0F and U+E0100–U+E01EF.
    They alter the glyph rendered for the preceding character and are
    invisible in most fonts.  No legitimate use in source code.

TAG_CHAR
    The Unicode "Tags" block U+E0000–U+E007F.  These characters are
    completely invisible, have no rendering, and were originally intended
    for language-tag metadata.  They are the highest-risk tracking vector:
    a 128-codepoint alphabet that can losslessly encode ASCII text (e.g. an
    author ID or build timestamp) as an invisible suffix on any comment line.

MIXED_ENDINGS
    Files that mix CRLF and LF line endings.  Inconsistent endings are a
    common side-effect of Windows/macOS tool chains touching the same file
    and can hide differences in diffs.

NON_ASCII
    Any byte above 0x7F.  Disabled by default; enable with
    ``strict_ascii=True`` when you want to enforce pure-ASCII source.

Skip ranges and allow patterns
Individual lines can be excluded from scanning in two ways:

1. **Annotation markers** — place these comments in the source file:

       // SANITIZE:SKIP_BEGIN
       ... lines with known-good non-ASCII content ...
       // SANITIZE:SKIP_END

   The markers are recognised in .cxx / .hxx / .css and .jade files
   (``//`` prefix) and in .py files (``#`` prefix).

2. **Programmatic skip_ranges** — pass a list of ``(first_line, last_line)``
   tuples (1-based, inclusive) to :class:`FileIntegrityProcessor`.

Allow-list
Pass a ``frozenset`` of Unicode code points to ``allow_codepoints``.
Any character whose ``ord()`` value is in that set is never reported,
regardless of category.  Useful for a known copyright symbol or a
deliberate non-breaking space in a comment.

Strip mode
When ``strip=True`` (requires the caller to also pass ``write=True`` at
the pipeline level) the processor rewrites the file with all flagged
characters removed or normalised:
    - BOM bytes stripped from the start of the file.
    - Zero-width / invisible / tag characters deleted.
    - BiDi override characters deleted.
    - Variation selectors deleted.
    - C0/C1 control chars (not TAB/LF/CR) replaced with a space.
    - U+007F DEL replaced with a space.
    - CRLF normalised to LF.

Strip mode does NOT touch lines that fall inside a skip range.

Usage

Standalone:

    from sdlos.features.sanitize import FileIntegrityProcessor

    proc = FileIntegrityProcessor()
    result = proc.process(Path("src/render_tree.cc"))
    for f in result.issues:
        print(f"  [{f.kind}] L{f.line}:{f.col}  {f.message}")

In the pipeline (report-only)::

    pipeline = PostProcessPipeline([
        FileIntegrityProcessor(),
        SCAProcessor(level=2),
        ClangFormatProcessor(),
    ])

In the pipeline (strip bad chars)::

    pipeline = PostProcessPipeline([
        FileIntegrityProcessor(strip=True),
        ...
    ])

With skip ranges::

    proc = FileIntegrityProcessor(
        skip_ranges=[(10, 50), (200, 210)],
        allow_codepoints=frozenset({0x00A9}),  # copyright ©
    )
"""
from __future__ import annotations

import dataclasses
import re
from enum import Enum
from pathlib import Path
from typing import Optional



class IntegrityKind(str, Enum):
    """Classification of a file-integrity finding."""
    BOM           = "bom"           # byte-order mark
    CONTROL_CHAR  = "control_char"  # C0/C1 controls, DEL
    ZERO_WIDTH    = "zero_width"    # invisible spacing / joiner chars
    BIDI_OVERRIDE = "bidi_override" # Trojan-Source BiDi controls
    VARIATION_SEL = "variation_sel" # glyph-variant selectors
    TAG_CHAR      = "tag_char"      # U+E0000-U+E007F invisible tags
    MIXED_ENDINGS = "mixed_endings" # CRLF mixed with LF
    NON_ASCII     = "non_ascii"     # any byte > 0x7F (strict mode)



@dataclasses.dataclass(frozen=True)
class IntegrityFinding:
    """A single file-integrity finding.

    Attributes
    kind:
        Category — one of :class:`IntegrityKind`.
    line:
        1-based line number.  0 for file-level findings (BOM, mixed endings).
    col:
        1-based column (character offset within the line).  0 for file-level.
    codepoint:
        Unicode code point of the offending character, or ``-1`` for
        multi-character / structural findings (BOM bytes, mixed endings).
    message:
        Human-readable description including the U+XXXX notation.
    """
    kind:      IntegrityKind
    line:      int
    col:       int
    codepoint: int
    message:   str


# Known character sets

# C0 control characters: U+0000–U+001F, minus the three legal whitespace chars.
_C0_CONTROLS: frozenset[int] = frozenset(range(0x00, 0x20)) - {0x09, 0x0A, 0x0D}

# DEL
_DEL: int = 0x7F

# C1 control characters: U+0080–U+009F
_C1_CONTROLS: frozenset[int] = frozenset(range(0x80, 0xA0))

# Zero-width / invisible Unicode — highest risk for bit-stream steganography.
_ZERO_WIDTH: frozenset[int] = frozenset({
    0x00AD,  # SOFT HYPHEN
    0x200B,  # ZERO WIDTH SPACE
    0x200C,  # ZERO WIDTH NON-JOINER
    0x200D,  # ZERO WIDTH JOINER
    0x2060,  # WORD JOINER
    0x2061,  # FUNCTION APPLICATION (invisible)
    0x2062,  # INVISIBLE TIMES
    0x2063,  # INVISIBLE SEPARATOR
    0x2064,  # INVISIBLE PLUS
    0x206A,  # INHIBIT SYMMETRIC SWAPPING
    0x206B,  # ACTIVATE SYMMETRIC SWAPPING
    0x206C,  # INHIBIT ARABIC FORM SHAPING
    0x206D,  # ACTIVATE ARABIC FORM SHAPING
    0x206E,  # NATIONAL DIGIT SHAPES
    0x206F,  # NOMINAL DIGIT SHAPES
    0xFEFF,  # ZERO WIDTH NO-BREAK SPACE / BOM — flagged outside BOM position
})

# BiDi override characters — CVE-2021-42574 "Trojan Source"
_BIDI_OVERRIDES: frozenset[int] = frozenset({
    0x200E,  # LEFT-TO-RIGHT MARK
    0x200F,  # RIGHT-TO-LEFT MARK
    0x202A,  # LEFT-TO-RIGHT EMBEDDING
    0x202B,  # RIGHT-TO-LEFT EMBEDDING
    0x202C,  # POP DIRECTIONAL FORMATTING
    0x202D,  # LEFT-TO-RIGHT OVERRIDE
    0x202E,  # RIGHT-TO-LEFT OVERRIDE
    0x2066,  # LEFT-TO-RIGHT ISOLATE
    0x2067,  # RIGHT-TO-LEFT ISOLATE
    0x2068,  # FIRST STRONG ISOLATE
    0x2069,  # POP DIRECTIONAL ISOLATE
})

# Variation selectors: U+FE00–U+FE0F (basic) and U+E0100–U+E01EF (supplement)
def _is_variation_selector(cp: int) -> bool:
    return 0xFE00 <= cp <= 0xFE0F or 0xE0100 <= cp <= 0xE01EF

# Unicode Tags block: U+E0000–U+E007F — completely invisible, no rendering.
def _is_tag_char(cp: int) -> bool:
    return 0xE0000 <= cp <= 0xE007F

# Known BOM signatures (raw bytes at file start)
_BOM_SIGNATURES: list[tuple[bytes, str]] = [
    (b"\xEF\xBB\xBF",     "UTF-8 BOM"),
    (b"\xFF\xFE\x00\x00", "UTF-32 LE BOM"),
    (b"\x00\x00\xFE\xFF", "UTF-32 BE BOM"),
    (b"\xFF\xFE",         "UTF-16 LE BOM"),
    (b"\xFE\xFF",         "UTF-16 BE BOM"),
]

# Annotation markers recognised in source files.
_SKIP_BEGIN_RE = re.compile(
    r"(?://|#)\s*SANITIZE:SKIP_BEGIN", re.IGNORECASE
)
_SKIP_END_RE = re.compile(
    r"(?://|#)\s*SANITIZE:SKIP_END", re.IGNORECASE
)


# Skip-range helpers

def _parse_annotation_ranges(lines: list[str]) -> list[tuple[int, int]]:
    """Scan *lines* for ``SANITIZE:SKIP_BEGIN`` / ``SANITIZE:SKIP_END`` markers.

    Returns a list of ``(first_line, last_line)`` tuples (1-based, inclusive).
    An unclosed BEGIN is implicitly closed at the last line of the file.
    """
    ranges: list[tuple[int, int]] = []
    open_at: Optional[int] = None
    for i, line in enumerate(lines, start=1):
        if _SKIP_BEGIN_RE.search(line):
            if open_at is None:
                open_at = i
        elif _SKIP_END_RE.search(line):
            if open_at is not None:
                ranges.append((open_at, i))
                open_at = None
    if open_at is not None:
        ranges.append((open_at, len(lines)))
    return ranges


def _build_skip_set(
    annotation_ranges: list[tuple[int, int]],
    manual_ranges:     list[tuple[int, int]],
) -> frozenset[int]:
    """Flatten all skip ranges into a set of skipped line numbers."""
    skipped: set[int] = set()
    for first, last in annotation_ranges + manual_ranges:
        skipped.update(range(first, last + 1))
    return frozenset(skipped)


# Core scanner

def scan_text(
    text:             str,
    skip_lines:       frozenset[int]  = frozenset(),
    allow_codepoints: frozenset[int]  = frozenset(),
    strict_ascii:     bool            = False,
    check_bidi:       bool            = True,
    check_zero_width: bool            = True,
    check_tag_chars:  bool            = True,
    check_variation:  bool            = True,
    check_control:    bool            = True,
) -> list[IntegrityFinding]:
    """Scan decoded *text* for suspicious characters.

    Parameters
    text:
        The full file content as a decoded Python ``str``.
    skip_lines:
        Set of 1-based line numbers to ignore entirely.
    allow_codepoints:
        Set of Unicode code points that are always permitted.
    strict_ascii:
        When True, flag every non-ASCII character (U+0080 and above) not
        already caught by a more specific check.
    check_bidi, check_zero_width, check_tag_chars, check_variation, check_control:
        Toggle individual check categories.

    Returns
    list[IntegrityFinding]
        Sorted by (line, col).
    """
    findings: list[IntegrityFinding] = []
    lines = text.splitlines(keepends=True)

    for line_no, line in enumerate(lines, start=1):
        if line_no in skip_lines:
            continue

        for col_no, ch in enumerate(line, start=1):
            cp = ord(ch)

            if cp in allow_codepoints:
                continue

            # C0/C1 control characters and DEL
            if check_control:
                if cp in _C0_CONTROLS:
                    findings.append(IntegrityFinding(
                        kind=IntegrityKind.CONTROL_CHAR,
                        line=line_no, col=col_no, codepoint=cp,
                        message=(
                            f"C0 control character U+{cp:04X} "
                            f"({_ctrl_name(cp)})"
                        ),
                    ))
                    continue
                if cp == _DEL:
                    findings.append(IntegrityFinding(
                        kind=IntegrityKind.CONTROL_CHAR,
                        line=line_no, col=col_no, codepoint=cp,
                        message="DEL character U+007F",
                    ))
                    continue
                if cp in _C1_CONTROLS:
                    findings.append(IntegrityFinding(
                        kind=IntegrityKind.CONTROL_CHAR,
                        line=line_no, col=col_no, codepoint=cp,
                        message=f"C1 control character U+{cp:04X}",
                    ))
                    continue

            #  Tag characters — invisible, tracking risk
            if check_tag_chars and _is_tag_char(cp):
                findings.append(IntegrityFinding(
                    kind=IntegrityKind.TAG_CHAR,
                    line=line_no, col=col_no, codepoint=cp,
                    message=(
                        f"Unicode tag character U+{cp:05X} "
                        f"(invisible; can encode '{chr(cp - 0xE0000)}' as a "
                        f"covert watermark)"
                    ),
                ))
                continue

            #  Variation selectors
            if check_variation and _is_variation_selector(cp):
                findings.append(IntegrityFinding(
                    kind=IntegrityKind.VARIATION_SEL,
                    line=line_no, col=col_no, codepoint=cp,
                    message=f"Variation selector U+{cp:04X} (invisible glyph modifier)",
                ))
                continue

            #  BiDi overrides — Trojan Source
            if check_bidi and cp in _BIDI_OVERRIDES:
                findings.append(IntegrityFinding(
                    kind=IntegrityKind.BIDI_OVERRIDE,
                    line=line_no, col=col_no, codepoint=cp,
                    message=(
                        f"Unicode BiDi control U+{cp:04X} "
                        f"({_bidi_name(cp)}) — CVE-2021-42574 vector"
                    ),
                ))
                continue

            # Zero-width / invisible joiners
            if check_zero_width and cp in _ZERO_WIDTH:
                findings.append(IntegrityFinding(
                    kind=IntegrityKind.ZERO_WIDTH,
                    line=line_no, col=col_no, codepoint=cp,
                    message=(
                        f"Zero-width / invisible character U+{cp:04X} "
                        f"({_zw_name(cp)})"
                    ),
                ))
                continue

            # Non-ASCII strict mode
            if strict_ascii and cp > 0x7F:
                findings.append(IntegrityFinding(
                    kind=IntegrityKind.NON_ASCII,
                    line=line_no, col=col_no, codepoint=cp,
                    message=f"Non-ASCII character U+{cp:04X} {ch!r} (strict mode)",
                ))

    return sorted(findings, key=lambda f: (f.line, f.col))


def scan_bytes(raw: bytes) -> tuple[Optional[IntegrityFinding], bytes]:
    """Check *raw* for a leading BOM and strip it if present.

    Returns a tuple of ``(finding_or_None, payload_without_bom)``.
    The returned bytes are the original *raw* with the BOM prefix removed
    so that subsequent UTF-8 decoding succeeds.
    """
    for bom_bytes, label in _BOM_SIGNATURES:
        if raw.startswith(bom_bytes):
            finding = IntegrityFinding(
                kind=IntegrityKind.BOM,
                line=0, col=0, codepoint=-1,
                message=f"{label} detected ({len(bom_bytes)} bytes: "
                        + " ".join(f"{b:02X}" for b in bom_bytes) + ")",
            )
            return finding, raw[len(bom_bytes):]
    return None, raw


def detect_mixed_endings(text: str) -> Optional[IntegrityFinding]:
    """Return a finding if *text* mixes CRLF and bare LF line endings."""
    has_crlf = "\r\n" in text
    # bare LF: present somewhere that is not part of a CRLF
    has_lf   = bool(re.search(r"(?<!\r)\n", text))
    if has_crlf and has_lf:
        crlf_count = text.count("\r\n")
        lf_count   = len(re.findall(r"(?<!\r)\n", text))
        return IntegrityFinding(
            kind=IntegrityKind.MIXED_ENDINGS,
            line=0, col=0, codepoint=-1,
            message=(
                f"Mixed line endings: {crlf_count} CRLF and "
                f"{lf_count} bare LF"
            ),
        )
    return None



def _strip_dangerous(
    raw:              bytes,
    skip_lines:       frozenset[int],
    check_bidi:       bool = True,
    check_zero_width: bool = True,
    check_tag_chars:  bool = True,
    check_variation:  bool = True,
    check_control:    bool = True,
    normalize_endings: bool = True,
) -> bytes:
    """Remove dangerous / invisible characters from *raw* and return clean bytes.

    Operates line-by-line so that ``skip_lines`` is honoured.  BOM is always
    stripped from the very start of the file.

    The result is re-encoded as UTF-8 with LF line endings (when
    ``normalize_endings`` is True).
    """
    # Strip leading BOM first.
    for bom_bytes, _ in _BOM_SIGNATURES:
        if raw.startswith(bom_bytes):
            raw = raw[len(bom_bytes):]
            break

    # Decode — try UTF-8, fall back to latin-1 so we never crash.
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        text = raw.decode("latin-1")

    # Normalise CRLF → LF on all lines (not just non-skipped ones).
    if normalize_endings:
        text = text.replace("\r\n", "\n").replace("\r", "\n")

    lines    = text.splitlines(keepends=True)
    out_lines: list[str] = []

    for line_no, line in enumerate(lines, start=1):
        if line_no in skip_lines:
            out_lines.append(line)
            continue

        cleaned: list[str] = []
        for ch in line:
            cp = ord(ch)
            # Always keep legal ASCII + newline
            if cp == 0x0A or cp == 0x0D or cp == 0x09:
                cleaned.append(ch)
                continue
            if cp < 0x80:
                # Replace C0 controls and DEL with a space
                if cp in _C0_CONTROLS or cp == _DEL:
                    if check_control:
                        cleaned.append(" ")
                    else:
                        cleaned.append(ch)
                else:
                    cleaned.append(ch)
                continue
            # Non-ASCII
            if check_control and cp in _C1_CONTROLS:
                cleaned.append(" ")
                continue
            if check_tag_chars and _is_tag_char(cp):
                continue  # delete — no replacement
            if check_variation and _is_variation_selector(cp):
                continue  # delete
            if check_bidi and cp in _BIDI_OVERRIDES:
                continue  # delete
            if check_zero_width and cp in _ZERO_WIDTH:
                continue  # delete
            cleaned.append(ch)

        out_lines.append("".join(cleaned))

    return "".join(out_lines).encode("utf-8")


# PostProcessor

class FileIntegrityProcessor:
    """Scan (and optionally sanitise) a source file for hidden or dangerous bytes.

    This processor implements the :class:`~sdlos.features.post_process.PostProcessor`
    protocol and can be inserted at any position in a
    :class:`~sdlos.features.post_process.PostProcessPipeline`.

    Place it **first** in the pipeline when stripping: subsequent processors
    (clang-format, SCA, …) then operate on already-clean bytes.

    Parameters

    strip:
        When True the processor rewrites the file with dangerous characters
        removed.  When False (default) it reports findings without modifying
        the file.
    skip_ranges:
        List of ``(first_line, last_line)`` tuples (1-based, inclusive) to
        exclude from scanning.  Lines in these ranges are never flagged and,
        in strip mode, are written back verbatim.
    allow_codepoints:
        Frozenset of Unicode code points that are always permitted even if
        they would otherwise be flagged.  Example: ``frozenset({0x00A9})``
        to allow the copyright symbol ©.
    strict_ascii:
        Flag every non-ASCII character (U+0080+) not already caught by a
        more specific check.  Off by default.
    check_bom:
        Detect leading BOM bytes.  Default: True.
    check_control:
        Detect C0/C1 control characters and DEL.  Default: True.
    check_zero_width:
        Detect invisible zero-width / joiner characters.  Default: True.
    check_bidi:
        Detect Unicode BiDi override characters (Trojan Source).  Default: True.
    check_tag_chars:
        Detect Unicode tag characters (U+E0000–U+E007F) — strongest
        watermark vector.  Default: True.
    check_variation:
        Detect variation selectors.  Default: True.
    check_mixed_endings:
        Detect mixed CRLF / LF line endings.  Default: True.
    normalize_endings:
        In strip mode, normalise all line endings to LF.  Default: True.
    """

    def __init__(
        self,
        strip:               bool               = False,
        skip_ranges:         Optional[list[tuple[int, int]]] = None,
        allow_codepoints:    frozenset[int]     = frozenset(),
        strict_ascii:        bool               = False,
        check_bom:           bool               = True,
        check_control:       bool               = True,
        check_zero_width:    bool               = True,
        check_bidi:          bool               = True,
        check_tag_chars:     bool               = True,
        check_variation:     bool               = True,
        check_mixed_endings: bool               = True,
        normalize_endings:   bool               = True,
    ) -> None:
        self.strip               = strip
        self.skip_ranges         = skip_ranges or []
        self.allow_codepoints    = allow_codepoints
        self.strict_ascii        = strict_ascii
        self.check_bom           = check_bom
        self.check_control       = check_control
        self.check_zero_width    = check_zero_width
        self.check_bidi          = check_bidi
        self.check_tag_chars     = check_tag_chars
        self.check_variation     = check_variation
        self.check_mixed_endings = check_mixed_endings
        self.normalize_endings   = normalize_endings

    #  PostProcessor protocol

    def process(self, path: Path) -> "PostProcessResult":
        """Scan *path* and optionally strip dangerous bytes.

        Returns a :class:`~sdlos.features.post_process.PostProcessResult`
        whose ``.issues`` list contains :class:`IntegrityFinding` objects.
        The ``.modified`` flag is set when the file was rewritten.
        """
        # Local import to avoid circular dependency at module load time.
        from .post_process import PostProcessResult

        try:
            raw = path.read_bytes()
        except OSError as exc:
            return PostProcessResult(
                processor="FileIntegrity",
                modified=False,
                error=f"Cannot read {path}: {exc}",
            )

        findings: list[IntegrityFinding] = []
        modified = False

        #  BOM check
        bom_finding: Optional[IntegrityFinding] = None
        clean_raw = raw
        if self.check_bom:
            bom_finding, clean_raw = scan_bytes(raw)
            if bom_finding:
                findings.append(bom_finding)

        #  Decode for text-level scanning
        try:
            text = clean_raw.decode("utf-8")
        except UnicodeDecodeError:
            # Non-UTF-8 file — report and bail; do not attempt text scans.
            findings.append(IntegrityFinding(
                kind=IntegrityKind.CONTROL_CHAR,
                line=0, col=0, codepoint=-1,
                message=(
                    "File is not valid UTF-8.  "
                    "Possible binary content or corrupt encoding."
                ),
            ))
            return PostProcessResult(
                processor="FileIntegrity",
                modified=False,
                issues=findings,
            )

        # Build skip set (annotation markers + manual ranges)
        lines      = text.splitlines(keepends=True)
        ann_ranges = _parse_annotation_ranges(lines)
        skip_set   = _build_skip_set(ann_ranges, self.skip_ranges)

        # Mixed line endings
        if self.check_mixed_endings:
            me = detect_mixed_endings(text)
            if me:
                findings.append(me)

        # Character-level scan
        char_findings = scan_text(
            text,
            skip_lines       = skip_set,
            allow_codepoints = self.allow_codepoints,
            strict_ascii     = self.strict_ascii,
            check_bidi       = self.check_bidi,
            check_zero_width = self.check_zero_width,
            check_tag_chars  = self.check_tag_chars,
            check_variation  = self.check_variation,
            check_control    = self.check_control,
        )
        findings.extend(char_findings)

        # Strip mode
        if self.strip and findings:
            clean_bytes = _strip_dangerous(
                raw,
                skip_lines        = skip_set,
                check_bidi        = self.check_bidi,
                check_zero_width  = self.check_zero_width,
                check_tag_chars   = self.check_tag_chars,
                check_variation   = self.check_variation,
                check_control     = self.check_control,
                normalize_endings = self.normalize_endings,
            )
            if clean_bytes != raw:
                path.write_bytes(clean_bytes)
                modified = True

        return PostProcessResult(
            processor="FileIntegrity",
            modified=modified,
            issues=findings,
        )

    #  Convenience

    def scan(self, path: Path) -> list[IntegrityFinding]:
        """Shorthand: scan *path* and return findings without touching the file.

        Equivalent to constructing the processor with ``strip=False`` and
        calling ``process(path).issues``.
        """
        result = self.process(path)
        return result.issues  # type: ignore[return-value]


# Rich summary

def print_findings(
    findings: list[IntegrityFinding],
    path:     Path,
    console   = None,
) -> None:
    """Print a Rich-formatted integrity report for *findings*.

    Parameters

    findings:
        List of :class:`IntegrityFinding` objects.
    path:
        Source file that was scanned (used as panel title).
    console:
        Rich Console instance.  A fresh stderr console is created if omitted.
    """
    try:
        from rich.console import Console
        from rich.table import Table
        from rich import box
    except ImportError:
        for f in findings:
            loc = f"L{f.line}:{f.col}" if f.line else "file"
            print(f"  [{f.kind.value}] {loc}  {f.message}")
        return

    con = console or Console()

    if not findings:
        con.print(f"  [green]✓[/]  [dim]{path.name}[/dim]  — no integrity issues")
        return

    _kind_style: dict[IntegrityKind, str] = {
        IntegrityKind.TAG_CHAR:      "bold red",
        IntegrityKind.BIDI_OVERRIDE: "bold red",
        IntegrityKind.ZERO_WIDTH:    "yellow",
        IntegrityKind.VARIATION_SEL: "yellow",
        IntegrityKind.CONTROL_CHAR:  "yellow",
        IntegrityKind.BOM:           "cyan",
        IntegrityKind.MIXED_ENDINGS: "cyan",
        IntegrityKind.NON_ASCII:     "dim white",
    }

    table = Table(
        box=box.SIMPLE_HEAD,
        show_header=True,
        header_style="bold",
        expand=True,
    )
    table.add_column("Kind",    width=16)
    table.add_column("Loc",     width=10, justify="right")
    table.add_column("U+",      width=8,  justify="right")
    table.add_column("Message")

    for f in findings:
        style = _kind_style.get(f.kind, "white")
        loc   = f"{f.line}:{f.col}" if f.line else "file"
        cp    = f"U+{f.codepoint:04X}" if f.codepoint >= 0 else "—"
        table.add_row(
            f"[{style}]{f.kind.value}[/]",
            f"[dim]{loc}[/]",
            f"[dim]{cp}[/]",
            f"[{style}]{f.message}[/]",
        )

    # Risk summary
    critical = sum(
        1 for f in findings
        if f.kind in (IntegrityKind.TAG_CHAR, IntegrityKind.BIDI_OVERRIDE)
    )
    high     = sum(
        1 for f in findings
        if f.kind in (IntegrityKind.ZERO_WIDTH, IntegrityKind.VARIATION_SEL)
    )

    con.print(f"\n[bold]{path.name}[/]  — integrity scan")
    con.print(table)
    parts = [f"[bold]{len(findings)}[/] issue(s)"]
    if critical:
        parts.append(f"[bold red]{critical} critical[/]")
    if high:
        parts.append(f"[yellow]{high} high[/]")
    con.print("  " + "  ".join(parts))

    if critical:
        con.print(
            "\n  [bold red]⚠  Critical:[/] tag chars or BiDi overrides detected — "
            "potential watermark or Trojan-Source attack.  "
            "Run with [bold]strip=True[/] to remove."
        )


# ── Name helpers (for readable messages) ─────────────────────────────────────

def _ctrl_name(cp: int) -> str:
    _NAMES: dict[int, str] = {
        0x00: "NUL", 0x01: "SOH", 0x02: "STX", 0x03: "ETX",
        0x04: "EOT", 0x05: "ENQ", 0x06: "ACK", 0x07: "BEL",
        0x08: "BS",  0x0B: "VT",  0x0C: "FF",  0x0E: "SO",
        0x0F: "SI",  0x10: "DLE", 0x11: "DC1", 0x12: "DC2",
        0x13: "DC3", 0x14: "DC4", 0x15: "NAK", 0x16: "SYN",
        0x17: "ETB", 0x18: "CAN", 0x19: "EM",  0x1A: "SUB",
        0x1B: "ESC", 0x1C: "FS",  0x1D: "GS",  0x1E: "RS",
        0x1F: "US",
    }
    return _NAMES.get(cp, f"0x{cp:02X}")


def _bidi_name(cp: int) -> str:
    _NAMES: dict[int, str] = {
        0x200E: "LRM",  0x200F: "RLM",
        0x202A: "LRE",  0x202B: "RLE",  0x202C: "PDF",
        0x202D: "LRO",  0x202E: "RLO",
        0x2066: "LRI",  0x2067: "RLI",  0x2068: "FSI",  0x2069: "PDI",
    }
    return _NAMES.get(cp, f"0x{cp:04X}")


def _zw_name(cp: int) -> str:
    _NAMES: dict[int, str] = {
        0x00AD: "SOFT HYPHEN",
        0x200B: "ZERO WIDTH SPACE",
        0x200C: "ZERO WIDTH NON-JOINER",
        0x200D: "ZERO WIDTH JOINER",
        0x2060: "WORD JOINER",
        0x2061: "FUNCTION APPLICATION",
        0x2062: "INVISIBLE TIMES",
        0x2063: "INVISIBLE SEPARATOR",
        0x2064: "INVISIBLE PLUS",
        0x206A: "INHIBIT SYMMETRIC SWAPPING",
        0x206B: "ACTIVATE SYMMETRIC SWAPPING",
        0x206C: "INHIBIT ARABIC FORM SHAPING",
        0x206D: "ACTIVATE ARABIC FORM SHAPING",
        0x206E: "NATIONAL DIGIT SHAPES",
        0x206F: "NOMINAL DIGIT SHAPES",
        0xFEFF: "ZERO WIDTH NO-BREAK SPACE",
    }
    return _NAMES.get(cp, f"0x{cp:04X}")
