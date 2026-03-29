"""
tests/test_run.py
=================
Unit tests for sdlos.commands.run:

  - RunConfig defaults and field values
  - _find_executable — flat layout (Ninja/Makefile), multi-config sub-dirs,
    not-found case, platform suffix handling
  - _needs_configure — CMakeCache.txt presence/absence detection
  - _configure — command constructed correctly (preset vs plain -B/-S)
  - run_app — build step invoked correctly, --no-build skips cmake,
    --clean passes --clean-first, --jobs emits -j N
  - run_app — auto-configures when build dir is absent or CMakeCache missing
  - run_app — --reconfigure forces configure on already-configured build
  - run_app — --preset passes to cmake configure command
  - run_app — raises SystemExit when build dir is missing with --no-build
  - run_app — raises SystemExit when executable cannot be located
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path
from unittest.mock import MagicMock, call, patch

import pytest

from sdlos.commands.run import (
    RunConfig,
    _configure,
    _find_executable,
    _needs_configure,
    run_app,
)


# ── Helpers ───────────────────────────────────────────────────────────────────

def _make_exe(directory: Path, name: str) -> Path:
    """Create a zero-byte fake executable at *directory*/<name>."""
    directory.mkdir(parents=True, exist_ok=True)
    exe = directory / name
    exe.touch()
    return exe


def _build_dir(tmp_path: Path) -> Path:
    """Return a build/ sub-directory (created)."""
    bd = tmp_path / "build"
    bd.mkdir(parents=True, exist_ok=True)
    return bd


# ── RunConfig ─────────────────────────────────────────────────────────────────

class TestRunConfig:
    def test_required_fields(self, tmp_path: Path) -> None:
        cfg = RunConfig(name="calc", build_dir=tmp_path)
        assert cfg.name == "calc"
        assert cfg.build_dir == tmp_path

    def test_defaults(self, tmp_path: Path) -> None:
        cfg = RunConfig(name="calc", build_dir=tmp_path)
        assert cfg.clean       is False
        assert cfg.watch       is False
        assert cfg.jobs        is None
        assert cfg.no_build    is False
        assert cfg.quiet       is False
        assert cfg.reconfigure is False
        assert cfg.preset      is None

    def test_override_all(self, tmp_path: Path) -> None:
        cfg = RunConfig(
            name="viz",
            build_dir=tmp_path,
            clean=True,
            watch=True,
            jobs=8,
            no_build=True,
            quiet=True,
            reconfigure=True,
            preset="macos-debug",
        )
        assert cfg.clean       is True
        assert cfg.watch       is True
        assert cfg.jobs        == 8
        assert cfg.no_build    is True
        assert cfg.quiet       is True
        assert cfg.reconfigure is True
        assert cfg.preset      == "macos-debug"

    def test_name_stored_verbatim(self, tmp_path: Path) -> None:
        cfg = RunConfig(name="my_shader_app", build_dir=tmp_path)
        assert cfg.name == "my_shader_app"


# ── _find_executable ──────────────────────────────────────────────────────────

class TestFindExecutable:
    """Tests for the executable-discovery helper."""

    # ── flat layout (Ninja / Unix Makefile) ───────────────────────────────────

    def test_flat_layout(self, tmp_path: Path) -> None:
        """<build>/<name> — the most common Ninja/Makefile layout."""
        bd = _build_dir(tmp_path)
        _make_exe(bd, "calc")
        found = _find_executable("calc", bd)
        assert found is not None
        assert found.name in ("calc", "calc.exe")

    def test_returns_path_object(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        _make_exe(bd, "myapp")
        found = _find_executable("myapp", bd)
        assert isinstance(found, Path)

    # ── multi-config sub-directories (Xcode / MSVC) ──────────────────────────

    def test_debug_subdir(self, tmp_path: Path) -> None:
        """<build>/Debug/<name> — Xcode / Visual Studio Debug."""
        bd = _build_dir(tmp_path)
        _make_exe(bd / "Debug", "calc")
        found = _find_executable("calc", bd)
        assert found is not None
        assert "Debug" in str(found)

    def test_release_subdir(self, tmp_path: Path) -> None:
        """<build>/Release/<name>."""
        bd = _build_dir(tmp_path)
        _make_exe(bd / "Release", "calc")
        found = _find_executable("calc", bd)
        assert found is not None
        assert "Release" in str(found)

    def test_relwithdebinfo_subdir(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        _make_exe(bd / "RelWithDebInfo", "calc")
        found = _find_executable("calc", bd)
        assert found is not None
        assert "RelWithDebInfo" in str(found)

    def test_minsizerel_subdir(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        _make_exe(bd / "MinSizeRel", "calc")
        found = _find_executable("calc", bd)
        assert found is not None

    # ── flat takes priority over sub-dirs ─────────────────────────────────────

    def test_flat_preferred_over_debug_subdir(self, tmp_path: Path) -> None:
        """Flat layout is checked first — reflects Ninja being the default."""
        bd = _build_dir(tmp_path)
        flat = _make_exe(bd, "calc")
        _make_exe(bd / "Debug", "calc")
        found = _find_executable("calc", bd)
        assert found == flat

    # ── not found ─────────────────────────────────────────────────────────────

    def test_not_found_returns_none(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        # No executable present at all.
        found = _find_executable("calc", bd)
        assert found is None

    def test_wrong_name_returns_none(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        _make_exe(bd, "other")
        found = _find_executable("calc", bd)
        assert found is None

    # ── different app names ───────────────────────────────────────────────────

    @pytest.mark.parametrize("name", ["calc", "styleguide", "my_shader_app", "cam"])
    def test_various_names(self, tmp_path: Path, name: str) -> None:
        bd = _build_dir(tmp_path)
        _make_exe(bd, name)
        found = _find_executable(name, bd)
        assert found is not None

    # ── directory is not treated as an executable ─────────────────────────────

    def test_directory_not_treated_as_exe(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        # Create a *directory* named "calc" instead of a file.
        (bd / "calc").mkdir()
        found = _find_executable("calc", bd)
        assert found is None


# ── _needs_configure ─────────────────────────────────────────────────────────

class TestNeedsConfigure:
    def test_returns_true_when_no_cache(self, tmp_path: Path) -> None:
        """Directory exists but has no CMakeCache.txt → needs configure."""
        bd = tmp_path / "build"
        bd.mkdir()
        assert _needs_configure(bd) is True

    def test_returns_false_when_cache_present(self, tmp_path: Path) -> None:
        """CMakeCache.txt present → already configured."""
        bd = tmp_path / "build"
        bd.mkdir()
        (bd / "CMakeCache.txt").write_text("# CMake cache\n", encoding="utf-8")
        assert _needs_configure(bd) is False

    def test_returns_true_for_empty_dir(self, tmp_path: Path) -> None:
        bd = tmp_path / "build"
        bd.mkdir()
        assert _needs_configure(bd) is True

    def test_only_cmake_cache_matters(self, tmp_path: Path) -> None:
        """Other files in the build dir are irrelevant."""
        bd = tmp_path / "build"
        bd.mkdir()
        (bd / "build.ninja").write_text("", encoding="utf-8")
        (bd / "compile_commands.json").write_text("{}", encoding="utf-8")
        # Still needs configure — no CMakeCache.txt
        assert _needs_configure(bd) is True


# ── _configure ────────────────────────────────────────────────────────────────

class TestConfigureCmd:
    """Tests for _configure — verifies the cmake command that is built."""

    def test_plain_configure_uses_B_S(self, tmp_path: Path) -> None:
        """`cmake -B <build> -S <root>` when no preset is given."""
        bd = tmp_path / "build"
        bd.mkdir()

        with patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run:
            result = _configure(tmp_path, bd, preset=None, quiet=True)

        assert result is True
        args = mock_run.call_args[0][0]
        assert args[0] == "cmake"
        assert "-B" in args
        assert "-S" in args
        assert str(bd) in args
        assert str(tmp_path) in args

    def test_preset_configure_uses_preset_flag(self, tmp_path: Path) -> None:
        """`cmake --preset <name>` when a preset is given."""
        bd = tmp_path / "build"
        bd.mkdir()

        with patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run:
            result = _configure(tmp_path, bd, preset="macos-debug", quiet=True)

        assert result is True
        args = mock_run.call_args[0][0]
        assert args[0] == "cmake"
        assert "--preset" in args
        assert "macos-debug" in args
        # -B / -S should NOT appear when using a preset
        assert "-B" not in args
        assert "-S" not in args

    def test_returns_false_on_cmake_failure(self, tmp_path: Path) -> None:
        bd = tmp_path / "build"
        bd.mkdir()

        with patch("sdlos.commands.run.subprocess.run", return_value=_completed(1)):
            result = _configure(tmp_path, bd, preset=None, quiet=True)

        assert result is False

    def test_preset_name_in_command(self, tmp_path: Path) -> None:
        bd = tmp_path / "build"
        bd.mkdir()

        with patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run:
            _configure(tmp_path, bd, preset="macos-release", quiet=True)

        args = mock_run.call_args[0][0]
        preset_idx = args.index("--preset")
        assert args[preset_idx + 1] == "macos-release"

    def test_cwd_is_project_root(self, tmp_path: Path) -> None:
        """cmake is invoked with cwd=project_root so relative paths work."""
        bd = tmp_path / "build"
        bd.mkdir()

        with patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run:
            _configure(tmp_path, bd, preset=None, quiet=True)

        assert mock_run.call_args[1]["cwd"] == str(tmp_path)


# ── run_app: argument / behaviour tests (subprocess mocked) ──────────────────
#
# We patch subprocess.run (cmake build) and subprocess.Popen (launch) so
# no real build or process is started.  We also pre-create a fake executable
# so _find_executable succeeds.

class _FakeProc:  # noqa: D101
    """Minimal stand-in for subprocess.Popen."""
    returncode = 0

    def wait(self, timeout=None):
        return self.returncode

    def poll(self):
        return self.returncode

    def terminate(self):
        pass

    def kill(self):
        pass


def _completed(returncode: int = 0) -> subprocess.CompletedProcess:
    return subprocess.CompletedProcess(args=[], returncode=returncode)


class TestRunApp:
    """run_app integration tests with subprocess mocked out."""

    # ── missing build dir with --no-build → SystemExit ───────────────────────

    def test_missing_build_dir_with_no_build_exits(self, tmp_path: Path) -> None:
        """--no-build skips cmake entirely; a missing build dir is still fatal."""
        project_root = tmp_path
        # Do NOT create the build directory.
        with pytest.raises(SystemExit):
            run_app("calc", project_root, build_dir=project_root / "build",
                    no_build=True)

    # ── missing executable → SystemExit ──────────────────────────────────────

    def test_missing_exe_exits(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        # Simulate configured build (CMakeCache present) but no exe.
        (bd / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")
        with (
            patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)),
            pytest.raises(SystemExit),
        ):
            run_app("calc", tmp_path, build_dir=bd)

    # ── normal build + launch ─────────────────────────────────────────────────

    def test_build_invokes_cmake(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        _make_exe(bd, "calc")
        # Simulate an already-configured build so no auto-configure fires.
        (bd / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")

        with (
            patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run,
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
        ):
            run_app("calc", tmp_path, build_dir=bd, quiet=True)

        assert mock_run.called
        # Last call should be the build step (configure may have run first).
        build_calls = [
            c for c in mock_run.call_args_list
            if "--build" in c[0][0]
        ]
        assert len(build_calls) == 1
        args = build_calls[0][0][0]
        assert "cmake" in args
        assert "--build" in args
        assert "--target" in args
        assert "calc" in args

    def test_build_target_is_app_name(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        _make_exe(bd, "viz")
        (bd / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")

        with (
            patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run,
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
        ):
            run_app("viz", tmp_path, build_dir=bd, quiet=True)

        build_calls = [c for c in mock_run.call_args_list if "--build" in c[0][0]]
        args = build_calls[0][0][0]
        target_idx = args.index("--target")
        assert args[target_idx + 1] == "viz"

    def test_build_dir_passed_to_cmake(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        _make_exe(bd, "calc")
        (bd / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")

        with (
            patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run,
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
        ):
            run_app("calc", tmp_path, build_dir=bd, quiet=True)

        build_calls = [c for c in mock_run.call_args_list if "--build" in c[0][0]]
        args = build_calls[0][0][0]
        build_idx = args.index("--build")
        assert Path(args[build_idx + 1]) == bd

    # ── --no-build skips cmake ────────────────────────────────────────────────

    def test_no_build_skips_cmake(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        _make_exe(bd, "calc")
        (bd / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")

        with (
            patch("sdlos.commands.run.subprocess.run") as mock_run,
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
        ):
            run_app("calc", tmp_path, build_dir=bd, no_build=True, quiet=True)

        mock_run.assert_not_called()

    # ── --clean adds --clean-first ────────────────────────────────────────────

    def test_clean_adds_clean_first(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        _make_exe(bd, "calc")
        (bd / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")

        with (
            patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run,
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
        ):
            run_app("calc", tmp_path, build_dir=bd, clean=True, quiet=True)

        build_calls = [c for c in mock_run.call_args_list if "--build" in c[0][0]]
        args = build_calls[0][0][0]
        assert "--clean-first" in args

    def test_no_clean_omits_clean_first(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        _make_exe(bd, "calc")
        (bd / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")

        with (
            patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run,
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
        ):
            run_app("calc", tmp_path, build_dir=bd, clean=False, quiet=True)

        build_calls = [c for c in mock_run.call_args_list if "--build" in c[0][0]]
        args = build_calls[0][0][0]
        assert "--clean-first" not in args

    # ── --jobs passes -j N ────────────────────────────────────────────────────

    def test_jobs_passes_j_flag(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        _make_exe(bd, "calc")
        (bd / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")

        with (
            patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run,
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
        ):
            run_app("calc", tmp_path, build_dir=bd, jobs=8, quiet=True)

        build_calls = [c for c in mock_run.call_args_list if "--build" in c[0][0]]
        args = build_calls[0][0][0]
        assert "-j" in args
        j_idx = args.index("-j")
        assert args[j_idx + 1] == "8"

    def test_no_jobs_omits_j_flag(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        _make_exe(bd, "calc")
        (bd / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")

        with (
            patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run,
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
        ):
            run_app("calc", tmp_path, build_dir=bd, jobs=None, quiet=True)

        build_calls = [c for c in mock_run.call_args_list if "--build" in c[0][0]]
        args = build_calls[0][0][0]
        assert "-j" not in args

    # ── failed build exits ────────────────────────────────────────────────────

    def test_failed_build_exits(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        _make_exe(bd, "calc")
        (bd / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")

        with (
            patch("sdlos.commands.run.subprocess.run", return_value=_completed(1)),
            pytest.raises(SystemExit),
        ):
            run_app("calc", tmp_path, build_dir=bd, quiet=True)

    # ── default build dir resolves to <project_root>/build ───────────────────

    def test_default_build_dir_is_project_root_build(self, tmp_path: Path) -> None:
        """When build_dir=None the default is <project_root>/build."""
        bd = tmp_path / "build"
        bd.mkdir()
        _make_exe(bd, "calc")
        (bd / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")

        with (
            patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run,
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
        ):
            run_app("calc", tmp_path, build_dir=None, quiet=True)

        build_calls = [c for c in mock_run.call_args_list if "--build" in c[0][0]]
        args = build_calls[0][0][0]
        build_idx = args.index("--build")
        assert Path(args[build_idx + 1]) == bd.resolve()

    # ── popen is called with the executable ──────────────────────────────────

    def test_popen_called_with_exe_path(self, tmp_path: Path) -> None:
        bd = _build_dir(tmp_path)
        exe = _make_exe(bd, "calc")
        (bd / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")

        with (
            patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)),
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()) as mock_popen,
        ):
            run_app("calc", tmp_path, build_dir=bd, quiet=True)

        popen_args = mock_popen.call_args[0][0]  # list passed as first positional
        assert str(exe) in popen_args


# ── Auto-configure tests ──────────────────────────────────────────────────────

class TestAutoConfigureRun:
    """
    Tests for the cmake configure-before-build logic in run_app.

    Scenarios
    ---------
    - Build dir absent   → auto-configure runs before cmake --build.
    - CMakeCache absent  → auto-configure runs before cmake --build.
    - CMakeCache present → no configure, goes straight to build.
    - --reconfigure      → configure runs even when already configured.
    - --preset           → passed through to _configure command.
    - Failed configure   → SystemExit before build is attempted.
    """

    # ── build dir absent → auto-configure ────────────────────────────────────

    def test_auto_configure_when_build_dir_absent(self, tmp_path: Path) -> None:
        """No build dir at all → configure runs first, creates the dir."""
        # We do NOT create build/ — run_app must create it and configure.
        build_path = tmp_path / "build"
        # Make the exe appear after configure would have "created" the dir.

        def _fake_run(cmd, **kwargs):
            # When cmake configure runs it creates the dir + cache.
            if "--build" not in cmd and "-B" in cmd:
                build_path.mkdir(parents=True, exist_ok=True)
                (build_path / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")
                _make_exe(build_path, "calc")
            return _completed(0)

        with (
            patch("sdlos.commands.run.subprocess.run", side_effect=_fake_run) as mock_run,
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
        ):
            run_app("calc", tmp_path, build_dir=build_path, quiet=True)

        call_cmds = [c[0][0] for c in mock_run.call_args_list]
        configure_calls = [c for c in call_cmds if "--build" not in c]
        build_calls     = [c for c in call_cmds if "--build" in c]
        assert len(configure_calls) >= 1, "configure should have been called"
        assert len(build_calls)     == 1, "build should have been called once"

    def test_auto_configure_when_no_cmake_cache(self, tmp_path: Path) -> None:
        """Build dir exists but no CMakeCache.txt → configure runs."""
        bd = _build_dir(tmp_path)
        _make_exe(bd, "calc")
        # Deliberately NOT writing CMakeCache.txt.

        with (
            patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run,
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
        ):
            run_app("calc", tmp_path, build_dir=bd, quiet=True)

        call_cmds = [c[0][0] for c in mock_run.call_args_list]
        configure_calls = [c for c in call_cmds if "--build" not in c]
        assert len(configure_calls) >= 1, "configure should run when CMakeCache is absent"

    def test_no_auto_configure_when_already_configured(self, tmp_path: Path) -> None:
        """CMakeCache.txt present → configure must NOT run again."""
        bd = _build_dir(tmp_path)
        _make_exe(bd, "calc")
        (bd / "CMakeCache.txt").write_text("# already configured\n", encoding="utf-8")

        with (
            patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run,
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
        ):
            run_app("calc", tmp_path, build_dir=bd, quiet=True)

        call_cmds = [c[0][0] for c in mock_run.call_args_list]
        configure_calls = [c for c in call_cmds if "--build" not in c]
        assert len(configure_calls) == 0, "should not re-configure an already-configured build"

    # ── --reconfigure flag ────────────────────────────────────────────────────

    def test_reconfigure_forces_configure_even_when_cached(self, tmp_path: Path) -> None:
        """--reconfigure triggers a configure pass even with CMakeCache.txt."""
        bd = _build_dir(tmp_path)
        _make_exe(bd, "calc")
        (bd / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")

        with (
            patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run,
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
        ):
            run_app("calc", tmp_path, build_dir=bd, reconfigure=True, quiet=True)

        call_cmds = [c[0][0] for c in mock_run.call_args_list]
        configure_calls = [c for c in call_cmds if "--build" not in c]
        assert len(configure_calls) >= 1, "--reconfigure should force a configure pass"

    def test_reconfigure_runs_before_build(self, tmp_path: Path) -> None:
        """Configure call must precede the build call in the call list."""
        bd = _build_dir(tmp_path)
        _make_exe(bd, "calc")
        (bd / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")
        call_order: list[str] = []

        def _record(cmd, **kwargs):
            call_order.append("configure" if "--build" not in cmd else "build")
            return _completed(0)

        with (
            patch("sdlos.commands.run.subprocess.run", side_effect=_record),
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
        ):
            run_app("calc", tmp_path, build_dir=bd, reconfigure=True, quiet=True)

        assert call_order[0] == "configure"
        assert "build" in call_order

    # ── --preset propagation ──────────────────────────────────────────────────

    def test_preset_passed_to_configure(self, tmp_path: Path) -> None:
        """--preset is forwarded to the cmake configure command."""
        bd = _build_dir(tmp_path)
        _make_exe(bd, "calc")
        # No CMakeCache → auto-configure will fire.

        with (
            patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run,
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
        ):
            run_app("calc", tmp_path, build_dir=bd,
                    preset="macos-debug", quiet=True)

        call_cmds = [c[0][0] for c in mock_run.call_args_list]
        configure_calls = [c for c in call_cmds if "--preset" in c]
        assert len(configure_calls) >= 1
        preset_idx = configure_calls[0].index("--preset")
        assert configure_calls[0][preset_idx + 1] == "macos-debug"

    def test_preset_not_passed_to_build(self, tmp_path: Path) -> None:
        """--preset must NOT appear in the cmake --build command."""
        bd = _build_dir(tmp_path)
        _make_exe(bd, "calc")
        (bd / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")

        with (
            patch("sdlos.commands.run.subprocess.run", return_value=_completed(0)) as mock_run,
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
        ):
            run_app("calc", tmp_path, build_dir=bd,
                    reconfigure=True, preset="macos-release", quiet=True)

        call_cmds = [c[0][0] for c in mock_run.call_args_list]
        build_calls = [c for c in call_cmds if "--build" in c]
        assert all("--preset" not in c for c in build_calls), \
            "--preset must only appear in the configure call, not the build call"

    # ── failed configure exits ────────────────────────────────────────────────

    def test_failed_configure_exits_before_build(self, tmp_path: Path) -> None:
        """If cmake configure fails, build must not be attempted."""
        bd = _build_dir(tmp_path)
        _make_exe(bd, "calc")
        # No CMakeCache → auto-configure will fire and fail.
        build_attempted = []

        def _fail_configure(cmd, **kwargs):
            if "--build" in cmd:
                build_attempted.append(True)
                return _completed(0)
            return _completed(1)  # configure fails

        with (
            patch("sdlos.commands.run.subprocess.run", side_effect=_fail_configure),
            patch("sdlos.commands.run.subprocess.Popen", return_value=_FakeProc()),
            pytest.raises(SystemExit),
        ):
            run_app("calc", tmp_path, build_dir=bd, quiet=True)

        assert not build_attempted, "build must not run after a configure failure"
