"""Tests for get_freezable() and unset_freezable()."""

import unittest
from immutable import (
    set_freezable, get_freezable, unset_freezable,
    FREEZABLE_YES, FREEZABLE_NO, FREEZABLE_EXPLICIT,
)


def make_freezable_class():
    """Create a fresh class marked as freezable."""
    class C:
        pass
    set_freezable(C, FREEZABLE_YES)
    return C


class TestGetFreezable(unittest.TestCase):
    """Tests for get_freezable()."""

    def test_no_status_set_returns_negative_one(self):
        class C:
            pass
        self.assertEqual(get_freezable(C()), -1)

    def test_returns_yes(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_YES)
        self.assertEqual(get_freezable(obj), FREEZABLE_YES)

    def test_returns_no(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_NO)
        self.assertEqual(get_freezable(obj), FREEZABLE_NO)

    def test_returns_explicit(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_EXPLICIT)
        self.assertEqual(get_freezable(obj), FREEZABLE_EXPLICIT)

    def test_reflects_override(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_YES)
        self.assertEqual(get_freezable(obj), FREEZABLE_YES)
        set_freezable(obj, FREEZABLE_NO)
        self.assertEqual(get_freezable(obj), FREEZABLE_NO)

    def test_type_level_status(self):
        C = make_freezable_class()
        # The class has FREEZABLE_YES; an instance with no per-object
        # status should inherit from the type.
        obj = C()
        self.assertEqual(get_freezable(obj), FREEZABLE_YES)

    def test_object_status_overrides_type(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_NO)
        # Per-object status should take precedence over the type's.
        self.assertEqual(get_freezable(obj), FREEZABLE_NO)
        # The type itself should still be YES.
        self.assertEqual(get_freezable(C), FREEZABLE_YES)


class TestUnsetFreezable(unittest.TestCase):
    """Tests for unset_freezable()."""

    def test_unset_removes_per_object_status(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_NO)
        self.assertEqual(get_freezable(obj), FREEZABLE_NO)
        unset_freezable(obj)
        # Falls back to the type's status (FREEZABLE_YES).
        self.assertEqual(get_freezable(obj), FREEZABLE_YES)

    def test_unset_when_nothing_set(self):
        class C:
            pass
        obj = C()
        # Should not raise even if nothing was set.
        unset_freezable(obj)
        self.assertEqual(get_freezable(obj), -1)

    def test_unset_reveals_type_status(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_EXPLICIT)
        self.assertEqual(get_freezable(obj), FREEZABLE_EXPLICIT)
        unset_freezable(obj)
        # Now the type's YES should show through.
        self.assertEqual(get_freezable(obj), FREEZABLE_YES)

    def test_unset_on_class(self):
        class C:
            pass
        set_freezable(C, FREEZABLE_NO)
        self.assertEqual(get_freezable(C), FREEZABLE_NO)
        unset_freezable(C)
        # After unsetting, falls back to the metaclass (type) status.
        # type may or may not have __freezable__ set depending on
        # interpreter state, so just verify it's no longer NO.
        self.assertNotEqual(get_freezable(C), FREEZABLE_NO)

    def test_unset_then_set_again(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_NO)
        unset_freezable(obj)
        set_freezable(obj, FREEZABLE_EXPLICIT)
        self.assertEqual(get_freezable(obj), FREEZABLE_EXPLICIT)


if __name__ == '__main__':
    unittest.main()
