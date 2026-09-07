import sys
import unittest
from immutable import freeze, is_frozen


class TestImplicitImmutability(unittest.TestCase):
    """Tests for objects that can be viewed as immutable."""

    def test_tuple_of_immortal_ints(self):
        """A tuple of small ints (immortal) can be viewed as immutable."""
        obj = (1, 2, 3)
        self.assertTrue(is_frozen(obj))

    def test_tuple_of_strings(self):
        """A tuple of interned strings can be viewed as immutable."""
        obj = ("hello", "world")
        self.assertTrue(is_frozen(obj))

    def test_tuple_of_none(self):
        """A tuple containing None can be viewed as immutable."""
        obj = (None, None)
        self.assertTrue(is_frozen(obj))

    def test_nested_tuples(self):
        """Nested tuples of immortal objects can be viewed as immutable."""
        obj = ((1, 2), (3, (4, 5)))
        self.assertTrue(is_frozen(obj))

    def test_tuple_with_mutable_list(self):
        """A tuple containing a mutable list cannot be viewed as immutable."""
        obj = (1, [2, 3])
        self.assertFalse(is_frozen(obj))

    def test_tuple_with_mutable_dict(self):
        """A tuple containing a mutable dict cannot be viewed as immutable."""
        obj = (1, {"a": 2})
        self.assertFalse(is_frozen(obj))

    def test_frozenset_of_ints(self):
        """A frozenset of ints can be viewed as immutable."""
        obj = frozenset([1, 2, 3])
        self.assertTrue(is_frozen(obj))

    def test_empty_tuple(self):
        """An empty tuple can be viewed as immutable."""
        obj = ()
        self.assertTrue(is_frozen(obj))

    def test_empty_frozenset(self):
        """An empty frozenset can be viewed as immutable."""
        obj = frozenset()
        self.assertTrue(is_frozen(obj))

    def test_already_frozen_object(self):
        """An already-frozen object should return True."""
        obj = [1, 2, 3]
        freeze(obj)
        self.assertTrue(is_frozen(obj))

    def test_tuple_containing_frozen_object(self):
        """A tuple containing a frozen list can be viewed as immutable."""
        inner = [1, 2, 3]
        freeze(inner)
        obj = (inner, 4, 5)
        self.assertTrue(is_frozen(obj))

    def test_mutable_list(self):
        """A plain mutable list cannot be viewed as immutable."""
        obj = [1, 2, 3]
        self.assertFalse(is_frozen(obj))

    def test_mutable_dict(self):
        """A plain mutable dict cannot be viewed as immutable."""
        obj = {"a": 1}
        self.assertFalse(is_frozen(obj))

    def test_tuple_with_bytes(self):
        """A tuple containing bytes can be viewed as immutable."""
        obj = (b"hello", b"world")
        self.assertTrue(is_frozen(obj))

    def test_tuple_with_float(self):
        """A tuple with floats can be viewed as immutable."""
        obj = (1.0, 2.5, 3.14)
        self.assertTrue(is_frozen(obj))

    def test_tuple_with_complex(self):
        """A tuple with complex numbers can be viewed as immutable."""
        obj = (1+2j, 3+4j)
        self.assertTrue(is_frozen(obj))

    def test_tuple_with_bool(self):
        """A tuple with booleans can be viewed as immutable."""
        obj = (True, False)
        self.assertTrue(is_frozen(obj))

    def test_is_frozen_freezes_non_immortal(self):
        """A graph that can be viewed as immutable gets frozen by is_frozen."""
        big = 10**100
        obj = (big,)
        result = is_frozen(obj)
        self.assertTrue(result)
        self.assertTrue(is_frozen(obj))

    def test_tuple_of_range(self):
        """A tuple containing a range object can be viewed as immutable."""
        obj = (range(10),)
        self.assertTrue(is_frozen(obj))

    def test_deeply_nested(self):
        """Deeply nested tuples can be viewed as immutable."""
        obj = (1,)
        for _ in range(100):
            obj = (obj,)
        self.assertTrue(is_frozen(obj))

    def test_tuple_with_set(self):
        """A tuple containing a mutable set cannot be viewed as immutable."""
        obj = (1, {2, 3})
        self.assertFalse(is_frozen(obj))

    def test_c_shallow_immutable_type(self):
        """A C type registered as shallow immutable can be viewed as immutable."""
        import _test_reachable
        freeze(_test_reachable.ShallowImmutable)
        obj = (_test_reachable.ShallowImmutable(1),)
        self.assertTrue(is_frozen(obj))

    def test_c_shallow_immutable_with_mutable_referent(self):
        """A C shallow immutable containing a mutable object cannot be viewed as immutable."""
        import _test_reachable
        freeze(_test_reachable.ShallowImmutable)
        obj = (_test_reachable.ShallowImmutable([1, 2]),)
        self.assertFalse(is_frozen(obj))

    def test_deeply_nested_no_stack_overflow(self):
        """Very deep nesting should not cause a stack overflow."""
        obj = (1,)
        for _ in range(10000):
            obj = (obj,)
        self.assertTrue(is_frozen(obj))

    def test_abandoned_walk_keeps_references(self):
        """An aborted walk must not drop references it never took.

        The walk pushes objects onto a worklist without increfing them, so
        anything still on the worklist when a mutable object aborts the walk
        used to be decrefed when the worklist was released. That freed the
        object while its real owners were still pointing at it, which showed
        up much later as a negative refcount.
        """
        # Built at runtime so it is neither interned nor immortal, which makes
        # its reference count fully accounted for by this test.
        item = "".join(["abandoned", "-", "worklist", "-", "entry"])
        # Tuples are traversed back to front, so `item` reaches the worklist
        # before the dict aborts the walk.
        obj = ({"mutable": 1}, item)

        before = sys.getrefcount(item)
        self.assertFalse(is_frozen(obj))
        self.assertEqual(sys.getrefcount(item), before)


if __name__ == '__main__':
    unittest.main()
