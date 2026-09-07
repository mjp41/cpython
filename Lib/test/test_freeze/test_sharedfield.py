import os
import unittest
from immutable import freeze, is_frozen, SharedField, set_freezable, FREEZABLE_NO
from test.support import import_helper


class TestSharedFieldBasic(unittest.TestCase):
    """Test basic SharedField with frozen values."""

    def test_get_returns_initial(self):
        sf = SharedField(42)
        self.assertEqual(sf.get(), 42)

    def test_set_frozen_value(self):
        sf = SharedField(42)
        sf.set(99)
        self.assertEqual(sf.get(), 99)

    def test_set_none(self):
        sf = SharedField(42)
        sf.set(None)
        self.assertIsNone(sf.get())

    def test_set_frozen_tuple(self):
        sf = SharedField(())
        sf.set((1, 2, 3))
        self.assertEqual(sf.get(), (1, 2, 3))

    def test_set_rejects_mutable(self):
        sf = SharedField(42)
        with self.assertRaises(TypeError):
            sf.set([1, 2, 3])

    def test_initial_value_is_frozen(self):
        sf = SharedField(42)
        self.assertTrue(is_frozen(sf.get()))

    def test_get_consistent(self):
        sf = SharedField("hello")
        self.assertIs(sf.get(), sf.get())


class TestSharedFieldSwap(unittest.TestCase):
    """Test the unconditional swap operation."""

    def test_swap_returns_old_value(self):
        sf = SharedField(42)
        old = sf.swap(99)
        self.assertEqual(old, 42)
        self.assertEqual(sf.get(), 99)

    def test_swap_rejects_mutable(self):
        sf = SharedField(42)
        with self.assertRaises(TypeError):
            sf.swap([])


class TestSharedFieldCompareAndSwap(unittest.TestCase):
    """Test the atomic compare-and-swap operation."""

    def test_cas_succeeds(self):
        sf = SharedField(42)
        old = sf.get()
        result = sf.compare_and_swap(old, 99)
        self.assertTrue(result)
        self.assertEqual(sf.get(), 99)

    def test_cas_fails_wrong_expected(self):
        sf = SharedField(42)
        result = sf.compare_and_swap(0, 99)
        self.assertFalse(result)
        self.assertEqual(sf.get(), 42)

    def test_cas_rejects_mutable_new(self):
        sf = SharedField(42)
        with self.assertRaises(TypeError):
            sf.compare_and_swap(42, [])

    def test_cas_wrong_arg_count(self):
        sf = SharedField(42)
        with self.assertRaises(TypeError):
            sf.compare_and_swap(42)
        with self.assertRaises(TypeError):
            sf.compare_and_swap(1, 2, 3)


class TestSharedFieldFreeze(unittest.TestCase):
    """Test SharedField within frozen object graphs."""

    def test_freeze_object_with_sharedfield(self):
        class Container:
            pass

        c = Container()
        c.field = SharedField(42)
        freeze(c)
        self.assertTrue(is_frozen(c))

    def test_get_after_freeze(self):
        class Container:
            pass

        c = Container()
        c.field = SharedField(42)
        freeze(c)
        self.assertEqual(c.field.get(), 42)

    def test_set_after_freeze(self):
        class Container:
            pass

        c = Container()
        c.field = SharedField(42)
        freeze(c)
        c.field.set(99)
        self.assertEqual(c.field.get(), 99)

    def test_swap_after_freeze(self):
        class Container:
            pass

        c = Container()
        c.field = SharedField(42)
        freeze(c)
        old = c.field.swap(99)
        self.assertEqual(old, 42)
        self.assertEqual(c.field.get(), 99)

    def test_compare_and_swap_after_freeze(self):
        class Container:
            pass

        c = Container()
        c.field = SharedField(42)
        freeze(c)
        old = c.field.get()
        self.assertTrue(c.field.compare_and_swap(old, 99))
        self.assertEqual(c.field.get(), 99)

    def test_sharedfield_itself_frozen(self):
        sf = SharedField(42)
        freeze(sf)
        self.assertTrue(is_frozen(sf))

    def test_set_rejects_mutable_after_freeze(self):
        class Container:
            pass

        c = Container()
        c.field = SharedField(42)
        freeze(c)
        with self.assertRaises(TypeError):
            c.field.set([1, 2, 3])


class TestSharedFieldErrors(unittest.TestCase):
    """Test error cases."""

    def test_no_args(self):
        with self.assertRaises(TypeError):
            SharedField()

    def test_non_freezable_initial(self):
        class NF:
            pass
        obj = NF()
        set_freezable(obj, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            SharedField(obj)

    def test_multiple_independent_fields(self):
        f1 = SharedField(1)
        f2 = SharedField(2)
        self.assertEqual(f1.get(), 1)
        self.assertEqual(f2.get(), 2)
        f1.set(10)
        self.assertEqual(f1.get(), 10)
        self.assertEqual(f2.get(), 2)


class TestSharedFieldSubinterpreters(unittest.TestCase):
    """Test that SharedField is shared across sub-interpreters."""

    def setUp(self):
        self._interpreters = import_helper.import_module('_interpreters')

    def _run_in_subinterp(self, code, shared=None):
        r, w = os.pipe()
        wrapped = (
            "import contextlib, os\n"
            f"with open({w}, 'w', encoding='utf-8') as spipe:\n"
            "    with contextlib.redirect_stdout(spipe):\n"
        )
        for line in code.splitlines():
            wrapped += "        " + line + "\n"

        interp = self._interpreters.create()
        try:
            self._interpreters.run_string(
                interp, wrapped, shared=shared or {})
        finally:
            with os.fdopen(r, encoding='utf-8') as rpipe:
                result = rpipe.read()
            self._interpreters.destroy(interp)
        return result

    def test_shared_field_readable_in_subinterp(self):
        """A frozen SharedField shared to a sub-interpreter should
        be readable there."""
        sf = SharedField(42)
        freeze(sf)

        output = self._run_in_subinterp(
            "print(sf.get())\n",
            shared={"sf": sf},
        )
        self.assertEqual(output.strip(), "42")

    def test_shared_field_set_visible_across_interps(self):
        """Setting a SharedField in a sub-interpreter should be
        visible in the main interpreter (shared state)."""
        sf = SharedField(0)
        freeze(sf)

        self._run_in_subinterp(
            "sf.set(42)\n",
            shared={"sf": sf},
        )
        # Unlike InterpreterLocal, SharedField is shared — change is visible
        self.assertEqual(sf.get(), 42)

    def test_shared_frozen_container_with_sharedfield(self):
        """A frozen container with a SharedField should share
        state across interpreters."""
        class Container:
            pass

        c = Container()
        c.counter = SharedField(0)
        freeze(c)

        self._run_in_subinterp(
            "c.counter.set(99)\n",
            shared={"c": c},
        )
        self.assertEqual(c.counter.get(), 99)


if __name__ == "__main__":
    unittest.main()
