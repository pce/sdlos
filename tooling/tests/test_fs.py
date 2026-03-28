"""
tests/test_fs.py
================
Unit tests for sdlos.core.fs:
  - write_atomic          — atomic write + content verification
  - backup_file           — unique .bak naming
  - write_file_safe       — skip / overwrite / dry-run logic
  - extract_user_regions  — marker parsing
  - splice_user_regions   — region preservation on regen
"""
import pytest
from pathlib import Path

from sdlos.core.fs import (
    USER_BEGIN,
    USER_END,
    backup_file,
    extract_user_regions,
    splice_user_regions,
    write_atomic,
    write_file_safe,
)


# ── write_atomic ──────────────────────────────────────────────────────────────

class TestWriteAtomic:
    def test_creates_file(self, tmp_path: Path) -> None:
        dest = tmp_path / "out.txt"
        write_atomic(dest, "hello")
        assert dest.read_text() == "hello"

    def test_overwrites_existing(self, tmp_path: Path) -> None:
        dest = tmp_path / "out.txt"
        dest.write_text("old")
        write_atomic(dest, "new")
        assert dest.read_text() == "new"

    def test_creates_parent_dirs(self, tmp_path: Path) -> None:
        dest = tmp_path / "a" / "b" / "c.txt"
        write_atomic(dest, "deep")
        assert dest.read_text() == "deep"

    def test_no_tmp_leftover_on_success(self, tmp_path: Path) -> None:
        dest = tmp_path / "out.txt"
        write_atomic(dest, "data")
        leftovers = [p for p in tmp_path.iterdir() if p != dest]
        assert leftovers == [], f"Unexpected leftover files: {leftovers}"

    def test_unicode_content(self, tmp_path: Path) -> None:
        dest = tmp_path / "unicode.txt"
        content = "こんにちは — sdlos 🎮"
        write_atomic(dest, content)
        assert dest.read_text(encoding="utf-8") == content


# ── backup_file ───────────────────────────────────────────────────────────────

class TestBackupFile:
    def test_returns_none_for_missing(self, tmp_path: Path) -> None:
        result = backup_file(tmp_path / "ghost.txt")
        assert result is None

    def test_creates_bak(self, tmp_path: Path) -> None:
        src = tmp_path / "file.txt"
        src.write_text("original")
        bak = backup_file(src)
        assert bak is not None
        assert bak.exists()
        assert bak.read_text() == "original"
        assert bak.suffix == ".bak"

    def test_source_unchanged_after_backup(self, tmp_path: Path) -> None:
        src = tmp_path / "file.txt"
        src.write_text("keep me")
        backup_file(src)
        assert src.read_text() == "keep me"

    def test_unique_bak_names(self, tmp_path: Path) -> None:
        src = tmp_path / "file.txt"
        src.write_text("v1")
        bak1 = backup_file(src)
        src.write_text("v2")
        bak2 = backup_file(src)
        src.write_text("v3")
        bak3 = backup_file(src)
        names = {bak1.name, bak2.name, bak3.name}
        assert len(names) == 3, f"Expected 3 unique backup names, got: {names}"

    def test_bak_suffix_on_dotted_name(self, tmp_path: Path) -> None:
        src = tmp_path / "foo.css"
        src.write_text("body {}")
        bak = backup_file(src)
        assert bak.name == "foo.css.bak"


# ── write_file_safe ───────────────────────────────────────────────────────────

class TestWriteFileSafe:
    def test_creates_new_file(self, tmp_path: Path) -> None:
        dest = tmp_path / "new.txt"
        result = write_file_safe(dest, "content", verbose=False)
        assert result is True
        assert dest.read_text() == "content"

    def test_skips_existing_without_overwrite(self, tmp_path: Path) -> None:
        dest = tmp_path / "existing.txt"
        dest.write_text("original")
        result = write_file_safe(dest, "new content", overwrite=False, verbose=False)
        assert result is False
        assert dest.read_text() == "original"

    def test_overwrites_existing_with_flag(self, tmp_path: Path) -> None:
        dest = tmp_path / "existing.txt"
        dest.write_text("original")
        result = write_file_safe(dest, "replaced", overwrite=True, verbose=False)
        assert result is True
        assert dest.read_text() == "replaced"

    def test_overwrite_creates_backup(self, tmp_path: Path) -> None:
        dest = tmp_path / "file.txt"
        dest.write_text("original")
        write_file_safe(dest, "replaced", overwrite=True, verbose=False)
        bak = dest.with_suffix(".txt.bak")
        assert bak.exists()
        assert bak.read_text() == "original"

    def test_dry_run_does_not_write(self, tmp_path: Path) -> None:
        dest = tmp_path / "new.txt"
        result = write_file_safe(dest, "content", dry_run=True, verbose=False)
        assert result is True
        assert not dest.exists()

    def test_dry_run_skip_existing(self, tmp_path: Path) -> None:
        dest = tmp_path / "existing.txt"
        dest.write_text("original")
        result = write_file_safe(dest, "new", overwrite=False, dry_run=True, verbose=False)
        assert result is False
        assert dest.read_text() == "original"

    def test_dry_run_overwrite_returns_true_without_writing(self, tmp_path: Path) -> None:
        dest = tmp_path / "existing.txt"
        dest.write_text("original")
        result = write_file_safe(dest, "new", overwrite=True, dry_run=True, verbose=False)
        assert result is True
        assert dest.read_text() == "original"


# ── extract_user_regions ──────────────────────────────────────────────────────

class TestExtractUserRegions:
    def _wrap(self, body: str) -> str:
        return f"{USER_BEGIN}{body}{USER_END}"

    def test_no_regions(self) -> None:
        assert extract_user_regions("no markers here") == []

    def test_single_region(self) -> None:
        text = self._wrap("\n    int x = 42;\n")
        regions = extract_user_regions(text)
        assert regions == ["\n    int x = 42;\n"]

    def test_two_regions(self) -> None:
        text = (
            "before\n"
            + self._wrap("\nfirst\n")
            + "\nmiddle\n"
            + self._wrap("\nsecond\n")
            + "\nafter"
        )
        regions = extract_user_regions(text)
        assert len(regions) == 2
        assert regions[0] == "\nfirst\n"
        assert regions[1] == "\nsecond\n"

    def test_empty_region(self) -> None:
        text = self._wrap("")
        regions = extract_user_regions(text)
        assert regions == [""]

    def test_multiline_region(self) -> None:
        body = "\n    // user code\n    bus.subscribe(...);\n"
        text = self._wrap(body)
        regions = extract_user_regions(text)
        assert regions == [body]


# ── splice_user_regions ───────────────────────────────────────────────────────

def _region(body: str) -> str:
    """Wrap *body* in user-region markers."""
    return f"{USER_BEGIN}{body}{USER_END}"


class TestSpliceUserRegions:
    def test_no_regions_returns_generated(self) -> None:
        generated = "int main() { return 0; }"
        existing  = "old code"
        assert splice_user_regions(generated, existing) == generated

    def test_preserves_single_region(self) -> None:
        user_code = "\n    // my custom code\n    int x = 99;\n"
        generated = f"prefix\n{_region('// placeholder')}\nsuffix"
        existing  = f"old prefix\n{_region(user_code)}\nold suffix"
        result = splice_user_regions(generated, existing)
        assert USER_BEGIN in result
        assert USER_END   in result
        assert "my custom code" in result
        assert "x = 99"        in result
        # Generated framing must be preserved
        assert result.startswith("prefix\n")
        assert result.endswith("\nsuffix")

    def test_preserves_two_regions(self) -> None:
        gen = (
            f"top\n{_region('gen1')}\nmid\n{_region('gen2')}\nbot"
        )
        ex = (
            f"top\n{_region('user1')}\nmid\n{_region('user2')}\nbot"
        )
        result = splice_user_regions(gen, ex)
        assert "user1" in result
        assert "user2" in result
        assert "gen1"  not in result
        assert "gen2"  not in result

    def test_more_generated_regions_than_existing(self) -> None:
        gen = (
            f"A\n{_region('gA')}\nB\n{_region('gB')}\nC"
        )
        ex = f"A\n{_region('eA')}\nB"
        result = splice_user_regions(gen, ex)
        # First region should come from existing
        assert "eA" in result
        # Second region is beyond existing count — keep generated placeholder
        assert "gB" in result

    def test_empty_user_region_preserved(self) -> None:
        gen = f"a\n{_region('placeholder')}\nb"
        ex  = f"a\n{_region('')}\nb"
        result = splice_user_regions(gen, ex)
        # The empty user region should replace the placeholder
        assert "placeholder" not in result
        assert USER_BEGIN in result
        assert USER_END   in result

    def test_roundtrip_overwrite(self) -> None:
        """Simulates: generate → user edits region → regen with --overwrite."""
        template_v1 = f"// generated v1\n{_region('// TODO')}\n// end"
        # User fills in their code
        user_edit   = f"// generated v1\n{_region(chr(10) + '    bus.subscribe(foo);' + chr(10))}\n// end"
        # Generator produces v2 (different framing, same region count)
        template_v2 = f"// generated v2\n{_region('// TODO')}\n// end"
        result = splice_user_regions(template_v2, user_edit)
        assert "generated v2"        in result
        assert "bus.subscribe(foo);" in result
        assert "TODO"                not in result
