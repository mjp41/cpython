import copy
import unittest
from regions import Region, is_local

try:
    import _testcapi
except ImportError:
    _testcapi = None


class A:
    """Plain, mutable, identity-hashable element class.

    Primed with freeze(A()) in setUpClass so the first live instance can enter a
    region without being auto-frozen.
    """
    pass


class TestRegionSlice(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        # Import here so a missing helper doesn't break collection of the class.
        from immutable import freeze
        freeze(A())

    def _sibling_elem(self):
        """Return (r2, elem) where elem is owned by sibling region r2."""
        r2 = Region()
        elem = A()
        r2.elem = elem
        return r2, elem

    # ---------------------------------------------------------------------- #
    # Group A — Transfer (construction moves local fields into a region)       #
    # ---------------------------------------------------------------------- #

    def test_construct_transfers_all_fields(self):
        r = Region()
        a1, a2, a3 = A(), A(), A()
        self.assertTrue(is_local(a1))

        r.s = slice(a1, a2, a3)

        for a in (a1, a2, a3):
            self.assertTrue(r.owns(a))
            self.assertFalse(is_local(a))

    def test_construct_single_arg_transfers_stop(self):
        # slice(x) stores x as .stop (start/step default to None).
        r = Region()
        stop = A()
        r.s = slice(stop)
        self.assertIs(r.s.stop, stop)
        self.assertTrue(r.owns(stop))

    # ---------------------------------------------------------------------- #
    # Group B — Returning-borrow (member getters start/stop/step)             #
    # ---------------------------------------------------------------------- #

    def test_start_getter_returns_borrow(self):
        r = Region()
        r.s = slice(A(), A(), A())
        base = r._lrc
        x = r.s.start
        self.assertEqual(r._lrc, base + 1)
        del x
        self.assertEqual(r._lrc, base)

    def test_stop_getter_returns_borrow(self):
        r = Region()
        r.s = slice(A(), A(), A())
        base = r._lrc
        x = r.s.stop
        self.assertEqual(r._lrc, base + 1)
        del x
        self.assertEqual(r._lrc, base)

    def test_step_getter_returns_borrow(self):
        r = Region()
        r.s = slice(A(), A(), A())
        base = r._lrc
        x = r.s.step
        self.assertEqual(r._lrc, base + 1)
        del x
        self.assertEqual(r._lrc, base)

    def test_immutable_field_getter_is_neutral(self):
        # Exact ints are immutable in-region: no borrow on read.
        r = Region()
        r.s = slice(1, 10, 2)
        base = r._lrc
        x = r.s.start
        self.assertEqual(r._lrc, base)
        del x
        self.assertEqual(r._lrc, base)

    # ---------------------------------------------------------------------- #
    # Group C — Neutral operations (repr, hash, richcompare)                  #
    # ---------------------------------------------------------------------- #

    def test_repr_is_neutral(self):
        r = Region()
        r.s = slice(A(), A(), A())
        base = r._lrc
        self.assertIsInstance(repr(r.s), str)
        self.assertEqual(r._lrc, base)

    def test_repr_elevates_lrc_during_call(self):
        # The self borrow is live while __repr__ of a field runs.
        class RecordLrc:
            def __init__(self, region):
                self.region = region
                self.seen = None
            def __repr__(self):
                self.seen = self.region._lrc
                return "RecordLrc"
        r = Region()
        probe = RecordLrc(r)
        r.s = slice(probe, A(), A())
        base = r._lrc
        repr(r.s)
        self.assertGreater(probe.seen, base)   # elevated during the call
        self.assertEqual(r._lrc, base)         # neutral afterwards

    def test_hash_is_neutral(self):
        r = Region()
        r.s = slice(A(), A(), A())
        base = r._lrc
        self.assertIsInstance(hash(r.s), int)
        self.assertEqual(r._lrc, base)

    def test_hash_matches_local(self):
        r = Region()
        r.s = slice(1, 10, 2)
        self.assertEqual(hash(r.s), hash(slice(1, 10, 2)))

    def test_richcompare_eq_is_neutral(self):
        r = Region()
        r.s = slice(A(), A(), A())
        base = r._lrc
        self.assertTrue(r.s == r.s)
        self.assertEqual(r._lrc, base)

    def test_richcompare_identity_shortcut(self):
        r = Region()
        r.s = slice(A(), A(), A())
        base = r._lrc
        self.assertTrue(r.s == r.s)
        self.assertTrue(r.s <= r.s)
        self.assertTrue(r.s >= r.s)
        self.assertFalse(r.s != r.s)
        self.assertEqual(r._lrc, base)

    def test_richcompare_ordering_parity(self):
        r = Region()
        r.s = slice(1, 5, 2)
        base = r._lrc
        self.assertEqual(r.s == slice(1, 5, 2), True)
        self.assertEqual(r.s < slice(2, 5, 2), True)
        self.assertEqual(r.s != slice(1, 6, 2), True)
        self.assertEqual(r._lrc, base)

    def test_richcompare_non_slice_is_not_equal(self):
        r = Region()
        r.s = slice(1, 5, 2)
        base = r._lrc
        self.assertFalse(r.s == 5)
        self.assertFalse(5 == r.s)
        self.assertTrue(r.s != 5)
        self.assertEqual(r._lrc, base)

    def test_richcompare_non_identity_with_mutable_fields_is_neutral(self):
        # Two *distinct* region-owned slices with mutable fields: this skips the
        # identity shortcut and goes through PyTuple_Pack(3, start, stop, step),
        # which runs PyRegion_AddRef per field. Assert the borrows are balanced.
        r = Region()
        a1, a2 = A(), A()
        r.s1 = slice(a1, a2, None)
        r.s2 = slice(a1, a2, None)   # equal by fields, but a different object
        base = r._lrc
        self.assertTrue(r.s1 == r.s2)
        self.assertFalse(r.s1 != r.s2)
        self.assertEqual(r._lrc, base)

    # ---------------------------------------------------------------------- #
    # Group D — indices (correctness + neutrality)                            #
    # ---------------------------------------------------------------------- #

    def test_indices_is_neutral(self):
        r = Region()
        r.s = slice(1, 10, 2)
        base = r._lrc
        self.assertEqual(r.s.indices(10), (1, 10, 2))
        self.assertEqual(r._lrc, base)

    def test_indices_parity_with_local_slice(self):
        cases = [
            (slice(1, 10, 2), 10),
            (slice(None, None, None), 7),
            (slice(10, 1, -1), 20),
            (slice(-3, None, None), 10),
            (slice(None, -2, None), 10),
        ]
        for sl, n in cases:
            with self.subTest(sl=sl, n=n):
                r = Region()
                r.s = sl
                self.assertEqual(r.s.indices(n), sl.indices(n))

    def test_indices_huge_length_preserves_precision(self):
        # Regression guard for _PySlice_GetLongIndices: length must not be
        # truncated through Py_ssize_t.
        r = Region()
        r.s = slice(1, None, 2)
        self.assertEqual(r.s.indices(2 ** 100), slice(1, None, 2).indices(2 ** 100))

    def test_indices_negative_length_raises_and_is_retryable(self):
        r = Region()
        r.s = slice(1, 10, 2)
        base = r._lrc
        with self.assertRaises(ValueError):
            r.s.indices(-1)
        # The failed call must not leave the slice/region unusable.
        self.assertEqual(r.s.indices(10), (1, 10, 2))
        self.assertEqual(r._lrc, base)

    # ---------------------------------------------------------------------- #
    # Group E — __reduce__ (Py_BuildValue borrow tracking)                    #
    # ---------------------------------------------------------------------- #

    def test_reduce_borrows_mutable_fields(self):
        r = Region()
        a1, a2 = A(), A()
        r.s = slice(a1, a2, None)
        base = r._lrc
        red = r.s.__reduce__()             # (slice, (a1, a2, None)) -> holds a1,a2
        self.assertIs(red[1][0], a1)
        self.assertIs(red[1][1], a2)
        self.assertEqual(r._lrc, base + 2)  # returning-borrow: +1 per region field
        del red
        self.assertEqual(r._lrc, base)      # released -> baseline (no underflow)

    def test_reduce_all_three_fields_borrowed(self):
        r = Region()
        r.s = slice(A(), A(), A())
        base = r._lrc
        red = r.s.__reduce__()
        self.assertEqual(r._lrc, base + 3)
        del red
        self.assertEqual(r._lrc, base)

    def test_reduce_immutable_fields_are_neutral(self):
        r = Region()
        r.s = slice(1, 10, 2)
        base = r._lrc
        red = r.s.__reduce__()
        self.assertEqual(r._lrc, base)
        del red
        self.assertEqual(r._lrc, base)

    def test_reduce_repeated_does_not_corrupt_lrc(self):
        r = Region()
        r.s = slice(A(), A(), None)
        base = r._lrc
        for _ in range(10):
            red = r.s.__reduce__()
            del red
        self.assertEqual(r._lrc, base)

    def test_reduce_none_fields_neutral(self):
        # All-None slice: the reduce payload holds only immutable singletons.
        r = Region()
        r.s = slice(None, None, None)
        base = r._lrc
        red = r.s.__reduce__()
        self.assertEqual(red[1], (None, None, None))
        self.assertEqual(r._lrc, base)
        del red
        self.assertEqual(r._lrc, base)

    def test_reduce_reconstructs_equal_slice(self):
        s = slice(1, 10, 2)
        cls, args = s.__reduce__()
        self.assertEqual(cls(*args), s)
        self.assertEqual(copy.copy(s), s)
        self.assertEqual(copy.deepcopy(s), s)

    # ---------------------------------------------------------------------- #
    # Group F — Dealloc (RemoveRef per field balances construction)           #
    # ---------------------------------------------------------------------- #

    def test_construct_drop_roundtrip_is_neutral(self):
        r = Region()
        r.keep = []          # anchor so the region is not collected
        base = r._lrc
        for _ in range(50):
            r.s = slice(A(), A(), A())
            r.s = None
        self.assertEqual(r._lrc, base)

    def test_replacing_slice_field_does_not_leak(self):
        r = Region()
        r.s = slice(A(), A(), A())
        base = r._lrc
        r.s = slice(A(), A(), A())   # old slice + its fields dropped, new moved in
        self.assertEqual(r._lrc, base)

    # ---------------------------------------------------------------------- #
    # Group G — Isolation (sibling-region element rejected on move-in)        #
    # ---------------------------------------------------------------------- #

    def test_move_in_rejects_sibling_region_field(self):
        r1 = Region()
        r2, elem = self._sibling_elem()
        base = r1._lrc

        with self.assertRaises(RuntimeError):
            r1.s = slice(elem)

        self.assertFalse(hasattr(r1, "s"))   # target unchanged (no partial state)
        self.assertTrue(r2.owns(elem))       # ownership unchanged
        self.assertEqual(r1._lrc, base)      # LRC neutral (temp borrow rolled back)

    def test_move_in_rejects_sibling_among_other_fields(self):
        # sibling element in the middle: whole move must roll back.
        r1 = Region()
        good1, good2 = A(), A()
        r2, bad = self._sibling_elem()
        base = r1._lrc

        with self.assertRaises(RuntimeError):
            r1.s = slice(good1, bad, good2)

        self.assertFalse(hasattr(r1, "s"))
        self.assertTrue(is_local(good1))     # not transferred
        self.assertTrue(is_local(good2))     # not transferred
        self.assertTrue(r2.owns(bad))
        self.assertEqual(r1._lrc, base)

    # ---------------------------------------------------------------------- #
    # Group H — Construction helpers via slicing syntax (behaviour + no leak) #
    #                                                                          #
    # These are BEHAVIOUR tests: slice syntax always produces integer/None     #
    # start/stop/step (immutable), so the _PyBuildSlice_* barriers are no-ops  #
    # here. They assert correctness and that construction leaks no borrows,    #
    # not that the barrier fires (the reduce/getter groups cover that).        #
    # ---------------------------------------------------------------------- #

    def test_extended_slice_syntax_on_region_object(self):
        r = Region()
        r.lst = [0, 1, 2, 3, 4, 5]
        base = r._lrc
        self.assertEqual(r.lst[1:5:2], [1, 3])
        self.assertEqual(r.lst[::-1], [5, 4, 3, 2, 1, 0])
        self.assertEqual(r._lrc, base)   # subscript slices leak no borrows

    def test_range_slicing_uses_getlongindices(self):
        # range slicing routes through _PySlice_GetLongIndices.
        r = Region()
        r.rng = range(2 ** 100)
        base = r._lrc
        self.assertEqual(r.rng[1:5], range(1, 5))
        self.assertEqual(r._lrc, base)


@unittest.skipUnless(_testcapi, "requires _testcapi")
class TestRegionPyBuildValue(unittest.TestCase):
    """Direct regression tests for the Py_BuildValue container barriers.

    slice.__reduce__ only exercises the tuple path (do_mktuple). These hit both
    do_mktuple and do_mklist in Python/modsupport.c at the C level: storing a
    region-owned object into a Py_BuildValue-built tuple/list must register a
    borrow (PyRegion_AddRef), otherwise the container's dealloc RemoveRef drives
    the region LRC below its true value.
    """

    @classmethod
    def setUpClass(cls):
        from immutable import freeze
        freeze(A())

    def _check(self, builder, expected_type):
        r = Region()
        a1, a2 = A(), A()
        r.a1 = a1
        r.a2 = a2                          # both region-owned, mutable
        base = r._lrc
        obj = builder(a1, a2, None)        # third element is immutable (None)
        self.assertIsInstance(obj, expected_type)
        self.assertIs(obj[0], a1)
        self.assertIs(obj[1], a2)
        # returning-borrow: +1 per region-owned element while the result lives
        self.assertEqual(r._lrc, base + 2)
        del obj
        # released: borrows removed, back to baseline (no underflow)
        self.assertEqual(r._lrc, base)

    def test_do_mktuple_borrows_region_elements(self):
        self._check(_testcapi.pybuildvalue_tuple, tuple)

    def test_do_mklist_borrows_region_elements(self):
        self._check(_testcapi.pybuildvalue_list, list)

    def test_do_mktuple_immutable_elements_neutral(self):
        r = Region()
        r.s = slice(1, 2, 3)               # anchor so region has data
        base = r._lrc
        obj = _testcapi.pybuildvalue_tuple(1, 2, None)
        self.assertEqual(r._lrc, base)
        del obj
        self.assertEqual(r._lrc, base)

    def test_ellipsis_into_region_is_neutral(self):
        r = Region()
        r.e = ...
        base = r._lrc
        self.assertIs(r.e, Ellipsis)
        self.assertEqual(repr(r.e), "Ellipsis")
        self.assertEqual(r.e.__reduce__(), "Ellipsis")
        self.assertEqual(r._lrc, base)


if __name__ == "__main__":
    unittest.main()
