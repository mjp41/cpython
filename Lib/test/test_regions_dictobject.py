import unittest

class TestRegionsDictObject(unittest.TestCase):
    def setUp(self):
        enableinvariant()

    def test_dict_insert_empty_dict(self):
        # Create Region with Empty dictionary
        r = Region()
        d = {}
        r.add_object(d)
        n = {}
        # Add local object to region
        d["foo"] = n
        self.assertTrue(r.owns_object(n))

    def test_dict_insert_nonempty_dict(self):
        # Create Region with Nonempty dictionary
        r = Region()
        d = {}
        d["bar"] = 1
        r.add_object(d)
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
        r.add_object(d)
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
        r.add_object(d)
        # Clear dictionary
        d.clear()
        # As LRC is not checked by the invariant, this test cannot
        # check anything useful yet.
