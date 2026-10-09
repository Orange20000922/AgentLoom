from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import unittest

import httpx


spec = importlib.util.spec_from_file_location("gateway_benchmark", Path(__file__).resolve().parents[2] / "tools/gateway_concurrency_benchmark.py")
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


def event(name: str, sequence: int, **values: object) -> str:
    return f"event: {name}\ndata: {json.dumps({'sequence': sequence, **values})}\n\n"


class GatewayStreamBenchmarkTest(unittest.IsolatedAsyncioTestCase):
    # 失败样本必须保留为诊断数据，不能因 HTTP 200 将失败 Turn 计入成功统计。
    async def consume(self, body: str, content_type: str = "text/event-stream"):
        async def handle(request):
            return httpx.Response(200, text=body, headers={"content-type": content_type})
        async with httpx.AsyncClient(transport=httpx.MockTransport(handle), base_url="http://test") as client:
            return await benchmark.post_stream(client, {"stream": True}, {})

    async def test_committed_terminal_reassembles_text_and_records_stage_times(self):
        body = ": heartbeat\n\n" + event("TextDelta", 1, text="中文") + event("TextDelta", 2, text=" reply")
        body += event("GenerationCompleted", 3) + event("TurnCompleted", 4, committed=True, data={"reply": {"role": "assistant"}})
        final, elapsed, metrics = await self.consume(body)
        self.assertEqual(final["data"]["reply"]["content"], "中文 reply")
        self.assertLessEqual(metrics["firstDeltaMs"], metrics["generationEndMs"])
        self.assertLessEqual(metrics["generationEndMs"], metrics["turnCompleteMs"])
        self.assertLessEqual(metrics["turnCompleteMs"], elapsed)
        self.assertGreaterEqual(metrics["generationEndToCommitMs"], 0)

    async def test_incomplete_duplicate_and_uncommitted_streams_are_diagnostics(self):
        first = event("TextDelta", 1, text="partial")
        bad = [
            first,
            first + event("TextDelta", 1, text="duplicate"),
            first + event("GenerationCompleted", 2) + event("TurnCompleted", 3, committed=False),
            first + event("GenerationCompleted", 2) + event("TurnCompleted", 3, committed=True) + event("TextDelta", 4, text="late"),
        ]
        for body in bad:
            with self.subTest(body=body), self.assertRaises(RuntimeError):
                await self.consume(body)

    async def test_failed_turn_and_wrong_content_type_are_not_counted_as_success(self):
        with self.assertRaisesRegex(RuntimeError, "CANCELLED"):
            await self.consume(event("TurnFailed", 1, error={"code": "CANCELLED"}))
        with self.assertRaisesRegex(RuntimeError, "text/event-stream"):
            await self.consume("{}", "application/json")


if __name__ == "__main__":
    unittest.main()
