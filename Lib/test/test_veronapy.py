import unittest

# This is a canary to check that global variables are not made immutable
# when others are made immutable
global_canary = {}

global0 = 0

global1 = 2
def global1_inc():
    global global1
    global1 += 1
    return global1

class MutableGlobalTest(unittest.TestCase):
    # Add initial test to confirm that global_canary is mutable
    def test_global_mutable(self):
        self.assertTrue(not isimmutable(global_canary))

class BaseObjectTest(unittest.TestCase):
    obj = None

    def setUp(self):
        makeimmutable(self.obj)

    def test_immutable(self):
        self.assertTrue(isimmutable(self.obj))

    def test_add_attribute(self):
        with self.assertRaises(NotWriteableError):
            self.obj.new_attribute = 'value'

    def test_type_immutable(self):
        self.assertTrue(isimmutable(type(self.obj)))


class TestBasicObject(BaseObjectTest):
    class C:
        pass

    obj = C()

class TestFloat(unittest.TestCase):
    def test_freeze_float(self):
        obj = 0.0
        makeimmutable(obj)
        self.assertTrue(isimmutable(obj))

class TestFloatType(unittest.TestCase):
    def test_float_type_immutable(self):
        obj = 0.0
        c = obj.__class__
        self.assertTrue(isimmutable(c))

class TestList(BaseObjectTest):
    class C:
        pass

    obj = [C(), C(), 1, "two", None]

    def test_set_item(self):
        with self.assertRaises(NotWriteableError):
            self.obj[0] = None

    def test_set_slice(self):
        with self.assertRaises(NotWriteableError):
            self.obj[1:3] = [None, None]

    def test_append(self):
        with self.assertRaises(NotWriteableError):
            self.obj.append(TestList.C())

    def test_extend(self):
        with self.assertRaises(NotWriteableError):
            self.obj.extend([TestList.C()])

    def test_insert(self):
        with self.assertRaises(NotWriteableError):
            self.obj.insert(0, TestList.C())

    def test_pop(self):
        with self.assertRaises(NotWriteableError):
            self.obj.pop()

    def test_remove(self):
        with self.assertRaises(NotWriteableError):
            self.obj.remove(1)

    def test_reverse(self):
        with self.assertRaises(NotWriteableError):
            self.obj.reverse()

    def test_clear(self):
        with self.assertRaises(NotWriteableError):
            self.obj.clear()

    def test_sort(self):
        with self.assertRaises(NotWriteableError):
            self.obj.sort()


class TestDict(BaseObjectTest):
    class C:
        pass

    obj = {1: C(), "two": C()}

    def test_set_item_exists(self):
        with self.assertRaises(NotWriteableError):
            self.obj[1] = None

    def test_set_item_new(self):
        with self.assertRaises(NotWriteableError):
            self.obj["three"] = TestDict.C()

    def test_del_item(self):
        with self.assertRaises(NotWriteableError):
            del self.obj[1]

    def test_clear(self):
        with self.assertRaises(NotWriteableError):
            self.obj.clear()

    def test_pop(self):
        with self.assertRaises(NotWriteableError):
            self.obj.pop(1)

    def test_popitem(self):
        with self.assertRaises(NotWriteableError):
            self.obj.popitem()

    def test_setdefault(self):
        with self.assertRaises(NotWriteableError):
            self.obj.setdefault("three", TestDict.C())

    def test_update(self):
        with self.assertRaises(NotWriteableError):
            self.obj.update({1: None})


class TestSet(BaseObjectTest):
    obj = {1, "two", None, True}

    def test_add(self):
        with self.assertRaises(NotWriteableError):
            self.obj.add(1)

    def test_clear(self):
        with self.assertRaises(NotWriteableError):
            self.obj.clear()

    def test_discard(self):
        with self.assertRaises(NotWriteableError):
            self.obj.discard(1)

    def test_pop(self):
        with self.assertRaises(NotWriteableError):
            self.obj.pop()

    def test_remove(self):
        with self.assertRaises(NotWriteableError):
            self.obj.remove(1)

    def test_update(self):
        with self.assertRaises(NotWriteableError):
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
        with self.assertRaises(NotWriteableError):
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

            return inc

        test = c()
        self.assertEqual(test(), 1)
        self.assertEqual(test(), 2)
        makeimmutable(test)
        self.assertRaises(NotWriteableError, test)

    def test_global(self):
        def d():
            global global0
            global0 += 1
            return global0

        self.assertEqual(d(), 1)
        makeimmutable(d)
        self.assertTrue(isimmutable(global0))
        self.assertFalse(isimmutable(global_canary))
        self.assertRaises(NotWriteableError, d)

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

    def test_global_fun(self):
        def d():
            return global1_inc()
        
        makeimmutable(d)
        self.assertTrue(isimmutable(global1))
        self.assertTrue(isimmutable(global1_inc))
        self.assertFalse(isimmutable(global_canary))
        self.assertRaises(NotWriteableError, d)


class TestMethods(unittest.TestCase):
    class C:
        def __init__(self):
            self.val = -1

        def a(self):
            return abs(self.val)

        def b(self, x):
            self.val = self.val + x

    def test_lambda(self):
        obj = TestMethods.C()
        obj.c = lambda x: pow(x, 2)
        makeimmutable(obj)
        self.assertTrue(isimmutable(TestMethods.C))
        self.assertTrue(isimmutable(pow))
        self.assertRaises(NotWriteableError, obj.b, 1)
        self.assertEqual(obj.c(2), 4)

    def test_method(self):
        obj = TestMethods.C()
        makeimmutable(obj)
        self.assertEqual(obj.a(), 1)
        self.assertTrue(isimmutable(abs))
        self.assertTrue(isimmutable(obj.val))
        self.assertRaises(NotWriteableError, obj.b, 1)


class TestLocals(unittest.TestCase):
    class C:
        def __init__(self):
            self.val = 0
        def a(self, locs):
            self.l = locs
    def test_locals(self):
        # Inner scope used to prevent locals() containing self,
        # and preventing the test updating state.
        def inner():
            obj = TestLocals.C()
            obj2 = TestLocals.C()
            l = locals()
            obj.a(l)
            obj3 = TestLocals.C()
            makeimmutable(obj)
            return obj, obj2, obj3
        obj, obj2, obj3 = inner()
        self.assertTrue(isimmutable(obj))
        self.assertTrue(isimmutable(obj2))
        self.assertFalse(isimmutable(obj3))

class TestWeakRef(unittest.TestCase):
    class B:
        pass
    
    class C:
        # Function that takes a object, and stores it in a weakref field.
        def __init__(self, obj):
            import weakref
            self.obj = weakref.ref(obj)
        def val(self):
            return self.obj()
        
    def test_weakref(self):
        obj = TestWeakRef.B()
        c = TestWeakRef.C(obj)
        makeimmutable(c)
        self.assertTrue(isimmutable(c))
        self.assertTrue(c.val() is obj)
        # Following line is not true in the current implementation
        # self.assertTrue(isimmutable(c.val()))
        self.assertFalse(isimmutable(c.val()))
        obj = None
        # Following line is not true in the current implementation
        # this means me can get a race on weak references 
        # self.assertTrue(c.val() is obj)
        self.assertIsNone(c.val())

# This test will make the Python environment unusable.
# Should perhaps forbid making the frame immutable.
# class TestStackCapture(unittest.TestCase):
#     def test_stack_capture(self):
#         import sys
#         x = {}
#         x["frame"] = sys._getframe()
#         makeimmutable(x)
#         self.assertTrue(isimmutable(x))
#         self.assertTrue(isimmutable(x["frame"]))


if __name__ == '__main__':
    unittest.main()
