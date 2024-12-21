import unittest
from using import *

# Initial test cases for using and cowns
# Note: no concurrency test yet
class UsingTest(unittest.TestCase):
    obj = None

    def setUp(self):
        makeimmutable(self.obj)

    def test_cown(self):
        def invalid_assignment1(c):
            c.value = 42
        def invalid_assignment2(c):
            c.f = 42
        def invalid_assignment3(c):
            c["g"] = 42

        c = Cown()
        self.assertRaises(AttributeError, invalid_assignment1, c)
        self.assertRaises(AttributeError, invalid_assignment2, c)
        self.assertRaises(TypeError, invalid_assignment3, c)
        # Cannot access unacquired cown
        self.assertRaises(RegionError, lambda _ : c.get(), c)
        self.assertRaises(RegionError, lambda _ : c.set(Region()), c)

    def test_cown_aquired_access(self):
        c = Cown()
        @using(c)
        def _():
            c.set(self.obj)
        @using(c)
        def _():
            self.assertEqual(c.get(), self.obj)

    # Returns the state of a cown as a string
    # Hacky but want to avoid adding methods to cowns just for testing
    def hacky_state_check(self, cown, expected_state):
        s = repr(cown)
        return expected_state in s

    def test_release(self):
        r = Region()
        c = Cown(r)
        self.assertFalse(r.is_open())
        self.assertTrue(self.hacky_state_check(c, "released"))

    def test_early_release_cown(self):
        c = Cown()
        @using(c)
        def _():
            self.assertTrue(self.hacky_state_check(c, "acquired"))
            c.set(c)
            self.assertTrue(self.hacky_state_check(c, "released"))
        self.assertTrue(self.hacky_state_check(c, "released"))

    def test_early_release_closed_region(self):
        c = Cown()
        self.assertTrue(self.hacky_state_check(c, "released"))
        @using(c)
        def _():
            self.assertTrue(self.hacky_state_check(c, "acquired"))
            r = Region()
            self.assertFalse(r.is_open())
            c.set(r)
            self.assertTrue(self.hacky_state_check(c, "released"))
        self.assertTrue(self.hacky_state_check(c, "released"))

    def test_early_release_immutable(self):
        c = Cown()
        @using(c)
        def _():
            self.assertTrue(self.hacky_state_check(c, "acquired"))
            c.set(self.obj)
            self.assertTrue(self.hacky_state_check(c, "released"))
        self.assertTrue(self.hacky_state_check(c, "released"))

    def test_pending_release(self):
        r = Region()
        r.open()
        self.assertTrue(r.is_open())
        c = Cown(r)
        self.assertTrue(self.hacky_state_check(c, "pending-release"))
        r.close()
        self.assertFalse(r.is_open())
        self.assertTrue(self.hacky_state_check(c, "released"))

    def test_acquire(self):
        c = Cown(Region())
        self.assertTrue(self.hacky_state_check(c, "released"))
        @using(c)
        def _():
            r = c.get()
            r.open()
            self.assertTrue(self.hacky_state_check(c, "acquired"))
            r.close()
            self.assertTrue(self.hacky_state_check(c, "released"))
            self.assertFalse(r.is_open())
        self.assertTrue(self.hacky_state_check(c, "released"))

    def test_region_cown_ptr(self):
        r = Region()
        r.f = Cown()
        self.assertTrue(True)
