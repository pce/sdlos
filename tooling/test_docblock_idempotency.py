"""
Test: Docblock Processor Idempotency
Verifies that the docblock processor is now idempotent and converges.
"""

def test_docblock_idempotency():
    """Docblock processor should be idempotent (no duplicate docblocks on re-runs)."""
    from pathlib import Path
    from sdlos.features.docblocks import Docblocks

    # Original source with one undocumented function
    source_v0 = """
#include <cstdint>

void process_data(int count) {
    // Process data here
}
""".strip()

    # Expected after first run: docblock added
    processor = Docblocks(dry_run=False)  # dry_run so we don't touch disk

    # Apply first time
    source_v1 = processor.apply_to_source(source_v0, path=Path("test.cxx"))

    # Verify docblock was inserted
    assert "/**" in source_v1, "First run should insert docblock"
    assert "@brief" in source_v1, "Should have @brief tag"

    # Apply second time (should be no-op)
    source_v2 = processor.apply_to_source(source_v1, path=Path("test.cxx"))

    # Verify idempotency: source should not change
    assert source_v1 == source_v2, "Second run should not modify the file (idempotent)"

    # Verify convergence: apply third time should still be identical
    source_v3 = processor.apply_to_source(source_v2, path=Path("test.cxx"))
    assert source_v2 == source_v3, "Third run should also produce identical result (convergent)"

    # Verify no duplicates
    docblock_count = source_v2.count("/**")
    assert docblock_count == 1, f"Should have exactly 1 docblock, got {docblock_count}"

    print("✅ Idempotency smoke test PASSED")
    print(f"   v0: {len(source_v0)} chars")
    print(f"   v1: {len(source_v1)} chars (docblock added)")
    print(f"   v2: {len(source_v2)} chars (identical to v1 ✓)")
    print(f"   v3: {len(source_v3)} chars (identical to v2 ✓)")


def test_multiline_docblock_detection():
    """Enhanced detection should handle multi-line docblocks correctly."""
    from sdlos.features.docblocks import _has_preceding_docblock

    # Multi-line docblock followed by function declaration
    lines = [
        "/**",
        " * @brief Process data",
        " *",
        " * @param count  Number of items",
        " *",
        " * @return Result code",
        " */",
        "int process(int count);",
    ]

    # Line 8 (1-based) is the function declaration
    # Should detect the multi-line docblock above it
    has_docblock = _has_preceding_docblock(lines, 8)
    assert has_docblock, "Should detect multi-line docblock"

    print("✅ Multi-line docblock detection smoke test PASSED")


def test_triple_slash_detection():
    """Should detect /// style comments correctly."""
    from sdlos.features.docblocks import _has_preceding_docblock

    # Triple-slash style docblock
    lines = [
        "/// @brief Process data",
        "/// @param count Number of items",
        "/// @return Result code",
        "int process(int count);",
    ]

    has_docblock = _has_preceding_docblock(lines, 4)
    assert has_docblock, "Should detect triple-slash docblock"

    print("✅ Triple-slash detection smoke test PASSED")


def test_no_docblock_detection():
    """Should correctly identify undocumented functions."""
    from sdlos.features.docblocks import _has_preceding_docblock

    # No docblock, just blank lines and function
    lines = [
        "",
        "",
        "int process(int count);",
    ]

    has_docblock = _has_preceding_docblock(lines, 3)
    assert not has_docblock, "Should detect missing docblock"

    print("✅ Missing docblock detection smoke test PASSED")


if __name__ == "__main__":
    test_docblock_idempotency()
    test_multiline_docblock_detection()
    test_triple_slash_detection()
    test_no_docblock_detection()
    print("\n🎉 All idempotency smoke tests PASSED! :confetti")


