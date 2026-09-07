import unittest
from struct import Struct

from .test_common import BaseObjectTest


class TestStruct(BaseObjectTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=Struct("i"), **kwargs)


if __name__ == "__main__":
    unittest.main()
