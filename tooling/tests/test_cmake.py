"""
tests/test_cmake.py
===================
Unit tests for sdlos.core.cmake:
  - cmake_snippet      — formatted sdlos_jade_app() block generation
  - write_app_cmake    — <name>.cmake file written into the app directory
  - find_project_root  — upward walk detection
"""
import pytest
from pathlib import Path

from sdlos.config.schema import AppConfig
from sdlos.core.cmake import cmake_snippet, write_app_cmake, find_project_root


# ── cmake_snippet ─────────────────────────────────────────────────────────────

class TestCmakeSnippet:
    def test_minimal_no_data(self):
        s = cmake_snippet("hello", 800, 600, has_data=False)
        assert "sdlos_jade_app(hello" in s
        assert "hello.jade" in s
        assert "hello_behavior.cxx" in s
        assert "WIN_W 800" in s
        assert "WIN_H 600" in s
        assert "DATA_DIR" not in s

    def test_none_win_omits_win_lines(self):
        s = cmake_snippet("hello", None, None, has_data=False)
        assert "WIN_W" not in s
        assert "WIN_H" not in s
        assert "sdlos_jade_app(hello" in s

    def test_none_win_still_closes_with_paren(self):
        s = cmake_snippet("hello", None, None, has_data=False)
        assert s.strip().endswith(")")

    def test_with_data_dir(self):
        s = cmake_snippet("myapp", 1280, 800, has_data=True)
        assert "DATA_DIR examples/apps/myapp/data" in s

    def test_closes_with_paren(self):
        s = cmake_snippet("calc", 800, 600, has_data=False)
        lines = s.splitlines()
        assert lines[-1].strip() == ")"

    def test_closes_with_paren_with_data(self):
        s = cmake_snippet("calc", None, None, has_data=True)
        lines = s.splitlines()
        assert lines[-1].strip() == ")"

    def test_paths_use_name(self):
        s = cmake_snippet("my_app", 800, 600, has_data=False)
        assert "examples/apps/my_app/my_app.jade" in s
        assert "examples/apps/my_app/my_app_behavior.cxx" in s

    def test_extra_resources(self):
        extras = [
            'sdlos_copy_resource_to(foo "examples/apps/foo/data/models/a.glb" "data/models/a.glb")',
        ]
        s = cmake_snippet("foo", 800, 600, has_data=True, extra_resources=extras)
        assert "sdlos_copy_resource_to" in s
        assert "a.glb" in s

    def test_no_extra_resources_by_default(self):
        s = cmake_snippet("bar", 800, 600, has_data=False)
        assert "sdlos_copy_resource" not in s

    def test_win_dimensions_appear(self):
        s = cmake_snippet("wtest", 1920, 1080, has_data=False)
        assert "WIN_W 1920" in s
        assert "WIN_H 1080" in s


# ── write_app_cmake ───────────────────────────────────────────────────────────

def _cfg(name: str = "my_app", **kwargs) -> AppConfig:
    defaults = dict(overwrite=False, dry_run=False, verbose=False)
    defaults.update(kwargs)
    return AppConfig(name=name, **defaults)


class TestWriteAppCmake:
    def test_creates_cmake_file(self, tmp_path: Path) -> None:
        app_dir = tmp_path / "my_app"
        app_dir.mkdir()
        snippet = cmake_snippet("my_app", 800, 600, False)
        write_app_cmake(app_dir, snippet, _cfg("my_app"))
        assert (app_dir / "my_app.cmake").exists()

    def test_cmake_file_named_after_app(self, tmp_path: Path) -> None:
        app_dir = tmp_path / "viz_shader"
        app_dir.mkdir()
        snippet = cmake_snippet("viz_shader", 1280, 800, True)
        write_app_cmake(app_dir, snippet, _cfg("viz_shader"))
        assert (app_dir / "viz_shader.cmake").exists()
        assert not (app_dir / "my_app.cmake").exists()

    def test_cmake_file_contains_snippet(self, tmp_path: Path) -> None:
        app_dir = tmp_path / "my_app"
        app_dir.mkdir()
        snippet = cmake_snippet("my_app", 1280, 720, has_data=True)
        write_app_cmake(app_dir, snippet, _cfg("my_app"))
        text = (app_dir / "my_app.cmake").read_text()
        assert "sdlos_jade_app(my_app" in text
        assert "WIN_W 1280"            in text
        assert "WIN_H 720"             in text
        assert "DATA_DIR examples/apps/my_app/data" in text

    def test_cmake_file_ends_with_newline(self, tmp_path: Path) -> None:
        app_dir = tmp_path / "my_app"
        app_dir.mkdir()
        snippet = cmake_snippet("my_app", 800, 600, False)
        write_app_cmake(app_dir, snippet, _cfg("my_app"))
        text = (app_dir / "my_app.cmake").read_text()
        assert text.endswith("\n")

    def test_dry_run_does_not_write(self, tmp_path: Path) -> None:
        app_dir = tmp_path / "my_app"
        app_dir.mkdir()
        snippet = cmake_snippet("my_app", 800, 600, False)
        write_app_cmake(app_dir, snippet, _cfg("my_app", dry_run=True))
        assert not (app_dir / "my_app.cmake").exists()

    def test_skip_existing_without_overwrite(self, tmp_path: Path) -> None:
        app_dir = tmp_path / "my_app"
        app_dir.mkdir()
        cmake_file = app_dir / "my_app.cmake"
        cmake_file.write_text("# custom\n", encoding="utf-8")
        snippet = cmake_snippet("my_app", 800, 600, False)
        write_app_cmake(app_dir, snippet, _cfg("my_app", overwrite=False))
        assert cmake_file.read_text() == "# custom\n"

    def test_overwrite_replaces_existing(self, tmp_path: Path) -> None:
        app_dir = tmp_path / "my_app"
        app_dir.mkdir()
        cmake_file = app_dir / "my_app.cmake"
        cmake_file.write_text("# old\n", encoding="utf-8")
        snippet = cmake_snippet("my_app", 800, 600, False)
        write_app_cmake(app_dir, snippet, _cfg("my_app", overwrite=True))
        assert "sdlos_jade_app(my_app" in cmake_file.read_text()

    def test_extra_resources_in_file(self, tmp_path: Path) -> None:
        app_dir = tmp_path / "my_app"
        app_dir.mkdir()
        extras = ['sdlos_copy_resource_to(my_app "examples/apps/my_app/data/models/cube.glb" "data/models/cube.glb")']
        snippet = cmake_snippet("my_app", 800, 600, True, extra_resources=extras)
        write_app_cmake(app_dir, snippet, _cfg("my_app"))
        text = (app_dir / "my_app.cmake").read_text()
        assert "sdlos_copy_resource_to" in text
        assert "cube.glb"               in text

    def test_glob_pattern_would_find_file(self, tmp_path: Path) -> None:
        """The .cmake file sits exactly where the root CMakeLists.txt glob expects it."""
        apps_dir = tmp_path / "examples" / "apps" / "my_app"
        apps_dir.mkdir(parents=True)
        snippet = cmake_snippet("my_app", 800, 600, False)
        write_app_cmake(apps_dir, snippet, _cfg("my_app"))
        # Simulate: file(GLOB ... "examples/apps/*/*.cmake")
        found = list((tmp_path / "examples" / "apps").glob("*/*.cmake"))
        assert len(found) == 1
        assert found[0].name == "my_app.cmake"


# ── find_project_root ─────────────────────────────────────────────────────────

class TestFindProjectRoot:
    def test_finds_root_with_project_sdlos(self, tmp_path: Path) -> None:
        cmake = tmp_path / "CMakeLists.txt"
        cmake.write_text("project(sdlos VERSION 1.0)\n", encoding="utf-8")
        sub = tmp_path / "examples" / "apps" / "myapp"
        sub.mkdir(parents=True)
        found = find_project_root(sub)
        assert found == tmp_path

    def test_finds_root_from_same_directory(self, tmp_path: Path) -> None:
        cmake = tmp_path / "CMakeLists.txt"
        cmake.write_text("project(sdlos)\n", encoding="utf-8")
        found = find_project_root(tmp_path)
        assert found == tmp_path

    def test_falls_back_when_no_matching_root(self, tmp_path: Path) -> None:
        # A CMakeLists.txt that does NOT contain "project(sdlos"
        cmake = tmp_path / "CMakeLists.txt"
        cmake.write_text("project(other)\n", encoding="utf-8")
        sub = tmp_path / "deep" / "path"
        sub.mkdir(parents=True)
        # Should not raise — falls back to the module's own parent chain
        result = find_project_root(sub)
        assert isinstance(result, Path)

    def test_stops_at_filesystem_root(self, tmp_path: Path) -> None:
        # Walk from a directory with no relevant CMakeLists.txt anywhere
        isolated = tmp_path / "isolated"
        isolated.mkdir()
        result = find_project_root(isolated)
        assert isinstance(result, Path)
