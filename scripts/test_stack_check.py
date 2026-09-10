import unittest

from check_recipe_stack import frame_size


class StackCheckTests(unittest.TestCase):
    def test_ignores_nested_lambda_and_checks_largest_compiler_clone(self):
        output = """00000000 <Store::prepare(String const&)::{lambda()#1}::operator()() const>:
   0: 004136 entry a1, 32
00000000 <Store::prepare(String const&)>:
   0: 004136 entry a1, 0x3c0
00000000 <Store::prepare(String const&) [clone .constprop.0]>:
   0: 004136 entry a1, 1024
"""
        self.assertEqual(frame_size(output, "Store::prepare"), 1024)
        with self.assertRaises(ValueError):
            frame_size(output, "Store::missing")


if __name__ == "__main__":
    unittest.main()
