import sys
import unittest

from immutable import freeze, is_frozen, ImmutableModule

class TestModuleProxy(unittest.TestCase):
    def setUp(self):
        sys.modules.pop("random", None)
        sys.mut_modules.pop("random", None)

    def test_freeze_function_with_random_module_creates_proxy(self):
        import random
        self.assertFalse(is_frozen(random))

        def coin():
            return random.random()

        freeze(coin)

        captured_random = coin.__closure__[0].cell_contents
        self.assertTrue(is_frozen(captured_random))
        self.assertTrue(is_frozen(random))
        self.assertIsInstance(captured_random, ImmutableModule)
        self.assertIsInstance(random, ImmutableModule)

        self.assertIn("random", sys.mut_modules)
        mut_random = sys.mut_modules["random"]
        self.assertIsNot(mut_random, captured_random)
        self.assertFalse(is_frozen(mut_random))
        self.assertIsInstance(mut_random, sys.__class__)

    def test_random_state_remains_mutable_via_proxy(self):
        import random
        self.assertFalse(is_frozen(random))

        def coin():
            return random.random()

        freeze(coin)

        random.seed(42)
        first = coin()
        second = coin()

        random.seed(42)
        self.assertEqual(first, coin())
        self.assertEqual(second, coin())

    def test_proxy_attribute_writes_delegate_to_mutable_module(self):
        import random
        self.assertFalse(is_frozen(random))

        def coin():
            return random.random()

        freeze(coin)

        proxy_random = coin.__closure__[0].cell_contents
        mut_random = sys.mut_modules["random"]
        attr = "_freeze_proxy_canary"

        had_original = hasattr(mut_random, attr)
        original = getattr(mut_random, attr, None)
        try:
            proxy_random._freeze_proxy_canary = 42
            self.assertEqual(mut_random._freeze_proxy_canary, 42)
        finally:
            if had_original:
                setattr(mut_random, attr, original)
            elif hasattr(mut_random, attr):
                delattr(mut_random, attr)

    def test_proxy_reimport(self):
        import random
        self.assertFalse(is_frozen(random))

        freeze(random)

        # Unimport "random"
        old_mut_random = sys.mut_modules["random"]
        del sys.mut_modules["random"]

        # This should reimport the module
        random.random()



if __name__ == '__main__':
    unittest.main()
