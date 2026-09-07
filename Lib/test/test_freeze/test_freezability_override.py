"""Tests for FreezabilityOverride and require_mutable context managers."""

import unittest
from immutable import (
    freeze, is_frozen, set_freezable, get_freezable,
    FreezabilityOverride, require_mutable,
    FREEZABLE_YES, FREEZABLE_NO, FREEZABLE_EXPLICIT,
)


def make_freezable_class():
    """Create a fresh class marked as freezable."""
    class C:
        pass
    set_freezable(C, FREEZABLE_YES)
    return C


class TestFreezabilityOverride(unittest.TestCase):
    """Tests for the FreezabilityOverride context manager."""

    def test_basic_override_and_restore(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_YES)
        with FreezabilityOverride(obj, FREEZABLE_NO):
            self.assertEqual(get_freezable(obj), FREEZABLE_NO)
        self.assertEqual(get_freezable(obj), FREEZABLE_YES)

    def test_override_prevents_freeze(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_YES)
        with FreezabilityOverride(obj, FREEZABLE_NO):
            with self.assertRaises(TypeError):
                freeze(obj)
        # After the context manager, freezing should work again.
        freeze(obj)
        self.assertTrue(is_frozen(obj))

    def test_override_allows_freeze(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_NO)
        with FreezabilityOverride(obj, FREEZABLE_YES):
            freeze(obj)
            self.assertTrue(is_frozen(obj))

    def test_override_to_explicit(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_YES)
        with FreezabilityOverride(obj, FREEZABLE_EXPLICIT):
            self.assertEqual(get_freezable(obj), FREEZABLE_EXPLICIT)
        self.assertEqual(get_freezable(obj), FREEZABLE_YES)

    def test_restores_on_exception(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_YES)
        with self.assertRaises(RuntimeError):
            with FreezabilityOverride(obj, FREEZABLE_NO):
                self.assertEqual(get_freezable(obj), FREEZABLE_NO)
                raise RuntimeError("deliberate")
        # Status should be restored even after an exception.
        self.assertEqual(get_freezable(obj), FREEZABLE_YES)

    def test_returns_obj_from_enter(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_YES)
        with FreezabilityOverride(obj, FREEZABLE_NO) as returned:
            self.assertIs(returned, obj)

    def test_no_prior_status_restores_default(self):
        C = make_freezable_class()
        obj = C()
        # Object inherits YES from type, no per-object status.
        self.assertEqual(get_freezable(obj), FREEZABLE_YES)
        with FreezabilityOverride(obj, FREEZABLE_NO):
            self.assertEqual(get_freezable(obj), FREEZABLE_NO)
        # The inherited YES is restored (now set explicitly on the
        # object — this is the "no-effort unset" design choice).
        self.assertEqual(get_freezable(obj), FREEZABLE_YES)

    def test_no_status_anywhere_restores_yes(self):
        class C:
            pass
        obj = C()
        self.assertEqual(get_freezable(obj), -1)
        with FreezabilityOverride(obj, FREEZABLE_NO):
            self.assertEqual(get_freezable(obj), FREEZABLE_NO)
        # When no status existed anywhere, restores to FREEZABLE_YES.
        self.assertEqual(get_freezable(obj), FREEZABLE_YES)

    def test_nested_overrides(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_YES)
        with FreezabilityOverride(obj, FREEZABLE_NO):
            self.assertEqual(get_freezable(obj), FREEZABLE_NO)
            with FreezabilityOverride(obj, FREEZABLE_EXPLICIT):
                self.assertEqual(get_freezable(obj), FREEZABLE_EXPLICIT)
            self.assertEqual(get_freezable(obj), FREEZABLE_NO)
        self.assertEqual(get_freezable(obj), FREEZABLE_YES)

    def test_override_on_class(self):
        C = make_freezable_class()
        self.assertEqual(get_freezable(C), FREEZABLE_YES)
        with FreezabilityOverride(C, FREEZABLE_NO):
            self.assertEqual(get_freezable(C), FREEZABLE_NO)
        self.assertEqual(get_freezable(C), FREEZABLE_YES)


class TestRequireMutable(unittest.TestCase):
    """Tests for the require_mutable context manager."""

    def test_sets_freezable_no(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_YES)
        with require_mutable(obj):
            self.assertEqual(get_freezable(obj), FREEZABLE_NO)
        self.assertEqual(get_freezable(obj), FREEZABLE_YES)

    def test_prevents_freeze_inside(self):
        C = make_freezable_class()
        obj = C()
        with require_mutable(obj):
            with self.assertRaises(TypeError):
                freeze(obj)

    def test_raises_if_already_frozen(self):
        C = make_freezable_class()
        obj = C()
        freeze(obj)
        with self.assertRaises(TypeError):
            with require_mutable(obj):
                pass  # should never reach here

    def test_restores_on_exception(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_YES)
        with self.assertRaises(RuntimeError):
            with require_mutable(obj):
                raise RuntimeError("deliberate")
        self.assertEqual(get_freezable(obj), FREEZABLE_YES)

    def test_returns_obj_from_enter(self):
        C = make_freezable_class()
        obj = C()
        with require_mutable(obj) as returned:
            self.assertIs(returned, obj)


if __name__ == '__main__':
    unittest.main()
