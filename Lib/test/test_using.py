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
        r = None
        self.assertTrue(self.hacky_state_check(c, "pending-release"))
        c.get().close()
        self.assertTrue(self.hacky_state_check(c, "released"))

    def test_acquire(self):
        c = Cown(Region())
        self.assertTrue(self.hacky_state_check(c, "released"))
        @using(c)
        def _():
            r = c.get()
            r.open()
            self.assertTrue(self.hacky_state_check(c, "acquired"))
            r = None
            c.get().close()
            self.assertTrue(self.hacky_state_check(c, "acquired"))
        self.assertTrue(self.hacky_state_check(c, "released"))

    def test_region_cown_ptr(self):
        r = Region()
        r.f = Cown()
        self.assertTrue(True)

    def test_invalid_cown_init(self):
         # Create cown with invalid init value
        self.assertRaises(RegionError, Cown, [42])

    def test_threads(self):
        from threading import Thread
        from using import using


        class Counter(object):
            def __init__(self, value):
                self.value = value

            def inc(self):
                self.value += 1

            def dec(self):
                self.value -= 1

            def __repr__(self):
                return "Counter(" + str(self.value) + ")"


        # Freezes the **class** -- not needed explicitly later
        makeimmutable(Counter)

        def ThreadSafeValue(value):
            r = Region("counter region")
            r.value = value
            c = Cown(r)
            # Dropping value, r and closing not needed explicitly later
            del value
            del r
            c.get().close()
            return c

        def work(c):
            for _ in range(0, 100):
                c.inc()

        def work_in_parallel(c):
            @using(c)
            def _():
                work(c.get().value)


        c = ThreadSafeValue(Counter(0))

        t1 = Thread(target=work_in_parallel, args=(c,))
        t2 = Thread(target=work_in_parallel, args=(c,))
        t1.start()
        t2.start()
        t1.join()
        t2.join()

        result = 0
        @using(c)
        def _():
            nonlocal result
            result = c.get().value.value
        if result != 200:
            self.fail()

    def test_thread_creation(self):
        from using import PyronaThread as T

        class Mutable: pass
        self.assertRaises(RuntimeError, lambda x: T(target=print, args=(Mutable(),)), None)
        self.assertRaises(RuntimeError, lambda x: T(target=print, kwargs={'a' : Mutable()}), None)
        self.assertRaises(RuntimeError, lambda x: T(target=print, args=(Mutable(),), kwargs={'a' : Mutable()}), None)
        self.assertRaises(RuntimeError, lambda x: T(target=print, args=(Mutable(), 42)), None)
        self.assertRaises(RuntimeError, lambda x: T(target=print, args=(Mutable(), Cown())), None)
        self.assertRaises(RuntimeError, lambda x: T(target=print, args=(Mutable(), Region())), None)

        T(target=print, kwargs={'imm' : 42, 'cown' : Cown(), 'region' : Region()})
        T(target=print, kwargs={'a': 42})
        T(target=print, kwargs={'a': Cown()})
        T(target=print, kwargs={'a': Region()})

        T(target=print, args=(42, Cown(), Region()))
        T(target=print, args=(42,))
        T(target=print, args=(Cown(),))
        T(target=print, args=(Region(),))
        self.assertTrue(True) # To make sure we got here correctly
