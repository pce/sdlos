"""
tests/test_create.py
====================
Integration tests for sdlos.commands.create:

  - dry_run never touches the filesystem  (regression: mkdir ran unconditionally)
  - create_app writes the three scaffold files
  - create_app creates the data/ skeleton only when --data-dir is set
  - create_app skips existing files without --overwrite
  - create_app preserves user regions on --overwrite
  - cmake snippet is printed when --patch-cmake is not set
  - cmake is patched when --patch-cmake is set
  - with_model copy path (happy + missing-file)
"""
from __future__ import annotations

import textwrap
from pathlib import Path

import pytest

from sdlos.commands.create import create_app
from sdlos.config.schema import AppConfig
from sdlos.core.fs import USER_BEGIN, USER_END


# ── Helpers ───────────────────────────────────────────────────────────────────

def _cfg(**kwargs) -> AppConfig:
    """Return a minimal valid AppConfig, overridable by kwargs."""
    defaults = dict(
        name="my_app",
        template="minimal",
        win_w=None,
        win_h=None,
        data_dir=False,
        overwrite=False,
        dry_run=False,
        verbose=False,
    )
    defaults.update(kwargs)
    return AppConfig(**defaults)


def _app_dir(root: Path, name: str = "my_app") -> Path:
    return root / "examples" / "apps" / name


def _data_dir(root: Path, name: str = "my_app") -> Path:
    """Runtime assets live at examples/data/<name>/, separate from source."""
    return root / "examples" / "data" / name


def _fake_cmake(root: Path) -> Path:
    cmake = root / "CMakeLists.txt"
    cmake.write_text("project(sdlos VERSION 1.0)\n", encoding="utf-8")
    return cmake


# ── dry_run: nothing written ──────────────────────────────────────────────────

class TestDryRun:
    def test_dry_run_creates_no_app_dir(self, tmp_path: Path) -> None:
        cfg = _cfg(dry_run=True)
        create_app(cfg, tmp_path)
        assert not _app_dir(tmp_path).exists()

    def test_dry_run_creates_no_data_dir(self, tmp_path: Path) -> None:
        """Regression: _scaffold_data_dir used to mkdir unconditionally."""
        cfg = _cfg(dry_run=True, data_dir=True)
        create_app(cfg, tmp_path)
        assert not _data_dir(tmp_path).exists()

    def test_dry_run_creates_no_data_subdirs(self, tmp_path: Path) -> None:
        cfg = _cfg(dry_run=True, data_dir=True)
        create_app(cfg, tmp_path)
        for sub in ("shaders/msl", "shaders/spirv", "img", "models"):
            assert not (_data_dir(tmp_path) / sub).exists()

    def test_dry_run_creates_no_source_files(self, tmp_path: Path) -> None:
        cfg = _cfg(dry_run=True)
        create_app(cfg, tmp_path)
        assert not (_app_dir(tmp_path) / "my_app.jade").exists()
        assert not (_app_dir(tmp_path) / "my_app.css").exists()
        assert not (_app_dir(tmp_path) / "my_app_behavior.cxx").exists()

    def test_dry_run_writes_no_cmake_file(self, tmp_path: Path) -> None:
        cfg = _cfg(dry_run=True)
        create_app(cfg, tmp_path)
        assert not (_app_dir(tmp_path) / "my_app.cmake").exists()

    def test_dry_run_does_not_copy_model(self, tmp_path: Path) -> None:
        model = tmp_path / "cube.glb"
        model.write_bytes(b"GLBX")
        cfg = _cfg(dry_run=True, data_dir=True, with_model=str(model))
        create_app(cfg, tmp_path)
        assert not (_data_dir(tmp_path) / "models" / "cube.glb").exists()

    @pytest.mark.parametrize("template", ["minimal", "shader", "camera"])
    def test_dry_run_all_templates(self, tmp_path: Path, template: str) -> None:
        cfg = _cfg(dry_run=True, template=template, data_dir=True)
        create_app(cfg, tmp_path)
        assert not _app_dir(tmp_path).exists()


# ── Normal create ─────────────────────────────────────────────────────────────

class TestCreateApp:
    def test_creates_app_dir(self, tmp_path: Path) -> None:
        cfg = _cfg()
        create_app(cfg, tmp_path)
        assert _app_dir(tmp_path).is_dir()

    def test_writes_jade(self, tmp_path: Path) -> None:
        cfg = _cfg()
        create_app(cfg, tmp_path)
        jade = _app_dir(tmp_path) / "my_app.jade"
        assert jade.exists()
        assert "my_app" in jade.read_text()

    def test_writes_css(self, tmp_path: Path) -> None:
        cfg = _cfg()
        create_app(cfg, tmp_path)
        css = _app_dir(tmp_path) / "my_app.css"
        assert css.exists()
        assert "#root" in css.read_text()

    def test_writes_behavior(self, tmp_path: Path) -> None:
        cfg = _cfg()
        create_app(cfg, tmp_path)
        cxx = _app_dir(tmp_path) / "my_app_behavior.cxx"
        assert cxx.exists()
        content = cxx.read_text()
        assert "void jade_app_init(" in content
        assert "MyAppState" in content

    def test_no_data_dir_by_default(self, tmp_path: Path) -> None:
        cfg = _cfg(data_dir=False)
        create_app(cfg, tmp_path)
        assert not _data_dir(tmp_path).exists()

    def test_data_dir_created_when_requested(self, tmp_path: Path) -> None:
        cfg = _cfg(data_dir=True)
        create_app(cfg, tmp_path)
        data = _data_dir(tmp_path)
        assert data.is_dir()
        assert (data / "shaders" / "msl").is_dir()
        assert (data / "shaders" / "spirv").is_dir()
        assert (data / "img").is_dir()
        assert (data / "models").is_dir()

    def test_gitkeep_written(self, tmp_path: Path) -> None:
        cfg = _cfg(data_dir=True)
        create_app(cfg, tmp_path)
        assert (_data_dir(tmp_path) / ".gitkeep").exists()

    @pytest.mark.parametrize("template", ["minimal", "shader", "camera"])
    def test_all_templates_write_three_files(
        self, tmp_path: Path, template: str
    ) -> None:
        cfg = _cfg(template=template)
        create_app(cfg, tmp_path)
        d = _app_dir(tmp_path)
        assert (d / "my_app.jade").exists()
        assert (d / "my_app.css").exists()
        assert (d / "my_app_behavior.cxx").exists()


# ── skip / overwrite ──────────────────────────────────────────────────────────

class TestOverwrite:
    def test_skip_existing_without_flag(self, tmp_path: Path) -> None:
        cfg = _cfg()
        create_app(cfg, tmp_path)
        jade = _app_dir(tmp_path) / "my_app.jade"
        jade.write_text("// my custom jade\n", encoding="utf-8")

        # Second run without --overwrite — file must be unchanged.
        create_app(cfg, tmp_path)
        assert jade.read_text() == "// my custom jade\n"

    def test_overwrite_replaces_file(self, tmp_path: Path) -> None:
        cfg = _cfg()
        create_app(cfg, tmp_path)
        jade = _app_dir(tmp_path) / "my_app.jade"
        jade.write_text("// custom\n", encoding="utf-8")

        cfg_ow = _cfg(overwrite=True)
        create_app(cfg_ow, tmp_path)
        # The file should be freshly generated, not the custom content.
        assert "// custom" not in jade.read_text()
        assert "my_app" in jade.read_text()

    def test_overwrite_preserves_user_regions(self, tmp_path: Path) -> None:
        """Core guarantee: user code between markers survives --overwrite."""
        cfg = _cfg()
        create_app(cfg, tmp_path)
        cxx = _app_dir(tmp_path) / "my_app_behavior.cxx"

        # Simulate developer filling in a user region.
        original = cxx.read_text()
        user_code = "\n    bus.subscribe(\"my_app:go\", [](auto){});\n"
        patched = original.replace(
            USER_BEGIN + "\n\n    // bus.subscribe",
            USER_BEGIN + user_code + "\n    // bus.subscribe",
        )
        # If the simple replace found nothing, inject into the first region directly.
        if patched == original:
            patched = original.replace(USER_BEGIN, USER_BEGIN + user_code, 1)
        cxx.write_text(patched, encoding="utf-8")

        # Overwrite — user code must survive.
        cfg_ow = _cfg(overwrite=True)
        create_app(cfg_ow, tmp_path)
        result = cxx.read_text()
        assert 'bus.subscribe("my_app:go"' in result
        assert USER_BEGIN in result
        assert USER_END   in result

    def test_overwrite_creates_backup(self, tmp_path: Path) -> None:
        cfg = _cfg()
        create_app(cfg, tmp_path)
        cxx = _app_dir(tmp_path) / "my_app_behavior.cxx"

        cfg_ow = _cfg(overwrite=True)
        create_app(cfg_ow, tmp_path)
        bak = cxx.with_suffix(".cxx.bak")
        assert bak.exists()


# ── CMake file ────────────────────────────────────────────────────────────────

class TestCmakeIntegration:
    def test_cmake_file_written_alongside_sources(self, tmp_path: Path) -> None:
        """create_app writes <name>.cmake next to the jade/css/cxx files."""
        cfg = _cfg()
        create_app(cfg, tmp_path)
        cmake_file = _app_dir(tmp_path) / "my_app.cmake"
        assert cmake_file.exists()

    def test_cmake_file_contains_app_registration(self, tmp_path: Path) -> None:
        cfg = _cfg()
        create_app(cfg, tmp_path)
        text = (_app_dir(tmp_path) / "my_app.cmake").read_text()
        assert "sdlos_jade_app(my_app" in text

    def test_cmake_file_not_written_in_dry_run(self, tmp_path: Path) -> None:
        cfg = _cfg(dry_run=True)
        create_app(cfg, tmp_path)
        assert not (_app_dir(tmp_path) / "my_app.cmake").exists()

    def test_cmake_file_skipped_without_overwrite(self, tmp_path: Path) -> None:
        cfg = _cfg()
        create_app(cfg, tmp_path)
        cmake_file = _app_dir(tmp_path) / "my_app.cmake"
        cmake_file.write_text("# custom\n", encoding="utf-8")
        create_app(cfg, tmp_path)
        assert cmake_file.read_text() == "# custom\n"

    def test_cmake_file_overwritten_with_flag(self, tmp_path: Path) -> None:
        cfg = _cfg()
        create_app(cfg, tmp_path)
        cmake_file = _app_dir(tmp_path) / "my_app.cmake"
        cmake_file.write_text("# custom\n", encoding="utf-8")
        create_app(_cfg(overwrite=True), tmp_path)
        assert "sdlos_jade_app(my_app" in cmake_file.read_text()

    def test_cmake_file_includes_data_dir(self, tmp_path: Path) -> None:
        cfg = _cfg(data_dir=True)
        create_app(cfg, tmp_path)
        text = (_app_dir(tmp_path) / "my_app.cmake").read_text()
        assert "DATA_DIR" in text

    def test_cmake_file_no_data_dir_when_not_set(self, tmp_path: Path) -> None:
        cfg = _cfg(data_dir=False)
        create_app(cfg, tmp_path)
        text = (_app_dir(tmp_path) / "my_app.cmake").read_text()
        assert "DATA_DIR" not in text

    def test_cmake_file_at_glob_discoverable_path(self, tmp_path: Path) -> None:
        """File sits at examples/apps/<name>/<name>.cmake — matches the root glob."""
        cfg = _cfg()
        create_app(cfg, tmp_path)
        found = list((tmp_path / "examples" / "apps").glob("*/*.cmake"))
        assert len(found) == 1
        assert found[0].name == "my_app.cmake"

    def test_each_template_writes_cmake_file(self, tmp_path: Path) -> None:
        for template in ("minimal", "shader", "camera"):
            app_name = f"{template}_app"
            cfg = _cfg(name=app_name, template=template)
            create_app(cfg, tmp_path)
            cmake_file = _app_dir(tmp_path, app_name) / f"{app_name}.cmake"
            assert cmake_file.exists(), f"Missing {cmake_file}"
            assert f"sdlos_jade_app({app_name}" in cmake_file.read_text()


# ── Model copy ────────────────────────────────────────────────────────────────

class TestModelCopy:
    def test_model_copied_to_data_models(self, tmp_path: Path) -> None:
        model = tmp_path / "cube.glb"
        model.write_bytes(b"GLB\x00")
        cfg = _cfg(data_dir=True, with_model=str(model))
        create_app(cfg, tmp_path)
        dest = _data_dir(tmp_path) / "models" / "cube.glb"
        assert dest.exists()
        assert dest.read_bytes() == b"GLB\x00"

    def test_missing_model_does_not_crash(self, tmp_path: Path) -> None:
        cfg = _cfg(data_dir=True, with_model="/nonexistent/cube.glb")
        # Should warn but not raise.
        create_app(cfg, tmp_path)
        dest = _data_dir(tmp_path) / "models" / "cube.glb"
        assert not dest.exists()

    def test_model_not_copied_in_dry_run(self, tmp_path: Path) -> None:
        model = tmp_path / "cube.glb"
        model.write_bytes(b"GLB\x00")
        cfg = _cfg(dry_run=True, data_dir=True, with_model=str(model))
        create_app(cfg, tmp_path)
        dest = _data_dir(tmp_path) / "models" / "cube.glb"
        assert not dest.exists()

    def test_model_skip_if_exists_without_overwrite(self, tmp_path: Path) -> None:
        model = tmp_path / "cube.glb"
        model.write_bytes(b"new")
        # Pre-place an existing model.
        cfg = _cfg(data_dir=True, with_model=str(model))
        create_app(cfg, tmp_path)
        dest = _data_dir(tmp_path) / "models" / "cube.glb"
        dest.write_bytes(b"old")

        # Second run without --overwrite — old content preserved.
        create_app(cfg, tmp_path)
        assert dest.read_bytes() == b"old"

    def test_model_overwritten_with_flag(self, tmp_path: Path) -> None:
        model = tmp_path / "cube.glb"
        model.write_bytes(b"new")
        cfg = _cfg(data_dir=True, with_model=str(model), overwrite=True)
        create_app(cfg, tmp_path)
        dest = _data_dir(tmp_path) / "models" / "cube.glb"
        dest.write_bytes(b"old")

        model.write_bytes(b"updated")
        create_app(cfg, tmp_path)
        assert dest.read_bytes() == b"updated"
