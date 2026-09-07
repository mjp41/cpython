"""Tests for freeze() with multiple arguments."""

import unittest
from immutable import (
    freeze, is_frozen, set_freezable,
    FREEZABLE_EXPLICIT, FREEZABLE_NO, FREEZABLE_YES,
)


def make_freezable_class():
    """Create a fresh class marked as freezable."""
    class C:
        pass
    set_freezable(C, FREEZABLE_YES)
    return C


class TestMultiFreezeBasic(unittest.TestCase):
    """Basic tests for freeze() accepting multiple arguments."""

    def test_single_arg(self):
        """Single-arg freeze still works."""
        C = make_freezable_class()
        obj = C()
        freeze(obj)
        self.assertTrue(is_frozen(obj))

    def test_two_args(self):
        C = make_freezable_class()
        a, b = C(), C()
        freeze(a, b)
        self.assertTrue(is_frozen(a))
        self.assertTrue(is_frozen(b))

    def test_many_args(self):
        C = make_freezable_class()
        objs = [C() for _ in range(10)]
        freeze(*objs)
        for obj in objs:
            self.assertTrue(is_frozen(obj))

    def test_zero_args_raises(self):
        with self.assertRaises(TypeError):
            freeze()

    def test_already_frozen_skipped(self):
        """Already-frozen objects among the arguments don't cause errors."""
        C = make_freezable_class()
        a, b = C(), C()
        freeze(a)
        self.assertTrue(is_frozen(a))
        freeze(a, b)
        self.assertTrue(is_frozen(b))

    def test_all_already_frozen(self):
        """Calling freeze on objects that are all already frozen is a no-op."""
        C = make_freezable_class()
        a, b = C(), C()
        freeze(a)
        freeze(b)
        freeze(a, b)  # should not raise
        self.assertTrue(is_frozen(a))
        self.assertTrue(is_frozen(b))


class TestMultiFreezeSharedGraph(unittest.TestCase):
    """Tests where multiple roots share reachable objects."""

    def test_shared_child(self):
        """Two roots pointing to the same child object."""
        C = make_freezable_class()
        child = C()
        a, b = C(), C()
        a.child = child
        b.child = child
        freeze(a, b)
        self.assertTrue(is_frozen(a))
        self.assertTrue(is_frozen(b))
        self.assertTrue(is_frozen(child))

    def test_cross_references(self):
        """Roots that reference each other."""
        C = make_freezable_class()
        a, b = C(), C()
        a.other = b
        b.other = a
        freeze(a, b)
        self.assertTrue(is_frozen(a))
        self.assertTrue(is_frozen(b))


class TestMultiFreezeExplicit(unittest.TestCase):
    """FREEZABLE_EXPLICIT interacts correctly with multiple roots."""

    def test_explicit_as_single_root(self):
        """EXPLICIT object passed directly to freeze() succeeds."""
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_EXPLICIT)
        freeze(obj)
        self.assertTrue(is_frozen(obj))

    def test_explicit_as_child_fails(self):
        """EXPLICIT object reached as child (not a root) is rejected."""
        C = make_freezable_class()
        parent, child = C(), C()
        parent.child = child
        set_freezable(child, FREEZABLE_EXPLICIT)
        with self.assertRaises(TypeError):
            freeze(parent)
        self.assertFalse(is_frozen(child))

    def test_explicit_as_one_of_multiple_roots(self):
        """EXPLICIT object listed as a root in multi-arg freeze succeeds."""
        C = make_freezable_class()
        a, b = C(), C()
        a.child = b
        set_freezable(b, FREEZABLE_EXPLICIT)
        freeze(a, b)
        self.assertTrue(is_frozen(a))
        self.assertTrue(is_frozen(b))

    def test_multiple_explicit_roots(self):
        """Multiple EXPLICIT objects all passed as roots."""
        C = make_freezable_class()
        a, b = C(), C()
        set_freezable(a, FREEZABLE_EXPLICIT)
        set_freezable(b, FREEZABLE_EXPLICIT)
        freeze(a, b)
        self.assertTrue(is_frozen(a))
        self.assertTrue(is_frozen(b))

    def test_explicit_child_not_in_roots_fails(self):
        """EXPLICIT child reachable from one root but not itself a root."""
        C = make_freezable_class()
        a, b, c = C(), C(), C()
        a.child = c
        set_freezable(c, FREEZABLE_EXPLICIT)
        # c is not in the roots list, so it should fail
        with self.assertRaises(TypeError):
            freeze(a, b)
        self.assertFalse(is_frozen(c))


class TestMultiFreezeNotFreezable(unittest.TestCase):
    """FREEZABLE_NO objects block the entire multi-arg freeze."""

    def test_one_not_freezable_blocks_all(self):
        C = make_freezable_class()
        a, b = C(), C()
        set_freezable(b, FREEZABLE_NO)
        a.child = b
        with self.assertRaises(TypeError):
            freeze(a, b)

    def test_not_freezable_root(self):
        C = make_freezable_class()
        a, b = C(), C()
        set_freezable(a, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(a, b)


class TestMultiFreezeAtomicity(unittest.TestCase):
    """All-or-nothing: if any object can't be frozen, none should be."""

    def test_not_freezable_root_leaves_others_unfrozen(self):
        """When one root is FREEZABLE_NO, the other root stays unfrozen."""
        C = make_freezable_class()
        a, b = C(), C()
        set_freezable(b, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(a, b)
        self.assertFalse(is_frozen(a))
        self.assertFalse(is_frozen(b))

    def test_not_freezable_child_leaves_parent_unfrozen(self):
        """When a child is FREEZABLE_NO, the parent root stays unfrozen."""
        C = make_freezable_class()
        parent, child = C(), C()
        parent.child = child
        set_freezable(child, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(parent)
        self.assertFalse(is_frozen(parent))
        self.assertFalse(is_frozen(child))

    def test_not_freezable_child_leaves_all_roots_unfrozen(self):
        """Multi-root: one root's child is FREEZABLE_NO, all roots stay unfrozen."""
        C = make_freezable_class()
        a, b, bad_child = C(), C(), C()
        a.child = bad_child
        set_freezable(bad_child, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(a, b)
        self.assertFalse(is_frozen(a))
        self.assertFalse(is_frozen(b))
        self.assertFalse(is_frozen(bad_child))

    def test_not_freezable_child_bad_root_last(self):
        """Bad root listed last — good root traversed first, still rolled back."""
        C = make_freezable_class()
        good, bad_parent, bad_child = C(), C(), C()
        bad_parent.child = bad_child
        set_freezable(bad_child, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(good, bad_parent)
        self.assertFalse(is_frozen(good))
        self.assertFalse(is_frozen(bad_parent))
        self.assertFalse(is_frozen(bad_child))

    def test_not_freezable_child_bad_root_first(self):
        """Bad root listed first — good root traversed after, still rolled back."""
        C = make_freezable_class()
        good, bad_parent, bad_child = C(), C(), C()
        bad_parent.child = bad_child
        set_freezable(bad_child, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(bad_parent, good)
        self.assertFalse(is_frozen(good))
        self.assertFalse(is_frozen(bad_parent))
        self.assertFalse(is_frozen(bad_child))

    def test_explicit_child_not_root_leaves_all_unfrozen(self):
        """EXPLICIT child not listed as root blocks freeze; nothing frozen."""
        C = make_freezable_class()
        a, b, explicit_child = C(), C(), C()
        a.child = explicit_child
        set_freezable(explicit_child, FREEZABLE_EXPLICIT)
        with self.assertRaises(TypeError):
            freeze(a, b)
        self.assertFalse(is_frozen(a))
        self.assertFalse(is_frozen(b))
        self.assertFalse(is_frozen(explicit_child))

    def test_explicit_child_bad_root_last(self):
        """EXPLICIT blocker's parent listed last — good root rolled back."""
        C = make_freezable_class()
        good, parent, explicit_child = C(), C(), C()
        parent.child = explicit_child
        set_freezable(explicit_child, FREEZABLE_EXPLICIT)
        with self.assertRaises(TypeError):
            freeze(good, parent)
        self.assertFalse(is_frozen(good))
        self.assertFalse(is_frozen(parent))
        self.assertFalse(is_frozen(explicit_child))

    def test_explicit_child_bad_root_first(self):
        """EXPLICIT blocker's parent listed first — good root rolled back."""
        C = make_freezable_class()
        good, parent, explicit_child = C(), C(), C()
        parent.child = explicit_child
        set_freezable(explicit_child, FREEZABLE_EXPLICIT)
        with self.assertRaises(TypeError):
            freeze(parent, good)
        self.assertFalse(is_frozen(good))
        self.assertFalse(is_frozen(parent))
        self.assertFalse(is_frozen(explicit_child))

    def test_many_roots_one_bad_none_frozen(self):
        """Many freezable roots plus one FREEZABLE_NO: none get frozen."""
        C = make_freezable_class()
        good = [C() for _ in range(5)]
        bad = C()
        set_freezable(bad, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(*good, bad)
        for obj in good:
            self.assertFalse(is_frozen(obj))
        self.assertFalse(is_frozen(bad))


class TestMultiFreezeExplicitNested(unittest.TestCase):
    """EXPLICIT annotation with nesting: child inside a root argument."""

    def test_explicit_nested_in_root_without_being_root_fails(self):
        """An EXPLICIT child nested inside a root is rejected when not a root itself."""
        C = make_freezable_class()
        outer = C()
        inner = C()
        outer.inner = inner
        set_freezable(inner, FREEZABLE_EXPLICIT)
        with self.assertRaises(TypeError):
            freeze(outer)
        self.assertFalse(is_frozen(outer))
        self.assertFalse(is_frozen(inner))

    def test_explicit_nested_in_root_and_also_root_succeeds(self):
        """An EXPLICIT child nested inside another root succeeds when also a root."""
        C = make_freezable_class()
        outer = C()
        inner = C()
        outer.inner = inner
        set_freezable(inner, FREEZABLE_EXPLICIT)
        freeze(outer, inner)
        self.assertTrue(is_frozen(outer))
        self.assertTrue(is_frozen(inner))

    def test_explicit_deeply_nested_as_root_succeeds(self):
        """Deeply nested EXPLICIT object succeeds when listed as root."""
        C = make_freezable_class()
        a, b, c = C(), C(), C()
        a.child = b
        b.child = c
        set_freezable(c, FREEZABLE_EXPLICIT)
        freeze(a, c)
        self.assertTrue(is_frozen(a))
        self.assertTrue(is_frozen(b))
        self.assertTrue(is_frozen(c))

    def test_explicit_deeply_nested_not_root_fails(self):
        """Deeply nested EXPLICIT object fails when not listed as root."""
        C = make_freezable_class()
        a, b, c = C(), C(), C()
        a.child = b
        b.child = c
        set_freezable(c, FREEZABLE_EXPLICIT)
        with self.assertRaises(TypeError):
            freeze(a)
        self.assertFalse(is_frozen(a))
        self.assertFalse(is_frozen(b))
        self.assertFalse(is_frozen(c))

    def test_multiple_explicit_nested_all_roots(self):
        """Multiple EXPLICIT objects nested in a chain, all listed as roots."""
        C = make_freezable_class()
        a, b, c = C(), C(), C()
        a.child = b
        b.child = c
        set_freezable(b, FREEZABLE_EXPLICIT)
        set_freezable(c, FREEZABLE_EXPLICIT)
        freeze(a, b, c)
        self.assertTrue(is_frozen(a))
        self.assertTrue(is_frozen(b))
        self.assertTrue(is_frozen(c))

    def test_multiple_explicit_nested_one_missing_from_roots(self):
        """Two EXPLICIT in chain but only one is a root — freeze fails, nothing frozen."""
        C = make_freezable_class()
        a, b, c = C(), C(), C()
        a.child = b
        b.child = c
        set_freezable(b, FREEZABLE_EXPLICIT)
        set_freezable(c, FREEZABLE_EXPLICIT)
        # b is a root but c is not
        with self.assertRaises(TypeError):
            freeze(a, b)
        self.assertFalse(is_frozen(a))
        self.assertFalse(is_frozen(b))
        self.assertFalse(is_frozen(c))


class TestFreezeReturnValue(unittest.TestCase):
    """freeze() returns its first argument."""

    def test_single_arg_returns_it(self):
        C = make_freezable_class()
        obj = C()
        result = freeze(obj)
        self.assertIs(result, obj)

    def test_multi_arg_returns_first(self):
        C = make_freezable_class()
        a, b, c = C(), C(), C()
        result = freeze(a, b, c)
        self.assertIs(result, a)

    def test_two_arg_returns_first(self):
        C = make_freezable_class()
        a, b = C(), C()
        result = freeze(a, b)
        self.assertIs(result, a)


if __name__ == '__main__':
    unittest.main()
