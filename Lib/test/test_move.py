import unittest
import sys
import inspect

global_var_1 = 0
global_var_2 = 2

# This class tests the move of local variables, these are called `NAME`
# in Pythons grammar.
class TestNameMoves(unittest.TestCase):

    def test_move_local_basic(self):
        x = 42
        y = move x
        self.assertEqual(y, 42)
        # Check that x has been deleted from locals
        self.assertNotIn("x", locals())

    def test_move_local_as_arg(self):
        def take_one(x):
            self.assertEqual(x, "Super secret value")

        y = "Super secret value"
        take_one(move y)

        # Check that y has been deleted from locals
        self.assertNotIn("y", locals())

    def test_read_local_before_move(self):
        def take_several(cp, mv):
            self.assertEqual(cp, "Mister Flesh Stick")
            self.assertEqual(mv, "Mister Flesh Stick")

        x = "Mister Flesh Stick"

        # Test that any reads of `x` before `move x` are valid
        take_several(x, move x)

        # Check that `x` has been removed
        self.assertNotIn("x", locals())

    def test_read_local_after_move(self):
        def take_several(moved_val, was_found_after_move):
            self.assertEqual(moved_val, "Sir Reginald")
            self.assertFalse(was_found_after_move)

        x = "Sir Reginald"
        # Test that x is no longer in `locals()` after the move
        take_several(move x, "x" in locals())

    # Checks that `move` nulls the previous reference before a function is
    # called with the moved value. This property is checked from the ref count
    # apposed to just `"x" in locals()` like above.
    def test_move_local_deletes_ref_before_func_call(self):
        def check_unique(self, unique_value):
            # Ref count should be 2, one from the `unique_value` variable and
            # one from the argument being passed in
            self.assertEqual(sys.getrefcount(unique_value), 2)
            # Moving it into `sys.getrefcount()` should give a refcount of 1
            self.assertEqual(sys.getrefcount(move unique_value), 1)

        # Warning, the type of the object can influence the ref count.
        # A string will have additional ref counts when used as a function
        # argument. A dictionary will not add any magical references.
        source = {}

        # Move the value into the uniqueness check
        check_unique(self, move source)
        # Check that `x` has been removed
        self.assertNotIn("x", locals())

class TestGlobalMoves(unittest.TestCase):
    # Check that we can move global variables and that this move is
    # visible to other functions.
    def test_move_global(self):
        def setup():
            global global_var_1
            global global_var_2
            global_var_1 = "Magic value I"
            global_var_2 = None
        def move_globals():
            global global_var_1
            global global_var_2
            global_var_2 = move global_var_1
        def check_globals(self):
            global global_var_2
            self.assertEqual(global_var_2, "Magic value I")

        setup()
        move_globals()
        check_globals(self)

# This class tests the move of attributes
class TestAttributeMoves(unittest.TestCase):
    # A simple class to add attributes to
    class A:
        pass

    def test_move_attribute_basic(self):
        a = self.A()
        a.f = 12

        f = move a.f
        self.assertEqual(f, 12)

        # Check that the attribute has been deleted
        with self.assertRaises(AttributeError):
            x = a.f

    def test_move_attribute_as_arg(self):
        def take_one(x):
            self.assertEqual(x, "Claptrap")

        a = self.A()
        a.name = "Claptrap"
        take_one(move a.name)

        # Check that the attribute has been deleted
        with self.assertRaises(AttributeError):
            x = a.name

    def test_read_attribute_before_move(self):
        def take_several(cp, mv):
            self.assertEqual(cp, "Dr. Zed")
            self.assertEqual(mv, "Dr. Zed")

        a = self.A()
        a.name = "Dr. Zed"

        # Test that any reads of `a.name` before `move a.name` are valid
        take_several(a.name, move a.name)

        # Check that the attribute has been deleted
        with self.assertRaises(AttributeError):
            x = a.name

    def test_read_attribute_after_move(self):
        def take_several(moved_val, was_found_after_move):
            self.assertEqual(moved_val, "Scooter")
            self.assertFalse(was_found_after_move)

        a = self.A()
        a.name = "Scooter"

        # Test that x is no longer in `a` after the move
        take_several(move a.name, "name" in dir(a))

    # Checks that `move` removes the previous reference before a function is
    # called with the moved value. This property is checked by the ref count.
    def test_move_attribute_deletes_ref_before_func_call(self):
        def check_unique(unique_value):
            # Ref count should be 2, one from the `unique_value` variable and
            # one from the argument being passed in
            self.assertEqual(sys.getrefcount(unique_value), 2)
            # Moving it into `sys.getrefcount()` should give a refcount of 1
            self.assertEqual(sys.getrefcount(move unique_value), 1)

        # Warning, the type of the object can influence the ref count.
        # A string will have additional ref counts when used as a function

        a = self.A()
        a.source = {}

        # Move the value into the uniqueness check
        check_unique(move a.source)
        # Check that the attribute has been deleted
        with self.assertRaises(AttributeError):
            x = a.source

# This class tests the move of list items, also called subscript in Python's
# grammar
class TestListMoves(unittest.TestCase):
    def test_move_list_item_from_start(self):
        lst = [1, 2, 3, 4, 5]

        # Test moving the first element
        first = move lst[0]
        self.assertEqual(first, 1)

        # Check that the value has been removed from the list
        self.assertEqual(lst, [2, 3, 4, 5])

    def test_move_list_item_from_end(self):
        lst = [1, 2, 3, 4, 5]

        # Test moving the first element
        last = move lst[-1]
        self.assertEqual(last, 5)

        # Check that the value has been removed from the list
        self.assertEqual(lst, [1, 2, 3, 4])

    def test_move_list_drain(self):
        lst = [1, 2, 3, 4, 5]
        self.assertEqual(1, move lst[0])
        self.assertEqual(2, move lst[0])
        self.assertEqual(3, move lst[0])
        self.assertEqual(4, move lst[0])
        self.assertEqual(5, move lst[0])
        self.assertEqual(len(lst), 0)

    def test_move_list_back_into_list(self):
        lst = [1, 2, 3]

        # This should take the value '1' and then replace the new value in slot
        # `lst[0]`
        lst[0] = move lst[0]

        self.assertEqual(lst[0], 1)
        self.assertEqual(lst[1], 3)

    def test_move_list_range(self):
        lst = [1, 2, 3, 4, 5]

        sub = move lst[1:4]
        self.assertEqual(lst, [1, 5])
        self.assertEqual(sub, [2, 3, 4])

    def test_move_list_all_items(self):
        lst = [1, 2, 3, 4, 5]

        sub = move lst[:]
        self.assertEqual(lst, [])
        self.assertEqual(sub, [1, 2, 3, 4, 5])

    def test_move_list_with_stride(self):
        odd = [1, 2, 3, 4, 5, 6]

        even = move odd[1::2]
        self.assertEqual(odd, [1, 3, 5])
        self.assertEqual(even, [2, 4, 6])

    def test_move_list_reversed(self):
        lst = [1, 2, 3, 4, 5]

        sub = move lst[::-1]
        self.assertEqual(lst, [])
        self.assertEqual(sub, [5, 4, 3, 2, 1])

# This class tests the move of dictionary items.
class TestDictMoves(unittest.TestCase):
    def test_move_dict_item(self):
        map = {"1": "one", "2": "two", "3": "NaN"}

        name = move map["2"]
        self.assertEqual(name, "two")
        self.assertEqual(len(map), 2)
        self.assertNotIn("2", map)

# Move is a soft keyword, this should allow existing values and functions called
# `move` to remain valid. Let's test this:
class TestMoveKeywordSoftness(unittest.TestCase):
    def test_move_function_name(self):
        def move(move):
            self.assertEqual(move, "value")
        value = "value"
        # Call the `move` function with `move` as an argument
        move(value)

        self.assertEqual(value, "value")

        # Call the `move` function with the value moved out of `move`
        move(move value)
        # Check that `value` has been removed
        self.assertNotIn("value", locals())

    def test_move_variable_name(self):
        def take_one(move):
            self.assertEqual(move, "value")
        move = "value"

        # Call the `move` function with `move` as an argument
        take_one(move)
        self.assertEqual(move, "value")

        # Call the `move` function with the value moved out of `move`
        take_one(move move)
        # Check that `move` has been removed
        self.assertNotIn("move", locals())

if __name__ == "__main__":
    unittest.main()
