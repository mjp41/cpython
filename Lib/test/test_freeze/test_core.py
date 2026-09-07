import unittest
from immutable import freeze, is_frozen

from .test_common import BaseObjectTest


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
        self.assertTrue(not is_frozen(global_canary))


class TestBasicObject(BaseObjectTest):
    class C:
        pass

    def __init__(self, *args, **kwargs):
        BaseObjectTest.__init__(self, *args, obj=self.C(), **kwargs)


class TestFloat(unittest.TestCase):
    def test_freeze_float(self):
        obj = 0.0
        freeze(obj)
        self.assertTrue(is_frozen(obj))

class TestFloatType(unittest.TestCase):
    def test_float_type_immutable(self):
        obj = 0.0
        c = obj.__class__
        self.assertTrue(is_frozen(c))

class TestList(BaseObjectTest):
    class C:
        pass

    def __init__(self, *args, **kwargs):
        obj = [self.C(), self.C(), 1, "two", None]
        BaseObjectTest.__init__(self, *args, obj=obj, **kwargs)

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
            self.obj += [TestList.C()]

    def test_clear(self):
        with self.assertRaises(TypeError):
            self.obj.clear()

    def test_sort(self):
        with self.assertRaises(TypeError):
            self.obj.sort()


class TestDict(BaseObjectTest):
    class C:
        pass

    def __init__(self, *args, **kwargs):
        obj = {1: self.C(), "two": self.C()}
        BaseObjectTest.__init__(self, *args, obj=obj, **kwargs)

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
    def __init__(self, *args, **kwargs):
        obj = {1, "two", None, True}
        BaseObjectTest.__init__(self, *args, obj=obj, **kwargs)

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
        freeze(self.obj)

    def test_immutable(self):
        self.assertTrue(is_frozen(self.obj))
        self.assertTrue(is_frozen(self.obj.a))
        self.assertTrue(is_frozen(self.obj.a.b))
        self.assertTrue(is_frozen(self.obj.d))
        self.assertTrue(is_frozen(self.obj.d[0]))
        self.assertTrue(is_frozen(self.obj.d[0].e))
        self.assertTrue(is_frozen(self.obj.g))
        self.assertTrue(is_frozen(self.obj.g[1]))
        self.assertTrue(is_frozen(self.obj.g[1].h))
        self.assertTrue(is_frozen(self.obj.g["two"]))
        self.assertTrue(is_frozen(self.obj.g["two"].i))

    def test_set_const(self):
        with self.assertRaises(TypeError):
            self.obj.const = 1

    def test_type_immutable(self):
        self.assertTrue(is_frozen(type(self.obj)))
        self.assertTrue(is_frozen(type(self.obj).const))


class TestFunctions(unittest.TestCase):
    def setUp(self):
        def a():
            return 1

        self.obj = a
        freeze(self.obj)

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
        freeze(test)
        self.assertRaises(TypeError, test)

    def test_nonlocal_changed(self):
        v = 0
        def c():
            nonlocal v
            v += 1

            def inc():
                return v + 1

            return inc

        test = c()
        self.assertEqual(test(), 2)
        test = c()
        self.assertEqual(test(), 3)
        freeze(test)
        v = 5
        self.assertEqual(test(), 3)

    def test_global(self):
        def d():
            global global0
            global0 += 1
            return global0

        self.assertEqual(d(), 1)
        freeze(d)
        self.assertTrue(is_frozen(global0))
        self.assertFalse(is_frozen(global_canary))
        self.assertRaises(TypeError, d)

    def test_hidden_global(self):
        global global0
        def hide_access():
            global global0
            global0 += 1
            return global0
        def d():
            return hide_access()
        global0 = 0
        self.assertEqual(d(), 1)
        freeze(d)
        self.assertRaises(TypeError, d)

    def test_builtins(self):
        def e():
            test = list(range(5))
            return sum(test)

        freeze(e)
        self.assertTrue(is_frozen(list))
        self.assertTrue(is_frozen(range))
        self.assertTrue(is_frozen(sum))

    def test_builtins_nested(self):
        def g():
            def nested_test():
                test = list(range(10))
                return sum(test)

            return nested_test()

        freeze(g)
        self.assertTrue(is_frozen(list))
        self.assertTrue(is_frozen(range))
        self.assertTrue(is_frozen(sum))

    def test_global_fun(self):
        def d():
            return global1_inc()

        freeze(d)
        self.assertTrue(is_frozen(global1))
        self.assertTrue(is_frozen(global1_inc))
        self.assertFalse(is_frozen(global_canary))
        self.assertRaises(TypeError, d)

    def test_globals_copy(self):
        def f():
            global global0
            ref_1 = global0
            ref_2 = global0
            return global0

        expected = f()
        freeze(f)
        self.assertEqual(f(), expected)
        global0 = 10
        self.assertEqual(f(), expected)


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
        freeze(obj)
        self.assertTrue(is_frozen(TestMethods.C))
        self.assertTrue(is_frozen(pow))
        self.assertRaises(TypeError, obj.b, 1)
        self.assertEqual(obj.c(2), 4)

    def test_method(self):
        obj = TestMethods.C()
        freeze(obj)
        self.assertEqual(obj.a(), 1)
        self.assertTrue(is_frozen(obj))
        self.assertTrue(is_frozen(abs))
        self.assertTrue(is_frozen(obj.val))
        self.assertRaises(TypeError, obj.b, 1)
        # Second test as the byte code can be changed by the first call
        self.assertRaises(TypeError, obj.b, 1)


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
            freeze(obj)
            return obj, obj2, obj3
        obj, obj2, obj3 = inner()
        self.assertTrue(is_frozen(obj))
        self.assertTrue(is_frozen(obj2))
        self.assertFalse(is_frozen(obj3))

class TestDictMutation(unittest.TestCase):
    class C:
        def __init__(self):
            self.x = 0

        def get(self):
            return self.x

        def set(self, x):
            d = self.__dict__
            d['x'] = x

    def test_dict_mutation(self):
        obj = TestDictMutation.C()
        freeze(obj)
        self.assertTrue(is_frozen(obj))
        self.assertRaises(TypeError, obj.set, 1)
        self.assertEqual(obj.get(), 0)

    def test_dict_mutation2(self):
        obj = TestDictMutation.C()
        obj.set(1)
        self.assertEqual(obj.get(), 1)
        freeze(obj)
        self.assertEqual(obj.get(), 1)
        self.assertTrue(is_frozen(obj))
        self.assertRaises(TypeError, obj.set, 1)

    def test_dict_mutation3(self):
        obj = TestDictMutation.C()
        d = obj.__dict__
        freeze(d)
        # Should obj be frozen?
        # self.assertTrue(is_frozen(obj))
        # The following line should raise an exception, as we are trying to mutate the dict
        with self.assertRaises(TypeError):
            obj.f = 1

    def test_dict_mutation4(self):
        obj = TestDictMutation.C()
        d = obj.__dict__
        def step():
            d['f'] = 1
        # Cause function to be optimised
        step()
        step()
        step()
        freeze(d)
        with self.assertRaises(TypeError):
            step()

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
        freeze(c)
        self.assertTrue(is_frozen(c))
        self.assertTrue(c.val() is obj)
        self.assertTrue(is_frozen(c.val()))
        obj = None
        # The reference should remain as it was reachable through a frozen weakref.
        self.assertTrue(c.val() is not None)

    # Thread safety of weakrefs is tested in test_freeze/test_weakref.py

class TestStackCapture(unittest.TestCase):
     def test_stack_capture(self):
         import sys
         x = {}
         x["frame"] = sys._getframe()
         freeze(x)
         self.assertTrue(is_frozen(x))
         self.assertTrue(is_frozen(x["frame"]))


class TestSubclass(unittest.TestCase):
    def test_subclass(self):
        class C:
            def __init__(self, val):
                self.val = val

            def a(self):
                return self.val

            def b(self, val):
                self.val = val

        c_obj = C(1)
        freeze(c_obj)
        self.assertTrue(is_frozen(c_obj))
        self.assertTrue(is_frozen(C))
        class D(C):
            def __init__(self, val):
                super().__init__(val)
                self.val2 = val * 2

            def b(self):
                return self.val2

            def c(self, val):
                self.val = val

        d_obj = D(1)
        self.assertEqual(d_obj.a(), 1)
        self.assertEqual(d_obj.b(), 2)
        self.assertTrue(isinstance(d_obj, C))
        self.assertTrue(issubclass(D, C))

class TestImport(unittest.TestCase):
    def test_import(self):
        def f():
            # immutable objects are not allowed to import
            # modules. This will result in an ImportError.
            from . import mock
            return mock.a

        freeze(f)

        with self.assertRaises(ImportError):
            f()


class TestFunctionAttributes(unittest.TestCase):
    def test_function_attributes(self):
        def f():
            pass

        freeze(f)

        with self.assertRaises(TypeError):
            f.__annotations__ = {}

        with self.assertRaises(TypeError):
            f.__annotations__["foo"] = 2

        with self.assertRaises(TypeError):
            f.__builtins__ = {}

        with self.assertRaises(TypeError):
            f.__builtins__["foo"] = 2

        with self.assertRaises(TypeError):
            def g():
                pass
            f.__code__ = g.__code__

        with self.assertRaises(TypeError):
            f.__defaults__ = (1,2)

        with self.assertRaises(TypeError):
            f.__dict__ = {}

        with self.assertRaises(TypeError):
            f.__dict__["foo"] = {}

        with self.assertRaises(TypeError):
            f.__doc__ = "foo"

        with self.assertRaises(TypeError):
            f.__globals__ = {}

        with self.assertRaises(TypeError):
            f.__globals__["foo"] = 2

        with self.assertRaises(TypeError):
            f.__kwdefaults__ = {}

        with self.assertRaises(TypeError):
            f.__module__ = "foo"

        with self.assertRaises(TypeError):
            f.__name__ = "foo"

        with self.assertRaises(TypeError):
            f.__qualname__ = "foo"

        with self.assertRaises(TypeError):
            f.__type_params__ = (1,2)

class TestFunctionDefaults(unittest.TestCase):
    def test_function_defaults(self):
        bdef = {}
        def f(b=bdef):
            return b

        freeze(f)

        self.assertTrue(is_frozen(bdef))

    def test_function_kwdefaults(self):
        bdef = {}
        def f(a, **b):
            return a, b
        f.__kwdefaults__ = bdef

        freeze(f)

        self.assertTrue(is_frozen(bdef))


class TestInheritFromCType(unittest.TestCase):
    class C(list):
        pass

    def test_inherit_from_list(self):
        obj = TestInheritFromCType.C()
        freeze(obj)

if __name__ == '__main__':
    unittest.main()
