"""对召回、模型路由和协议证据分别验收；阈值只用开发集选择，留出集不参与选择。"""
import argparse
import json
import math
from pathlib import Path
import statistics


def mean(values):
    return statistics.mean(values) if values else None


def recall_metrics(rows, threshold=None):
    tp = fp = fn = exact = negatives = false_triggers = 0
    margins, ranks, kl = [], [], []
    for row in rows:
        target = row["expect_tool_id"]
        scores = row["vector_scores"]
        selected = ({key for key, value in scores.items() if value >= threshold} if threshold is not None
                    else {hit["tool_id"] for hit in row["hits"]})
        wanted = {target} if target else set()
        tp += len(selected & wanted)
        fp += len(selected - wanted)
        fn += len(wanted - selected)
        exact += selected == wanted
        if not target:
            negatives += 1
            false_triggers += bool(selected)
        else:
            ranking = sorted(scores, key=lambda key: scores[key], reverse=True)
            ranks.append(ranking.index(target) + 1)
            margins.append(scores[target] - max(value for key, value in scores.items() if key != target))
            # P 为标注的单目标分布，Q 为候选余弦分数按固定温度转换的分布。
            # KL(P||Q)=-log Q(target)，不是对原始向量坐标计算 KL；无工具输入不纳入此指标。
            logits = [value / 0.1 for value in scores.values()]
            peak = max(logits)
            log_z = peak + math.log(sum(math.exp(value - peak) for value in logits))
            kl.append(log_z - scores[target] / 0.1)
    precision = tp / (tp + fp) if tp + fp else 0.0
    recall = tp / (tp + fn) if tp + fn else 0.0
    return dict(n=len(rows), positives=len(ranks), exact=exact, tp=tp, fp=fp, fn=fn,
                precision=precision, recall=recall, f1=2*precision*recall/(precision+recall) if precision+recall else 0.0,
                negative_false_triggers=false_triggers, negatives=negatives,
                vector_top1=sum(rank == 1 for rank in ranks), vector_mrr=mean([1/r for r in ranks]),
                mean_margin=mean(margins), minimum_margin=min(margins) if margins else None,
                mean_target_kl_nats_temperature_0_1=mean(kl))


def audit_wire(path):
    events = {}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if "llm_payload " not in line:
            continue
        item = json.loads(line.split("llm_payload ", 1)[1])
        events.setdefault(item["request_id"], {})[item["stage"]] = item
    mismatches, dsml, raw_count, truncated = [], [], 0, 0
    thinking_modes, models, tool_choices = set(), set(), set()
    encoded = []
    for rid, stages in events.items():
        truncated += sum(item["truncated"] for item in stages.values())
        if "encoded_request" in stages and "body" in stages["encoded_request"]:
            request = json.loads(stages["encoded_request"]["body"])
            thinking_modes.add(request.get("thinking", {}).get("type", "absent"))
            tool_choices.add(request.get("tool_choice", "absent"))
            encoded.append(request)
        raw = stages.get("raw_response")
        if not raw or raw["http_status"] != 200 or "body" not in raw:
            continue
        raw_count += 1
        body = json.loads(raw["body"])
        models.add(body.get("model", ""))
        message = body["choices"][0]["message"]
        content = message.get("content") or ""
        if "DSML" in content or "<｜tool" in content or "<|tool" in content:
            dsml.append(rid)
        decoded_event = stages.get("decoded_response")
        if not decoded_event or "body" not in decoded_event:
            mismatches.append([rid, "decode_error_or_truncated"])
            continue
        decoded = json.loads(decoded_event["body"])
        raw_calls = [(call["id"], call["function"]["name"], json.loads(call["function"]["arguments"]))
                     for call in message.get("tool_calls") or []]
        parsed_calls = [(call["id"], call["name"], json.loads(call["arguments"])) for call in decoded["tool_calls"]]
        if raw_calls != parsed_calls or content != decoded["content"] or message.get("reasoning_content") != decoded["reasoning_content"]:
            mismatches.append([rid, "fields_differ"])
    return dict(raw_responses=raw_count, raw_decode_mismatches=mismatches, dsml=dsml,
                truncated=truncated, thinking_modes=sorted(thinking_modes), models=sorted(models),
                tool_choices=sorted(tool_choices)), encoded


def analyze(root):
    summary = {}
    encoded_by_run = {}
    for file in sorted(root.glob("*.jsonl")):
        rows = [json.loads(line) for line in file.read_text(encoding="utf-8-sig").splitlines()]
        result = {}
        for split in ("development", "held_out"):
            subset = [r for r in rows if r["held_out"] == (split == "held_out")]
            result[split] = recall_metrics(subset)
        if file.stem.startswith("vector_"):
            development = [r for r in rows if not r["held_out"]]
            holdout = [r for r in rows if r["held_out"]]
            curve = [{"threshold": i/100, **recall_metrics(development, i/100)} for i in range(101)]
            chosen = max(curve, key=lambda p: (p["f1"], p["precision"], p["threshold"]))["threshold"]
            result["threshold_selection"] = dict(development_selected=chosen, development=recall_metrics(development, chosen),
                held_out=recall_metrics(holdout, chosen), development_curve=curve)
        if file.stem.startswith("llm_"):
            result["llm"] = dict(n=len(rows), positives=sum(r["positive"] for r in rows),
                target_hits=sum(r.get("llm_called_expected", False) for r in rows),
                strict_routes=sum(r.get("route_ok", False) for r in rows),
                not_declared=[r["case_id"] for r in rows if r["positive"] and not r["expected_declared"]],
                route_failures=[dict(case=r["case_id"], repetition=r["repetition"], calls=r.get("llm_tool_calls", []),
                                     content=r.get("content", "")) for r in rows if not r.get("route_ok", False)],
                follow_up_failures=[r["case_id"] for r in rows if r.get("follow_up_ok") is False],
                api_errors=[r["case_id"] for r in rows if not r.get("llm_ok", False)],
                text_tool_markup=[dict(case=r["case_id"], repetition=r["repetition"],
                                       declared=r["declared_tools"]) for r in rows
                                  if any(marker in r.get("content", "") for marker in ("DSML", "<｜tool", "<tool_call"))],
                reasoning_responses=sum(bool(r.get("reasoning_content")) for r in rows),
                mean_latency_ms=mean([r["latency_ms"] for r in rows]),
                mean_completion_tokens=mean([r["completion_tokens"] for r in rows if "completion_tokens" in r]))
            result["wire"], requests = audit_wire(Path(str(file) + ".payload.log"))
            # 只比较首轮请求，后续结果与 reasoning 自然受模型输出影响。
            encoded_by_run[file.stem] = [r for r in requests if not any(m["role"] == "tool" for m in r["messages"])]
        summary[file.stem] = result
    # 验证 thinking A/B 的首轮线协议除开关外一致，而不是只相信实验配置。
    comparisons = {}
    for prompt in ("baseline", "scoped"):
        off = encoded_by_run.get(f"llm_always_{prompt}_off")
        on = encoded_by_run.get(f"llm_always_{prompt}_on")
        if off is not None and on is not None:
            for request in off + on:
                request.pop("thinking", None)
            comparisons[prompt] = dict(off=len(off), on=len(on), equal_except_thinking=off == on)
    output = {"experiments": summary, "wire_ab_checks": comparisons}
    (root / "analysis.json").write_text(json.dumps(output, ensure_ascii=False, indent=2), encoding="utf-8")
    for name, result in summary.items():
        if "llm" in result:
            r = result["llm"]
            print(name, f"route={r['strict_routes']}/{r['n']}", f"target={r['target_hits']}/{r['positives']}",
                  f"reasoning={r['reasoning_responses']}", "followup_fail=", r["follow_up_failures"])
        else:
            print(name, [(split, result[split]["exact"], result[split]["n"], round(result[split]["f1"], 3))
                         for split in ("development", "held_out")])
    print("wire_ab_checks", comparisons)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    analyze(parser.parse_args().directory)
