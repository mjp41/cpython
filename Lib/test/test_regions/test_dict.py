from unittest import expectedFailure
import unittest
from regions import Region, is_local
from immutable import freeze, is_frozen

class TestRegionDict(unittest.TestCase):
    def check_view_ref(self, create_view):
        # Setup
        r = Region()
        r.dict = {}
        base_lrc = r._lrc

        # Pre-condition
        self.assertGreater(base_lrc, 0)

        # Action: Creating a view
        view = create_view(r.dict)
        self.assertEqual(r._lrc, base_lrc + 1)

        # Action: Check mapping of view
        mapping = view.mapping
        self.assertEqual(r._lrc, base_lrc + 2)

        # Clearing references could decrease the LRC
        mapping = None
        self.assertEqual(r._lrc, base_lrc + 1)
        view = None
        self.assertEqual(r._lrc, base_lrc)

    def test_new_dict_refs_from_item_view(self):
        self.check_view_ref(lambda dict: dict.items())

    def test_new_dict_refs_from_keys_view(self):
        self.check_view_ref(lambda dict: dict.keys())

    def test_new_dict_refs_from_values_view(self):
        self.check_view_ref(lambda dict: dict.values())

class BaseTestRegionDictKeys(unittest.TestCase):
    __test__ = False

    def check_dict_construction_value_lrc(self, build):
        """
        This checks that the different dictionary constructions adjust the LRC
        """
        class Value:
            pass
        freeze(Value())

        val1 = Value()
        val2 = Value()
        val3 = Value()
        val_region = Region()
        val_region.val1 = val1
        val_region.val2 = val2
        val_region.val3 = val3
        self.assertTrue(val_region.owns(val1) and val_region.owns(val2) and val_region.owns(val3))
        val_lrc = val_region._lrc

        # Create a dictionary and check that the LRCs have changed
        d = build(val1, val2, val3)
        self.assertTrue(is_local(d))
        self.assertEqual(val_region._lrc, val_lrc + 3,
                         f"The LRC of the value region should be adjusted. Base LRC: {val_lrc}")


    def check_dict_construction_key_lrc(self, key1, key2, key3, build):
        """
        This checks that the different dictionary constructions adjust the LRC
        """
        class Value:
            pass
        freeze(Value())

        key_region = Region()
        key_region.k1 = key1
        key_region.k2 = key2
        key_region.k3 = key3

        key_lrc = key_region._lrc

        # Create a dictionary and check that the LRCs have changed
        d = build(Value(), Value(), Value())
        self.assertEqual(key_region._lrc, key_lrc + 3,
                         f"The LRC of the key region should be adjusted. Base LRC: {key_lrc}")


    def check_dict_assign(self, dict, key):
        """
        This checks if a region takes ownership if a is inserted into
        the given `dict` using the given `key`
        """

        class SomeObject:
            pass
        freeze(SomeObject())

        # Setup
        r = Region()
        r.dict = dict
        value = SomeObject()

        # Pre-condition
        self.assertTrue(r.owns(r.dict))
        self.assertTrue(is_local(value))

        # Action
        r.dict[key] = value

        # Post-condition
        self.assertTrue(r.owns(key) or is_frozen(key))
        self.assertTrue(r.owns(value))

    def check_dict_remove_ref(self, key, action):
        """
        This tests if the replacement of a key correctly decrements
        the LRC of a region
        """

        class ContainedObject:
            pass
        freeze(ContainedObject())

        # Setup
        r = Region()
        r.obj = ContainedObject()
        local = {}
        local[key] = r.obj
        lrc = r._lrc

        # Pre-condition
        self.assertTrue(r.owns(r.obj))
        self.assertTrue(lrc > 0)
        self.assertTrue(is_local(local))

        # The action should decrement the LRC
        action(local, key)

        # Post-condition
        self.assertEqual(r._lrc, lrc - 1)

    def check_dict_item_access(self, key, access, lrc_offset = 1):
        """
        Checks if calling the given `access` function increases the
        LRC of the region. Note that access should return the reference
        """

        # Setup
        r = Region()
        r.dict = {}
        r.dict[key] = [1, 2]
        lrc = r._lrc

        # Pre-condition
        self.assertGreater(lrc, 0)

        # Action
        local = access(r.dict, key)

        # Post-condition
        self.assertEqual(r._lrc, lrc + lrc_offset)

    def check_dict_get_default(self, key):
        """
        Checks that the `get()` method of the dictionary
        """
        class SomeObject:
            pass
        freeze(SomeObject())

        # Setup
        r = Region()
        r.obj = SomeObject()
        lrc = r._lrc

        # Pre-condition
        self.assertGreater(lrc, 0)

        # Action
        dict = {}
        local = dict.get(key, r.obj)

        # Post-condition
        self.assertEqual(r._lrc, lrc + 1)

    def check_loop_lrc_change(self, region, iter_src, loop_lrc_effect, iter_lrc_cost = 1, check_lrc_reset=True):
        # Check loop iterations change the LRC
        lrc = region._lrc
        i = 0
        for v in iter_src:
            self.assertEqual(region._lrc, lrc + iter_lrc_cost + loop_lrc_effect,
                             f"Fail in iteration: {i} base LRC {lrc} + {iter_lrc_cost} for iter")
            if check_lrc_reset:
                v = None
                self.assertEqual(region._lrc, lrc + iter_lrc_cost,
                                f"LRC didn't reset in iteration: {i} base LRC {lrc} + {iter_lrc_cost} for iter")
            i += 1

        # Setting v to none should reset the LRC to pre-loop levels
        v = None

        # Check LRC is back to pre-loop levels
        self.assertEqual(region._lrc, lrc)

    def check_dict_view(self, key1, key2, create_view, loop_lrc_effect, iter_lrc_cost = 1, check_lrc_reset=True):
        class SomeObject:
            pass
        freeze(SomeObject())

        # Setup
        r = Region()
        r.dict = {}
        r.dict[key1] = SomeObject()
        r.dict[key2] = SomeObject()

        # Create the view
        view = create_view(r.dict)

        self.check_loop_lrc_change(r, view, loop_lrc_effect, iter_lrc_cost, check_lrc_reset)

    def check_pop(self, key):
        class SomeObject:
            pass
        freeze(SomeObject())

        # Setup
        r = Region()
        r.obj = SomeObject()
        d = {}

        # Precondition
        d[key] = r.obj
        base_lrc = r._lrc
        self.assertGreaterEqual(base_lrc, 1)

        # Action
        d.pop(key)
        self.assertEqual(r._lrc, base_lrc - 1)

        # Check pop(key, default) with a new dictionary
        d = {}
        base_lrc = r._lrc
        local_ref = d.pop(key, r.obj)
        self.assertEqual(r._lrc, base_lrc + 1)
        local_ref = None
        self.assertEqual(r._lrc, base_lrc)

    def check_popitem(self, key1, key2, lrc_for_key):
        class Value:
            pass
        freeze(Value())

        # Setup
        r = Region()
        r.obj1 = Value()
        r.obj2 = Value()
        r.key1 = key1
        r.key2 = key2
        d = {}

        # Pre-condition
        d[key1] = r.obj1
        d[key2] = r.obj2

        # Pop 1. item
        base_lrc = r._lrc
        local_ref = d.popitem()
        self.assertEqual(r._lrc, base_lrc, "The LRC should remain unchanged due to `local_ref`")
        local_ref = None
        self.assertEqual(r._lrc, base_lrc - lrc_for_key - 1)

        # Pop 2. item
        base_lrc = r._lrc
        local_ref = d.popitem()
        self.assertEqual(r._lrc, base_lrc, "The LRC should remain unchanged due to `local_ref`")
        local_ref = None
        self.assertEqual(r._lrc, base_lrc - lrc_for_key - 1)

        # Make sure the dict is empty
        with self.assertRaises(KeyError):
            d.popitem()

    def check_setdefault_new_key(self, key, lrc_for_key):
        """
        Checks that the `setdefault()` method of the dictionary correctly adjusts the LRC.
        This tests the case case then the key is not present in the set
        """
        class SomeObject:
            pass
        freeze(SomeObject())

        # Setup
        r = Region()
        r.obj = SomeObject()
        r.key = key
        d = {}
        base_lrc = r._lrc

        # Pre-condition
        self.assertGreater(base_lrc, 0)

        # Action: setdefault with non-existing key
        local_ref = d.setdefault(key, r.obj)

        # Post-condition: LRC should increase for the inserted key, value pair
        # plus the returned reference
        self.assertEqual(r._lrc, base_lrc + 1 + lrc_for_key + 1,
                         f"LRC should increase when setdefault inserts a new value")

        # Removing the local reference should only remove one LRC
        local_ref = None
        self.assertEqual(r._lrc, base_lrc + 1 + lrc_for_key)

    def check_setdefault_present_key(self, key, lrc_for_key):
        """
        Checks that the `setdefault()` method of the dictionary correctly adjusts the LRC.
        This tests the case case then the key is not present in the set
        """
        class SomeObject:
            pass
        freeze(SomeObject())

        # Setup
        r1 = Region()
        r1.obj = SomeObject()
        r1.key = key
        d = {}
        d[key] = r1.obj
        r1_base_lrc = r1._lrc

        r2 = Region()
        r2.unused_default = SomeObject()
        r2_base_lrc = r2._lrc

        # Action: setdefault with non-existing key
        local_ref = d.setdefault(key, r2.unused_default)

        # Post-condition
        self.assertEqual(r2._lrc, r2_base_lrc, "r2.unused_default should be unused")
        self.assertEqual(r1._lrc, r1_base_lrc + 1, "the new local ref should be tracked")
        local_ref = None
        self.assertEqual(r1._lrc, r1_base_lrc, "LRC should be reset")

class TestRegionDictUnicodeKeys(BaseTestRegionDictKeys):
    @unittest.expectedFailure # FIXME(regions): xFrednet: Broken until WBs in zip have been added
    def test_dict_construction_wip(self):
        self.check_dict_construction_value_lrc(
            lambda v1, v2, v3: dict(zip(["key1", "key2", "key3"], [v1, v2, v3])))

    def test_dict_construction(self):
        self.check_dict_construction_value_lrc(
            lambda v1, v2, v3: {"key1": v1, "key2": v2, "key3": v3})
        self.check_dict_construction_value_lrc(
            lambda v1, v2, v3: dict(key1=v1, key2=v2, key3=v3))
        self.check_dict_construction_value_lrc(
            lambda v1, v2, v3: dict([("key1", v1), ("key2", v2), ("key3", v3)]))
        self.check_dict_construction_value_lrc(
            lambda v1, v2, v3: dict({"key1": v1, "key2": v2, "key3": v3}))
        self.check_dict_construction_value_lrc(
            lambda v1, v2, v3: dict({"key1": v1, "key2": v2}, key3=v3))

    def test_wb_insert_empty(self):
        self.check_dict_assign({}, "some-key")

    def test_wb_insert_filled(self):
        self.check_dict_assign({"pre-filled": "dict"}, "some-key")

    def test_wb_replace(self):
        def replace(dict, key):
            dict[key] = None
        self.check_dict_remove_ref("ascii-key", replace)

    def test_wb_del(self):
        def del_key(dict, key):
            del dict[key]
        self.check_dict_remove_ref("ascii-key", del_key)

    def test_wb_clear(self):
        def clear(dict, key):
            dict.clear()
        self.check_dict_remove_ref("unicode <3 key", clear)

    def test_wb_subscript(self):
        self.check_dict_item_access("key", lambda dict, key: dict[key])

    def test_wb_get(self):
        self.check_dict_item_access("key", lambda dict, key: dict.get(key))

    def test_wb_get_default(self):
        self.check_dict_get_default("Default, IDK, just give me default")

    def test_wb_copy(self):
        self.check_dict_item_access("another key", lambda dict, key: dict.copy())

    def test_wb_pop(self):
        self.check_pop("Cool Key")

    def test_wb_popitem(self):
        self.check_popitem("Cool Key", "Best Key", lrc_for_key=0)

    def test_wb_keys_view(self):
        self.check_dict_view("K1", "K2", lambda d: d.keys(), loop_lrc_effect=0)

    def test_wb_values_view(self):
        self.check_dict_view("K1", "K2", lambda d: d.values(), loop_lrc_effect=1)

    def test_wb_items_view(self):
        # Python's dictionary iterator caches the tuple used during iteration.
        # This is good for performance, but means that the LRC doesn't
        # reset if we clear the loop variable.
        self.check_dict_view("K1", "K2", lambda d: d.items(), loop_lrc_effect=1, check_lrc_reset=False)

    def test_wb_iter_dict(self):
        self.check_dict_view("K1", "K2", lambda d: d, loop_lrc_effect=0)

    def test_wb_iter_dict_reversed(self):
        # The iterator doesn't effect the LRC, since we create it with the `reversed`
        self.check_dict_view("K1", "K2", lambda d: reversed(d), loop_lrc_effect=0, iter_lrc_cost=0)

    def test_wb_setdefault(self):
        self.check_setdefault_new_key("setdefault-key", lrc_for_key=0)
        self.check_setdefault_present_key("Meow", lrc_for_key=0)

    def test_unicode_key_clear_regression(self):
        r = Region()
        r.a = {"key": 21}
        r.a.clear()
        self.assertFalse(r.is_dirty)


class TestRegionDictObjectKeys(BaseTestRegionDictKeys):
    class Key:
        pass

    @classmethod
    def setUpClass(cls):
        freeze(cls.Key)

    def test_dict_construction_value_lrc(self):
        self.check_dict_construction_value_lrc(
            lambda v1, v2, v3: {self.Key(): v1, self.Key(): v2, self.Key(): v3})
        self.check_dict_construction_value_lrc(
            lambda v1, v2, v3: dict([(self.Key(), v1), (self.Key(), v2), (self.Key(), v3)]))
        self.check_dict_construction_value_lrc(
            lambda v1, v2, v3: dict({self.Key(): v1, self.Key(): v2, self.Key(): v3}))

    @unittest.expectedFailure # FIXME(regions): xFrednet: Broken until WBs in zip have been added
    def test_dict_construction_key_lrc_wip(self):
        self.check_dict_construction_value_lrc(
            lambda v1, v2, v3: dict(zip([self.Key(), self.Key(), self.Key()], [v1, v2, v3])))
        key1 = self.Key()
        key2 = self.Key()
        key3 = self.Key()
        self.check_dict_construction_key_lrc(key1, key2, key3,
            lambda v1, v2, v3: dict(zip([key1, key2, key3], [v1, v2, v3])))

    def test_dict_construction_key_lrc(self):
        key1 = self.Key()
        key2 = self.Key()
        key3 = self.Key()
        self.check_dict_construction_key_lrc(key1, key2, key3,
            lambda v1, v2, v3: {key1: v1, key2: v2, key3: v3})
        key1 = self.Key()
        key2 = self.Key()
        key3 = self.Key()
        self.check_dict_construction_key_lrc(key1, key2, key3,
            lambda v1, v2, v3: dict([(key1, v1), (key2, v2), (key3, v3)]))
        key1 = self.Key()
        key2 = self.Key()
        key3 = self.Key()
        self.check_dict_construction_key_lrc(key1, key2, key3,
            lambda v1, v2, v3: dict({key1: v1, key2: v2, key3: v3}))

    def test_wb_insert_empty(self):
        self.check_dict_assign({}, self.Key())
        self.check_dict_assign({}, self.Key())

    def test_wb_insert_filled(self):
        self.check_dict_assign({"pre-filled": "dict"}, self.Key())
        self.check_dict_assign({"pre-filled": "dict"}, self.Key())

    def test_wb_replace(self):
        def replace(dict, key):
            dict[key] = None
        self.check_dict_remove_ref(self.Key(), replace)

    def test_wb_del(self):
        def del_key(dict, key):
            del dict[key]
        self.check_dict_remove_ref(self.Key(), del_key)

    def test_wb_clear(self):
        def clear(dict, key):
            dict.clear()
        self.check_dict_remove_ref(self.Key(), clear)

    def test_wb_subscript(self):
        self.check_dict_item_access(self.Key(), lambda dict, key: dict[key])

    def test_wb_get(self):
        self.check_dict_item_access(self.Key(), lambda dict, key: dict.get(key))

    def test_wb_get_default(self):
        self.check_dict_get_default(self.Key())

    def test_wb_copy(self):
        # LRC increase of 2: 1x for the key 1x for the item
        self.check_dict_item_access(self.Key(), lambda dict, key: dict.copy(), lrc_offset = 2)

    def test_wb_pop(self):
        self.check_pop(self.Key())

    def test_wb_popitem(self):
        self.check_popitem(self.Key(), self.Key(), lrc_for_key=1)

    def test_wb_key_list(self):
        self.check_dict_item_access(self.Key(), lambda dict, key: list(dict))

    def test_wb_keys_view(self):
        self.check_dict_view(self.Key(), self.Key(), lambda d: d.keys(), 1)

    def test_wb_values_view(self):
        self.check_dict_view(self.Key(), self.Key(), lambda d: d.values(), 1)

    def test_wb_items_view(self):
        # Python's dictionary iterator caches the tuple used during iteration.
        # This is good for performance, but means that the LRC doesn't
        # reset if we clear the loop variable.
        self.check_dict_view(self.Key(), self.Key(), lambda d: d.items(), loop_lrc_effect=2, check_lrc_reset=False)

    def test_wb_iter_dict(self):
        self.check_dict_view(self.Key(), self.Key(), lambda d: d, loop_lrc_effect=1)

    def test_wb_iter_reversed_dict(self):
        # The iterator doesn't effect the LRC, since we create it with the `reversed`
        self.check_dict_view(self.Key(), self.Key(), lambda d: reversed(d), loop_lrc_effect=1, iter_lrc_cost=0)

    def test_wb_setdefault(self):
        self.check_setdefault_new_key(self.Key(), lrc_for_key=1)
        self.check_setdefault_present_key(self.Key(), lrc_for_key=1)


# FIXME(regions): xFrednet: Set operations on views, like `&`, `^` and `|`
#                           are currently not tested and probably don't work.

# TODO: classmethod fromkeys(iterable, value=None, /)
# TODO: dict.update(???)
# TODO: dict1 | dict2
# TODO: dict1 |= dict2
