import unittest
import dis

class TestCellWriteBarrier(unittest.TestCase):
    class A:
        pass

    # Create a cell by capturing a variable in a closure
    # The content of the cell is an instance of A which can be added to regions.
    def make_cell(self):
        x = self.A()
        return (lambda: x).__closure__[0]

    def setUp(self):
        # This freezes A and super and meta types of A namely `type` and `object`
        makeimmutable(self.A)

    def test_cell_region_propagates(self):
        c = self.make_cell()
        r = Region()

        # Move c into the region r
        r.c = c

        # Make sure region r now owns the cell and its content
        self.assertTrue(r.owns_object(c))
        self.assertTrue(r.owns_object(c.cell_contents))

    def test_cell_add_ref(self):
        c = self.make_cell()
        r = Region()

        # Move c into the region r
        r.c = c

        # Make sure region r takes ownership of new references from c
        self.assertTrue(r.owns_object(c))
        self.assertTrue(r.owns_object(c.cell_contents))

    def test_cell_remove_ref(self):
        c = self.make_cell()
        r1 = Region()
        child = Region()

        # Move c into the region r1
        r1.c = c
        self.assertTrue(r1.owns_object(c))

        # Make `child` a subregion of `r1`
        c.cell_contents = child
        self.assertEqual(c.cell_contents, child)

        # Replaceing the value in `c` should unparent the child region
        # We can test this by reparenting it to `r2` without an exception
        r2 = Region()
        c.cell_contents = None
        r2.child = child

    def test_cell_replace_same_bridge(self):
        # This test makes sure that reassigning a pointer to a bridge object with
        # the same value doesn't throw an exception, due the write barrier
        # believing that there are two owning references to the same bridge object.
        c = self.make_cell()
        r = Region()
        child = Region()

        # Move c into the region r
        r.c = c
        self.assertTrue(r.owns_object(c))

        # Make `child` a subregion of `r1`
        c.cell_contents = child
        self.assertEqual(c.cell_contents, child)

        # Reassigning the owning pointer to the bridge should be fine
        # since this writes replaces the previous owning reference
        c.cell_contents = child

# It's not possible to test the existance of all bytecodes. The dis only
# shows the unoptimized version of the bytecode. For testing specilized or
# optimized it's therefore not possible to check for the presence of that
# bytecode in the `dis`. Instead we can only check for the effects of the
# bytecodes.
#
# During test development it's also possible to verify that the expected
# bytecode is triggered by setting a breakpoint in it. Note that it's also
# run during the startup of the runtime and by other tests, so make sure
# the test in question actually started, when the breakpoint is triggered.
class TestBytecodeWriteBarrier(unittest.TestCase):
    # Define a class with __slots__
    class ClassWithSlots:
        # Use `__slots__` to get the `STORE_ATTR_SLOT` bytecode
        __slots__ = ('slot',)

        def __init__(self):
            self.slot = None

    class ClassWithAttr:
        def __init__(self):
            self.attr = None

    def setUp(self):
        # This freezes A and super and meta types of A namely `type` and `object`
        makeimmutable(self.ClassWithSlots)
        makeimmutable(self.ClassWithAttr)

    def test_delete_deref(self):
        r = Region()
        r.field = {}
        x = r.field

        # Make sure the region knows about local reference from x
        self.assertFalse(r.try_close())

        def del_x():
            nonlocal x
            del x # This triggers the DELETE_DEREF opcode

        # Create the function and disassemble to confirm DELETE_DEREF is present
        bytecode = dis.Bytecode(del_x)
        self.assertIn("DELETE_DEREF", [instr.opname for instr in bytecode])

        self.assertTrue(r.is_open())

        # Call the function forcing the deletion of x which should
        # close the region.
        del_x()

        self.assertFalse(r.is_open())

    # This tests that references stored by `STORE_ATTR_SLOT` are known
    # by the write barrier
    def test_store_attr_slot_add_reference(self):
        def set_value(obj, val):
            obj.slot = val  # This triggers the STORE_ATTR_SLOT opcode

        # Setup a region and a class with slots
        r = Region()
        r.slots = self.ClassWithSlots()
        self.assertTrue(r.owns_object(r.slots))

        # Run `set_value` multiple times to optimize it
        set_value(r.slots, None)
        set_value(r.slots, {})

        # Create a new local object
        new_object = {}
        new_object["data"] = {}

        # Store the object in slots
        set_value(r.slots, new_object)

        # Verify the region ownership
        self.assertTrue(r.owns_object(new_object))
        self.assertTrue(r.owns_object(new_object["data"]))

    # This tests that references removed by `STORE_ATTR_SLOT` are known
    # by the write barrier
    def test_store_attr_slot_remove_reference(self):
        def set_value(obj, val):
            obj.slot = val  # This triggers the STORE_ATTR_SLOT opcode

        # Setup a region and a class with slots
        r = Region()
        r.data = {}
        slots = self.ClassWithSlots()

        # Run `set_value` multiple times to optimize it
        set_value(slots, None)
        set_value(slots, {})

        # Create local reference into the region
        set_value(slots, r.data)

        # Make sure the region knows about the reference from the slot
        self.assertFalse(r.try_close())

        # Clear the reference from slots
        set_value(slots, None)

        # Check that the region was closed.
        self.assertFalse(r.is_open())

    # This tests that references stored by `STORE_ATTR_INSTANCE_VALUE` are known
    # by the write barrier
    def test_store_attr_instance_value_add_reference(self):
        def set_value(obj, val):
            obj.attr = val  # This triggers the STORE_ATTR_INSTANCE_VALUE opcode

        # Setup a region and a class with attributes
        r = Region()
        r.attr = self.ClassWithAttr()
        self.assertTrue(r.owns_object(r.attr))

        # Run `set_value` multiple times to optimize it
        set_value(r.attr, None)
        set_value(r.attr, {})

        # Create a new local object
        new_object = {}
        new_object["data"] = {}

        # Store the object in attributes
        set_value(r.attr, new_object)

        # Verify the region ownership
        self.assertTrue(r.owns_object(new_object))
        self.assertTrue(r.owns_object(new_object["data"]))

    # This tests that references removed by `STORE_ATTR_INSTANCE_VALUE` are known
    # by the write barrier
    def test_store_attr_instance_value_remove_reference(self):
        def set_value(obj, val):
            obj.attr = val  # This triggers the STORE_ATTR_INSTANCE_VALUE opcode

        # Setup a region and a class with attributes
        r = Region()
        r.data = {}
        attr = self.ClassWithAttr()

        # Run `set_value` multiple times to optimize it
        set_value(attr, None)
        set_value(attr, {})

        # Create local reference into the region
        set_value(attr, r.data)

        # Make sure the region knows about the reference from the slot
        self.assertFalse(r.try_close())

        # Clear the reference from attributes
        set_value(attr, None)

        # Check that the region was closed.
        self.assertFalse(r.is_open())

if __name__ == "__main__":
    unittest.main()
