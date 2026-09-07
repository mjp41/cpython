"""Tests for freeze warnings when tp_reachable is missing.

Uses the _test_reachable C extension which provides two static types:

  HasTraverseNoReachable  – has tp_traverse, tp_reachable deliberately NULL
  NoTraverseNoReachable   – neither tp_traverse nor tp_reachable
"""
import subprocess
import sys
import textwrap
import unittest


class TestReachableWarnings(unittest.TestCase):
    """Test that freeze logs warnings when tp_reachable is missing."""

    def _run_code(self, code):
        """Run code in a subprocess and return (stdout, stderr)."""
        result = subprocess.run(
            [sys.executable, "-c", textwrap.dedent(code)],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout, result.stderr

    def test_warn_tp_traverse_no_tp_reachable(self):
        """Warn when a C type has tp_traverse but no tp_reachable."""
        stdout, stderr = self._run_code("""\
            import _immutable, _test_reachable
            obj = _test_reachable.HasTraverseNoReachable(42)
            _immutable.freeze(obj)
        """)
        self.assertIn(
            "freeze: type '_test_reachable.HasTraverseNoReachable' "
            "has tp_traverse but no tp_reachable",
            stderr,
        )

    def test_warn_no_traverse_no_reachable(self):
        """Warn when a C type has neither tp_traverse nor tp_reachable."""
        stdout, stderr = self._run_code("""\
            import _immutable, _test_reachable
            obj = _test_reachable.NoTraverseNoReachable()
            _immutable.freeze(obj)
        """)
        self.assertIn(
            "freeze: type '_test_reachable.NoTraverseNoReachable' "
            "has no tp_traverse and no tp_reachable",
            stderr,
        )

    def test_warn_only_once_per_type(self):
        """A type should only produce the warning on the first freeze."""
        stdout, stderr = self._run_code("""\
            import _immutable, _test_reachable
            _immutable.freeze(_test_reachable.HasTraverseNoReachable(1))
            _immutable.freeze(_test_reachable.HasTraverseNoReachable(2))
            _immutable.freeze(_test_reachable.HasTraverseNoReachable(3))
        """)
        msg = (
            "freeze: type '_test_reachable.HasTraverseNoReachable' "
            "has tp_traverse but no tp_reachable"
        )
        count = stderr.count(msg)
        self.assertEqual(count, 1, f"Expected 1 warning, got {count}:\n{stderr}")

    def test_warn_different_types_separately(self):
        """Different types should each produce their own warning."""
        stdout, stderr = self._run_code("""\
            import _immutable, _test_reachable
            _immutable.freeze(_test_reachable.HasTraverseNoReachable(1))
            _immutable.freeze(_test_reachable.NoTraverseNoReachable())
        """)
        self.assertIn("HasTraverseNoReachable", stderr)
        self.assertIn("NoTraverseNoReachable", stderr)

    def test_no_warning_with_tp_reachable(self):
        """No warning for a type that has tp_reachable set."""
        stdout, stderr = self._run_code("""\
            import _immutable, _test_reachable
            obj = _test_reachable.HasReachable(42)
            _immutable.freeze(obj)
        """)
        self.assertNotIn("HasReachable", stderr)


    def test_warn_tp_traverse_skips_type(self):
        """Warn when a C type has tp_traverse that doesn't visit the type"""
        stdout, stderr = self._run_code("""\
            import _immutable, _test_reachable
            obj = _test_reachable.IncorrectTraverseNoReachableHeap(42)
            _immutable.freeze(obj)
        """)
        self.assertIn(
            "freeze: type '_test_reachable.IncorrectTraverseNoReachableHeap' "
            "is not visiting the type during traversal",
            stderr,
        )

if __name__ == "__main__":
    unittest.main()
