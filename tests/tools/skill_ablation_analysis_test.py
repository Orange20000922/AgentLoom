"""实验统计回归：验证额外调用计入误召回、KL 的语义标签依赖及留出集不参与调阈值。"""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("ablation", Path(__file__).resolve().parents[2] / "tools/analyze_skill_ablation.py")
ablation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ablation)


def row(target, scores, held_out=False):
    return dict(expect_tool_id=target, vector_scores=scores, held_out=held_out,
                hits=[{"tool_id": key} for key in scores])


class AblationAnalysisTest(unittest.TestCase):
    def test_target_hit_does_not_hide_false_positive(self):
        metrics = ablation.recall_metrics([row("a", {"a": 0.8, "b": 0.7})])
        self.assertEqual((metrics["tp"], metrics["fp"], metrics["exact"]), (1, 1, 0))
        self.assertEqual(metrics["recall"], 1)
        self.assertEqual(metrics["precision"], 0.5)

    def test_same_score_distribution_with_wrong_semantic_assignment_is_worse(self):
        correct = ablation.recall_metrics([row("a", {"a": 0.9, "b": 0.1})])
        shuffled = ablation.recall_metrics([row("a", {"a": 0.1, "b": 0.9})])
        self.assertEqual(correct["vector_top1"], 1)
        self.assertEqual(shuffled["vector_top1"], 0)
        self.assertLess(correct["mean_target_kl_nats_temperature_0_1"], shuffled["mean_target_kl_nats_temperature_0_1"])

    def test_holdout_does_not_retune_threshold(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            rows = [row("a", {"a": 0.9, "b": 0.1}), row("", {"a": 0.2, "b": 0.1}),
                    row("a", {"a": 0.4, "b": 0.1}, True)]
            (root / "vector_test.jsonl").write_text("\n".join(json.dumps(r) for r in rows), encoding="utf-8")
            ablation.analyze(root)
            result = json.loads((root / "analysis.json").read_text(encoding="utf-8"))
            selection = result["experiments"]["vector_test"]["threshold_selection"]
            self.assertEqual(selection["development_selected"], 0.9)
            self.assertEqual(selection["held_out"]["recall"], 0)


if __name__ == "__main__":
    unittest.main()
