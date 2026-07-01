import gc
import unittest
from regions import Region, is_local
from immutable import freeze, is_frozen

BIG = 2 ** 63   # forces the longrange_iterator / long-index paths


class TestRegionRange(unittest.TestCase):

    # ---------------------------------------------------------------------- #
    # Group A — the range object: frozen on region entry, read-only ops       #
    # ---------------------------------------------------------------------- #

    def test_range_is_frozen_on_region_entry(self):
        """A range holds only exact ints -> it is deeply immutable and freezes."""
        r = Region()
        rng = range(10)
        self.assertTrue(is_local(rng))

        r.rng = rng

        self.assertTrue(is_frozen(rng))     # frozen, not contained
        self.assertFalse(is_local(rng))
        self.assertFalse(r.owns(rng))       # immutable objects are not owned

    def test_frozen_range_attributes(self):
        """start/stop/step remain readable after the range is frozen."""
        r = Region()
        r.rng = range(2, 20, 3)
        self.assertEqual((r.rng.start, r.rng.stop, r.rng.step), (2, 20, 3))

    def test_len_and_bool(self):
        r = Region()
        r.rng = range(0, 20, 2)
        base = r._lrc
        self.assertEqual(len(r.rng), 10)
        self.assertTrue(bool(r.rng))
        self.assertFalse(bool(range(0)))
        self.assertEqual(r._lrc, base)

    def test_repr(self):
        r = Region()
        r.rng = range(1, 10, 2)
        self.assertEqual(repr(r.rng), "range(1, 10, 2)")
        r.rng2 = range(0, 5)
        self.assertEqual(repr(r.rng2), "range(0, 5)")

    def test_subscript_index(self):
        """range_item / compute_range_item on a region range."""
        r = Region()
        r.rng = range(0, 20, 2)
        base = r._lrc
        self.assertEqual(r.rng[3], 6)
        self.assertEqual(r.rng[-1], 18)
        self.assertEqual(r._lrc, base)
        with self.assertRaises(IndexError):
            r.rng[100]

    def test_subscript_slice(self):
        """compute_slice on a region range returns a new (also frozen) range."""
        r = Region()
        r.rng = range(0, 20, 2)
        base = r._lrc
        sub = r.rng[1:4]
        self.assertEqual(list(sub), [2, 4, 6])
        self.assertEqual(r._lrc, base)

    def test_contains(self):
        r = Region()
        r.rng = range(0, 20, 2)
        base = r._lrc
        self.assertIn(10, r.rng)
        self.assertNotIn(11, r.rng)
        self.assertEqual(r._lrc, base)

    def test_index_and_count(self):
        r = Region()
        r.rng = range(0, 20, 2)
        base = r._lrc
        self.assertEqual(r.rng.index(10), 5)
        self.assertEqual(r.rng.count(10), 1)
        self.assertEqual(r.rng.count(11), 0)
        with self.assertRaises(ValueError):
            r.rng.index(11)
        self.assertEqual(r._lrc, base)

    def test_equality_and_hash(self):
        r = Region()
        r.rng = range(0, 10, 2)
        base = r._lrc
        self.assertEqual(r.rng, range(0, 10, 2))
        self.assertNotEqual(r.rng, range(0, 10, 3))
        self.assertEqual(hash(r.rng), hash(range(0, 10, 2)))
        self.assertEqual(r._lrc, base)

    def test_reduce(self):
        r = Region()
        r.rng = range(1, 10, 2)
        state = r.rng.__reduce__()
        self.assertIs(state[0], range)
        self.assertEqual(state[1], (1, 10, 2))

    def test_iteration_yields_fresh_ints_neutral(self):
        """Iterating a region range yields fresh local ints; LRC stays neutral."""
        r = Region()
        r.rng = range(3)
        base = r._lrc
        out = list(r.rng)
        self.assertEqual(out, [0, 1, 2])
        self.assertEqual(r._lrc, base)   # yielded ints are not borrows into r

    # ---------------------------------------------------------------------- #
    # Group B — range_iterator (fast, C long fields)                          #
    # ---------------------------------------------------------------------- #

    def test_fast_iter_owned_by_region(self):
        r = Region()
        it = iter(range(5))
        self.assertEqual(type(it).__name__, "range_iterator")
        r.it = it
        self.assertTrue(r.owns(it))         # mutable iterator -> contained

    def test_fast_iter_yields_correct_and_neutral(self):
        r = Region()
        r.it = iter(range(5))
        base = r._lrc
        self.assertEqual(list(r.it), [0, 1, 2, 3, 4])
        self.assertEqual(r._lrc, base)

    def test_fast_iter_length_hint(self):
        r = Region()
        r.it = iter(range(5))
        self.assertEqual(r.it.__length_hint__(), 5)
        next(r.it)
        self.assertEqual(r.it.__length_hint__(), 4)

    def test_fast_iter_reduce_and_setstate(self):
        r = Region()
        r.it = iter(range(5))
        next(r.it)
        state = r.it.__reduce__()
        self.assertEqual(state[1], (range(1, 5),))
        r.it.__setstate__(1)
        self.assertEqual(list(r.it), [2, 3, 4])

    def test_fast_iter_exhaustion(self):
        r = Region()
        r.it = iter(range(1))
        next(r.it)
        with self.assertRaises(StopIteration):
            next(r.it)

    # ---------------------------------------------------------------------- #
    # Group C — longrange_iterator (PyObject* exact-long fields)              #
    # ---------------------------------------------------------------------- #

    def test_long_iter_owned_by_region(self):
        r = Region()
        it = iter(range(BIG, BIG + 3))
        self.assertEqual(type(it).__name__, "longrange_iterator")
        r.it = it
        self.assertTrue(r.owns(it))

    def test_long_iter_yields_correct_and_neutral(self):
        """longrangeiter_next mutates start/len in place; iteration is neutral."""
        r = Region()
        r.it = iter(range(BIG, BIG + 3))
        base = r._lrc
        self.assertEqual(list(r.it), [BIG, BIG + 1, BIG + 2])
        self.assertEqual(r._lrc, base)

    def test_long_iter_length_hint(self):
        r = Region()
        r.it = iter(range(BIG, BIG + 5))
        self.assertEqual(r.it.__length_hint__(), 5)
        next(r.it)
        self.assertEqual(r.it.__length_hint__(), 4)

    def test_long_iter_reduce_and_setstate(self):
        r = Region()
        r.it = iter(range(BIG, BIG + 5))
        next(r.it)
        state = r.it.__reduce__()
        self.assertIs(state[0], iter)
        r.it.__setstate__(1)
        self.assertEqual(next(r.it), BIG + 2)

    def test_long_iter_exhaustion(self):
        r = Region()
        r.it = iter(range(BIG, BIG + 1))
        next(r.it)
        with self.assertRaises(StopIteration):
            next(r.it)

    # ---------------------------------------------------------------------- #
    # Group D — reversed(range) (range_reverse -> both iterator kinds)        #
    # ---------------------------------------------------------------------- #

    def test_reversed_small_range(self):
        r = Region()
        r.rng = range(3)
        it = reversed(r.rng)
        self.assertEqual(type(it).__name__, "range_iterator")
        self.assertEqual(list(it), [2, 1, 0])

    def test_reversed_large_range(self):
        r = Region()
        r.rng = range(BIG, BIG + 3)
        it = reversed(r.rng)
        self.assertEqual(type(it).__name__, "longrange_iterator")
        self.assertEqual(list(it), [BIG + 2, BIG + 1, BIG])

    # ---------------------------------------------------------------------- #
    # Group E — construction and argument errors                              #
    # ---------------------------------------------------------------------- #

    def test_construction_variants(self):
        self.assertEqual(list(range(3)), [0, 1, 2])
        self.assertEqual(list(range(2, 5)), [2, 3, 4])
        self.assertEqual(list(range(1, 6, 2)), [1, 3, 5])

    def test_zero_step_raises(self):
        with self.assertRaises(ValueError):
            range(0, 10, 0)

    def test_no_args_raises(self):
        with self.assertRaises(TypeError):
            range()

    def test_too_many_args_raises(self):
        with self.assertRaises(TypeError):
            range(0, 10, 2, 3)

    def test_non_integer_arg_raises(self):
        with self.assertRaises(TypeError):
            range(1.5)

    # ---------------------------------------------------------------------- #
    # Group F — dealloc                                                       #
    # ---------------------------------------------------------------------- #

    def test_dealloc_region_range_no_crash(self):
        r = Region()
        r.rng = range(10)
        r = None
        gc.collect()

    def test_dealloc_region_iterators_no_crash(self):
        r = Region()
        r.fast = iter(range(5))
        r.long = iter(range(BIG, BIG + 5))
        next(r.fast)
        next(r.long)
        r = None
        gc.collect()


if __name__ == "__main__":
    unittest.main()
