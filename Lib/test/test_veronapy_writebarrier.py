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

