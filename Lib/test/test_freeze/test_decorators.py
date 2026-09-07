"""Tests for immutable module decorators."""

import unittest
from immutable import (
    freeze, is_frozen, freezable, unfreezable, explicitlyFreezable, frozen,
)


class TestFreezableDecorator(unittest.TestCase):

    def test_freezable_class_can_be_frozen(self):
        @freezable
        class C:
            pass
        obj = C()
        freeze(obj)
        self.assertTrue(is_frozen(obj))

    def test_freezable_returns_class(self):
        @freezable
        class C:
            pass
        self.assertEqual(C.__name__, 'C')


class TestUnfreezableDecorator(unittest.TestCase):

    def test_unfreezable_class_raises(self):
        @unfreezable
        class C:
            pass
        obj = C()
        with self.assertRaises(TypeError):
            freeze(obj)
        self.assertFalse(is_frozen(obj))

    def test_unfreezable_returns_class(self):
        @unfreezable
        class C:
            pass
        self.assertEqual(C.__name__, 'C')


class TestExplicitlyFreezableDecorator(unittest.TestCase):

    def test_explicit_direct_freeze_succeeds(self):
        @explicitlyFreezable
        class C:
            pass
        # The class itself can be frozen when passed directly.
        freeze(C)
        self.assertTrue(is_frozen(C))

    def test_explicit_as_child_fails(self):
        @freezable
        class Parent:
            pass

        @explicitlyFreezable
        class Child:
            pass

        p = Parent()
        p.child = Child()
        # Child's type is EXPLICIT, so freezing parent (which reaches
        # Child's type as a child) should fail.
        with self.assertRaises(TypeError):
            freeze(p)

    def test_explicit_returns_class(self):
        @explicitlyFreezable
        class C:
            pass
        self.assertEqual(C.__name__, 'C')


class TestFrozenDecorator(unittest.TestCase):

    def test_frozen_class_is_frozen(self):
        @frozen
        class C:
            pass
        self.assertTrue(is_frozen(C))

    def test_frozen_class_is_immutable(self):
        @frozen
        class C:
            pass
        with self.assertRaises(TypeError):
            C.new_attr = 42

    def test_frozen_returns_class(self):
        @frozen
        class C:
            pass
        self.assertEqual(C.__name__, 'C')


class TestFreezeReturnValue(unittest.TestCase):
    """freeze() returns its first argument."""

    def test_returns_same_object(self):
        @freezable
        class C:
            pass
        obj = C()
        result = freeze(obj)
        self.assertIs(result, obj)

    def test_returns_first_of_many(self):
        @freezable
        class C:
            pass
        a, b, c = C(), C(), C()
        result = freeze(a, b, c)
        self.assertIs(result, a)

    def test_frozen_decorator_returns_class(self):
        @frozen
        class C:
            pass
        self.assertIsInstance(C, type)
        self.assertTrue(is_frozen(C))


if __name__ == '__main__':
    unittest.main()
