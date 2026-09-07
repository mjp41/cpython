import os
import unittest
from immutable import freeze, is_frozen, InterpreterLocal
from test.support import import_helper


class TestInterpreterLocalBasic(unittest.TestCase):
    """Test basic InterpreterLocal with immutable default value."""

    def test_get_returns_default(self):
        field = InterpreterLocal(42)
        self.assertEqual(field.get(), 42)

    def test_set(self):
        field = InterpreterLocal(42)
        field.set(99)
        self.assertEqual(field.get(), 99)

    def test_get_consistent(self):
        field = InterpreterLocal("hello")
        self.assertIs(field.get(), field.get())

    def test_none_default(self):
        field = InterpreterLocal(None)
        self.assertIsNone(field.get())

    def test_tuple_default(self):
        t = (1, 2, 3)
        field = InterpreterLocal(t)
        self.assertEqual(field.get(), (1, 2, 3))


class TestInterpreterLocalFactory(unittest.TestCase):
    """Test InterpreterLocal with factory callable."""

    def test_factory_returns_new_value(self):
        field = InterpreterLocal(lambda: [])
        result = field.get()
        self.assertIsInstance(result, list)
        self.assertEqual(result, [])

    def test_factory_called_once(self):
        field = InterpreterLocal(lambda: [])
        first = field.get()
        second = field.get()
        self.assertIs(first, second)

    def test_factory_value_is_mutable(self):
        field = InterpreterLocal(lambda: [])
        field.get().append(1)
        self.assertEqual(field.get(), [1])

    def test_factory_set_overrides(self):
        field = InterpreterLocal(lambda: [])
        field.get()  # initialise
        field.set([1, 2, 3])
        self.assertEqual(field.get(), [1, 2, 3])


class TestInterpreterLocalFreeze(unittest.TestCase):
    """Test that InterpreterLocal works within frozen object graphs."""

    def test_freeze_object_with_interpreterlocal(self):
        class Container:
            pass

        c = Container()
        c.field = InterpreterLocal(42)
        freeze(c)
        self.assertTrue(is_frozen(c))

    def test_value_accessible_after_freeze(self):
        class Container:
            pass

        c = Container()
        c.field = InterpreterLocal(42)
        freeze(c)
        self.assertEqual(c.field.get(), 42)

    def test_value_mutable_after_freeze(self):
        class Container:
            pass

        c = Container()
        c.field = InterpreterLocal(42)
        freeze(c)
        c.field.set(99)
        self.assertEqual(c.field.get(), 99)

    def test_factory_works_after_freeze(self):
        class Container:
            pass

        c = Container()
        c.field = InterpreterLocal(lambda: {})
        freeze(c)
        result = c.field.get()
        self.assertIsInstance(result, dict)

    def test_interpreterlocal_itself_frozen(self):
        field = InterpreterLocal(42)
        freeze(field)
        self.assertTrue(is_frozen(field))

    def test_factory_result_mutable_after_freeze(self):
        class Container:
            pass

        c = Container()
        c.field = InterpreterLocal(lambda: [])
        freeze(c)
        c.field.get().append("item")
        self.assertEqual(c.field.get(), ["item"])


class TestInterpreterLocalErrors(unittest.TestCase):
    """Test error cases."""

    def test_no_args(self):
        with self.assertRaises(TypeError):
            InterpreterLocal()

    def test_multiple_independent_fields(self):
        f1 = InterpreterLocal(1)
        f2 = InterpreterLocal(2)
        self.assertEqual(f1.get(), 1)
        self.assertEqual(f2.get(), 2)
        f1.set(10)
        self.assertEqual(f1.get(), 10)
        self.assertEqual(f2.get(), 2)

    def test_non_freezable_default(self):
        """Non-freezable default should raise at construction."""
        from immutable import set_freezable, FREEZABLE_NO
        class NF:
            pass
        obj = NF()
        set_freezable(obj, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            InterpreterLocal(obj)

    def test_non_freezable_factory(self):
        """Non-freezable factory should raise at construction."""
        from immutable import set_freezable, FREEZABLE_NO
        def factory():
            return []
        set_freezable(factory, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            InterpreterLocal(factory)


class TestInterpreterLocalSubinterpreters(unittest.TestCase):
    """Test that InterpreterLocal provides per-interpreter isolation."""

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

    def test_shared_frozen_object_gets_default_in_subinterp(self):
        """A frozen InterpreterLocal shared to a sub-interpreter
        should return the default value there, not main's value."""
        field = InterpreterLocal(42)
        field.set(999)
        self.assertEqual(field.get(), 999)

        # Freeze so it can be shared directly (immutable sharing)
        freeze(field)

        output = self._run_in_subinterp(
            "print(field.get())\n",
            shared={"field": field},
        )
        self.assertEqual(output.strip(), "42")

    def test_shared_frozen_object_set_independent(self):
        """Setting a value in the sub-interpreter should not affect main."""
        field = InterpreterLocal(0)
        freeze(field)

        self._run_in_subinterp(
            "field.set(123)\n"
            "print(field.get())\n",
            shared={"field": field},
        )
        # Main interpreter's value should still be 0
        self.assertEqual(field.get(), 0)

    def test_shared_frozen_container_with_interpreterlocal(self):
        """A frozen container with an InterpreterLocal field should
        provide per-interpreter isolation when shared."""
        class Container:
            pass

        c = Container()
        c.counter = InterpreterLocal(lambda: [])
        freeze(c)

        # Main interpreter uses the field
        c.counter.get().append("main")
        self.assertEqual(c.counter.get(), ["main"])

        # Sub-interpreter gets its own fresh list from the factory
        output = self._run_in_subinterp(
            "c.counter.get().append('sub')\n"
            "print(c.counter.get())\n",
            shared={"c": c},
        )
        self.assertEqual(output.strip(), "['sub']")

        # Main should be unaffected
        self.assertEqual(c.counter.get(), ["main"])


if __name__ == "__main__":
    unittest.main()
