import gc
import unittest
from regions import Region, is_local
from immutable import freeze


class A:
    """Plain, mutable element class (region-movable), primed in setUpClass."""
    pass


class Seq:
    """Sequence with __getitem__/__len__ but NO __reversed__ -> PyReversed_Type."""
    def __init__(self, items):
        self.items = items

    def __getitem__(self, i):
        if i >= len(self.items):
            raise IndexError(i)
        return self.items[i]

    def __len__(self):
        return len(self.items)


class WithReversed:
    """Exposes __reversed__ so reversed() delegates instead of building PyReversed_Type."""
    def __init__(self, items):
        self.items = items

    def __reversed__(self):
        return iter(self.items[::-1])


class TestRegionReversed(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        # Prime each user type so its type object is frozen up front and the
        # first live instance can move into a region without auto-freezing the
        # type underneath the move.
        freeze(A())
        freeze(Seq([]))
        freeze(WithReversed([]))

    # ---------------------------------------------------------------------- #
    # Group A — Construction / transfer / delegation                          #
    # ---------------------------------------------------------------------- #

    def test_reversed_transfers_into_region(self):
        """Assigning a reversed iterator into a region transfers ownership."""
        r = Region()
        it = reversed(Seq([A()]))
        self.assertEqual(type(it).__name__, "reversed")
        self.assertTrue(is_local(it))

        r.it = it

        self.assertTrue(r.owns(it))
        self.assertFalse(is_local(it))

    def test_reversed_transfers_reachable_items(self):
        """Transfer is transitive: the sequence and its items move into the region."""
        a, b = A(), A()
        r = Region()

        r.it = reversed(Seq([a, b]))

        self.assertTrue(r.owns(a))
        self.assertTrue(r.owns(b))

    def test_reversed_delegates_to_dunder(self):
        """A __reversed__ method is looked up and delegated to (not PyReversed_Type)."""
        r = Region()
        r.wr = WithReversed([A(), A()])

        it = reversed(r.wr)

        self.assertNotEqual(type(it).__name__, "reversed")
        self.assertEqual(len(list(it)), 2)

    def test_reversed_none_dunder_raises(self):
        """__reversed__ = None marks the object non-reversible -> TypeError."""
        class NotReversible:
            __reversed__ = None
        with self.assertRaises(TypeError):
            reversed(NotReversible())

    def test_reversed_non_sequence_raises(self):
        """A non-sequence object is not reversible -> TypeError."""
        class NotASequence:
            pass
        with self.assertRaises(TypeError):
            reversed(NotASequence())

    def test_reversed_no_args_raises(self):
        with self.assertRaises(TypeError):
            reversed()

    def test_reversed_too_many_args_raises(self):
        with self.assertRaises(TypeError):
            reversed(Seq([]), Seq([]))

    # ---------------------------------------------------------------------- #
    # Group B — Iteration (reversed_next)                                     #
    # ---------------------------------------------------------------------- #

    def test_next_returns_item_borrow(self):
        """Each next() yields a borrow of the item; LRC rises while it is held."""
        r = Region()
        r.seq = Seq([A(), A()])
        it = reversed(r.seq)          # local iterator; it->seq borrows r.seq
        base = r._lrc

        item = next(it)
        self.assertEqual(r._lrc, base + 1)   # borrow of the yielded item
        item = None
        self.assertEqual(r._lrc, base)

    def test_exhaustion_removes_seq_borrow(self):
        """Exhaustion releases ro->seq via PyRegion_CLEAR (RemoveRef).

        Regression guard: a local reversed iterator over a region-owned
        sequence holds a borrow of that sequence.  Exhausting it must release
        the borrow.  With the earlier plain-Py_CLEAR the RemoveRef was skipped,
        so the region's LRC leaked permanently (reversed_dealloc then saw an
        already-NULL seq and could not remove it).
        """
        r = Region()
        r.seq = Seq([A(), A()])
        base = r._lrc
        it = reversed(r.seq)
        self.assertEqual(r._lrc, base + 1)   # it->seq borrow recorded

        result = list(it)                     # exhaust; result holds item borrows
        result = None                         # drop those item borrows

        # Exhaustion already released the seq borrow, even though `it` is alive.
        self.assertEqual(r._lrc, base)

        it = None
        self.assertEqual(r._lrc, base)

    def test_early_abandon_removes_seq_borrow_in_dealloc(self):
        """Dropping a non-exhausted reversed iterator releases its seq borrow."""
        r = Region()
        r.seq = Seq([A(), A(), A()])
        base = r._lrc
        it = reversed(r.seq)
        self.assertEqual(r._lrc, base + 1)

        item = next(it)      # partially consumed
        item = None
        self.assertEqual(r._lrc, base + 1)   # seq borrow still held (not exhausted)

        it = None            # reversed_dealloc -> PyRegion_CLEAR -> RemoveRef
        self.assertEqual(r._lrc, base)

    def test_reversed_in_region_iteration_is_neutral(self):
        """A region-owned reversed iterator (seq in the same region) stays neutral.

        Here ro and seq are co-contained, so the seq reference is intra-region
        (RemoveRef is a no-op); iteration must still be LRC-neutral once the
        yielded items are released.
        """
        r = Region()
        r.seq = Seq([A(), A()])
        r.it = reversed(r.seq)
        base = r._lrc

        result = list(r.it)
        result = None
        self.assertEqual(r._lrc, base)

    def test_reversed_order_is_correct(self):
        """reversed yields items back-to-front."""
        self.assertEqual(list(reversed(Seq([10, 20, 30]))), [30, 20, 10])

    def test_exhaustion_raises_stopiteration(self):
        """A fully consumed reversed iterator raises StopIteration."""
        it = reversed(Seq([1]))
        next(it)
        with self.assertRaises(StopIteration):
            next(it)

    # ---------------------------------------------------------------------- #
    # Group C — __length_hint__ (reversed_len)                                #
    # ---------------------------------------------------------------------- #

    def test_length_hint_decreases(self):
        """__length_hint__ reflects the remaining count and is LRC-neutral."""
        r = Region()
        r.seq = Seq([A(), A(), A()])
        it = reversed(r.seq)
        base = r._lrc

        self.assertEqual(it.__length_hint__(), 3)
        next(it)
        self.assertEqual(it.__length_hint__(), 2)
        self.assertEqual(r._lrc, base)   # reads a size + returns an int; no borrow

    def test_length_hint_exhausted_is_zero(self):
        it = reversed(Seq([1, 2]))
        list(it)
        self.assertEqual(it.__length_hint__(), 0)

    # ---------------------------------------------------------------------- #
    # Group D — __reduce__ (reversed_reduce)                                  #
    # ---------------------------------------------------------------------- #

    def test_reduce_active_structure(self):
        """__reduce__ carries (reversed, (seq,), index) while active."""
        seq = Seq([10, 20, 30])
        it = reversed(seq)
        state = it.__reduce__()
        self.assertIs(state[0], reversed)
        self.assertIs(state[1][0], seq)
        self.assertEqual(state[2], 2)     # index == n-1

    def test_reduce_exhausted_structure(self):
        """Once exhausted, __reduce__ drops the index and empties the args."""
        it = reversed(Seq([1]))
        list(it)
        state = it.__reduce__()
        self.assertIs(state[0], reversed)
        self.assertEqual(state[1], ((),))

    def test_reduce_borrows_seq(self):
        """__reduce__ borrows ro->seq into its result; LRC rises while held."""
        r = Region()
        r.seq = Seq([A()])
        it = reversed(r.seq)
        base = r._lrc

        state = it.__reduce__()
        self.assertEqual(r._lrc, base + 1)   # seq borrowed into the reduce tuple

        state = None
        self.assertEqual(r._lrc, base)

    # ---------------------------------------------------------------------- #
    # Group E — __setstate__ (reversed_setstate)                              #
    # ---------------------------------------------------------------------- #

    def test_setstate_sets_index(self):
        """__setstate__ resumes iteration from the given index."""
        it = reversed(Seq([10, 20, 30]))
        it.__setstate__(1)
        self.assertEqual(list(it), [20, 10])

    def test_setstate_clamps_high(self):
        """An index above n-1 is clamped to n-1."""
        it = reversed(Seq([10, 20, 30]))
        it.__setstate__(100)
        self.assertEqual(list(it), [30, 20, 10])

    def test_setstate_clamps_low(self):
        """An index below -1 is clamped to -1 (exhausted)."""
        it = reversed(Seq([10, 20, 30]))
        it.__setstate__(-5)
        self.assertEqual(list(it), [])

    def test_setstate_is_lrc_neutral(self):
        """__setstate__ only touches the integer index; no borrows."""
        r = Region()
        r.seq = Seq([A(), A(), A()])
        it = reversed(r.seq)
        base = r._lrc

        it.__setstate__(1)
        self.assertEqual(r._lrc, base)

    # ---------------------------------------------------------------------- #
    # Group F — Other slots                                                   #
    # ---------------------------------------------------------------------- #

    def test_self_iteration_identity(self):
        """iter(reversed) returns the same object (PyObject_SelfIter)."""
        r = Region()
        r.it = reversed(Seq([A()]))
        self.assertIs(iter(r.it), r.it)

    def test_dealloc_region_reversed_no_crash(self):
        """Dropping a region that owns a reversed iterator tears down cleanly."""
        r = Region()
        r.it = reversed(Seq([A(), A()]))
        next(r.it)
        r = None
        gc.collect()

    def test_gc_traverse_no_crash(self):
        """A region-owned reversed iterator survives a gc pass (reversed_traverse)."""
        elem = A()
        r = Region()
        r.it = reversed(Seq([elem]))
        gc.collect()
        self.assertIs(next(r.it), elem)


if __name__ == "__main__":
    unittest.main()
