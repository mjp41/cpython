import sys
import unittest
from immutable import freeze, is_frozen, freezable
from immutable import TracingRegion as Region
from immutable import Cown

def sort_region_error(msg):
    """Normalize a 'region could not be closed' message by sorting its
    per-object lines. Useful for deterministic test assertions, since the
    object order comes from hashtable iteration and isn't stable."""
    header, *lines = msg.splitlines()
    return [header, *sorted(lines)]

class TestTraceRefs(unittest.TestCase):

    def test_release_error(self):
        x = [1]
        y = [2]

        c = Cown(Region())
        c.value.x = x
        c.value.y = y

        with self.assertRaises(RuntimeError) as cm:
            c.release()

        self.assertEqual(
            sort_region_error(str(cm.exception)),
            [
                "The region could not be closed due to:",
                "- 1 incoming reference to '[1]'",
                "- 1 incoming reference to '[2]'"
            ])

    def test_release_error(self):
        # The object order in the error message is based on the address
        # and therefore fairly random. All elements look the same of
        # make testing stable.
        l = [[1], [1], [1], [1], [1], [1], [1], [1]]

        c = Cown(Region())
        c.value.x = []

        for i in range(len(l)):
            c.value.x.append(l[i])

        with self.assertRaises(RuntimeError) as cm:
            c.release()

        self.assertEqual(
            sort_region_error(str(cm.exception)),
            [
                "The region could not be closed due to:",
                "- 1 incoming reference to '[1]'",
                "- 1 incoming reference to '[1]'",
                "- 1 incoming reference to '[1]'",
                "- 1 incoming reference to '[1]'",
                "- 1 incoming reference to '[1]'",
                "- 3 references to other objects",
            ])

        # The cown should now be released
        l = None
        c.release()


class TestRegionOpening(unittest.TestCase):
    def test_open_after_acquire(self):
        c = Cown(Region())
        c.value.x = []

        c.release()
        c.acquire()

        self.assertTrue(c._is_closed())
        c.value.x = None
        self.assertFalse(c._is_closed())

    def test_release_closed_region(self):
        c = Cown(Region())
        c.value.x = []

        c.release()
        c.acquire()

        self.assertTrue(c._is_closed())

        c.release()


class TestImplicitFreeze(unittest.TestCase):
    def test_implicit_freeze_func(self):
        @freezable
        def some_func():
            pass
        c = Cown(Region())

        c.value.obj = some_func
        self.assertFalse(is_frozen(c.value.obj))
        c.release()
        c.acquire()
        self.assertTrue(is_frozen(c.value.obj))

    def test_implicit_freeze_type(self):
        @freezable
        class A:
            pass
        c = Cown(Region())

        c.value.obj = A
        self.assertFalse(is_frozen(c.value.obj))
        c.release()
        c.acquire()
        self.assertTrue(is_frozen(c.value.obj))

    def test_implicit_freeze_module(self):
        import random;
        c = Cown(Region())

        c.value.obj = random
        self.assertFalse(is_frozen(c.value.obj))
        c.release()
        c.acquire()
        self.assertTrue(is_frozen(c.value.obj))

        # Unimport module
        sys.modules.pop("random", None)
        sys.mut_modules.pop("random", None)

    def test_implicit_freeze_str(self):
        c = Cown(Region())

        c.value.obj = "Ducks are cool"
        c.release()
        c.acquire()
        self.assertTrue(is_frozen(c.value.obj))

    def test_implicit_freeze_int(self):
        c = Cown(Region())

        c.value.obj = 17
        c.release()
        c.acquire()
        self.assertTrue(is_frozen(c.value.obj))

