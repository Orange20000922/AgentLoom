"""控制变量实验入口；不打印凭据，不并行共享模型进程，所有本机配置与输出写入 ignored build。"""
import argparse
import copy
import json
from pathlib import Path
import subprocess


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")


def profiles(spec):
    base = {"extra_cases": spec["extra_cases"], "min_score": 0.78, "recall_only": True}
    variants = {"recall_baseline": copy.deepcopy(base)}
    for name, seeds in [("english", spec["english_seeds"]), ("bilingual", spec["bilingual_seeds"])]:
        p = copy.deepcopy(base)
        p["tools"] = {key: {"l4_payload": value} for key, value in seeds.items()}
        variants["recall_" + name] = p
    p = copy.deepcopy(base)
    p["tools"] = spec["expanded_keywords"]
    variants["recall_keywords"] = p
    p = copy.deepcopy(variants["recall_bilingual"])
    for key, value in spec["expanded_keywords"].items():
        p["tools"][key].update(value)
    variants["recall_combined"] = p
    for name in ("baseline", "bilingual", "english"):
        p = copy.deepcopy(variants["recall_" + name])
        p.update(enable_regex=False, min_score=0.0, top_k=4)
        variants["vector_" + name] = p
    # 固定工具集合与参数，两个 prompt × 两个 thinking，重复两次观测模型随机性。
    for scoped in (False, True):
        for thinking in (False, True):
            name = f"llm_always_{'scoped' if scoped else 'baseline'}_{'on' if thinking else 'off'}"
            p = copy.deepcopy(base)
            p.update(recall_only=False, thinking=thinking, repetitions=2, case_ids=spec["focus_cases"],
                     temperature=1.0, top_p=1.0, max_tokens=8192)
            if scoped:
                p["prompt_suffix"] = spec["scoped_prompt"]
            variants[name] = p
    # 分级比较保持同样的提示词、召回配置与思考模式，仅改变 Require/Recall/Always。
    for layer in ("recall", "required"):
        variants[f"llm_{layer}_scoped_off"] = copy.deepcopy(variants["llm_always_scoped_off"])
    # 空召回时，分别去掉提示中的工具名、增加可用性约束，定位正文伪调用。
    p = copy.deepcopy(variants["llm_recall_scoped_off"])
    p.update(case_ids=["kb_en", "hold_price_en"], repetitions=3)
    p["prompt_suffix"] = p["prompt_suffix"].replace("Sales guidance uses kb_query", "Sales guidance uses an available knowledge retrieval tool").replace(
        "Use document_analyze", "Use an available document analysis tool").replace("and profile_read", "and an available customer profile tool")
    variants["llm_recall_no_named_tools_off"] = p
    p = copy.deepcopy(variants["llm_recall_scoped_off"])
    p.update(case_ids=["kb_en", "hold_price_en"], repetitions=3)
    p["prompt_suffix"] += "\nAvailability rule: Only functions declared in the current request are callable. If no suitable function is declared, clearly state that the external lookup is unavailable; do not invent a function or write tool-call markup in ordinary content."
    variants["llm_recall_availability_guard_off"] = p
    for name, p in variants.items():
        p["id"] = name
    return variants


def run(exe, base, out, name, p):
    report = out / f"{name}.jsonl"
    if report.exists():
        raise RuntimeError(f"Refusing to overwrite existing evidence: {report}")
    profile = out / f"{name}.experiment.json"
    write(profile, p)
    layer = name.split("_")[1] if name.startswith("llm_") else "recall"
    cmd = [str(exe), str(base), str(report), layer, "bound", "combined", "production", str(profile)]
    with (out / f"{name}.log").open("w", encoding="utf-8") as log:
        result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=1800, check=False)
    rows = [json.loads(line) for line in report.read_text(encoding="utf-8-sig").splitlines()] if report.exists() else []
    expected = (len(p["case_ids"]) if "case_ids" in p else 25 + len(p["extra_cases"])) * p.get("repetitions", 1)
    print(f"{name}: exit={result.returncode}, rows={len(rows)}/{expected}", flush=True)
    if result.returncode not in (0, 1) or len(rows) != expected:
        raise RuntimeError(f"Experiment incomplete; inspect {name}.log")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("stage", choices=["recall", "llm", "prepare"])
    parser.add_argument("--base-config", required=True)
    parser.add_argument("--output", default="build/reports/issue14_ablation")
    parser.add_argument("--exe", default="build/x64-Release-All-v145/Release/skill_llm_e2e_test.exe")
    parser.add_argument("--only", nargs="*")
    args = parser.parse_args()
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=True)
    spec = read(Path(__file__).parent / "skill_experiments/ablation_v1.json")
    variants = profiles(spec)
    if args.only and not set(args.only) <= variants.keys():
        parser.error("--only contains an unknown experiment name")
    # 配置中的路径来自既有 ignored 测试配置；不复制到公开文档或标准输出。
    config = read(args.base_config)
    for section, keys in (("llm", ("api_key_file", "ca_bundle_path")),
                          ("embedding", ("tokenizer_path", "model_path"))):
        for key in keys:
            value = config.get(section, {}).get(key)
            if value and not Path(value).is_absolute():
                config[section][key] = str((Path(args.base_config).resolve().parent / value).resolve())
    config["llm"]["model"] = "deepseek-flash"
    config["llm"]["timeout_ms"] = 120000
    base = out / "provider.local.json"
    write(base, config)
    for name, p in variants.items():
        if args.only and name not in args.only:
            continue
        if args.stage == "prepare":
            write(out / f"{name}.experiment.json", p)
        elif (args.stage == "llm") == name.startswith("llm_"):
            run(Path(args.exe).resolve(), base, out, name, p)


if __name__ == "__main__":
    main()
