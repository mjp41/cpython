from gc import collect
import unittest
from immutable import freeze

class GCInteropTest(unittest.TestCase):
  def test_collect(self):
    # Make an object
    a = {}
    # Change generation
    collect()
    # Freeze it
    freeze(a)
    # f
    collect()
