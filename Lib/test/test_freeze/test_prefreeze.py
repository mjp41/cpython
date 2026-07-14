import unittest

from immutable import freeze, is_frozen, set_freezable, FREEZABLE_NO


class TestPreFreezeHook(unittest.TestCase):
    def test_prefreeze_hook_is_called(self):
        class C:
            def __init__(self):
                self.hook_calls = 0

            def __pre_freeze__(self):
                self.hook_calls += 1

        obj = C()
        freeze(obj)

        self.assertEqual(obj.hook_calls, 1)
        self.assertTrue(is_frozen(obj))

    def test_prefreeze_hook_runs_before_object_is_frozen(self):
        class C:
            def __init__(self):
                self.was_frozen_inside_hook = None

            def __pre_freeze__(self):
                self.was_frozen_inside_hook = is_frozen(obj)

        obj = C()
        freeze(obj)

        self.assertIs(obj.was_frozen_inside_hook, False)
        self.assertTrue(is_frozen(obj))

    def test_prefreeze_hook_remains_called_after_failure(self):
        class C:
            def __init__(self):
                self.hook_calls = 0
                self.child = {}
                set_freezable(self.child, FREEZABLE_NO)

            def __pre_freeze__(self):
                self.hook_calls += 1

        obj = C()

        with self.assertRaises(TypeError):
            freeze(obj)
        with self.assertRaises(TypeError):
            freeze(obj)

        self.assertEqual(obj.hook_calls, 1)
        self.assertFalse(is_frozen(obj))

    def test_nested_freeze(self):
        class A:
            def __init__(self, field):
                self.field = field
            def __pre_freeze__(self):
                freeze(self.field)

        a = A(A(None))

        # Freezing A should succeed even with nested `freeze()` calls
        freeze(a)

        # Objects frozen by nested freeze calls should remain frozen
        self.assertTrue(is_frozen(a))
        self.assertTrue(is_frozen(a.field))
        self.assertTrue(is_frozen(a.field.field))

    def test_nested_cycle(self):
        class A:
            def __init__(self, next):
                self.next = next
            def __pre_freeze__(self):
                freeze(self.next)

        # Create a cycle of pre-freezes
        a = A(None)
        b = A(a)
        c = A(b)
        d = A(c)
        e = A(d)
        a.next = e

        # Freezing should succeed even with the cycle of pre-freezes
        freeze(a)

        # Check the objects are frozen
        self.assertTrue(is_frozen(a))
        self.assertTrue(is_frozen(b))
        self.assertTrue(is_frozen(c))
        self.assertTrue(is_frozen(d))
        self.assertTrue(is_frozen(e))

    def test_nested_freeze_stays_frozen_on_fail(self):
        class A:
            def __init__(self):
                self.freezable = {}
                self.unfreezable = {}
                set_freezable(self.unfreezable, FREEZABLE_NO)

            def __pre_freeze__(self):
                freeze(self.freezable)

        a = A()

        # Freezing A should succeed even with nested `freeze()` calls
        with self.assertRaises(TypeError):
            freeze(a)

        # Objects frozen by nested freeze calls should remain frozen
        self.assertFalse(is_frozen(a))
        self.assertTrue(is_frozen(a.freezable))

    def test_pre_freeze_can_stop_freezing(self):
        class A:
            def __init__(self, fail):
                self.fail = fail
            def __pre_freeze__(self):
                if self.fail:
                    raise ValueError(2)

        # This should fail, since the pre-freeze throws a ValueError
        a = A(True)
        with self.assertRaises(ValueError):
            freeze(a)
        self.assertFalse(is_frozen(a))

        # This should succeed, since the pre-freeze succeeds
        a = A(False)
        freeze(a)
        self.assertTrue(is_frozen(a))

    def test_pre_freeze_self(self):
        class A:
            def __pre_freeze__(self):
                freeze(self)

        # This assumes that list items are visited in order
        a = A()
        b = A()
        set_freezable(b, FREEZABLE_NO)
        lst = [a, b]

        # Freezing lst will fail due to b
        with self.assertRaises(TypeError):
            freeze(lst)

        # a should remain frozen due to its pre-freeze
        self.assertTrue(is_frozen(a))
        self.assertFalse(is_frozen(b))

    def test_nested_freeze_restarts_incomplete_scc(self):
        class A:
            pass

        class Restart:
            def __pre_freeze__(self):
                freeze(self)

        a = A()
        a.l = [a, Restart()]
        l = a.l

        freeze(A)
        freeze(a)

        self.assertTrue(is_frozen(a))
        self.assertTrue(is_frozen(l))
        self.assertTrue(is_frozen(l[1]))

    def test_nested_freeze_restart_clears_non_gc_visited(self):
        class A:
            pass

        class Restart:
            def __pre_freeze__(self):
                freeze(self)

        a = A()
        a.leaf = "unique-nongc-string"
        a.restart = Restart()

        freeze(a)

        self.assertTrue(is_frozen(a))
        self.assertTrue(is_frozen(a.restart))

    def test_failure_rolls_back_incomplete_scc(self):
        class A:
            pass

        bad = {}
        set_freezable(bad, FREEZABLE_NO)
        a = A()
        a.l = [a, bad]
        l = a.l

        with self.assertRaises(TypeError):
            freeze(a)

        self.assertFalse(is_frozen(a))
        self.assertFalse(is_frozen(l))
        self.assertFalse(is_frozen(bad))

if __name__ == "__main__":
    unittest.main()
