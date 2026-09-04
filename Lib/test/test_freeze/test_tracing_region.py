import gc
import re
import sys
import unittest
import weakref
from immutable import freeze, is_frozen, freezable
from immutable import TracingRegion as Region
from immutable import Cown, InterpreterLocal, RegionRef
from test.support import import_helper, os_helper

def sort_region_error(msg):
    """Normalize a 'region could not be closed' message by masking the object
    addresses and sorting its per-object lines. Useful for deterministic test
    assertions, since the addresses differ per run and the object order comes
    from hashtable iteration and isn't stable."""
    header, *lines = re.sub(r"0x[0-9a-fA-F]+", "0x...", msg).splitlines()
    return [header, *sorted(lines)]

class TestTracing(unittest.TestCase):
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

    def test_failed_multi_parent_region_close(self):
        r1 = Region()
        r2 = Region()
        r3 = Region()
        r2.sub = r3
        r1.lst = [r3, r2, r3]
        del r2
        del r3

        c = Cown(r1)
        del r1

        with self.assertRaises(RuntimeError) as cm:
            c.release()

        self.assertEqual(
            sort_region_error(str(cm.exception)),
            [
                "The region could not be closed due to:",
                "- 3 incoming references to TracingRegion '<TracingRegion closed>'",
            ])


    def test_failed_cyclic_region_close(self):
        r1 = Region()
        r2 = Region()
        r3 = Region()

        r1.r2 = r2
        r2.r3 = r3
        r3.r1 = r1
        c = Cown(r1)

        del r1
        del r2
        del r3

        with self.assertRaises(RuntimeError) as cm:
            c.release()

        self.assertEqual(
            sort_region_error(str(cm.exception)),
            [
                "the region 0x... can not be closed as it attempts to reference one of its parent regions 0x...",
            ])

    def test_weak_ref_in_region(self):
        @freezable
        class A:
            pass

        r1 = Region()
        r1.obj = A()
        r1.wref1 = weakref.ref(r1.obj)
        wref2 = weakref.ref(r1.obj)

        c = Cown(r1)
        del r1

        # Releasing should clear all external weak references
        c.release()
        self.assertIsNone(wref2());

        # All internal weak references should remain valid
        c.acquire()
        self.assertEqual(c.value.wref1(), c.value.obj);

    def test_weak_ref_to_bridge(self):
        """
        The closing code and cowns currently assume that bridges can't have weak references.
        This tests asserts this. We can add support for weak refs, but that would require some
        engineering and the question is if this is even needed.
        """

        r1 = Region()
        with self.assertRaises(TypeError) as err:
            weakref.ref(r1)

        self.assertEqual(str(err.exception), "cannot create weak reference to 'TracingRegion' object")


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


class TestRegionRef(unittest.TestCase):
    """A region reference does not keep a region open. It checks on every
    dereference whether this interpreter may reach the target, and opens the
    region tree on the way."""

    def _obj(self, tag=0):
        @freezable
        class A:
            pass
        obj = A()
        obj.tag = tag
        return obj

    def test_deref_while_open(self):
        r = Region()
        r.obj = self._obj(1)
        rr = RegionRef(r.obj)
        self.assertIs(rr(), r.obj)

    def test_survives_close_unlike_weakref(self):
        """A close clears the plain weak references pointing into the region
        but re-homes the region references instead."""
        r = Region()
        r.obj = self._obj(2)
        wref = weakref.ref(r.obj)
        rr = RegionRef(r.obj)

        c = Cown(r)
        del r
        c.release()

        self.assertIsNone(wref())
        c.acquire()
        self.assertEqual(rr().tag, 2)

    def test_denied_while_released(self):
        r = Region()
        r.obj = self._obj(3)
        rr = RegionRef(r.obj)
        c = Cown(r)
        del r
        c.release()

        with self.assertRaises(RuntimeError) as cm:
            rr()
        self.assertIn("released cown", str(cm.exception))

    def test_deref_opens_the_region(self):
        r = Region()
        r.obj = self._obj(4)
        rr = RegionRef(r.obj)
        c = Cown(r)
        del r
        c.release()
        c.acquire()

        self.assertTrue(c._is_closed())
        obj = rr()
        self.assertFalse(c._is_closed())
        self.assertIs(obj, c.value.obj)

    def test_deref_opens_the_whole_chain(self):
        """A reference into a nested region has to open every region above it,
        not just the one holding the target."""
        child = Region()
        child.obj = self._obj(5)
        rr = RegionRef(child.obj)
        r = Region()
        r.child = child
        del child

        c = Cown(r)
        del r
        c.release()
        c.acquire()

        self.assertTrue(c._is_closed())
        self.assertEqual(rr().tag, 5)
        self.assertFalse(c._is_closed())
        self.assertEqual(c.value.child.obj.tag, 5)

    def test_release_after_acquire_without_opening(self):
        """A release does not re-trace an already closed region, so nothing
        re-stamps its node. The cown node is what keeps the next owner able to
        dereference."""
        r = Region()
        r.obj = self._obj(6)
        rr = RegionRef(r.obj)
        c = Cown(r)
        del r

        c.release()
        c.acquire()
        c.release()

        with self.assertRaises(RuntimeError):
            rr()

        c.acquire()
        self.assertEqual(rr().tag, 6)

    def test_region_outliving_its_cown(self):
        r = Region()
        r.obj = self._obj(7)
        rr = RegionRef(r.obj)
        c = Cown(r)
        del r
        c.release()
        c.acquire()

        escaped = c.value
        del c
        gc.collect()

        self.assertEqual(rr().tag, 7)
        self.assertIsNotNone(escaped)

    def test_target_moving_between_regions(self):
        """A close re-homes every reference to the objects it traced, so an
        object that changed regions resolves through the one it lives in."""
        c1 = Cown(Region())
        c1.value.obj = self._obj(8)
        rr = RegionRef(c1.value.obj)
        c1.release()
        c1.acquire()

        c2 = Cown(Region())
        c2.value.obj = c1.value.obj
        c1.value.obj = None

        # Closing the old region should not restrict the region reference
        c1.release()
        self.assertEqual(rr().tag, 8)

        # Closing the owning reference should restrict the region reference
        c2.release()
        with self.assertRaises(RuntimeError):
            rr()

        # Opening the owning cown allows the region reference again
        c2.acquire()
        self.assertEqual(rr().tag, 8)

    def test_dead_target(self):
        r = Region()
        r.obj = self._obj(9)
        rr = RegionRef(r.obj)
        r.obj = None
        gc.collect()

        self.assertIsNone(rr())
        self.assertIn("dead", repr(rr))

    def test_frozen_target_drops_the_check(self):
        """A frozen object is reachable from everywhere, so its references stop
        carrying an ownership check."""
        obj = self._obj(10)
        rr = RegionRef(obj)
        freeze(obj)
        self.assertEqual(rr().tag, 10)

    def test_repr_does_not_open_the_region(self):
        r = Region()
        r.obj = self._obj(11)
        rr = RegionRef(r.obj)
        c = Cown(r)
        del r
        c.release()

        self.assertIn("unavailable", repr(rr))

        c.acquire()
        self.assertTrue(c._is_closed())
        self.assertIn("to '", repr(rr))
        self.assertTrue(c._is_closed())

    def test_no_callback_argument(self):
        # FIXME(regions): Callbacks are not supported yet.
        obj = self._obj(12)
        with self.assertRaises(TypeError):
            RegionRef(obj, lambda ref: None)

    def test_equality(self):
        obj = self._obj(13)
        other = self._obj(13)
        self.assertEqual(RegionRef(obj), RegionRef(obj))
        self.assertNotEqual(RegionRef(obj), RegionRef(other))

    def test_failed_close_leaves_the_reference_local(self):
        """A close that fails leaves its node unresolved. It has to end up
        local to this interpreter, or the reference would be stuck."""
        child = Region()
        child.obj = self._obj(15)
        rr = RegionRef(child.obj)
        r = Region()
        r.child = child
        del child

        leak = self._obj(16)
        r.leak = leak
        c = Cown(r)
        del r

        with self.assertRaises(RuntimeError):
            c.release()
        self.assertEqual(rr().tag, 15)

    def test_closed_but_not_released_leaves_the_reference_local(self):
        """The tree can close and the cown still refuse to release. Nothing
        roots the region in that case, so it stays ours."""
        r = Region()
        r.obj = self._obj(17)
        rr = RegionRef(r.obj)
        c = Cown(r)
        del r

        held = c.value
        with self.assertRaises(RuntimeError) as cm:
            c.release()
        self.assertIn("incoming references", str(cm.exception))
        self.assertEqual(rr().tag, 17)

        del held
        gc.collect()
        c.release()
        with self.assertRaises(RuntimeError):
            rr()

    def test_deeply_nested_region(self):
        inner = Region()
        inner.obj = self._obj(18)
        rr = RegionRef(inner.obj)
        node = inner
        for _ in range(30):
            outer = Region()
            outer.child = node
            node = outer
        c = Cown(node)
        del node, inner, outer

        c.release()
        c.acquire()
        self.assertEqual(rr().tag, 18)

    def test_repeated_close_open_cycles(self):
        """Each close allocates a fresh node and re-homes the reference onto
        it. The old ones have to go away with it."""
        r = Region()
        r.obj = self._obj(19)
        rr = RegionRef(r.obj)
        c = Cown(r)
        del r

        for _ in range(100):
            c.release()
            c.acquire()
            self.assertEqual(rr().tag, 19)

    def test_target_collected_as_cyclic_garbage(self):
        """The collector clears weak references itself, without going through
        the type. A RegionRef is not a weakref subtype, so every one of those
        paths has to know about it."""
        obj = self._obj(20)
        obj.self = obj
        rr = RegionRef(obj)
        del obj
        gc.collect()
        self.assertIsNone(rr())

    def test_reference_itself_is_cyclic_garbage(self):
        """A RegionRef that is itself unreachable has to be cleared before the
        garbage is deleted, or a __del__ could still dereference it."""
        obj = self._obj(21)
        cell = [RegionRef(obj)]
        cell.append(cell)
        del cell
        gc.collect()
        self.assertIsNotNone(obj)

    def test_not_freezable(self):
        """Freezing would have to either drag the target into the frozen set or
        let an immutable object reach a mutable one. Neither is acceptable, so
        a reference is simply not freezable -- while its type still is, since
        closing a region freezes the type of everything it moves."""
        obj = self._obj(22)
        rr = RegionRef(obj)
        with self.assertRaises(TypeError):
            freeze(rr)
        # The type itself stays freezable: closing a region freezes the type of
        # everything it moves, so a reference could not live in one otherwise.
        freeze(RegionRef)
        self.assertTrue(is_frozen(RegionRef))
        # The target must still be able to die cleanly afterwards.
        del obj
        gc.collect()
        self.assertIsNone(rr())

    def test_already_frozen_target_is_unrestricted(self):
        """Freezing before or after the reference is created has to give the
        same answer, otherwise the semantics depend on ordering."""
        early = self._obj(23)
        freeze(early)
        rr_early = RegionRef(early)

        late = self._obj(24)
        rr_late = RegionRef(late)
        freeze(late)

        self.assertEqual(rr_early().tag, 23)
        self.assertEqual(rr_late().tag, 24)

    def test_hash_is_checked_even_when_cached(self):
        """A cached hash would otherwise stand as an answer about an object
        this interpreter may no longer touch."""
        r = Region()
        r.obj = self._obj(26)
        rr = RegionRef(r.obj)
        hash(rr)

        c = Cown(r)
        del r
        c.release()
        with self.assertRaises(RuntimeError):
            hash(rr)

        c.acquire()
        self.assertIsInstance(hash(rr), int)

    def test_repr_preserves_a_pending_exception(self):
        """repr() runs from error reporting paths, so a denied check must not
        wipe the error state the caller is carrying."""
        r = Region()
        r.obj = self._obj(25)
        rr = RegionRef(r.obj)
        c = Cown(r)
        del r
        c.release()

        try:
            raise ValueError("caller's error")
        except ValueError:
            self.assertIn("unavailable", repr(rr))
            self.assertIsInstance(sys.exception(), ValueError)

    def test_reference_inside_a_region(self):
        """A region reference is not followed by the close trace, so storing
        one in a region does not drag its target in."""
        outside = self._obj(14)
        r = Region()
        r.rr = RegionRef(outside)
        c = Cown(r)
        del r

        c.release()
        self.assertFalse(is_frozen(outside))
        c.acquire()
        self.assertIs(c.value.rr(), outside)


class TestRegionRefSubinterpreters(unittest.TestCase):
    """The point of the ownership check: another interpreter may only
    dereference what it actually owns."""

    def setUp(self):
        self._interpreters = import_helper.import_module('_interpreters')

    def _run_in_subinterp(self, code, shared=None):
        interp = self._interpreters.create()
        try:
            self._interpreters.run_string(interp, code, shared=shared or {})
        finally:
            self._interpreters.destroy(interp)

    def test_frozen_target_reachable_from_everywhere(self):
        """A frozen target is shareable, so its references carry no ownership
        check at all -- whichever order the freeze and the reference happened
        in."""
        @freezable
        class A:
            pass

        early = A(); early.tag = "early"
        freeze(early)
        late = A(); late.tag = "late"

        r = Region()
        r.early_ref = RegionRef(early)
        r.late_ref = RegionRef(late)
        freeze(late)
        c = Cown(r)
        del r
        c.release()

        self._run_in_subinterp("""
c.acquire()
assert c.value.early_ref().tag == "early", "frozen before the ref was created"
assert c.value.late_ref().tag == "late", "frozen after the ref was created"
c.release()
""", shared={"c": c})

    def test_foreign_deallocation_does_not_transfer_ownership(self):
        """A cown is immutable and may live inside a region, so the last
        reference to it can be dropped by an interpreter that never owned it.
        Its region must go to the cown's owner, not to whoever runs the
        deallocator."""
        @freezable
        class A:
            pass

        inner_region = Region()
        inner_region.obj = A()
        inner_region.obj.tag = "owned by the creator"
        mine = RegionRef(inner_region.obj)
        travelling = RegionRef(inner_region.obj)

        inner = Cown(inner_region)
        del inner_region
        inner.release()
        inner.acquire()
        # Keep the region alive past the cown, so the cown can die alone.
        escaped = inner.value

        outer_region = Region()
        outer_region.inner = inner
        outer_region.ref = travelling
        del travelling
        outer = Cown(outer_region)
        del outer_region
        outer.release()
        del inner
        gc.collect()

        self._run_in_subinterp("""
import gc
c.acquire()
c.value.inner = None      # drops the last reference to a cown we never owned
gc.collect()
try:
    c.value.ref()
    raise AssertionError("reached a region owned by another interpreter")
except RuntimeError:
    pass
c.release()
""", shared={"c": outer})

        # The creator still owns it, and is not locked out of its own region.
        self.assertEqual(escaped.obj.tag, "owned by the creator")
        self.assertEqual(mine().tag, "owned by the creator")

    def test_foreign_deallocation_defers_the_teardown_to_the_owner(self):
        """The last reference to a cown owned by this interpreter may be
        dropped by another one. The region inside is reference counted
        non-atomically and tracked in this interpreter's GC list, so the
        teardown has to be handed back here instead of running there."""
        log = os_helper.TESTFN
        self.addCleanup(os_helper.unlink, log)

        @freezable
        class Marker:
            def __del__(self):
                with open(self.log, "a") as f:
                    f.write("region\n")

        inner_region = Region()
        inner_region.marker = Marker()
        inner_region.marker.log = log
        ref = RegionRef(inner_region.marker)

        inner = Cown(inner_region)
        del inner_region
        inner.release()
        inner.acquire()          # owned by this interpreter from here on

        outer_region = Region()
        outer_region.inner = inner
        outer = Cown(outer_region)
        del outer_region
        outer.release()
        del inner
        gc.collect()

        with open(log, "w"):
            pass

        self._run_in_subinterp("""
import gc
c.acquire()
c.value.inner = None      # drops the last reference to a cown we never owned
gc.collect()
with open(log, "a") as f:
    f.write("subinterpreter ")
c.release()
""", shared={"c": outer, "log": log})

        # The teardown was scheduled here and runs at the next eval breaker.
        recorded = []
        for _ in range(10000):
            with open(log) as f:
                recorded = f.read().split()
            if len(recorded) == 2:
                break
        self.assertEqual(recorded, ["subinterpreter", "region"])
        self.assertIsNone(ref())

    def test_owner_may_deref_and_others_may_not(self):
        """`local` was never in a region, so nothing ever re-homes the
        reference to it and it stays local to this interpreter. `owned` travels
        with the region and becomes reachable by whoever holds the cown."""
        @freezable
        class A:
            pass

        local = A()
        local.tag = "local"

        r = Region()
        r.local_ref = RegionRef(local)
        r.owned = A()
        r.owned.tag = "owned"
        r.owned_ref = RegionRef(r.owned)
        c = Cown(r)
        del r
        c.release()

        self._run_in_subinterp("""
c.acquire()
try:
    c.value.local_ref()
    raise AssertionError("reached a foreign local object")
except RuntimeError:
    pass
assert c.value.owned_ref().tag == "owned"
c.release()
""", shared={"c": c})

        c.acquire()
        self.assertEqual(c.value.local_ref().tag, "local")
