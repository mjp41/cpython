import unittest

class TestRegionsDictObject(unittest.TestCase):
    def setUp(self):
        enableinvariant()

    def test_dict_insert_empty_dict(self):
        # Create Region with Empty dictionary
        r = Region()
        d = {}
        r.body = d
        n = {}
        # Add local object to region
        d["foo"] = n
        self.assertTrue(r.owns_object(n))

    def test_dict_insert_nonempty_dict(self):
        # Create Region with Nonempty dictionary
        r = Region()
        d = {}
        d["bar"] = 1
        r.body = d
        # Add local object to region
        n = {}
        d["foo"] = n
        self.assertTrue(r.owns_object(n))

    def test_dict_update_dict(self):
        # Create Region with Nonempty dictionary
        r = Region()
        d = {}
        n1 = {}
        d["foo"] = n1
        r.body = d
        # Update dictionary to contain a local object
        n2 = {}
        d["foo"] = n2
        self.assertTrue(r.owns_object(n2))

    def test_dict_clear(self):
        # Create Region with Nonempty dictionary
        r = Region()
        d = {}
        n = {}
        d["foo"] = n
        r.body = d
        # Clear dictionary
        d.clear()
        # As LRC is not checked by the invariant, this test cannot
        # check anything useful yet.

    def test_dict_copy(self):
        r = Region()
        d = {}
        r.body = d
        r2 = Region()
        d["foo"] = r2
        d.copy()

    def test_dict_setdefault(self):
        r = Region("outer")
        d = {}
        r.body = d
        r2 = Region("inner")
        d["foo"] = r2
        d.setdefault("foo", r2)
        self.assertRaises(RegionError, d.setdefault, "bar", r2)

    def test_dict_update(self):
        # Create a region containing two dictionaries
        r = Region()
        d = {}
        r.body = d
        d2 = {}
        r.body2 = d2
        # Add a contained region to the first dictionary
        d["reg"] = Region()
        # Update the second dictionary to contain the elements of the first
        self.assertRaises(RegionError, d2.update, d)
        self.assertRaises(RegionError, d2.update, d)