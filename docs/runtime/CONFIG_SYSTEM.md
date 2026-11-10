# 配置系统扩展说明

## 概述

AgentLoom 使用 JSON 配置、CLI override 和 section registry 组合加载 Server 配置。配置解析集中在 `src/config/`，每个 section 独立负责字段加载、CLI 参数和校验，最终统一写入 `MultimodalServerOptions`。

当前已注册的主要 section：

| Section | 职责 |
|---------|------|
| `models` | BERT、VLM、mmproj、ViT 和运行时模型路径 |
| `grpc` | gRPC 地址、端口、消息限制和统计阈值 |
| `auth` | inference server metadata token |
| `limits` | 图片、prompt、token 和温度限制 |
| `vram_guard` | GPU 内存监控和 OOM 卸载策略 |
| `embedding` | tokenizer、ONNX embedding 和 pooling |
| `vlm_cache` | 结果缓存、vector cache 和 Prompt KV Cache |
| `llm` | OpenAI-compatible LLM backend 和凭据来源 |
| `emotion` | Persona 情绪驱动的生成预算策略 |
| `http` | HTTP/WebSocket runtime |
| `gateway_auth` | Gateway JWT、cookie、SQLite/Redis session store |
| `persona_gateway` | Persona 默认配置、线程池、静态文件和文档存储 |
| `skill_session` | Skill Session timeout、容量和维护周期 |

> 文档状态：当前配置入口说明。新增字段时以相应 `src/config/sections/*_section.cpp` 为实现真值，并同步更新 example JSON 和本文件。

## LLM 请求与响应诊断

`llm.debug_payloads` 默认为 `false`，`llm.debug_payload_max_bytes` 默认为 65536，
配置范围为 16 到 16777216 字节。显式开启并将 logger 级别设为 Debug 后，
OpenAI-compatible 同步、异步及 SSE 客户端通过 `llm-payload` logger 记录
`encoded_request`、`raw_response` / `raw_sse`、`decoded_response` / `decode_error`。
未注册该模块 logger 时使用默认 logger。进程内 `request_id` 关联同次请求及重试。

日志包含请求消息、工具 schema、实际 `tool_choice`，以及上游返回的 content、tool_calls、
reasoning_content 和 finish_reason；认证头不进入日志。上游未返回 reasoning 时记录 null，
不通过额外提示要求模型输出隐藏推理。日志会包含对话内容，应仅在受控诊断中开启。
超限记录按 UTF-8 边界保留首尾，并标记原始字节数和省略字节数；截断日志不能用于证明中间没有 DSML。
SSE 在解码前收集有界原始字节，在请求终结时输出，支持跨 chunk 的多字节文本取证。

诊断不改变协议返回值，也不把文本中的 DSML 自动转换成可执行工具。
欠调用应区分缺参澄清、模型选择、上游泄漏与本地解析丢失；reasoning 的文字意图本身不能证明上游解析失败。

## HTTP 服务器配置

### JSON 配置

```json
{
  "http": {
    "address": "0.0.0.0",
    "port": 8080,
    "io_threads": 4,
    "request_timeout_seconds": 30,
    "websocket_idle_timeout_seconds": 300,
    "request_body_limit_mb": 16,
    "websocket_read_buffer_limit_mb": 16
  }
}
```

### 字段说明

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `address` | string | `"0.0.0.0"` | 监听地址 |
| `port` | number | `8080` | 监听端口 (1-65535) |
| `io_threads` | number | `1` | IO 线程数（建议 CPU 核心数） |
| `request_timeout_seconds` | number | `30` | HTTP 请求超时（秒） |
| `websocket_idle_timeout_seconds` | number | `60` | WebSocket 空闲超时（秒） |
| `request_body_limit_mb` | number | `16` | HTTP 请求体大小限制（MB） |
| `websocket_read_buffer_limit_mb` | number | `16` | WebSocket 读缓冲区限制（MB） |

### CLI 参数

```bash
--http-address 0.0.0.0
--http-port 8080
--http-threads 4
--http-request-timeout 30
--http-websocket-timeout 300
--http-body-limit 16
--http-websocket-buffer-limit 16
```

## Persona 情绪生成预算

参考 Gateway 通过 `emotion.generation` 提供默认生成预算和情绪标签倍率。SDK Runtime
保留标签乘法预算与 V-A 情绪状态调整，并在两阶段完成后统一按 `max_token_ratio`
限幅。调用方可通过 `ChatRequest::generation_override` 覆盖单次请求的基础参数。

```json
{
  "emotion": {
    "generation": {
      "max_tokens": 2048,
      "min_tokens": 100,
      "max_token_ratio": 1.25,
      "default_token_weight": 1.0,
      "high_intensity_threshold": 0.7,
      "high_intensity_multiplier": 1.1,
      "token_weights": {
        "neutral": 1.0,
        "curiosity": 1.15
      }
    }
  }
}
```

`token_weights` 是相对于基础 `max_tokens` 的乘法比例，不是百分比加成。
`min_tokens` 不会突破单次请求按 `max_token_ratio` 计算出的上限。

## LLM 响应完整性校验

HTTP framing、JSON、UTF-8、空内容、`finish_reason=length` 和 usage 一致性始终校验。
与模型 tokenizer 相关的 Token 数比较是可选增强：

```json
{
  "llm": {
    "response_validation": {
      "token_count_mode": "audit",
      "tokenizer_path": "models/deepseek/tokenizer.json",
      "tokenizer_model": "deepseek-chat",
      "max_token_difference": 2
    }
  }
}
```

- `off`：不加载 tokenizer；不影响其他完整性校验。
- `audit`：记录 Token 偏差；词表不可用或模型不匹配时跳过，不拒绝响应。
- `strict`：词表为必需项；计数失败或偏差超过阈值时返回错误。

Tokenizer 必须与实际 Provider 模型版本一致。动态模型别名、未回传的 reasoning Token
或 Provider 专用控制 Token 会影响严格计数，因此生产环境默认使用 `off`，确认 Provider
统计口径后再启用 `audit` 或 `strict`。

## Embedding 模型配置

### JSON 配置

```json
{
  "embedding": {
    "tokenizer_path": "path/to/tokenizer.json",
    "onnx_model_path": "path/to/embedding-model.onnx",
    "execution_provider": "auto",
    "allow_cpu_fallback": true,
    "cuda_device_id": 0,
    "intra_op_num_threads": 0,
    "inter_op_num_threads": 0,
    "pooling_strategy": "mean",
    "normalize": true,
    "expected_dimension": 384,
    "require_token_type_ids": false
  }
}
```

### 字段说明

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `tokenizer_path` | string | `""` | HuggingFace tokenizer.json 路径 |
| `onnx_model_path` | string | `""` | ONNX embedding 模型路径 |
| `execution_provider` | string | `"auto"` | 执行提供者：`auto` / `cpu` / `cuda` |
| `allow_cpu_fallback` | bool | `true` | CUDA 不可用时回退到 CPU |
| `cuda_device_id` | number | `0` | CUDA 设备 ID |
| `intra_op_num_threads` | number | `0` | 算子内线程数（0=自动） |
| `inter_op_num_threads` | number | `0` | 算子间线程数（0=自动） |
| `pooling_strategy` | string | `"mean"` | Pooling 策略：`mean` / `cls` |
| `normalize` | bool | `true` | 是否 L2 归一化 |
| `expected_dimension` | number | `0` | 预期维度（0=不校验） |
| `require_token_type_ids` | bool | `false` | 是否需要 token_type_ids |

### CLI 参数

```bash
--embedding-tokenizer path/to/tokenizer.json
--embedding-model path/to/model.onnx
--embedding-provider auto
--embedding-cuda-device 0
--embedding-intra-threads 0
--embedding-inter-threads 0
--embedding-pooling mean
--embedding-normalize true
--embedding-dimension 384
```

### 验证规则

- **可选配置**：embedding section 完全可选，不配置则不加载模型
- **路径校验**：如果配置了任一路径，则两个路径都必须提供
- **Pooling 策略**：只支持 `mean` 和 `cls`
- **维度校验**：`expected_dimension > 0` 时，首次推理会校验实际维度

## 使用示例

### 完整配置文件

参考 `config.example.json`：

```json
{
  "models": {
    "llm": "models/llama-3.2-3b-instruct-q4_k_m.gguf",
    "bert": "models/bert-emotion.onnx"
  },
  "http": {
    "port": 8080,
    "io_threads": 4
  },
  "embedding": {
    "tokenizer_path": "models/tokenizer.json",
    "onnx_model_path": "models/minilm-l12-v2.onnx",
    "pooling_strategy": "mean",
    "normalize": true,
    "expected_dimension": 384,
    "batch_enabled": true,
    "batch_max_pending_requests": 1024,
    "batch_max_size": 16,
    "batch_max_wait_ms": 2,
    "batch_max_inflight": 1
  },
  "grpc": {
    "host": "127.0.0.1",
    "port": "50051",
    "max_pollers": 0
  }
}
```

### 启动命令

```bash
# 使用配置文件
./multimodal_inference_server --config config.json

# CLI 覆盖配置
./multimodal_inference_server --config config.json \
  --http-port 9090 \
  --embedding-model models/custom-embedding.onnx
```

### 代码中访问配置

```cpp
#include "server_options.h"

void InitializeEmbedding(const MultimodalServerOptions& options) {
    if (options.embedding.tokenizer_path.empty()) {
        // Embedding 未配置，跳过
        return;
    }

    // 加载 tokenizer
    auto tokenizer = vector::HfTokenizer::LoadFromFile(
        options.embedding.tokenizer_path
    ).value();

    // 加载 ONNX 模型
    vector::EmbeddingModelOptions opts;
    opts.model_path = options.embedding.onnx_model_path;
    opts.pooling = (options.embedding.pooling_strategy == "mean")
        ? vector::PoolingStrategy::Mean
        : vector::PoolingStrategy::Cls;
    opts.normalize = options.embedding.normalize;
    opts.expected_dimension = options.embedding.expected_dimension;
    opts.execution_provider = options.embedding.execution_provider;
    opts.cuda_device_id = options.embedding.cuda_device_id;

    auto model = vector::OnnxTextEmbeddingModel::Load(std::move(opts)).value();

    // 创建 pipeline
    auto pipeline = std::make_shared<vector::EmbeddingPipeline>(
        std::make_shared<vector::HfTokenizer>(std::move(tokenizer)),
        std::move(model)
    );
}

void InitializeHttpServer(const MultimodalServerOptions& options) {
    net::HttpServer server(options.http);
    // ...
}
```

## 配置优先级

1. **CLI 参数** — 最高优先级
2. **JSON 配置文件** — 中等优先级
3. **代码默认值** — 最低优先级

CLI 参数会覆盖 JSON 配置。

## 故障排查

### 问题：embedding 模型加载失败

**检查**：
1. 路径是否正确（绝对路径或相对于工作目录）
2. ONNX 模型 IR version 是否兼容（需要 IR ≤ 9）
3. tokenizer.json 是否存在

**解决**：
```bash
# 验证路径
ls -l path/to/tokenizer.json
ls -l path/to/model.onnx

# 检查 IR version
python -c "import onnx; m = onnx.load('model.onnx'); print(f'IR: {m.ir_version}')"

# 降级 IR version（如果需要）
python -c "import onnx; m = onnx.load('model.onnx'); m.ir_version = 9; onnx.save(m, 'model.onnx')"
```

### 问题：HTTP 端口被占用

**错误**：`bind: Address already in use`

**解决**：
```bash
# 检查端口占用
netstat -ano | findstr :8080

# 更换端口
--http-port 8081
```

### 问题：pooling_strategy 配置错误

**错误**：`embedding.pooling_strategy must be 'mean' or 'cls'`

**解决**：
```json
{
  "embedding": {
    "pooling_strategy": "mean"  // 只能是 "mean" 或 "cls"
  }
}
```

## 下一步

配置系统已就绪，可以：
1. **集成到主程序**：在 `main.cpp` 中加载配置并初始化 HTTP 服务器和 Embedding 模型
2. **添加更多 section**：如 Redis、Faiss、GStreamer 配置
3. **实现热重载**：监听配置文件变化并动态更新
