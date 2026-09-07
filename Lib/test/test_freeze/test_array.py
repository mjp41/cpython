from array import array
from .test_common import BaseObjectTest


class TestArray(BaseObjectTest):
    def __init__(self, *args, **kwargs):
        obj = array('i', [1, 2, 3, 4])
        BaseObjectTest.__init__(self, *args, obj=obj, **kwargs)

    def test_set_item(self):
        with self.assertRaises(TypeError):
            self.obj[0] = 5

    def test_set_slice(self):
        with self.assertRaises(TypeError):
            self.obj[1:3] = [6, 7]

    def test_append(self):
        with self.assertRaises(TypeError):
            self.obj.append(8)

    def test_extend(self):
        with self.assertRaises(TypeError):
            self.obj.extend(array('i', [9]))

    def test_insert(self):
        with self.assertRaises(TypeError):
            self.obj.insert(0, 10)

    def test_pop(self):
        with self.assertRaises(TypeError):
            self.obj.pop()

    def test_remove(self):
        with self.assertRaises(TypeError):
            self.obj.remove(1)

    def test_delete(self):
        with self.assertRaises(TypeError):
            del self.obj[0]

    def test_reverse(self):
        with self.assertRaises(TypeError):
            self.obj.reverse()

    def test_inplace_repeat(self):
        with self.assertRaises(TypeError):
            self.obj *= 2

    def test_inplace_concat(self):
        with self.assertRaises(TypeError):
            self.obj += array('i', [11])

    def test_byteswap(self):
        with self.assertRaises(TypeError):
            self.obj.byteswap()
