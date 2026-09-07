from test.support import import_helper
import unittest

from .test_common import BaseNotFreezableTest

bz2 = import_helper.import_module('bz2')
from bz2 import BZ2Compressor, BZ2Decompressor


class TestBZ2Compressor(BaseNotFreezableTest):
    def test_not_freezable(self):
        self.check_not_freezable(BZ2Compressor())


class TestBZ2Decompressor(BaseNotFreezableTest):
    def test_not_freezable(self):
        self.check_not_freezable(BZ2Decompressor())


if __name__ == '__main__':
    unittest.main()
