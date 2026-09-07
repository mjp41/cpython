import gc
import unittest
import weakref
from immutable import (
    freeze, is_frozen, set_freezable,
    FREEZABLE_YES, FREEZABLE_NO, FREEZABLE_EXPLICIT, FREEZABLE_PROXY,
)


def make_freezable_class():
    """Create a fresh class marked as freezable."""
    class C:
        pass
    set_freezable(C, FREEZABLE_YES)
    return C


class TestSetFreezableYes(unittest.TestCase):
    """FREEZABLE_YES: object is always freezable."""

    def test_freeze_succeeds(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_YES)
        freeze(obj)
        self.assertTrue(is_frozen(obj))

    def test_freeze_as_child_succeeds(self):
        C = make_freezable_class()
        parent = C()
        child = C()
        parent.child = child
        set_freezable(child, FREEZABLE_YES)
        freeze(parent)
        self.assertTrue(is_frozen(child))


class TestSetFreezableNo(unittest.TestCase):
    """FREEZABLE_NO: object can never be frozen."""

    def test_freeze_raises(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(obj)
        self.assertFalse(is_frozen(obj))

    def test_freeze_as_child_raises(self):
        C = make_freezable_class()
        parent = C()
        child = C()
        parent.child = child
        set_freezable(child, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(parent)
        self.assertFalse(is_frozen(child))
        self.assertFalse(is_frozen(parent))


class TestSetFreezableExplicit(unittest.TestCase):
    """FREEZABLE_EXPLICIT: freezable only when freeze() is called directly on it."""

    def test_direct_freeze_succeeds(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_EXPLICIT)
        freeze(obj)
        self.assertTrue(is_frozen(obj))

    def test_child_freeze_raises(self):
        C = make_freezable_class()
        parent = C()
        child = C()
        parent.child = child
        set_freezable(child, FREEZABLE_EXPLICIT)
        with self.assertRaises(TypeError):
            freeze(parent)
        self.assertFalse(is_frozen(child))


class TestSetFreezableProxy(unittest.TestCase):
    """FREEZABLE_PROXY: only allowed on module objects."""

    def test_proxy_rejected_on_non_module(self):
        C = make_freezable_class()
        obj = C()
        with self.assertRaises(TypeError):
            set_freezable(obj, FREEZABLE_PROXY)

    def test_proxy_allowed_on_module(self):
        import types
        mod = types.ModuleType('test_proxy_mod')
        set_freezable(mod, FREEZABLE_PROXY)


class TestSetFreezableEdgeCases(unittest.TestCase):
    """Edge cases and error handling."""

    def test_invalid_status_raises(self):
        C = make_freezable_class()
        obj = C()
        with self.assertRaises(ValueError):
            set_freezable(obj, 99)
        with self.assertRaises(ValueError):
            set_freezable(obj, -1)

    def test_object_without_dict_uses_ob_flags(self):
        # Built-in ints don't support attributes, but ob_flags fallback
        # should work on 64-bit.
        import sys
        if sys.maxsize <= 2**31:
            self.skipTest("ob_flags fallback not available on 32-bit")
        set_freezable(42, FREEZABLE_NO)
        # Can't easily verify the flags directly, but it shouldn't raise.

    def test_gc_collects_tracked_object(self):
        C = make_freezable_class()
        obj = C()
        ref = weakref.ref(obj)
        set_freezable(obj, FREEZABLE_NO)
        del obj
        gc.collect()
        self.assertIsNone(ref())

    def test_override_status(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(obj)
        # Override to YES
        set_freezable(obj, FREEZABLE_YES)
        freeze(obj)
        self.assertTrue(is_frozen(obj))

    def test_unset_object_uses_default(self):
        # An object with no set_freezable should use existing freeze logic.
        C = make_freezable_class()
        obj = C()
        freeze(obj)
        self.assertTrue(is_frozen(obj))


class TestSetFreezableStorage(unittest.TestCase):
    """Test the attribute-first, weakref-fallback storage strategy."""

    def test_attr_storage_for_normal_objects(self):
        # Objects with __dict__ should get __freezable__ attribute set.
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_NO)
        self.assertEqual(obj.__freezable__, FREEZABLE_NO)

    def test_attr_stores_each_status(self):
        C = make_freezable_class()
        for status in (FREEZABLE_YES, FREEZABLE_NO,
                       FREEZABLE_EXPLICIT):
            obj = C()
            set_freezable(obj, status)
            self.assertEqual(obj.__freezable__, status,
                             f"__freezable__ should be {status}")

    def test_attr_storage_updates_on_override(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_NO)
        self.assertEqual(obj.__freezable__, FREEZABLE_NO)
        set_freezable(obj, FREEZABLE_YES)
        self.assertEqual(obj.__freezable__, FREEZABLE_YES)

    def test_ob_flags_fallback_for_slots_only(self):
        # Objects with __slots__ but no __dict__ use ob_flags for
        # instance-level set_freezable on 64-bit.
        import sys
        if sys.maxsize <= 2**31:
            self.skipTest("ob_flags fallback not available on 32-bit")
        class S:
            __slots__ = ('__weakref__', 'x')
        obj = S()
        set_freezable(obj, FREEZABLE_NO)
        # No __freezable__ attribute should be set on the instance
        # (it has no __dict__).
        self.assertFalse(hasattr(obj, '__freezable__'))
        # But the status should still be queryable during freeze.
        with self.assertRaises(TypeError):
            freeze(obj)

    def test_manual_freezable_attr_respected(self):
        # Manually setting __freezable__ on an object should be respected.
        C = make_freezable_class()
        obj = C()
        obj.__freezable__ = FREEZABLE_NO
        with self.assertRaises(TypeError):
            freeze(obj)


class TestSetFreezableLifetime(unittest.TestCase):
    """Ensure set_freezable does not keep objects alive longer than expected."""

    def test_attr_path_no_prevent_gc(self):
        # Objects with __dict__ use attribute storage.
        # set_freezable should not prevent collection.
        C = make_freezable_class()
        obj = C()
        ref = weakref.ref(obj)
        set_freezable(obj, FREEZABLE_YES)
        del obj
        gc.collect()
        self.assertIsNone(ref())

    def test_ob_flags_path_no_prevent_gc(self):
        # Objects with __slots__ use ob_flags storage.
        # set_freezable should not prevent collection.
        class S:
            __slots__ = ('__weakref__', 'x')
        set_freezable(S, FREEZABLE_YES)
        obj = S()
        ref = weakref.ref(obj)
        set_freezable(obj, FREEZABLE_NO)
        del obj
        gc.collect()
        self.assertIsNone(ref())

    def test_each_status_no_prevent_gc(self):
        # Verify for every status value that the object is collected.
        C = make_freezable_class()
        for status in (FREEZABLE_YES, FREEZABLE_NO,
                       FREEZABLE_EXPLICIT):
            obj = C()
            ref = weakref.ref(obj)
            set_freezable(obj, status)
            del obj
            gc.collect()
            self.assertIsNone(ref(),
                              f"Object with status {status} was kept alive")

    def test_overwritten_status_no_prevent_gc(self):
        # Override status multiple times, then delete.
        C = make_freezable_class()
        obj = C()
        ref = weakref.ref(obj)
        set_freezable(obj, FREEZABLE_NO)
        set_freezable(obj, FREEZABLE_YES)
        set_freezable(obj, FREEZABLE_EXPLICIT)
        del obj
        gc.collect()
        self.assertIsNone(ref())

    def test_cyclic_reference_with_set_freezable(self):
        # Objects in a reference cycle with set_freezable should
        # still be collected by the cycle detector.
        C = make_freezable_class()
        a = C()
        b = C()
        a.other = b
        b.other = a
        ref_a = weakref.ref(a)
        ref_b = weakref.ref(b)
        set_freezable(a, FREEZABLE_NO)
        set_freezable(b, FREEZABLE_NO)
        del a, b
        gc.collect()
        self.assertIsNone(ref_a())
        self.assertIsNone(ref_b())


class TestConstants(unittest.TestCase):
    """Verify the constant values are exposed correctly."""

    def test_constant_values(self):
        self.assertEqual(FREEZABLE_YES, 0)
        self.assertEqual(FREEZABLE_NO, 1)
        self.assertEqual(FREEZABLE_EXPLICIT, 2)
        self.assertEqual(FREEZABLE_PROXY, 3)


if __name__ == '__main__':
    unittest.main()
