"""检查两平台 CI 的测试目标均已构建并被 CTest 发现，防止部分构建伪装成完整回归。"""
import argparse
from collections import Counter
import json
from pathlib import Path
import subprocess


# 与两个工作流的 Build SDK and test targets 保持一致，不固定会随新用例增长的数量。
EXPECTED = set("""core_tests net_tests http_client_tests llm_tests llm_streaming_tests
tls_tests storage_tests semantic_cache_tests memory_fact_codec_tests vector_index_manager_tests
context_plan_benchmark document_tests config_tests persona_runtime_tests gateway_service_tests
async_grpc_runtime_tests""".split())


def validate(inventory):
    counts = Counter()
    for test in inventory.get("tests", []):
        command = test.get("command", [])
        if not command or not Path(command[0]).is_file():
            raise ValueError(f"Missing executable for {test['name']}: {command}")
        counts[Path(command[0]).stem] += 1
    missing = EXPECTED - counts.keys()
    if missing:
        raise ValueError("CI targets absent from discovered tests: " + ", ".join(sorted(missing)))
    return counts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    args = parser.parse_args()
    # 使用与实际执行完全相同的配置、标签和 Redis 排除条件。
    result = subprocess.run(["ctest", "--test-dir", args.build_dir, "-C", "Release",
                             "-L", "ci", "-E", "(ReloadBatchCycle|RedisV2Batches)",
                             "--show-only=json-v1"], check=True, capture_output=True,
                            text=True, encoding="utf-8")
    inventory = json.loads(result.stdout)
    output = Path(args.build_dir) / "ci-test-inventory.json"
    output.write_text(json.dumps(inventory, ensure_ascii=False, indent=2), encoding="utf-8")
    counts = validate(inventory)
    print(json.dumps(dict(sorted(counts.items())), ensure_ascii=False, indent=2))
    print(f"Validated {sum(counts.values())} tests across {len(counts)} executables")


if __name__ == "__main__":
    main()
