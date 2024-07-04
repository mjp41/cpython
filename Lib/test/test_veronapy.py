import unittest

global0 = 0

class BaseObjectTest(unittest.TestCase):
    obj = None

    def setUp(self):
        makeimmutable(self.obj)

    def test_immutable(self):
        self.assertTrue(isimmutable(self.obj))

    def test_add_attribute(self):
        with self.assertRaises(TypeError):
            self.obj.new_attribute = 'value'

    def test_type_immutable(self):
        self.assertTrue(isimmutable(type(self.obj)))


class TestBasicObject(BaseObjectTest):
    class C:
        pass

    obj = C()


class TestList(BaseObjectTest):
    class C:
        pass

    obj = [C(), C(), 1, "two", None]

    def test_set_item(self):
        with self.assertRaises(TypeError):
            self.obj[0] = None

    def test_set_slice(self):
        with self.assertRaises(TypeError):
            self.obj[1:3] = [None, None]

    def test_append(self):
        with self.assertRaises(TypeError):
            self.obj.append(TestList.C())

    def test_extend(self):
        with self.assertRaises(TypeError):
            self.obj.extend([TestList.C()])

    def test_insert(self):
        with self.assertRaises(TypeError):
            self.obj.insert(0, TestList.C())

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
    class C:
        pass

    obj = {1: C(), "two": C()}

    def test_set_item_exists(self):
        with self.assertRaises(TypeError):
            self.obj[1] = None

    def test_set_item_new(self):
        with self.assertRaises(TypeError):
            self.obj["three"] = TestDict.C()

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
            self.obj.setdefault("three", TestDict.C())

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
    def setUp(self):
        class C:
            const = 1

        self.obj = C()
        self.obj.a = C()
        self.obj.a.b = "c"
        self.obj.d = [C(), None]
        self.obj.d[0].e = "f"
        self.obj.g = {1: C(), "two": C()}
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

    def test_set_const(self):
        with self.assertRaises(TypeError):
            self.obj.const = 1

    def test_type_immutable(self):
        self.assertTrue(isimmutable(type(self.obj)))
        self.assertTrue(isimmutable(type(self.obj).const))


class TestFunctions(unittest.TestCase):
    def setUp(self):
        def a():
            return 1

        self.obj = a
        makeimmutable(self.obj)

    def test_new_function(self):
        def b():
            return 1

        self.assertEqual(b(), 1)

    def test_nonlocal(self):
        def c():
            v = 0

            def inc():
                nonlocal v
                v += 1
                return v

            return inc()

        self.assertEqual(c(), 1)
        makeimmutable(c)
        self.assertRaises(TypeError, c)

    def test_global(self):
        def d():
            global global0
            global0 += 1
            return global0

        self.assertEqual(d(), 1)
        makeimmutable(d)
        self.assertTrue(isimmutable(global0))
        self.assertRaises(TypeError, d)

    def test_builtins(self):
        def e():
            test = list(range(5))
            return sum(test)

        makeimmutable(e)
        self.assertTrue(isimmutable(list))
        self.assertTrue(isimmutable(range))
        self.assertTrue(isimmutable(sum))

    def test_builtins_nested(self):
        def g():
            def nested_test():
                test = list(range(10))
                return sum(test)

            return nested_test()

        makeimmutable(g)
        self.assertTrue(isimmutable(list))
        self.assertTrue(isimmutable(range))
        self.assertTrue(isimmutable(sum))


if __name__ == '__main__':
    unittest.main()

