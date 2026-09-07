import unittest
from test.support import import_helper
from immutable import is_frozen

from .test_common import BaseObjectTest


ctypes = import_helper.import_module('ctypes')


class TestCharArray(BaseObjectTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=ctypes.create_string_buffer(b"hello"), **kwargs)

    def test_raw(self):
        with self.assertRaises(TypeError):
            self.obj.raw = b"world"

        self.assertEqual(self.obj.raw, b"hello\x00")

    def test_value(self):
        with self.assertRaises(TypeError):
            self.obj.value = b"world"

        self.assertEqual(self.obj.value, b"hello")


class TestWCharArray(BaseObjectTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=ctypes.create_unicode_buffer("hello"), **kwargs)

    def test_value(self):
        with self.assertRaises(TypeError):
            self.obj.value = "world"

        self.assertEqual(self.obj.value, "hello")


class TestStructure(BaseObjectTest):
    class POINT(ctypes.Structure):
        _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int)]

    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=TestStructure.POINT(1, 2), **kwargs)

    def test_modify_field(self):
        with self.assertRaises(TypeError):
            self.obj.x = 3

        self.assertEqual(self.obj.x, 1)


class TestPointer(BaseObjectTest):
    class POINT(ctypes.Structure):
        _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int)]

    def __init__(self, *args, **kwargs):
        self.a = TestPointer.POINT(1, 2)
        super().__init__(*args, obj=ctypes.pointer(self.a), **kwargs)

    def test_contents_immutable(self):
        self.assertTrue(is_frozen(self.a))
        self.assertTrue(is_frozen(TestPointer.POINT))

    def test_set_contents(self):
        b = TestPointer.POINT(3, 4)
        with self.assertRaises(TypeError):
            self.obj.contents = b

        self.assertEqual(self.obj.contents.x, self.a.x)
        self.assertEqual(self.obj.contents.y, self.a.y)


class TestArray(BaseObjectTest):
    class POINT(ctypes.Structure):
        _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int)]

    def __init__(self, *args, **kwargs):
        TenPointsArrayType = TestArray.POINT * 10
        super().__init__(*args, obj=TenPointsArrayType(), **kwargs)

    def test_point_immutable(self):
        self.assertTrue(is_frozen(self.obj[0]))
        self.assertTrue(is_frozen(TestArray.POINT))

    def test_modify_item(self):
        with self.assertRaises(TypeError):
            self.obj[0].x = 1

        self.assertEqual(self.obj[0].x, 0)

    def test_ass_item(self):
        with self.assertRaises(TypeError):
            self.obj[0] = TestArray.POINT(1, 2)

    def test_ass_subscript(self):
        TwoPointsArrayType = TestArray.POINT * 2
        a = TwoPointsArrayType()
        with self.assertRaises(TypeError):
            self.obj[:2] = a


class TestUnion(BaseObjectTest):
    class INTPARTS(ctypes.Union):
        class SHORTS(ctypes.Structure):
            _fields_ = [("high", ctypes.c_short),
                        ("low", ctypes.c_short)]

        _fields_ = [("parts", SHORTS),
                    ("value", ctypes.c_int)]

    def __init__(self, *args, **kwargs):
        a = TestUnion.INTPARTS()
        a.value = ctypes.c_int(0x00FF00FF)
        super().__init__(*args, obj=a, **kwargs)

    def test_assign_part(self):
        with self.assertRaises(TypeError):
            self.obj.parts.high = 0

        self.assertEqual(self.obj.parts.high, 0xFF)

        with self.assertRaises(TypeError):
            self.obj.parts.low = 0

        self.assertEqual(self.obj.parts.low, 0xFF)
        self.assertEqual(self.obj.value, 0x00FF00FF)

    def test_assign_value(self):
        with self.assertRaises(TypeError):
            self.obj.value = 0x00FF00FF

        self.assertEqual(self.obj.value, 0x00FF00FF)
        self.assertEqual(self.obj.parts.high, 0xFF)
        self.assertEqual(self.obj.parts.low, 0xFF)


if __name__ == '__main__':
    unittest.main()
