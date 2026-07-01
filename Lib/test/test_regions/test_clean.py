from unittest import expectedFailure
import unittest
from regions import Region, is_local

class TestCleanRegion(unittest.TestCase):
    def mark_region_as_dirty(self, region: Region):
        region._make_dirty()

        self.assertTrue(region.is_dirty, "Region should be dirty here")

    def test_clean_marks_region_as_clean(self):
        region = Region()

        self.mark_region_as_dirty(region)
        cleaned = region.clean()

        self.assertFalse(region.is_dirty)
        self.assertEqual(cleaned, 1)

    @expectedFailure # More types need to be migrated to succeed again
    def test_clean_ignores_clean_subregions(self):
        region = Region()
        self.mark_region_as_dirty(region)
        region.sub = Region()
        detached_object = {}
        region.sub.obj = detached_object
        region.sub.obj = None

        # Precondition
        self.assertFalse(region.sub.is_dirty, "The subregion should be clean")
        self.assertTrue(region.sub.owns(detached_object))

        # Action - Only dirty regions should be effected by this clean call
        cleaned = region.clean()

        # Postcondition
        self.assertEqual(cleaned, 1)
        self.assertFalse(region.is_dirty, "The parent region should be cleaned")
        self.assertTrue(region.sub.owns(detached_object), "The subregion should remain uncleaned")

    def test_cleaning_also_cleans_dirty_subregion(self):
        region = Region()
        region.sub = Region()
        self.mark_region_as_dirty(region)
        self.mark_region_as_dirty(region.sub)

        sub = region.sub

        # Cleaning a dirty parent region should clean the child as well
        cleaned = region.clean()

        # The regions should now be clean and the LRC should be correct
        self.assertEqual(cleaned, 2)
        self.assertFalse(sub.is_dirty)
        self.assertEqual(sub._lrc, 1, "The sub region should only have an LRC of 1")
        self.assertEqual(sub._osc, 0, "No subregions should be present")
        self.assertEqual(region._osc, 1)

        # Removing the reference into sub, should close it and inform the parent
        sub = None
        self.assertEqual(region._osc, 0)

    def test_cleaning_finds_dirty_subregion(self):
        region = Region()
        region.sub = Region()
        region.sub.sub = Region()
        detached_object = {}
        region.sub.sub.obj = detached_object
        region.sub.sub.obj = None
        self.mark_region_as_dirty(region.sub.sub)

        # Precondition
        self.assertFalse(region.is_dirty, "The region should be clean")
        self.assertFalse(region.sub.is_dirty, "The subregion should be clean")
        self.assertTrue(region.sub.sub.owns(detached_object))

        # Action: Clean should find the dirty subsubregion
        cleaned = region.clean()

        # Postcondition
        self.assertEqual(cleaned, 1)
        self.assertTrue(is_local(detached_object))
        self.assertFalse(region.sub.sub.is_dirty, "The subsubregion should be clean")

    def test_clean_removes_unreachable(self):
        region = Region()
        obj = {}
        region.x = obj
        region.x = None

        # `region` should remain the owner of obj
        self.assertTrue(region.owns(obj))

        # Make the region dirty and clean it
        self.mark_region_as_dirty(region)
        region.clean()

        # Clean should have kicked `obj` from the region since it is no
        # longer reachable from the bridge object
        self.assertFalse(region.owns(obj))
        self.assertTrue(is_local(obj))

    def test_clean_keeps_name(self):
        region = Region("Marlin")

        self.mark_region_as_dirty(region)
        region.clean()

        self.assertEqual(region.name, "Marlin")
