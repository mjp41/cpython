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
        self.assertTrue(isimmutable(obj))
        self.assertTrue(isimmutable(abs))
        self.assertTrue(isimmutable(obj.val))
        self.assertRaises(NotWriteableError, obj.b, 1)
        # Second test as the byte code can be changed by the first call
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
        makeimmutable(obj)
        self.assertTrue(isimmutable(obj))
        self.assertRaises(NotWriteableError, obj.set, 1)
        self.assertEqual(obj.get(), 0)

    def test_dict_mutation2(self):
        obj = TestDictMutation.C()
        obj.set(1)
        self.assertEqual(obj.get(), 1)
        makeimmutable(obj)
        self.assertEqual(obj.get(), 1)
        self.assertTrue(isimmutable(obj))
        self.assertRaises(NotWriteableError, obj.set, 1)

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

class TestRegionOwnership(unittest.TestCase):
    class A:
        pass

    def setUp(self):
        # This freezes A and super and meta types of A namely `type` and `object`
        makeimmutable(self.A)
        # FIXME: remove this line when the write barrier works
        makeimmutable(type({}))
        enableinvariant()

    def test_default_ownership(self):
        a = self.A()
        r = Region()
        self.assertFalse(r.owns_object(a))

    def test_add_ownership(self):
        a = self.A()
        r = Region()
        r.add_object(a)
        self.assertTrue(r.owns_object(a))

    def test_remove_ownership(self):
        a = self.A()
        r = Region()
        r.add_object(a)
        r.remove_object(a)
        self.assertFalse(r.owns_object(a))

    def test_add_ownership2(self):
        a = self.A()
        r1 = Region()
        r2 = Region()
        r1.add_object(a)
        self.assertFalse(r2.owns_object(a))

    def test_add_object_is_deep(self):
        # Create linked objects (a) -> (b) -> (c)
        a = self.A()
        b = self.A()
        c = self.A()
        a.b = b
        b.c = c

        # Create a region and take ownership of a
        r = Region()
        r.add_object(a)

        # Check that b was also moved into the region
        self.assertTrue(r.owns_object(a))
        self.assertTrue(r.owns_object(b))
        self.assertTrue(r.owns_object(c))

    def test_should_fail_add_ownership_twice_2(self):
        a = self.A()
        r1 = Region("r1")
        r1.add_object(a)
        r2 = Region("r2")
        try:
            r2.add_object(a)
        except RegionError as e:
            self.assertEqual(e.source, r2)
            self.assertEqual(e.target, a)
        else:
            self.fail("Should not reach here -- a can't be owned by two objects")

    def test_init_with_name(self):
        r1 = Region()
        r2 = Region("Super-name")
        self.assertTrue("Super-name" in repr(r2))

        r3_name = "Trevligt-Name"
        r3a = Region(r3_name)
        r3b = Region(r3_name)
        self.assertTrue(r3_name in repr(r3a))
        self.assertTrue(r3_name in repr(r3b))
        self.assertTrue(isimmutable(r3_name))

    def test_init_invalid_name(self):
        self.assertRaises(TypeError, Region, 42)

    def test_init_same_name(self):
        r1 = Region("Andy")
        r2 = Region("Andy")
        # Check that we reach the end of the test
        self.assertTrue(True)

    def test_region__dict__(self):
        r = Region()
        r.f = self.A()
        # The above line will fail unless the region has gotten a dict
        self.assertTrue(True)

    def test_object__dict__(self):
        r = Region()
        a = self.A()
        b = self.A()
        r.add_object(b)
        r.f = a
        a.f = b
        d = a.__dict__
        self.assertTrue(r.owns_object(d))
        self.assertTrue(r.owns_object(a))
        self.assertTrue(r.owns_object(b))

    def test_allow_bridge_object_ref(self):
        # Create linked objects (a) -> (b)
        a = self.A()
        b = Region("Child")
        a.b = b

        # Create a region and take ownership of a
        r = Region("Parent")
        r.add_object(a)
        self.assertFalse(r.owns_object(b))
        self.assertTrue(r.owns_object(a))

    def test_should_fail_external_uniqueness(self):
        a = self.A()
        r1 = Region("r1")
        # Two refs from the local region are allowed
        a.f = r1
        a.g = r1
        r2 = Region("r2")
        try:
            r2.add_object(a)
        except RegionError as e:
            # Check that the error is on the appropriate objects
            self.assertEqual(e.source, a)
            self.assertEqual(e.target, r1)
        else:
            self.fail("Should not reach here -- a can't be owned by two objects")

class TestTryCloseRegion(unittest.TestCase):
    class A:
        pass

    def setUp(self):
        # This freezes A and super and meta types of A namely `type` and `object`
        makeimmutable(self.A)
        # FIXME: remove this line when the write barrier works
        makeimmutable(type({}))

    def test_try_close_fail(self):
        a = self.A()
        r1 = Region("r1")
        
        # This creates a reference into the region
        r1.a = a
        self.assertFalse(r1.try_close())


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

global_test_dict = 0
class TestGlobalDictMutation(unittest.TestCase):
    def g():
        def f1():
            globals()["global_test_dict"] += 1
            return globals()["global_test_dict"]
        makeimmutable(f1)
        return f1

    def test_global_dict_mutation(self):
        f1 = TestGlobalDictMutation.g()
        self.assertTrue(isimmutable(f1))
        self.assertRaises(NotWriteableError, f1)

class TestPoolAllocation(unittest.TestCase):
    # If pooling does not reset region between allocations,
    # then the second call to f will result in `a` being owned by
    # the first region that no has been deallocated.  This
    # will result in a UAF that ASAN can detect.
    def test_pool_allocation(self):
        def f():
            r = Region()
            a = {}
            r.add_object(a)
        f()
        f()

class TestGenericAliasBug(unittest.TestCase):
    # The code inside generic alias attempts to set
    # __orig_class__ on the empty tuple, which is not
    # allowed. The make immutable means this can fail
    # NotWriteableError rather than the TypeError or
    # AttributeError that would be raised otherwise.
    def test_generic_alias_bug(self):
        c = makeimmutable(())
        tuple[int]()

if __name__ == '__main__':
    unittest.main()
