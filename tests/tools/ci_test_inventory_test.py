"""回归 Windows 部分构建后 CTest 仍能运行少量测试的失败模式。"""
import importlib.util
from pathlib import Path
import tempfile
import unittest


script = Path(__file__).resolve().parents[2] / ".github/scripts/verify-ci-tests.py"
spec = importlib.util.spec_from_file_location("ci_inventory", script)
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)


class CiTestInventoryTest(unittest.TestCase):
    def test_all_targets_discovered(self):
        with tempfile.TemporaryDirectory() as directory:
            tests = []
            for name in inventory.EXPECTED:
                executable = Path(directory) / (name + ".exe")
                executable.touch()
                tests.append({"name": name + ".Case", "command": [str(executable)]})
            counts = inventory.validate({"tests": tests})
            self.assertEqual(set(counts), inventory.EXPECTED)
            # 模拟 PRE_TEST 将未构建的目标排除在 ci 标签之外：已有测试全绿也不能通过。
            with self.assertRaisesRegex(ValueError, "absent"):
                inventory.validate({"tests": tests[:2]})
            Path(tests[-1]["command"][0]).unlink()
            with self.assertRaisesRegex(ValueError, "Missing executable"):
                inventory.validate({"tests": tests})

    def test_empty_or_not_built_inventory_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "absent"):
            inventory.validate({"tests": []})
        with self.assertRaisesRegex(ValueError, "Missing executable"):
            inventory.validate({"tests": [{"name": "test_NOT_BUILT"}]})


if __name__ == "__main__":
    unittest.main()
