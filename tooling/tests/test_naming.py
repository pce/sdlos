"""
tests/test_naming.py
====================
Unit tests for sdlos.core.naming — pascal() and validate_name().
"""
import pytest
import click

from sdlos.core.naming import pascal, validate_name


# ── pascal() ─────────────────────────────────────────────────────────────────

class TestPascal:
    def test_single_word(self):
        assert pascal("calc") == "Calc"

    def test_snake_case(self):
        assert pascal("my_app") == "MyApp"

    def test_kebab_case(self):
        assert pascal("head-volley") == "HeadVolley"

    def test_mixed_separators(self):
        assert pascal("my_cool-app") == "MyCoolApp"

    def test_already_single_upper(self):
        assert pascal("A") == "A"

    def test_digits_preserved(self):
        assert pascal("app2d") == "App2d"

    def test_multi_segment_digits(self):
        assert pascal("sdl_3_renderer") == "Sdl3Renderer"

    def test_leading_digit_segment(self):
        # splitting on _ gives ["3", "d"] — capitalize keeps "3" then "D"
        assert pascal("3_d") == "3D"

    def test_empty_string(self):
        assert pascal("") == ""


# ── validate_name() ───────────────────────────────────────────────────────────

class TestValidateName:
    def test_valid_simple(self):
        assert validate_name("hello") == "hello"

    def test_valid_snake(self):
        assert validate_name("my_app") == "my_app"

    def test_valid_with_digits(self):
        assert validate_name("app2") == "app2"

    def test_uppercase_lowercased(self):
        assert validate_name("MyApp") == "myapp"

    def test_hyphens_converted(self):
        assert validate_name("head-volley") == "head_volley"

    def test_hyphen_and_upper(self):
        assert validate_name("Head-Volley") == "head_volley"

    def test_leading_digit_rejected(self):
        with pytest.raises(click.BadParameter):
            validate_name("1app")

    def test_empty_rejected(self):
        with pytest.raises(click.BadParameter):
            validate_name("")

    def test_space_rejected(self):
        with pytest.raises(click.BadParameter):
            validate_name("my app")

    def test_dot_rejected(self):
        with pytest.raises(click.BadParameter):
            validate_name("my.app")

    def test_slash_rejected(self):
        with pytest.raises(click.BadParameter):
            validate_name("my/app")
