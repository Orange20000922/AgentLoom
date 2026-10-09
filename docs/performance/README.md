# Performance And Experiment Documents

本目录保存带日期、环境、输入边界和原始数据索引的实验记录。性能文档可以验证或否定设计假设，但不直接取代 Architecture、Runtime、Data 或 Security 的接口规范。

## 当前基线

- [HTTP SSE 验证与压测（2026-10，Fake CUDA L0 阶梯与容量边界）](HTTP_SSE_STREAMING_REPORT_2026_10.md)

- [Agent Runtime E2E 压测与优化报告（2026-08）](AGENT_RUNTIME_E2E_PERFORMANCE_REPORT_2026_08.md)
- [Benchmark Architecture Decision (2026-08)](BENCHMARK_ARCHITECTURE_DECISION_2026_08.md)
- [Gateway Session Affinity A/B](GATEWAY_SESSION_AFFINITY_BENCHMARK.md)
- [Emotion Fusion Gate Experiment](EMOTION_FUSION_GATE_EXPERIMENT.md)

## 历史/模块报告

- [旧综合性能报告](PERFORMANCE_REPORT.md)
- [Vision Pipeline Analysis](VISION_PIPELINE_ANALYSIS.md)
- [E2E Test and Emotion Pipeline](E2E_TEST_AND_EMOTION_PIPELINE.md)

## 数据规则

- 正式报告必须记录本机环境、日期、源码/工作区版本、构建目录、Provider、模型和配置；
- 冷启动、稳定态、容量边界、失败诊断分开；
- 受控 A/B 才计算优化百分比，跨版本只描述趋势；
- 原始报告默认保存在 dated `build/reports/` 或既有 ignored `data/` 目录；
- 项目压测执行流见 `.agents/skills/agentloom-e2e-benchmark/`。
