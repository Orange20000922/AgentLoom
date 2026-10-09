from __future__ import annotations

import argparse
import asyncio
import json
import re
import statistics
import time
from pathlib import Path
from typing import Any

import httpx
import psutil


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BASE_URL = "http://127.0.0.1:18080"
PERSONA_REGISTRY = ROOT / "data" / "persona_gateway_e2e" / "persona_registry.json"
REPORT_DIR = ROOT / "data" / "persona_gateway_e2e" / "reports"
MEMORY_MARKER_PATTERN = re.compile(r"MEMORY_SECRET_[A-Za-z0-9_-]+")


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round((len(ordered) - 1) * p))))
    return ordered[index]


def summarize(values: list[float]) -> dict[str, float]:
    if not values:
        return {"count": 0, "min": 0, "avg": 0, "p50": 0, "p90": 0, "p95": 0, "p99": 0, "max": 0}
    return {
        "count": len(values),
        "min": min(values),
        "avg": statistics.fmean(values),
        "p50": percentile(values, 0.50),
        "p90": percentile(values, 0.90),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
        "max": max(values),
    }


def find_gateway_process() -> psutil.Process | None:
    for process in psutil.process_iter(["name", "cmdline"]):
        try:
            if (process.info.get("name") or "").lower() == "persona_gateway_e2e_server.exe":
                return process
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    return None


async def sample_memory(stop: asyncio.Event, interval_s: float) -> list[dict[str, float]]:
    samples: list[dict[str, float]] = []
    process = find_gateway_process()
    if not process:
        return samples

    started = time.perf_counter()
    while not stop.is_set():
        try:
            mem = process.memory_info()
            samples.append(
                {
                    "tSec": time.perf_counter() - started,
                    "rssMb": mem.rss / 1024 / 1024,
                    "privateMb": getattr(mem, "private", 0) / 1024 / 1024,
                    "vmsMb": mem.vms / 1024 / 1024,
                    "numThreads": process.num_threads(),
                }
            )
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            break
        await asyncio.sleep(interval_s)
    return samples


def load_persona(persona_id: str) -> dict[str, Any]:
    with PERSONA_REGISTRY.open("r", encoding="utf-8") as file:
        registry = json.load(file)
    for persona in registry.get("personas", []):
        if persona.get("personaId") == persona_id:
            return persona
    raise RuntimeError(f"persona not found: {persona_id}")


async def post_json(client: httpx.AsyncClient, path: str, payload: dict[str, Any], headers: dict[str, str] | None = None) -> tuple[dict[str, Any], float]:
    started = time.perf_counter()
    response = await client.post(f"{client.base_url}{path}", json=payload, headers=headers)
    elapsed_ms = (time.perf_counter() - started) * 1000
    if response.status_code != 200:
        raise RuntimeError(f"POST {path} failed {response.status_code}: {response.text}")
    return response.json(), elapsed_ms


async def get_json(client: httpx.AsyncClient, path: str, headers: dict[str, str] | None = None) -> tuple[dict[str, Any], float]:
    started = time.perf_counter()
    response = await client.get(f"{client.base_url}{path}", headers=headers)
    elapsed_ms = (time.perf_counter() - started) * 1000
    if response.status_code != 200:
        raise RuntimeError(f"GET {path} failed {response.status_code}: {response.text}")
    return response.json(), elapsed_ms


async def post_stream(client: httpx.AsyncClient, payload: dict[str, Any], headers: dict[str, str]) -> tuple[dict[str, Any], float, dict[str, float]]:
    # 复用原 Gateway 负载入口；测量真实增量可见时间与提交终态，不把完整答案人为切片。
    started = time.perf_counter()
    metrics: dict[str, float] = {}
    final: dict[str, Any] | None = None
    text: list[str] = []
    sequence = 0
    event_name = ""
    data_lines: list[str] = []
    async with client.stream("POST", f"{client.base_url}/api/chat/message", json=payload, headers=headers) as response:
        if response.status_code != 200:
            raise RuntimeError(f"stream chat HTTP status {response.status_code}")
        if response.headers.get("content-type", "").split(";", 1)[0].strip().lower() != "text/event-stream":
            raise RuntimeError("stream chat did not return text/event-stream")
        async for line in response.aiter_lines():
            if not line:
                if data_lines:
                    data = json.loads("\n".join(data_lines))
                    elapsed = (time.perf_counter() - started) * 1000
                    if event_name != "ping":
                        next_sequence = data.get("sequence")
                        if not isinstance(next_sequence, int) or next_sequence != sequence + 1:
                            raise RuntimeError("stream sequence is missing, duplicated or out of order")
                        sequence = next_sequence
                        if final is not None:
                            raise RuntimeError("business event arrived after turn terminal")
                    if event_name == "TextDelta":
                        metrics.setdefault("firstDeltaMs", elapsed)
                        text.append(str(data["text"]))
                    elif event_name == "GenerationCompleted":
                        metrics["generationEndMs"] = elapsed
                    elif event_name == "TurnFailed":
                        code = data.get("error", {}).get("code", "UNKNOWN")
                        raise RuntimeError(f"stream turn failed code={code}")
                    elif event_name == "TurnCompleted":
                        if not data.get("committed"):
                            raise RuntimeError("stream success terminal did not confirm commit")
                        metrics["turnCompleteMs"] = elapsed
                        final = data
                event_name, data_lines = "", []
                continue
            if line.startswith(":"):
                continue
            field, _, value = line.partition(":")
            if value.startswith(" "):
                value = value[1:]
            if field == "event":
                event_name = value
            elif field == "data":
                data_lines.append(value)
    if final is None or "generationEndMs" not in metrics or "firstDeltaMs" not in metrics:
        raise RuntimeError("stream ended without text, generation end or turn terminal")
    final.setdefault("data", {}).setdefault("reply", {})["content"] = "".join(text)
    metrics["generationEndToCommitMs"] = metrics["turnCompleteMs"] - metrics["generationEndMs"]
    metrics["streamQueuePeakBytes"] = float(final.get("streamQueue", {}).get("peakBytes", 0))
    metrics["streamQueuePeakItems"] = float(final.get("streamQueue", {}).get("peakItems", 0))
    return final, (time.perf_counter() - started) * 1000, metrics


async def run_virtual_user(
    client: httpx.AsyncClient,
    user_index: int,
    turns: int,
    persona: dict[str, Any],
    think_ms: int,
    start_delay_s: float = 0.0,
    chat_start_gate: "ChatStartGate | None" = None,
    chat_turn_gates: "list[ChatStartGate] | None" = None,
    setup_lock: "asyncio.Lock | None" = None,
    chat_window: "ChatMeasurementWindow | None" = None,
    upsert_persona: bool = False,
    tenant_count: int = 1,
    shared_user_across_tenants: bool = False,
    isolation_probe: bool = False,
    stream: bool = False,
) -> list[dict[str, Any]]:
    if start_delay_s > 0:
        await asyncio.sleep(start_delay_s)

    now = int(time.time() * 1000)
    tenant_index = user_index % max(1, tenant_count)
    user_slot = user_index // max(1, tenant_count) if shared_user_across_tenants else user_index
    tenant_id = f"bench-tenant-{tenant_index}"
    user_uuid = f"bench-user-{now}-{user_slot}"
    session_id = f"bench-session-{now}-{tenant_index}-{user_index}"
    expected_marker = f"MEMORY_SECRET_T{tenant_index}_U{user_index}_{now}"

    records: list[dict[str, Any]] = []

    def error_record(kind: str, error: Exception, turn: int | None = None) -> dict[str, Any]:
        record: dict[str, Any] = {
            "kind": kind,
            "user": user_index,
            "ok": False,
            "errorType": type(error).__name__,
            "error": str(error),
        }
        if turn is not None:
            record["turn"] = turn
        return record

    async def setup_user() -> dict[str, str]:
        register, register_ms = await post_json(
            client,
            "/api/auth/register",
            {
                "userUuid": user_uuid,
                "tenantId": tenant_id,
                "subject": user_uuid,
                "ttlSeconds": 3600,
            },
        )
        token = register["data"]["token"]
        headers = {"Authorization": f"Bearer {token}"}
        records.append({"kind": "register", "user": user_index, "ok": True, "latencyMs": register_ms})

        _, me_ms = await get_json(client, "/api/auth/me", headers=headers)
        records.append({"kind": "authMe", "user": user_index, "ok": True, "latencyMs": me_ms})

        if upsert_persona:
            _, persona_ms = await post_json(
                client,
                "/api/persona",
                {
                    "traceId": f"bench-persona-{user_index}",
                    "personaId": persona["personaId"],
                    "personality": persona["personality"],
                },
                headers=headers,
            )
            records.append({"kind": "upsertPersona", "user": user_index, "ok": True, "latencyMs": persona_ms})

        _, create_ms = await post_json(
            client,
            "/api/session/create",
            {
                "traceId": f"bench-session-{user_index}",
                "sessionId": session_id,
                "personaId": persona["personaId"],
                "personality": persona["personality"],
            },
            headers=headers,
        )
        records.append({"kind": "createSession", "user": user_index, "ok": True, "latencyMs": create_ms})
        return headers

    try:
        if setup_lock:
            async with setup_lock:
                headers = await setup_user()
        else:
            headers = await setup_user()
    except Exception as error:
        records.append(error_record("setup", error))
        if chat_turn_gates:
            for gate in chat_turn_gates:
                await gate.drop_participant()
        elif chat_start_gate:
            await chat_start_gate.drop_participant()
        return records

    if chat_start_gate:
        await chat_start_gate.wait()

    prompts = (
        [
            f"请记住：我今天的课程暗号是 {expected_marker}。这是只属于当前账户的私密学习事实。",
            "请只根据属于当前账户的对话和记忆，复述我的课程暗号。",
            "再次检查你的长期上下文，只输出你能确认属于当前账户的课程暗号。",
        ]
        if isolation_probe
        else [
            "李大志，老师刚讲完一元二次方程配方法，你现在听懂了吗？",
            "刚才你说后面没跟上，那你能说说卡在哪一步吗？",
            "那我们再看一次配方法，你觉得第一步应该先做什么？",
            "如果我把题目拆成更小的步骤，你愿意试着回答吗？",
        ]
    )

    for turn in range(turns):
        if turn > 0 and chat_turn_gates:
            await chat_turn_gates[turn].wait()
        message = prompts[turn % len(prompts)]
        if chat_window:
            await chat_window.request_started(turn)
        try:
            payload = {
                    "traceId": f"bench-chat-{now}-{user_index}-{turn}",
                    "sessionId": session_id,
                    "personaId": persona["personaId"],
                    "mode": "chat",
                    "message": message,
                    "stream": stream,
                }
            stream_metrics: dict[str, float] = {}
            if stream:
                body, chat_ms, stream_metrics = await post_stream(client, payload, headers)
            else:
                body, chat_ms = await post_json(client, "/api/chat/message", payload, headers=headers)
        except Exception as error:
            records.append(error_record("chat", error, turn))
            if chat_window:
                await chat_window.request_finished(turn)
            continue
        data = body.get("data", {})
        pipeline = data.get("pipelineLatency", {})
        memory = data.get("memory", {})
        reply_content = str(data.get("reply", {}).get("content", ""))
        observed_markers = sorted(set(MEMORY_MARKER_PATTERN.findall(reply_content)))
        if not isolation_probe:
            isolation_passed = True
        elif turn == 0:
            # 首轮是写入事实；真实 LLM 通常只确认“已记住”，不保证回显暗号。
            # 回显当前请求自己的暗号也属于合法行为；只要不出现其他租户/用户的暗号即可。
            isolation_passed = not observed_markers or observed_markers == [expected_marker]
        else:
            # 后续轮次才是记忆召回断言，必须只返回当前租户/用户自己的暗号。
            isolation_passed = observed_markers == [expected_marker]
        records.append(
            {
                "kind": "chat",
                "user": user_index,
                "ok": True,
                "turn": turn,
                "latencyMs": chat_ms,
                **stream_metrics,
                "backendTotalMs": pipeline.get("totalMs", body.get("latencyMs", 0)),
                "computeQueueWaitMs": pipeline.get("computeQueueWaitMs", 0),
                "computeStageMs": pipeline.get("computeStageMs", 0),
                "ioQueueWaitMs": pipeline.get("ioQueueWaitMs", 0),
                "ioStageMs": pipeline.get("ioStageMs", 0),
                "memoryContextMs": pipeline.get("memoryContextMs", 0),
                "promptBuildMs": pipeline.get("promptBuildMs", 0),
                "llmTotalMs": pipeline.get("llmTotalMs", 0),
                "answerCacheMs": pipeline.get("answerCacheMs", 0),
                "callbackToResponseMs": pipeline.get("callbackToResponseMs", 0),
                "l0Hit": memory.get("l0Hit", False),
                "l3Hit": memory.get("l3Hit", False),
                "replyPreview": reply_content[:200],
                "tenantId": tenant_id,
                "userUuid": user_uuid,
                "expectedMemoryMarker": expected_marker if isolation_probe else "",
                "observedMemoryMarkers": observed_markers,
                "memoryIsolationPassed": isolation_passed,
            }
        )
        if chat_window:
            await chat_window.request_finished(turn)
        if think_ms > 0:
            await asyncio.sleep(think_ms / 1000)

    try:
        _, close_ms = await post_json(
            client,
            "/api/session/close",
            {
                "traceId": f"bench-close-{user_index}",
                "sessionId": session_id,
                "reason": "benchmark_complete",
            },
            headers=headers,
        )
        records.append({"kind": "closeSession", "user": user_index, "ok": True, "latencyMs": close_ms})
    except Exception as error:
        records.append(error_record("closeSession", error))
    return records


class ChatStartGate:
    """让完成初始化的虚拟用户同时开始首轮 Chat，失败用户不会阻塞其他参与者。"""

    def __init__(self, participants: int) -> None:
        self._expected = participants
        self._arrived = 0
        self._condition = asyncio.Condition()

    async def wait(self) -> None:
        async with self._condition:
            self._arrived += 1
            if self._arrived >= self._expected:
                self._condition.notify_all()
                return
            await self._condition.wait_for(lambda: self._arrived >= self._expected)

    async def drop_participant(self) -> None:
        async with self._condition:
            self._expected -= 1
            if self._arrived >= self._expected:
                self._condition.notify_all()


class ChatMeasurementWindow:
    """仅统计 Chat burst，不把串行 setup、think time 或 Session close 混入吞吐。"""

    def __init__(self) -> None:
        self._lock = asyncio.Lock()
        self._started_at: float | None = None
        self._finished_at: float | None = None
        self._started = 0
        self._finished = 0
        self._turns: dict[int, dict[str, float | int | None]] = {}

    async def request_started(self, turn: int) -> None:
        async with self._lock:
            now = time.perf_counter()
            if self._started_at is None:
                self._started_at = now
            self._started += 1
            stats = self._turns.setdefault(
                turn, {"startedAt": None, "finishedAt": None, "startedRequests": 0, "finishedRequests": 0}
            )
            if stats["startedAt"] is None:
                stats["startedAt"] = now
            stats["startedRequests"] = int(stats["startedRequests"] or 0) + 1

    async def request_finished(self, turn: int) -> None:
        async with self._lock:
            now = time.perf_counter()
            self._finished += 1
            self._finished_at = now
            stats = self._turns.setdefault(
                turn, {"startedAt": None, "finishedAt": None, "startedRequests": 0, "finishedRequests": 0}
            )
            stats["finishedAt"] = now
            stats["finishedRequests"] = int(stats["finishedRequests"] or 0) + 1

    def summary(self) -> dict[str, float | int]:
        elapsed = 0.0
        if self._started_at is not None and self._finished_at is not None:
            elapsed = self._finished_at - self._started_at
        return {
            "startedRequests": self._started,
            "finishedRequests": self._finished,
            "elapsedSec": elapsed,
            "throughputReqPerSec": self._finished / elapsed if elapsed > 0 else 0.0,
        }

    def turn_summaries(self) -> dict[str, dict[str, float | int]]:
        summaries: dict[str, dict[str, float | int]] = {}
        for turn, stats in self._turns.items():
            started_at = stats["startedAt"]
            finished_at = stats["finishedAt"]
            elapsed = (
                float(finished_at) - float(started_at)
                if started_at is not None and finished_at is not None
                else 0.0
            )
            finished = int(stats["finishedRequests"] or 0)
            summaries[str(turn)] = {
                "startedRequests": int(stats["startedRequests"] or 0),
                "finishedRequests": finished,
                "elapsedSec": elapsed,
                "throughputReqPerSec": finished / elapsed if elapsed > 0 else 0.0,
            }
        return summaries


async def run(args: argparse.Namespace) -> dict[str, Any]:
    persona = load_persona(args.persona)
    stop_memory = asyncio.Event()
    memory_task = asyncio.create_task(sample_memory(stop_memory, args.memory_interval))
    started = time.perf_counter()

    limits = httpx.Limits(max_connections=max(args.concurrency * 4, 16), max_keepalive_connections=max(args.concurrency * 2, 8))
    timeout = httpx.Timeout(args.timeout)
    base_url = args.base_url.rstrip("/")
    chat_turn_gates = (
        [ChatStartGate(args.concurrency) for _ in range(args.turns)]
        if args.chat_turn_barrier and args.turns > 0
        else None
    )
    chat_start_gate = (
        chat_turn_gates[0]
        if chat_turn_gates is not None
        else (ChatStartGate(args.concurrency) if args.chat_start_barrier else None)
    )
    setup_lock = asyncio.Lock() if args.serial_setup else None
    chat_window = ChatMeasurementWindow()
    async with httpx.AsyncClient(base_url=base_url, timeout=timeout, limits=limits) as client:
        tasks = []
        for user in range(args.concurrency):
            if args.ramp_up_seconds > 0 and args.concurrency > 1:
                start_delay_s = args.ramp_up_seconds * user / (args.concurrency - 1)
            else:
                start_delay_s = 0.0
            tasks.append(run_virtual_user(
                client,
                user,
                args.turns,
                persona,
                args.think_ms,
                start_delay_s,
                chat_start_gate,
                chat_turn_gates,
                setup_lock,
                chat_window,
                args.upsert_persona,
                args.tenant_count,
                args.shared_user_across_tenants,
                args.isolation_probe,
                args.stream,
            ))
        nested = await asyncio.gather(*tasks)

    elapsed_s = time.perf_counter() - started
    stop_memory.set()
    memory_samples = await memory_task
    records = [record for user_records in nested for record in user_records]
    ok_records = [record for record in records if record.get("ok", True)]
    error_records = [record for record in records if not record.get("ok", True)]
    chat_records = [record for record in ok_records if record["kind"] == "chat"]
    isolation_records = [record for record in chat_records if args.isolation_probe]
    isolation_failures = [record for record in isolation_records if not record.get("memoryIsolationPassed", False)]

    chat_latency = [float(record["latencyMs"]) for record in chat_records]
    backend_latency = [float(record.get("backendTotalMs", 0)) for record in chat_records]
    memory_latency = [float(record.get("memoryContextMs", 0)) for record in chat_records]
    llm_latency = [float(record.get("llmTotalMs", 0)) for record in chat_records]
    compute_queue_latency = [float(record.get("computeQueueWaitMs", 0)) for record in chat_records]
    compute_stage_latency = [float(record.get("computeStageMs", 0)) for record in chat_records]
    io_queue_latency = [float(record.get("ioQueueWaitMs", 0)) for record in chat_records]
    io_stage_latency = [float(record.get("ioStageMs", 0)) for record in chat_records]
    callback_latency = [float(record.get("callbackToResponseMs", 0)) for record in chat_records]
    all_latency = [float(record["latencyMs"]) for record in ok_records if "latencyMs" in record]

    per_turn_latency: dict[str, dict[str, Any]] = {}
    for turn in range(args.turns):
        turn_records = [record for record in chat_records if record.get("turn") == turn]
        per_turn_latency[str(turn)] = {
            "count": len(turn_records),
            "chatEndToEnd": summarize([float(record["latencyMs"]) for record in turn_records]),
            "backendTotal": summarize([float(record.get("backendTotalMs", 0)) for record in turn_records]),
            "computeQueueWait": summarize([float(record.get("computeQueueWaitMs", 0)) for record in turn_records]),
            "ioQueueWait": summarize([float(record.get("ioQueueWaitMs", 0)) for record in turn_records]),
            "llmTotal": summarize([float(record.get("llmTotalMs", 0)) for record in turn_records]),
        }

    report = {
        "ok": not error_records and not isolation_failures,
        "scenario": args.scenario,
        "stream": args.stream,
        "streamQueuePeakBytes": max((record.get("streamQueuePeakBytes", 0) for record in chat_records), default=0),
        "streamQueuePeakItems": max((record.get("streamQueuePeakItems", 0) for record in chat_records), default=0),
        "baseUrl": base_url,
        "personaId": args.persona,
        "concurrency": args.concurrency,
        "turnsPerUser": args.turns,
        "chatStartBarrier": args.chat_start_barrier,
        "chatTurnBarrier": args.chat_turn_barrier,
        "setupMode": "serial" if args.serial_setup else "concurrent",
        "personaSource": "account_upsert" if args.upsert_persona else "server_default",
        "tenantCount": args.tenant_count,
        "sharedUserAcrossTenants": args.shared_user_across_tenants,
        "isolationProbe": args.isolation_probe,
        "plannedConcurrentChatRequests": args.concurrency if args.turns > 0 else 0,
        "rampUpSeconds": args.ramp_up_seconds,
        "totalRequests": len(records),
        "successfulRequests": len(ok_records),
        "errorCount": len(error_records),
        "totalChatRequests": len(chat_records),
        "chatSuccessRate": len(chat_records) / (args.concurrency * args.turns) if args.turns > 0 else 1.0,
        "elapsedSec": elapsed_s,
        "throughputReqPerSec": len(records) / elapsed_s if elapsed_s > 0 else 0,
        "throughputChatPerSec": len(chat_records) / elapsed_s if elapsed_s > 0 else 0,
        "chatMeasurementWindow": chat_window.summary(),
        "chatMeasurementWindowByTurn": chat_window.turn_summaries(),
        "latencyMs": {
            "allRequests": summarize(all_latency),
            "chatEndToEnd": summarize(chat_latency),
            "backendTotal": summarize(backend_latency),
            "computeQueueWait": summarize(compute_queue_latency),
            "computeStage": summarize(compute_stage_latency),
            "ioQueueWait": summarize(io_queue_latency),
            "ioStage": summarize(io_stage_latency),
            "memoryContext": summarize(memory_latency),
            "llmTotal": summarize(llm_latency),
            "callbackToResponse": summarize(callback_latency),
            **{name: summarize([float(record[name]) for record in chat_records if name in record])
               for name in ("firstDeltaMs", "generationEndMs", "turnCompleteMs", "generationEndToCommitMs") if args.stream},
        },
        "latencyMsByTurn": per_turn_latency,
        "memory": {
            "samples": memory_samples,
            "rssMb": summarize([sample["rssMb"] for sample in memory_samples]),
            "privateMb": summarize([sample["privateMb"] for sample in memory_samples]),
            "vmsMb": summarize([sample["vmsMb"] for sample in memory_samples]),
            "peakRssMb": max((sample["rssMb"] for sample in memory_samples), default=0),
            "peakPrivateMb": max((sample["privateMb"] for sample in memory_samples), default=0),
            "peakThreads": max((sample["numThreads"] for sample in memory_samples), default=0),
        },
        "l0": {
            "hits": sum(1 for record in chat_records if record.get("l0Hit")),
            "misses": sum(1 for record in chat_records if not record.get("l0Hit")),
        },
        "memoryIsolation": {
            "checkedReplies": len(isolation_records),
            "passedReplies": len(isolation_records) - len(isolation_failures),
            "failedReplies": len(isolation_failures),
            "passRate": (
                (len(isolation_records) - len(isolation_failures)) / len(isolation_records)
                if isolation_records else 1.0
            ),
            "failures": isolation_failures[:20],
        },
        "errors": error_records,
        "records": records if args.include_records else [],
    }
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--concurrency", type=int, default=2)
    parser.add_argument("--turns", type=int, default=2)
    parser.add_argument("--stream", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--persona", default="lidazhi")
    parser.add_argument("--think-ms", type=int, default=0)
    parser.add_argument("--ramp-up-seconds", type=float, default=0.0)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--memory-interval", type=float, default=0.5)
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL)
    parser.add_argument("--scenario", default="")
    parser.add_argument("--chat-start-barrier", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--chat-turn-barrier", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--serial-setup", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--upsert-persona", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--tenant-count", type=int, default=1)
    parser.add_argument("--shared-user-across-tenants", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--isolation-probe", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--include-records", action="store_true")
    parser.add_argument("--output", default="")
    args = parser.parse_args()
    if args.concurrency <= 0 or args.turns <= 0:
        parser.error("--concurrency and --turns must be positive")
    if args.tenant_count <= 0:
        parser.error("--tenant-count must be positive")
    if args.isolation_probe and args.turns < 2:
        parser.error("--isolation-probe requires at least two turns")

    report = asyncio.run(run(args))
    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    output = Path(args.output) if args.output else REPORT_DIR / f"gateway_benchmark_{int(time.time())}.json"
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    summary = {k: v for k, v in report.items() if k not in {"records"}}
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    print(f"[benchmark] report: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
