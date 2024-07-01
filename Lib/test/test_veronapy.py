import unittest

class BaseObjectTest(unittest.TestCase):
    obj = None

    def setUp(self):
        makeimmutable(self.obj)
    
    def test_immutable(self):
        self.assertTrue(isimmutable(self.obj))

    def test_add_attribute(self):
        with self.assertRaises(TypeError):
            self.obj.new_attribute = 'value'

class BasicObject:
    pass

class TestBasicObject(BaseObjectTest):
    obj = BasicObject()

class TestList(BaseObjectTest):
    obj = [BasicObject(), BasicObject(), 1, "two", None]

    def test_set_item(self):
        with self.assertRaises(TypeError):
            self.obj[0] = None
    
    def test_set_slice(self):
        with self.assertRaises(TypeError):
            self.obj[1:3] = [None, None]
    
    def test_append(self):
        with self.assertRaises(TypeError):
            self.obj.append(BasicObject())
    
    def test_extend(self):
        with self.assertRaises(TypeError):
            self.obj.extend([BasicObject()])
    
    def test_insert(self):
        with self.assertRaises(TypeError):
            self.obj.insert(0, BasicObject())
    
    def test_pop(self):
        with self.assertRaises(TypeError):
            self.obj.pop()
    
    def test_remove(self):
        with self.assertRaises(TypeError):
            self.obj.remove(1)
    
    def test_reverse(self):
        with self.assertRaises(TypeError):
            self.obj.reverse()
    
    def test_clear(self):
        with self.assertRaises(TypeError):
            self.obj.clear()
        
    def test_sort(self):
        with self.assertRaises(TypeError):
            self.obj.sort()

class TestDict(BaseObjectTest):
    obj = {1: BasicObject(), "two": BasicObject()}

    def test_set_item_exists(self):
        with self.assertRaises(TypeError):
            self.obj[1] = None
    
    def test_set_item_new(self):
        with self.assertRaises(TypeError):
            self.obj["three"] = BasicObject()
    
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
    
    def test_setdefault(self):
        with self.assertRaises(TypeError):
            self.obj.setdefault("three", BasicObject())
    
    def test_update(self):
        with self.assertRaises(TypeError):
            self.obj.update({1: None})

class TestSet(BaseObjectTest):
    obj = {1, "two", None, True}

    def test_add(self):
        with self.assertRaises(TypeError):
            self.obj.add(1)
    
    def test_clear(self):
        with self.assertRaises(TypeError):
            self.obj.clear()
    
    def test_discard(self):
        with self.assertRaises(TypeError):
            self.obj.discard(1)
    
    def test_pop(self):
        with self.assertRaises(TypeError):
            self.obj.pop()
    
    def test_remove(self):
        with self.assertRaises(TypeError):
            self.obj.remove(1)
    
    def test_update(self):
        with self.assertRaises(TypeError):
            self.obj.update([1, 2])

class TestMultiLevel(unittest.TestCase):
    obj = BasicObject()

    def setUp(self):
        self.obj.a = BasicObject()
        self.obj.a.b = "c"
        self.obj.d = [BasicObject(), None]
        self.obj.d[0].e = "f"
        self.obj
        self.obj.g = {1: BasicObject(), "two": BasicObject()}
        self.obj.g[1].h = True
        self.obj.g["two"].i = False
        makeimmutable(self.obj)

    def test_immutable(self):
        self.assertTrue(isimmutable(self.obj))
        self.assertTrue(isimmutable(self.obj.a))
        self.assertTrue(isimmutable(self.obj.a.b))
        self.assertTrue(isimmutable(self.obj.d))
        self.assertTrue(isimmutable(self.obj.d[0]))
        self.assertTrue(isimmutable(self.obj.d[0].e))
        self.assertTrue(isimmutable(self.obj.g))
        self.assertTrue(isimmutable(self.obj.g[1]))
        self.assertTrue(isimmutable(self.obj.g[1].h))
        self.assertTrue(isimmutable(self.obj.g["two"]))
        self.assertTrue(isimmutable(self.obj.g["two"].i))


if __name__ == '__main__':
    unittest.main()
