import gc
import re
import sys
import unittest
from immutable import freeze, is_frozen, freezable
from immutable import TracingRegion as Region
from immutable import Cown, InterpreterLocal

def sort_region_error(msg):
    """Normalize a 'region could not be closed' message by masking the object
    addresses and sorting its per-object lines. Useful for deterministic test
    assertions, since the addresses differ per run and the object order comes
    from hashtable iteration and isn't stable."""
    header, *lines = re.sub(r"0x[0-9a-fA-F]+", "0x...", msg).splitlines()
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
                "- 1 incoming reference to list '[1]'",
                "- 1 incoming reference to list '[2]'"
            ])

    def test_release_error_capped_output(self):
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
                "- 1 incoming reference to list '[1]'",
                "- 1 incoming reference to list '[1]'",
                "- 1 incoming reference to list '[1]'",
                "- 1 incoming reference to list '[1]'",
                "- 1 incoming reference to list '[1]'",
                "- 3 references to other objects",
            ])

        # The cown should now be released
        l = None
        c.release()

    def test_release_error_in_subregion(self):
        x = [1]

        c = Cown(Region())
        child = Region()
        child.x = x
        c.value.child = child

        with self.assertRaises(RuntimeError) as cm:
            c.release()

        self.assertEqual(
            sort_region_error(str(cm.exception)),
            [
                "The region could not be closed due to:",
                "- 1 incoming reference to list '[1]'",
            ])


class TestRegionOpening(unittest.TestCase):
    def test_open_after_acquire(self):
        c = Cown(Region())
        c.value.x = []
        self.assertFalse(c._is_closed())

        c.release()
        c.acquire()

        self.assertTrue(c._is_closed())
        c.value.x = None
        self.assertFalse(c._is_closed())

    def test_release_closed_region(self):
        c = Cown(Region())
        c.value.x = []
        self.assertFalse(c._is_closed())

        c.release()
        c.acquire()

        self.assertTrue(c._is_closed())

        c.release()

    def test_bridge_refs_keep_region_closed(self):
        c = Cown(Region())
        c.release()
        c.acquire()
        self.assertTrue(c._is_closed())

        # Adding new references to the bridge object should keep it closed.
        # only attribute accesses should open it.
        r1 = c.value
        r2 = c.value
        self.assertTrue(c._is_closed())

        # However, these references should prevent the cown from being released
        with self.assertRaises(RuntimeError) as cm:
            c.release()

        self.assertEqual(
            str(cm.exception),
            "the cown couldn't be released, due to the bridge having incoming references")

        # The release should succeed once all refs have been killed
        del r1
        del r2
        c.release()

    def test_sub_region_closing(self):
        @freezable
        class A:
            pass
        c = Cown(Region())
        c.value.a = A()
        c.value.a.child = Region()
        c.value.a.child.b = A()

        c.release()
        c.acquire()

        r2 = c.value.a.child
        c2 = Cown(r2)

        self.assertTrue(c2._is_closed())

    def test_sub_region_multiple_refs(self):
        @freezable
        class A:
            pass
        c = Cown(Region())
        c.value.a = A()
        sub = Region()
        c.value.a.child_a = sub
        c.value.a.child_b = sub
        # A reference to the bridge of a sub-region counts as an incoming
        # reference into the parent region, see
        # test_ref_to_sub_region_bridge_keeps_parent_open.
        del sub

        c.release()
        c.acquire()

        r2 = c.value.a.child_a
        c2 = Cown(r2)

        self.assertTrue(c2._is_closed())

    def test_ref_to_sub_region_bridge_keeps_parent_open(self):
        c1 = Cown(Region())
        c2 = Cown(Region())
        c1.value.child = c2.value

        self.assertFalse(c2._is_closed())

        with self.assertRaises(RuntimeError) as cm:
            c1.release()

        # Attempting to close the region c1 should have closed c2 and then
        # failed due to the incoming reference to the bridge stored in c2
        self.assertTrue(c2._is_closed())


        self.assertEqual(
            sort_region_error(str(cm.exception)),
            [
                "The region could not be closed due to:",
                "- 1 incoming reference to TracingRegion '<TracingRegion closed>'",
            ])



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


class TestClosedRegionTeardown(unittest.TestCase):
    """A closed region disposes of its own contents.

    Closing proves nothing outside the region references its members, so the
    death of the bridge object makes all of them garbage. The region finalizes
    and clears them itself rather than handing them to the GC.
    """

    def test_cycles_reclaimed_without_the_collector(self):
        """Check that cycles in closed regions are reclaimed without the collector"""

        @freezable
        class Node:
            pass

        def _live_nodes():
            """The number of nodes the collector can see."""
            return sum(1 for o in gc.get_objects() if type(o) is Node)

        gc.disable()
        try:
            before = _live_nodes()

            # Create a cycle
            a = Node()
            b = Node()
            a.b = b
            b.a = a

            # Move the cycle into a cown
            c = Cown(Region())
            c.value.cycle = a
            del a
            del b
            mid = _live_nodes()

            # Close the region
            c.release()
            del c

            leaked = _live_nodes() - before
        finally:
            gc.enable()

        self.assertEqual(mid, 2, "the cycle wasn't detected while the region is open")
        self.assertEqual(leaked, 0, "closed region contents were not reclaimed")

    def test_finalizers_run(self):
        local = InterpreterLocal(0)

        @freezable
        class Recorder:
            def __del__(self, local=local):
                local.set(local.get() + 1)

        c = Cown(Region())
        for i in range(5):
            setattr(c.value, "r%d" % i, Recorder())
        c.release()

        self.assertEqual(local.get(), 0)
        del c
        self.assertEqual(local.get(), 5)


    def test_finalizer_can_modify_the_bridge(self):
        local = InterpreterLocal(False)

        @freezable
        class Reenter:
            def __del__(self, local=local):
                # This will open the region and also prove that the finalizer ran
                local.set(self.bridge.reenter == self)
                self.bridge.__dict__ = {}

        # Create a cycle, and allow Reenter to modify the bridge
        c = Cown(Region())
        c.value.reenter = Reenter()
        c.value.reenter.bridge = c.value
        c.release()
        del c

        self.assertTrue(local.get(), "the finalizer did not run or got the wrong object")

    def test_finalizer_revivial(self):
        local_bridge = InterpreterLocal(None)
        local_medic = InterpreterLocal(None)

        @freezable
        class Medic:
            def __del__(self, loca_bridge=local_bridge, local_medic=local_medic):
                local_bridge.set(self.bridge)
                local_medic.set(self)

        c1 = Cown(Region())
        c1.value.reviver = Medic()
        c1.value.reviver.bridge = c1.value
        c1.release()
        del c1

        # Retrieve the revived bridge
        self.assertIsInstance(local_bridge.get(), Region);
        c2 = Cown(local_bridge.get())
        local_bridge.set(None)

        with self.assertRaises(RuntimeError) as e:
            c2.release()
        self.assertTrue(str(e.exception).endswith("has been finalized and cannot be closed again"))

        # Check that the revived medic object is valid
        self.assertIn("Medic object at 0x", str(local_medic.get()))

