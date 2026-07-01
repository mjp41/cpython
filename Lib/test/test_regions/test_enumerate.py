import gc
import types
import unittest
from regions import Region, is_local
from immutable import freeze


class A:
    pass


class TestRegionEnumerate(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        freeze(A())

    # ---------------------------------------------------------------------- #
    # Group A — Construction / transfer                                       #
    # ---------------------------------------------------------------------- #

    def test_enumerate_transfers_into_region(self):
        """Assigning an enumerate into a region transfers ownership of it."""
        r = Region()
        e = enumerate([A()])
        self.assertTrue(is_local(e))

        r.e = e

        self.assertTrue(r.owns(e))
        self.assertFalse(is_local(e))

    def test_enumerate_transfers_reachable_items(self):
        """Transfer is transitive: the underlying items move into the region."""
        a, b = A(), A()
        r = Region()

        r.e = enumerate([a, b])

        # a and b are reachable through en_sit's list -> owned by the region.
        self.assertTrue(r.owns(a))
        self.assertTrue(r.owns(b))
        self.assertFalse(is_local(a))
        self.assertFalse(is_local(b))

    def test_positional_start(self):
        """enumerate(iterable, start) positional form (vectorcall) works."""
        r = Region()
        e = enumerate([A()], 5)
        r.e = e
        self.assertTrue(r.owns(e))
        self.assertEqual(next(r.e)[0], 5)

    def test_keyword_start_and_iterable(self):
        """enumerate(iterable=..., start=...) keyword form (vectorcall) works."""
        r = Region()
        e = enumerate(iterable=[A()], start=3)
        r.e = e
        self.assertTrue(r.owns(e))
        self.assertEqual(next(r.e)[0], 3)

    def test_keyword_start_before_iterable(self):
        """enumerate(start=..., iterable=...) reversed kwarg order works."""
        r = Region()
        e = enumerate(start=7, iterable=[A()])
        r.e = e
        self.assertEqual(next(r.e)[0], 7)

    # ---------------------------------------------------------------------- #
    # Group B — Iteration returning-borrow (enum_next)                        #
    # ---------------------------------------------------------------------- #

    def test_next_returns_local_tuple_with_item_borrow(self):
        """next() yields a fresh *local* tuple; the item is a borrow (+1 LRC)."""
        r = Region()
        r.e = enumerate([A(), A()])
        base = r._lrc

        pair = next(r.e)
        self.assertTrue(is_local(pair))          # freshly-created result tuple
        self.assertEqual(pair[0], 0)             # index is an immortal small int
        # One extra borrow: the item held via the returned tuple.
        self.assertEqual(r._lrc, base + 1)

        pair = None
        self.assertEqual(r._lrc, base)

    def test_next_is_neutral_when_result_released(self):
        """Dropping each yielded pair returns the LRC to baseline (neutral)."""
        r = Region()
        r.e = enumerate([A(), A(), A()])
        base = r._lrc

        for _ in range(3):
            pair = next(r.e)
            self.assertEqual(r._lrc, base + 1)
            pair = None
            self.assertEqual(r._lrc, base)

    def test_list_consume_holds_borrow_per_item(self):
        """Consuming into a local list holds one borrow per region-owned item."""
        r = Region()
        r.e = enumerate([A(), A()])
        base = r._lrc

        result = list(r.e)
        self.assertEqual([p[0] for p in result], [0, 1])
        # Two items, each borrowed via the collected tuples.
        self.assertEqual(r._lrc, base + 2)

        result = None
        self.assertEqual(r._lrc, base)

    def test_exhaustion_raises_stopiteration(self):
        """Exhausting the enumerate raises StopIteration and stays neutral."""
        r = Region()
        r.e = enumerate([A()])
        base = r._lrc

        next(r.e)
        with self.assertRaises(StopIteration):
            next(r.e)
        self.assertEqual(r._lrc, base)

    def test_local_enumerate_over_region_list(self):
        """A local enumerate over a region-owned list borrows each yielded item.

        Here the enumerate object stays local while its items live in the
        region, so the per-yield borrow is observed on the *list's* region.
        """
        r = Region()
        r.lst = [A(), A()]
        e = enumerate(r.lst)          # en_sit borrows r.lst -> raises LRC
        base = r._lrc

        pair = next(e)
        self.assertEqual(r._lrc, base + 1)   # borrow of the yielded item
        pair = None
        self.assertEqual(r._lrc, base)

    def test_local_enumerate_all_local_is_correct(self):
        """Fully local enumerate exercises the recycle fast-path; values correct."""
        e = enumerate(["x", "y", "z"], start=1)
        self.assertEqual(list(e), [(1, "x"), (2, "y"), (3, "z")])

    # ---------------------------------------------------------------------- #
    # Group C — Long-index path (enum_next_long / increment_longindex)        #
    # ---------------------------------------------------------------------- #

    def test_longindex_construction_transfers(self):
        """enumerate(..., start=2**63) drives en_longindex and still transfers."""
        r = Region()
        e = enumerate([A()], start=2 ** 63)
        r.e = e
        self.assertTrue(r.owns(e))

    def test_longindex_iteration_yields_big_indices(self):
        """The long-index path yields consecutive big-int indices."""
        r = Region()
        r.e = enumerate([A(), A()], start=2 ** 63)

        indices = [p[0] for p in r.e]
        self.assertEqual(indices, [2 ** 63, 2 ** 63 + 1])

    def test_longindex_item_borrow_only(self):
        """On the long-index path only the item borrows; the big-int index does not.

        The index is an exact int, frozen on region entry, so it is immutable
        and untracked; the yielded pair therefore adds exactly one borrow.
        """
        r = Region()
        r.e = enumerate([A(), A()], start=2 ** 63)
        base = r._lrc

        pair = next(r.e)
        self.assertEqual(pair[0], 2 ** 63)
        self.assertEqual(r._lrc, base + 1)   # only the item, not the index

        pair = None
        self.assertEqual(r._lrc, base)

    def test_longindex_full_iteration_neutral(self):
        """Consuming the whole long-index enumerate returns the LRC to baseline."""
        r = Region()
        r.e = enumerate([A(), A(), A()], start=2 ** 63)
        base = r._lrc

        result = list(r.e)
        self.assertEqual(r._lrc, base + 3)   # three item borrows held
        result = None
        self.assertEqual(r._lrc, base)

    def test_longindex_local_is_correct(self):
        """Fully local long-index enumerate produces correct big indices."""
        e = enumerate(["a", "b"], start=2 ** 63)
        self.assertEqual(list(e), [(2 ** 63, "a"), (2 ** 63 + 1, "b")])

    # ---------------------------------------------------------------------- #
    # Group D — __reduce__ (enum_reduce)                                      #
    # ---------------------------------------------------------------------- #

    def test_reduce_structure_small_index(self):
        """__reduce__ returns (enumerate, (iterator, index)) for the small path."""
        r = Region()
        r.e = enumerate([A()], start=5)

        state = r.e.__reduce__()
        self.assertIs(state[0], enumerate)
        self.assertEqual(state[1][1], 5)

    def test_reduce_borrows_iterator(self):
        """__reduce__ borrows en_sit into its result; LRC rises while held."""
        r = Region()
        r.e = enumerate([A()], start=5)
        base = r._lrc

        state = r.e.__reduce__()
        # en_sit is borrowed into the reduce tuple; the small int index is not.
        self.assertEqual(r._lrc, base + 1)

        state = None
        self.assertEqual(r._lrc, base)

    def test_reduce_structure_long_index(self):
        """__reduce__ carries the big-int index directly on the long path."""
        r = Region()
        r.e = enumerate([A()], start=2 ** 63)

        state = r.e.__reduce__()
        self.assertIs(state[0], enumerate)
        self.assertEqual(state[1][1], 2 ** 63)

    def test_reduce_long_index_borrows_iterator_only(self):
        """On the long path the big-int index is frozen; only en_sit borrows."""
        r = Region()
        r.e = enumerate([A()], start=2 ** 63)
        base = r._lrc

        state = r.e.__reduce__()
        self.assertEqual(r._lrc, base + 1)   # en_sit only; en_longindex frozen

        state = None
        self.assertEqual(r._lrc, base)

    # ---------------------------------------------------------------------- #
    # Group E — Other slots                                                   #
    # ---------------------------------------------------------------------- #

    def test_class_getitem(self):
        """enumerate[...] returns a GenericAlias (tp_methods __class_getitem__)."""
        self.assertIsInstance(enumerate[int], types.GenericAlias)

    def test_self_iteration_identity(self):
        """iter(enumerate) returns the same object (PyObject_SelfIter)."""
        r = Region()
        r.e = enumerate([A()])
        self.assertIs(iter(r.e), r.e)

    def test_missing_iterable_raises(self):
        with self.assertRaises(TypeError):
            enumerate()

    def test_too_many_arguments_raises(self):
        with self.assertRaises(TypeError):
            enumerate([], 1, 2)

    def test_invalid_keyword_raises(self):
        with self.assertRaises(TypeError):
            enumerate([1], bad=2)

    # ---------------------------------------------------------------------- #
    # Group F — Dealloc / GC                                                  #
    # ---------------------------------------------------------------------- #

    def test_dealloc_region_enumerate_no_crash(self):
        """Dropping a region that owns an enumerate tears down cleanly."""
        r = Region()
        r.e = enumerate([A(), A()])
        next(r.e)
        r = None          # enum_dealloc -> PyRegion_CLEAR on en_sit/en_result
        gc.collect()

    def test_dealloc_longindex_enumerate_no_crash(self):
        """Same, after driving the long-index path (en_longindex populated)."""
        r = Region()
        r.e = enumerate([A(), A()], start=2 ** 63)
        next(r.e)
        r = None
        gc.collect()

    def test_gc_traverse_no_crash(self):
        """A region-owned enumerate survives a gc pass (enum_traverse)."""
        r = Region()
        r.e = enumerate([A()])
        gc.collect()
        self.assertEqual(next(r.e)[0], 0)


if __name__ == "__main__":
    unittest.main()
