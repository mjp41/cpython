import sys
import unittest
import weakref

from immutable import freeze, is_frozen


class A:
    pass


class Finalizable:
    def __del__(self):
        # Using a module field to escape immutability.
        sys.deallocated = True


class CallbackDetector:
    def __init__(self):
        self.called = False

    def callback(self, wr):
        self.called = True


def trigger_pending_calls():
    # Pending calls are checked, for example, when calling a function.
    pass


def dummy_callback(wr):
    pass


class TestRefcounts(unittest.TestCase):
    def test_weakref_to_frozen_object(self):
        baseline = A()
        a = A()
        freeze(a)
        wr = weakref.ref(a)
        # The weakref should be frozen to ensure atomic refcounting.
        # FIXME(Immutable): Freezing a weakref currently makes it strong.
        # self.assertTrue(is_frozen(wr))
        self.assertEqual(sys.getrefcount(wr), sys.getrefcount(baseline))

    def test_weakref_to_frozen_object_callback(self):
        baseline = A()
        a = A()
        freeze(a)
        wr = weakref.ref(a, dummy_callback)
        # The weakref should have had its refcount pre-emptively incremented.
        self.assertFalse(is_frozen(wr))
        self.assertEqual(sys.getrefcount(wr), sys.getrefcount(baseline) + 1)

    def test_freeze_object_with_weakref(self):
        baseline = A()
        a = A()
        wr = weakref.ref(a)
        freeze(a)
        # The weakref should be frozen to ensure atomic refcounting.
        # FIXME(Immutable): Freezing a weakref currently makes it strong.
        # self.assertTrue(is_frozen(wr))
        self.assertEqual(sys.getrefcount(wr), sys.getrefcount(baseline))

    def test_freeze_object_with_weakref_callback(self):
        baseline = A()
        a = A()
        wr = weakref.ref(a, dummy_callback)
        freeze(a)
        # The weakref should have had its refcount pre-emptively incremented.
        self.assertFalse(is_frozen(wr))
        self.assertEqual(sys.getrefcount(wr), sys.getrefcount(baseline) + 1)


class TestWeakrefList(unittest.TestCase):
    def test_remove_weakref(self):
        a = A()
        freeze(a)
        wr = weakref.ref(a)
        wr = None
        # The reference should have been removed.
        self.assertTrue(weakref.getweakrefcount(a) == 0)

    def test_reuse_weakref(self):
        a = A()
        freeze(a)
        wr1 = weakref.ref(a)
        wr2 = weakref.ref(a)
        # The weakrefs should be the same, as they refer to the same object.
        self.assertTrue(wr1 is wr2)


class TestCallbacks(unittest.TestCase):
    def setUp(self):
        sys.deallocated = False

    def test_callback_single(self):
        f = Finalizable()
        freeze(f)
        detector = CallbackDetector()
        wr = weakref.ref(f, detector.callback)
        f = None
        trigger_pending_calls()
        self.assertTrue(detector.called)
        self.assertTrue(sys.deallocated)

    def test_callback_scc(self):
        f = Finalizable()
        f.b = A()
        f.b.f = f
        freeze(f)
        detector = CallbackDetector()
        wr = weakref.ref(f, detector.callback)
        f = None
        trigger_pending_calls()
        self.assertTrue(detector.called)
        self.assertTrue(sys.deallocated)


if __name__ == '__main__':
    unittest.main()
