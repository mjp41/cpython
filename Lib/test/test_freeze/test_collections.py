from collections import defaultdict, deque
from immutable import freeze

from .test_common import BaseObjectTest


class TestDeque(BaseObjectTest):
    class C:
        pass

    def __init__(self, *args, **kwargs):
        obj = deque([self.C(), self.C(), 1, "two", None])
        BaseObjectTest.__init__(self, *args, obj=obj, **kwargs)

    def test_set_item(self):
        with self.assertRaises(TypeError):
            self.obj[0] = None

    def test_append(self):
        with self.assertRaises(TypeError):
            self.obj.append(TestDeque.C())

    def test_appendleft(self):
        with self.assertRaises(TypeError):
            self.obj.appendleft(TestDeque.C())

    def test_extend(self):
        with self.assertRaises(TypeError):
            self.obj.extend([TestDeque.C()])

    def test_extendleft(self):
        with self.assertRaises(TypeError):
            self.obj.extendleft([TestDeque.C()])

    def test_insert(self):
        with self.assertRaises(TypeError):
            self.obj.insert(0, TestDeque.C())

    def test_pop(self):
        with self.assertRaises(TypeError):
            self.obj.pop()

    def test_popleft(self):
        with self.assertRaises(TypeError):
            self.obj.popleft()

    def test_remove(self):
        with self.assertRaises(TypeError):
            self.obj.remove(1)

    def test_delete(self):
        with self.assertRaises(TypeError):
            del self.obj[0]

    def test_inplace_repeat(self):
        with self.assertRaises(TypeError):
            self.obj *= 2

    def test_inplace_concat(self):
        with self.assertRaises(TypeError):
            self.obj += [TestDeque.C()]

    def test_reverse(self):
        with self.assertRaises(TypeError):
            self.obj.reverse()

    def test_rotate(self):
        with self.assertRaises(TypeError):
            self.obj.rotate(1)

    def test_clear(self):
        with self.assertRaises(TypeError):
            self.obj.clear()

    def test_iter(self):
        it = iter(self.obj)
        with self.assertRaises(TypeError):
            freeze(it)

    def test_reviter(self):
        it = reversed(self.obj)
        with self.assertRaises(TypeError):
            freeze(it)


class TestDefaultDict(BaseObjectTest):
    def __init__(self, *args, **kwargs):
        s = [('yellow', 1), ('blue', 2), ('yellow', 3), ('blue', 4), ('red', 1)]
        obj = defaultdict(list)
        for k, v in s:
            obj[k].append(v)
        BaseObjectTest.__init__(self, *args, obj=obj, **kwargs)

    def test_set_item_exists(self):
        with self.assertRaises(TypeError):
            self.obj[1] = None

    def test_set_item_new(self):
        with self.assertRaises(TypeError):
            self.obj["three"] = 5

    def test_del_item(self):
        with self.assertRaises(TypeError):
            del self.obj[1]

    def test_clear(self):
        with self.assertRaises(TypeError):
            self.obj.clear()

    def test_pop(self):
        with self.assertRaises(TypeError):
            self.obj.pop(1)

    def test_popitem(self):
        with self.assertRaises(TypeError):
            self.obj.popitem()

    def test_update(self):
        with self.assertRaises(TypeError):
            self.obj.update({1: None})
