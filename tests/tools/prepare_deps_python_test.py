"""真实运行 Windows PowerShell 5.1 的私有 venv 初始化，离线验证首次缺包。"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

SOURCE = Path(__file__).resolve().parents[2] / "windows/scripts/prepare_deps.ps1"
SHELL = shutil.which("powershell.exe")


@unittest.skipUnless(sys.platform == "win32" and SHELL, "requires Windows PowerShell 5.1")
class PrepareDependenciesPythonTest(unittest.TestCase):
    def run_preparation(self, root, with_package):
        wheels = root / "wheels"
        wheels.mkdir()
        if with_package:
            # 只测试包的探测与安装生命周期；离线 wheel 无需真正的 zstd 实现。
            with zipfile.ZipFile(wheels / "zstandard-0.25.0-py3-none-any.whl", "w") as wheel:
                wheel.writestr("zstandard.py", "__version__ = '0.25.0'\n")
                metadata = "zstandard-0.25.0.dist-info/"
                wheel.writestr(metadata + "METADATA", "Metadata-Version: 2.1\nName: zstandard\nVersion: 0.25.0\n")
                wheel.writestr(metadata + "WHEEL", "Wheel-Version: 1.0\nRoot-Is-Purelib: true\nTag: py3-none-any\n")
                wheel.writestr(metadata + "RECORD", "")
        harness = root / "prepare.ps1"
        # AST 提取真实函数，避免触发下载 Faiss/MKL 等无关操作。
        harness.write_text(r'''param([string]$Source, [string]$Root)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$tokens = $null
$parseErrors = $null
$text = Get-Content -LiteralPath $Source -Raw -Encoding UTF8
$ast = [System.Management.Automation.Language.Parser]::ParseInput($text, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'Script parse failed' }
$definition = $ast.Find({ param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -eq 'Ensure-PythonEnvironment'
}, $true)
if (-not $definition) { throw 'Python preparation function missing' }
Invoke-Expression $definition.Extent.Text
function Write-Log { param([string]$Message) Write-Output $Message }
$PythonVenv = Join-Path $Root 'private-venv'
$PythonExe = Join-Path $PythonVenv 'Scripts\python.exe'
Ensure-PythonEnvironment
Ensure-PythonEnvironment
& $PythonExe -c "import zstandard; print(zstandard.__version__)"
if ($LASTEXITCODE -ne 0) { throw 'Installed package cannot be imported' }
''', encoding="utf-8-sig")
        environment = os.environ.copy()
        environment.update(PIP_NO_INDEX="1", PIP_FIND_LINKS=wheels.as_uri(),
                           PYTHONUTF8="1", PYTHONNOUSERSITE="1")
        return subprocess.run([SHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                               str(harness), str(SOURCE), str(root)], env=environment,
                              capture_output=True, text=True, encoding="utf-8", errors="replace",
                              timeout=120)

    def test_missing_package_is_installed_and_existing_environment_is_reused(self):
        with tempfile.TemporaryDirectory(prefix="agentloom-python-隔离 ") as temporary:
            result = self.run_preparation(Path(temporary), with_package=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count("Creating private Python environment:"), 1)
            self.assertEqual(result.stdout.count("Installing zstandard into the private Python environment"), 1)
            self.assertIn("0.25.0", result.stdout)

    def test_installation_failure_is_reported(self):
        with tempfile.TemporaryDirectory(prefix="agentloom-python-隔离 ") as temporary:
            result = self.run_preparation(Path(temporary), with_package=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Installing zstandard into the private Python environment", result.stdout)
            self.assertIn("Failed to install Python zstandard package", result.stderr)


if __name__ == "__main__":
    unittest.main()
