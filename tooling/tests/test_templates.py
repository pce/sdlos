"""
tests/test_templates.py
=======================
Integration tests for sdlos.templates.renderer:
  - All three templates (minimal, shader, camera) render without error.
  - Key identifiers (pascal name, app name, bus events) appear in output.
  - output_filename() maps kinds to correct filenames.
  - list_available() returns all three templates.
  - Jinja2 delimiter choice ({$ $}) does not corrupt C++ syntax.
"""
import re
import pytest

from sdlos.config.schema import AppConfig
from sdlos.templates.renderer import (
    KINDS,
    list_available,
    output_filename,
    render_template,
)


# ── Helpers ───────────────────────────────────────────────────────────────────

def _cfg(name: str, template: str = "minimal", **kwargs) -> AppConfig:
    return AppConfig(name=name, template=template, **kwargs)


# ── list_available / output_filename ─────────────────────────────────────────

class TestRendererMeta:
    def test_all_templates_available(self) -> None:
        available = list_available()
        assert "minimal" in available
        assert "shader"  in available
        assert "camera"  in available

    def test_output_filename_jade(self) -> None:
        assert output_filename("jade", "my_app") == "my_app.jade"

    def test_output_filename_css(self) -> None:
        assert output_filename("css", "my_app") == "my_app.css"

    def test_output_filename_behavior_cxx(self) -> None:
        assert output_filename("behavior_cxx", "my_app") == "my_app_behavior.cxx"

    def test_output_filename_unknown_raises(self) -> None:
        with pytest.raises(ValueError, match="Unknown kind"):
            output_filename("html", "my_app")

    def test_kinds_tuple_complete(self) -> None:
        assert set(KINDS) == {"jade", "css", "behavior_cxx"}


# ── Minimal template ──────────────────────────────────────────────────────────

class TestMinimalTemplate:
    @pytest.fixture(scope="class")
    def renders(self):
        cfg = _cfg("hello_world")
        return {k: render_template("minimal", k, cfg) for k in KINDS}

    def test_jade_contains_name(self, renders) -> None:
        assert "hello_world" in renders["jade"]

    def test_jade_has_root_col(self, renders) -> None:
        assert 'col(id="root"' in renders["jade"]

    def test_css_contains_name(self, renders) -> None:
        assert "hello_world" in renders["css"]

    def test_css_has_root_rule(self, renders) -> None:
        assert "#root" in renders["css"]

    def test_behavior_contains_pascal_name(self, renders) -> None:
        assert "HelloWorldState" in renders["behavior_cxx"]

    def test_behavior_has_jade_app_init(self, renders) -> None:
        assert "void jade_app_init(" in renders["behavior_cxx"]

    def test_behavior_make_shared(self, renders) -> None:
        # Critical: std::make_shared<HelloWorldState>() must not have
        # an extra '<' before the type — delimiter bug check.
        assert "std::make_shared<HelloWorldState>()" in renders["behavior_cxx"]
        assert "std::make_shared<<HelloWorldState>" not in renders["behavior_cxx"]

    def test_behavior_user_region_markers(self, renders) -> None:
        assert "--- enter the forrest ---" in renders["behavior_cxx"]
        assert "--- back to the sea ---"   in renders["behavior_cxx"]

    def test_behavior_no_raw_delimiter(self, renders) -> None:
        # {$ and $} must not appear in rendered output
        assert "{$" not in renders["behavior_cxx"]
        assert "$}" not in renders["behavior_cxx"]

    def test_no_jinja_delimiter_leakage(self, renders) -> None:
        for kind, text in renders.items():
            assert "{$" not in text, f"Unrendered delimiter in {kind}"
            assert "$}" not in text, f"Unrendered delimiter in {kind}"


# ── Shader template ───────────────────────────────────────────────────────────

class TestShaderTemplate:
    @pytest.fixture(scope="class")
    def renders(self):
        cfg = _cfg("my_shader", template="shader", win_w=1280, win_h=800)
        return {k: render_template("shader", k, cfg) for k in KINDS}

    def test_jade_canvas_id(self, renders) -> None:
        assert 'id="my_shader-canvas"' in renders["jade"]

    def test_jade_preset_onclick(self, renders) -> None:
        assert 'onclick="my_shader:preset"' in renders["jade"]

    def test_jade_inc_dec_events(self, renders) -> None:
        assert 'onclick="my_shader:inc"' in renders["jade"]
        assert 'onclick="my_shader:dec"' in renders["jade"]

    def test_css_sidebar_rule(self, renders) -> None:
        assert ".sidebar" in renders["css"]

    def test_css_preset_rule(self, renders) -> None:
        assert ".preset" in renders["css"]

    def test_behavior_pascal_struct(self, renders) -> None:
        assert "struct MyShaderState" in renders["behavior_cxx"]

    def test_behavior_preset_array(self, renders) -> None:
        assert "kMyShaderPresets" in renders["behavior_cxx"]

    def test_behavior_make_shared_clean(self, renders) -> None:
        assert "std::make_shared<MyShaderState>()" in renders["behavior_cxx"]

    def test_behavior_bus_subscribe(self, renders) -> None:
        assert 'bus.subscribe("my_shader:preset"' in renders["behavior_cxx"]
        assert 'bus.subscribe("my_shader:inc"'    in renders["behavior_cxx"]
        assert 'bus.subscribe("my_shader:dec"'    in renders["behavior_cxx"]

    def test_behavior_user_region_markers(self, renders) -> None:
        assert "--- enter the forrest ---" in renders["behavior_cxx"]
        assert "--- back to the sea ---"   in renders["behavior_cxx"]

    def test_no_delimiter_leakage(self, renders) -> None:
        for kind, text in renders.items():
            assert "{$" not in text, f"Unrendered delimiter in shader/{kind}"
            assert "$}" not in text, f"Unrendered delimiter in shader/{kind}"

    def test_cpp_braces_intact(self, renders) -> None:
        # C++ aggregate initialisers must not be corrupted
        cxx = renders["behavior_cxx"]
        assert re.search(r'\{\s*"preset_a"', cxx), "Preset initialiser brace missing"
        assert re.search(r'\}\s*;', cxx),           "Closing brace+semicolon missing"


# ── Camera template ───────────────────────────────────────────────────────────

class TestCameraTemplate:
    @pytest.fixture(scope="class")
    def renders(self):
        cfg = _cfg("my_cam", template="camera")
        return {k: render_template("camera", k, cfg) for k in KINDS}

    def test_jade_canvas_id(self, renders) -> None:
        assert 'id="my_cam-canvas"' in renders["jade"]

    def test_jade_filter_onclick(self, renders) -> None:
        assert 'onclick="my_cam:filter"' in renders["jade"]

    def test_jade_device_name_id(self, renders) -> None:
        assert 'id="my_cam-device-name"' in renders["jade"]

    def test_css_has_preset_rule(self, renders) -> None:
        assert ".preset" in renders["css"]

    def test_behavior_pascal_struct(self, renders) -> None:
        assert "struct MyCamState" in renders["behavior_cxx"]

    def test_behavior_filter_array(self, renders) -> None:
        assert "kMyCamFilters" in renders["behavior_cxx"]

    def test_behavior_make_shared_clean(self, renders) -> None:
        assert "std::make_shared<MyCamState>()" in renders["behavior_cxx"]

    def test_behavior_bus_subscribe_filter(self, renders) -> None:
        assert 'bus.subscribe("my_cam:filter"' in renders["behavior_cxx"]

    def test_behavior_sdl_event_hook(self, renders) -> None:
        assert "SDL_EVENT_MOUSE_BUTTON_DOWN" in renders["behavior_cxx"]
        assert "pixelScaleX()"              in renders["behavior_cxx"]

    def test_behavior_dragnum_include(self, renders) -> None:
        assert '#include "widgets/number_dragger.hh"' in renders["behavior_cxx"]

    def test_behavior_user_region_markers(self, renders) -> None:
        assert "--- enter the forrest ---" in renders["behavior_cxx"]
        assert "--- back to the sea ---"   in renders["behavior_cxx"]

    def test_no_delimiter_leakage(self, renders) -> None:
        for kind, text in renders.items():
            assert "{$" not in text, f"Unrendered delimiter in camera/{kind}"
            assert "$}" not in text, f"Unrendered delimiter in camera/{kind}"


# ── Cross-template: different app names ──────────────────────────────────────

class TestNameSubstitution:
    @pytest.mark.parametrize("name,expected_pascal", [
        ("calc",       "Calc"),
        ("my_app",     "MyApp"),
        ("head_volley","HeadVolley"),
        ("sdl3_demo",  "Sdl3Demo"),
    ])
    def test_pascal_name_in_behavior(self, name: str, expected_pascal: str) -> None:
        cfg = _cfg(name, template="minimal")
        cxx = render_template("minimal", "behavior_cxx", cfg)
        assert f"struct {expected_pascal}State" in cxx
        assert f"std::make_shared<{expected_pascal}State>()" in cxx

    @pytest.mark.parametrize("name", ["calc", "my_app", "head_volley"])
    def test_name_in_jade(self, name: str) -> None:
        cfg = _cfg(name, template="minimal")
        jade = render_template("minimal", "jade", cfg)
        assert name in jade

    @pytest.mark.parametrize("template", ["minimal", "shader", "camera"])
    def test_all_templates_render_for_same_name(self, template: str) -> None:
        cfg = _cfg("smoke_test", template=template)
        for kind in KINDS:
            text = render_template(template, kind, cfg)
            assert len(text) > 0
            assert "{$" not in text
            assert "$}" not in text


# ── Unknown template / kind errors ───────────────────────────────────────────

class TestRendererErrors:
    def test_unknown_kind_raises_value_error(self) -> None:
        cfg = _cfg("err_app")
        with pytest.raises(ValueError, match="Unknown template kind"):
            render_template("minimal", "unknown_kind", cfg)

    def test_unknown_template_raises_click_exception(self) -> None:
        import click
        cfg = _cfg("err_app")
        with pytest.raises(click.ClickException):
            render_template("nonexistent_template", "jade", cfg)
