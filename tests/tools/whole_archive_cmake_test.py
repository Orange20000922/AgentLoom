"""真实安装包、ALIAS 与源码构建的 CMake 整库链接回归。"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
preferred = Path("C:/Program Files/CMake/bin/cmake.exe")
CMAKE = os.environ.get("AGENTLOOM_TEST_CMAKE") or (
    str(preferred) if sys.platform == "win32" and preferred.is_file() else shutil.which("cmake"))


@unittest.skipUnless(CMAKE, "requires CMake and a C++ compiler")
class WholeArchiveCMakeTest(unittest.TestCase):
    def test_duplicate_direct_transitive_and_alias_references_preserve_registration(self):
        with tempfile.TemporaryDirectory(prefix="agentloom-whole-archive-") as temporary:
            root = Path(temporary)
            fixture = ROOT / "tests/cmake/whole_archive"
            producer, consumer, prefix = root / "producer", root / "consumer", root / "install"
            common = ["-DCMAKE_BUILD_TYPE=Release", f"-DAGENTLOOM_SOURCE_DIR={ROOT.as_posix()}"]
            if sys.platform != "win32":
                common += ["-G", "Ninja" if shutil.which("ninja") else "Unix Makefiles"]
            ctest = Path(CMAKE).with_name("ctest.exe" if sys.platform == "win32" else "ctest")
            commands = [
                [CMAKE, "-S", fixture, "-B", producer, *common,
                 "-DWHOLE_ARCHIVE_PRODUCER=ON", f"-DCMAKE_INSTALL_PREFIX={prefix}"],
                [CMAKE, "--build", producer, "--config", "Release"],
                [CMAKE, "--install", producer, "--config", "Release"],
                [CMAKE, "-S", fixture, "-B", consumer, *common, f"-DFIXTURE_PREFIX={prefix.as_posix()}"],
                [CMAKE, "--build", consumer, "--config", "Release"],
                [str(ctest) if ctest.is_file() else "ctest", "--test-dir", consumer,
                 "-C", "Release", "--output-on-failure", "--no-tests=error"],
            ]
            # 不依赖 SDK/第三方库；每次真正编译、安装、导入并运行四种 consumer。
            for command in commands:
                result = subprocess.run([str(item) for item in command], capture_output=True,
                                        text=True, encoding="utf-8", errors="replace", timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
