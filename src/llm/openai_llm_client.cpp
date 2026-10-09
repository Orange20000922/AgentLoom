#include "openai_llm_client.h"
#include "logger_adapter.h"

#include <boost/asio.hpp>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <exception>
#include <thread>
#include <unordered_map>
#include <utility>

namespace agent::llm {

namespace {

// 同步/异步共享 HTTP envelope；线协议仅提供 endpoint 和自有编码结果。
core::Result<net::HttpClientRequest> BuildHttpRequest(
    const OpenAiLlmClientOptions& options, const ChatCompletionRequest& request) {
    const LlmProtocolContext context{options.default_model, options.response_validation};
    auto encoded = options.protocol->EncodeRequest(request, context);
    if (!encoded.ok()) return encoded.status();
    auto url = options.base_url;
    if (url.back() != '/') url.push_back('/');
    url.append(options.protocol->Endpoint());
    net::HttpClientRequest http_request;
    http_request.method = "POST";
    http_request.url = std::move(url);
    http_request.headers.push_back({"Content-Type", "application/json"});
    if (request.stream) http_request.headers.push_back({"Accept", "text/event-stream"});
    if (!options.api_key.empty()) {
        http_request.headers.push_back({"Authorization", "Bearer " + options.api_key});
    }
    http_request.body = std::move(encoded).value();
    http_request.timeout_ms = options.timeout_ms;
    return http_request;
}

core::Result<ChatCompletionResponse> DecodeHttpResponse(
    const net::HttpClientResponse& response, const ChatCompletionRequest& request,
    const OpenAiLlmClientOptions& options, core::LoggerAdapter& logger) {
    const auto status_code = response.status;
    const auto& body = response.body;
    if (status_code != 200) {
        core::ErrorCode code = core::ErrorCode::InternalError;
        if (status_code == 401 || status_code == 403) {
            code = core::ErrorCode::PermissionDenied;
        } else if (status_code == 429) {
            code = core::ErrorCode::ResourceExhausted;
        } else if (status_code >= 500 && status_code < 600) {
            code = core::ErrorCode::Unavailable;
        }
        return core::Status(code,
            "LLM API returned status " + std::to_string(status_code) + ": " + body);
    }

    const LlmProtocolContext context{options.default_model, options.response_validation};
    try {
        return options.protocol->DecodeResponse(body, request, context, logger);
    } catch (const std::exception& error) {
        // 第三方协议异常必须在异步 noexcept 回调边界前转为统一 Status。
        logger.error("LLM protocol response exception: {}", error.what());
        return core::Status::Error(core::ErrorCode::InternalError,
                                  std::string("LLM client exception: ") + error.what());
    } catch (...) {
        logger.error("LLM protocol response exception: unknown error");
        return core::Status::Error(core::ErrorCode::InternalError,
                                  "LLM client exception: unknown error");
    }
}

}

core::Result<std::unique_ptr<OpenAiLlmClient>> OpenAiLlmClient::Create(
    OpenAiLlmClientOptions options,
    net::IHttpClient& http_client) {

    if (options.base_url.empty()) {
        return core::Status(core::ErrorCode::InvalidArgument,
            "OpenAI LLM client requires base_url");
    }
    if (options.require_api_key && options.api_key.empty()) {
        return core::Status(core::ErrorCode::InvalidArgument,
            "OpenAI LLM client requires api_key");
    }

    if (!options.protocol) options.protocol = std::make_shared<ChatCompletionsProtocol>();
    auto client = std::unique_ptr<OpenAiLlmClient>(
        new OpenAiLlmClient(std::move(options), http_client));
    return client;
}

OpenAiLlmClient::OpenAiLlmClient(OpenAiLlmClientOptions options, net::IHttpClient& http_client)
    : options_(std::move(options)), http_client_(http_client) {}

OpenAiLlmClient::~OpenAiLlmClient() = default;

core::Result<ChatCompletionResponse> OpenAiLlmClient::Complete(
    const ChatCompletionRequest& req) {
    try {
        if (auto status = options_.protocol->ValidateRequest(req); !status.ok()) {
            core::LoggerAdapter::ForModule("llm-client").warn("LLM request rejected: {}", status.message());
            return status;
        }
        if (req.stream)
            return core::Status::Error(core::ErrorCode::InvalidArgument, "synchronous Complete does not support streaming");
        return ExecuteWithRetry(req);
    } catch (const std::exception& e) {
        return core::Status(core::ErrorCode::InternalError,
            std::string("LLM client exception: ") + e.what());
    } catch (...) {
        return core::Status(core::ErrorCode::InternalError,
            "LLM client exception: unknown error");
    }
}

core::Result<ChatCompletionResponse> OpenAiLlmClient::ExecuteWithRetry(
    const ChatCompletionRequest& req) {

    auto built = BuildHttpRequest(options_, req);
    if (!built.ok()) return built.status();
    auto http_req = std::move(built).value();

    core::Status last_result_status;
    for (int attempt = 0; attempt <= options_.retry_policy.max_retries; ++attempt) {
        if (attempt > 0) {
            auto delay = options_.retry_policy.BackoffFor(attempt - 1);
            std::this_thread::sleep_for(delay);
        }

        auto last_result = http_client_.Execute(http_req);
        last_result_status = last_result.status();
        if (!last_result) {
            // Transport failure — retry if we have attempts left.
            if (attempt < options_.retry_policy.max_retries) {
                continue;
            }
            return last_result.status();
        }

        auto& http_resp = last_result.value();
        // 5xx = server error, retry.  4xx = client error, don't retry.
        if (http_resp.status >= 500 && http_resp.status < 600) {
            if (attempt < options_.retry_policy.max_retries) {
                continue;
            }
        }

        auto logger = core::LoggerAdapter::ForModule("llm-client");
        return DecodeHttpResponse(http_resp, req, options_, logger);
    }

    return last_result_status;
}

namespace {

class AsyncOpenAiOperation;

} // namespace

struct OpenAiAsyncLlmClient::Impl final
    : public std::enable_shared_from_this<OpenAiAsyncLlmClient::Impl> {
    using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

    Impl(OpenAiLlmClientOptions client_options, net::IAsyncHttpClient& client)
        : options(std::move(client_options)),
          http_client(client),
          retry_guard(boost::asio::make_work_guard(retry_context)),
          logger(core::LoggerAdapter::ForModule("async-llm-client")) {}

    core::Status Start() {
        try {
            auto self = shared_from_this();
            retry_thread = std::thread([self] {
                try {
                    self->retry_context.run();
                } catch (const std::exception& error) {
                    self->logger.error("异步 LLM retry runtime 异常退出: {}", error.what());
                } catch (...) {
                    self->logger.error("异步 LLM retry runtime 发生未知异常");
                }
            });
            return core::Status::Ok();
        } catch (const std::exception& error) {
            retry_guard.reset();
            retry_context.stop();
            return core::Status::Error(
                core::ErrorCode::InternalError,
                std::string("failed to start async LLM retry runtime: ") + error.what());
        }
    }

    core::Status Register(const std::shared_ptr<AsyncOpenAiOperation>& operation) {
        std::lock_guard lock(mutex);
        if (stopping) {
            return core::Status::Error(core::ErrorCode::Cancelled,
                                       "async LLM client is shutting down");
        }
        operations.emplace(operation.get(), operation);
        return core::Status::Ok();
    }

    void Unregister(AsyncOpenAiOperation* operation) noexcept {
        std::lock_guard lock(mutex);
        operations.erase(operation);
    }

    void Shutdown() noexcept;

    OpenAiLlmClientOptions options;
    net::IAsyncHttpClient& http_client;
    boost::asio::io_context retry_context;
    WorkGuard retry_guard;
    core::LoggerAdapter logger;
    std::thread retry_thread;
    std::mutex mutex;
    bool stopping = false;
    std::unordered_map<AsyncOpenAiOperation*, std::shared_ptr<AsyncOpenAiOperation>> operations;
};

namespace {

class AsyncOpenAiOperation final : public IAsyncLlmOperation,
                                   public std::enable_shared_from_this<AsyncOpenAiOperation> {
public:
    AsyncOpenAiOperation(std::shared_ptr<OpenAiAsyncLlmClient::Impl> owner,
                         ChatCompletionRequest request,
                         net::HttpClientRequest http_request,
                         IAsyncLlmClient::Callback callback,
                         std::unique_ptr<ILlmStreamDecoder> decoder = {})
        : owner_(std::move(owner)),
          request_(std::move(request)),
          http_request_(std::move(http_request)),
          retry_timer_(owner_->retry_context),
          callback_(std::move(callback)), decoder_(std::move(decoder)) {}

    void Start() noexcept {
        BeginAttempt();
    }

    void Cancel() noexcept override {
        if (cancel_requested_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        std::shared_ptr<net::IAsyncHttpOperation> http_operation;
        {
            std::lock_guard lock(mutex_);
            http_operation = http_operation_;
        }
        if (http_operation) {
            http_operation->Cancel();
        }
        auto self = shared_from_this();
        boost::asio::post(owner_->retry_context, [self] {
            boost::system::error_code ignored;
            self->retry_timer_.cancel(ignored);
        });
        Finish(core::Status::Error(core::ErrorCode::Cancelled,
                                   "async LLM completion cancelled"));
    }

private:
    void BeginAttempt() noexcept {
        if (completed_.load(std::memory_order_acquire)) {
            return;
        }
        auto self = shared_from_this();
        if (request_.stream) {
            BeginStream(self);
            return;
        }
        auto submitted = owner_->http_client.ExecuteAsync(
            http_request_,
            [self](core::Result<net::HttpClientResponse> response) {
                self->OnHttpComplete(std::move(response));
            });
        if (!submitted.ok()) {
            OnAttemptFailure(submitted.status());
            return;
        }
        bool cancel = false;
        {
            std::lock_guard lock(mutex_);
            cancel = completed_.load(std::memory_order_acquire);
            if (!cancel) http_operation_ = submitted.value();
        }
        if (cancel) submitted.value()->Cancel();
    }

    void BeginStream(const std::shared_ptr<AsyncOpenAiOperation>& self) noexcept {
        auto* client = dynamic_cast<net::IAsyncStreamingHttpClient*>(&owner_->http_client);
        if (!client) {
            Finish(core::Status::Error(core::ErrorCode::Unimplemented, "HTTP client has no streaming capability"));
            return;
        }
        try {
            net::HttpStreamOptions stream_options;
            stream_options.max_body_bytes = owner_->options.stream_limits.max_generation_bytes;
            stream_options.read_buffer_bytes = std::min(stream_options.read_buffer_bytes, stream_options.max_body_bytes);
            auto submitted = client->ExecuteStreamingAsync(http_request_, stream_options, {
                .on_headers = [self](const net::HttpClientResponse& headers) {
                    if (headers.status != 200)
                        return DecodeHttpResponse(headers, self->request_, self->owner_->options,
                                                  self->owner_->logger).status();
                    for (const auto& header : headers.headers) {
                        auto name = header.name;
                        auto value = header.value;
                        const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
                        std::transform(name.begin(), name.end(), name.begin(), lower);
                        std::transform(value.begin(), value.end(), value.begin(), lower);
                        const auto semicolon = value.find(';');
                        auto media = value.substr(0, semicolon);
                        while (!media.empty() && media.back() == ' ') media.pop_back();
                        const auto first = media.find_first_not_of(' ');
                        if (name == "content-type" && first != std::string::npos &&
                            media.substr(first) == "text/event-stream") return core::Status::Ok();
                    }
                    return core::Status::Error(core::ErrorCode::DataLoss, "LLM response is not text/event-stream");
                },
                .on_body = [self](std::string_view bytes) {
                    std::lock_guard lock(self->stream_mutex_);
                    if (self->completed_.load() || self->cancel_requested_.load())
                        return core::Status::Error(core::ErrorCode::Cancelled, "LLM stream cancelled");
                    return self->decoder_->Feed(bytes);
                },
                .on_complete = [self](core::Status status) {
                    std::lock_guard lock(self->stream_mutex_);
                    if (self->completed_.load()) return;
                    try {
                        self->Finish(status.ok() ? self->decoder_->Finish()
                                                 : core::Result<ChatCompletionResponse>(status));
                    } catch (...) {
                        self->Finish(core::Status::Error(core::ErrorCode::InternalError,
                                                        "LLM stream decoder finish failed"));
                    }
                },
            });
            if (!submitted.ok()) {
                Finish(submitted.status());
                return;
            }
            bool cancel = false;
            {
                std::lock_guard lock(mutex_);
                cancel = completed_.load();
                if (!cancel) http_operation_ = submitted.value();
            }
            if (cancel) submitted.value()->Cancel();
        } catch (...) {
            Finish(core::Status::Error(core::ErrorCode::InternalError, "LLM stream submission failed"));
        }
    }

    void OnHttpComplete(core::Result<net::HttpClientResponse> result) noexcept {
        {
            std::lock_guard lock(mutex_);
            http_operation_.reset();
        }
        if (completed_.load(std::memory_order_acquire)) {
            return;
        }
        if (!result.ok()) {
            OnAttemptFailure(result.status());
            return;
        }
        auto response = std::move(result).value();
        if (response.status >= 500 && response.status < 600 && CanRetry()) {
            ScheduleRetry();
            return;
        }
        Finish(DecodeHttpResponse(response, request_, owner_->options, owner_->logger));
    }

    void OnAttemptFailure(const core::Status& status) noexcept {
        if (completed_.load(std::memory_order_acquire)) {
            return;
        }
        if (cancel_requested_.load(std::memory_order_acquire)) {
            Finish(core::Status::Error(core::ErrorCode::Cancelled,
                                       "async LLM completion cancelled"));
            return;
        }
        if (CanRetry()) {
            ScheduleRetry();
            return;
        }
        Finish(status);
    }

    bool CanRetry() const noexcept {
        return attempt_ < owner_->options.retry_policy.max_retries;
    }

    void ScheduleRetry() noexcept {
        const auto delay = owner_->options.retry_policy.BackoffFor(attempt_);
        ++attempt_;
        auto self = shared_from_this();
        boost::asio::post(owner_->retry_context, [self, delay] {
            if (self->completed_.load(std::memory_order_acquire)) {
                return;
            }
            self->retry_timer_.expires_after(delay);
            self->retry_timer_.async_wait([self](const boost::system::error_code& error) {
                if (error == boost::asio::error::operation_aborted ||
                    self->completed_.load(std::memory_order_acquire)) {
                    return;
                }
                self->BeginAttempt();
            });
        });
    }

    void Finish(core::Result<ChatCompletionResponse> result) noexcept {
        // 取消可以来自任意业务线程；不能与增量 callback 并发访问 decoder/sink。
        std::lock_guard stream_lock(stream_mutex_);
        if (cancel_requested_.load(std::memory_order_acquire))
            result = core::Status::Error(core::ErrorCode::Cancelled, "async LLM completion cancelled");
        if (completed_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        boost::asio::post(owner_->retry_context, [self = shared_from_this()] {
            boost::system::error_code ignored;
            self->retry_timer_.cancel(ignored);
        });
        owner_->Unregister(this);
        if (!result.ok()) owner_->logger.warn("LLM operation failed code={}", static_cast<int>(result.status().code()));
        auto callback = std::move(callback_);
        try {
            callback(std::move(result));
        } catch (const std::exception& error) {
            owner_->logger.error("异步 LLM callback 抛出异常: {}", error.what());
        } catch (...) {
            owner_->logger.error("异步 LLM callback 抛出未知异常");
        }
    }

    std::shared_ptr<OpenAiAsyncLlmClient::Impl> owner_;
    ChatCompletionRequest request_;
    net::HttpClientRequest http_request_;
    boost::asio::steady_timer retry_timer_;
    IAsyncLlmClient::Callback callback_;
    std::unique_ptr<ILlmStreamDecoder> decoder_;
    std::recursive_mutex stream_mutex_;
    std::mutex mutex_;
    std::shared_ptr<net::IAsyncHttpOperation> http_operation_;
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> completed_{false};
    std::int32_t attempt_ = 0;
};

} // namespace

void OpenAiAsyncLlmClient::Impl::Shutdown() noexcept {
    std::vector<std::shared_ptr<AsyncOpenAiOperation>> pending;
    {
        std::lock_guard lock(mutex);
        if (stopping) {
            return;
        }
        stopping = true;
        pending.reserve(operations.size());
        for (const auto& [_, operation] : operations) {
            pending.push_back(operation);
        }
    }
    for (const auto& operation : pending) {
        operation->Cancel();
    }
    pending.clear();
    retry_guard.reset();
    if (retry_thread.joinable()) {
        if (retry_thread.get_id() == std::this_thread::get_id()) {
            retry_thread.detach();
        } else {
            retry_thread.join();
        }
    }
}

core::Result<std::unique_ptr<OpenAiAsyncLlmClient>> OpenAiAsyncLlmClient::Create(
    OpenAiLlmClientOptions options,
    net::IAsyncHttpClient& http_client) {
    if (options.base_url.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "OpenAI async LLM client requires base_url");
    }
    if (options.require_api_key && options.api_key.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "OpenAI async LLM client requires api_key");
    }
    if (options.timeout_ms <= 0 || options.retry_policy.max_retries < 0) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "OpenAI async LLM timeout and retry options are invalid");
    }
    try {
        if (!options.protocol) options.protocol = std::make_shared<ChatCompletionsProtocol>();
        auto impl = std::make_shared<Impl>(std::move(options), http_client);
        if (auto status = impl->Start(); !status.ok()) {
            return status;
        }
        return std::unique_ptr<OpenAiAsyncLlmClient>(new OpenAiAsyncLlmClient(std::move(impl)));
    } catch (const std::exception& error) {
        return core::Status::Error(
            core::ErrorCode::InternalError,
            std::string("failed to create async LLM client: ") + error.what());
    }
}

OpenAiAsyncLlmClient::OpenAiAsyncLlmClient(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

OpenAiAsyncLlmClient::~OpenAiAsyncLlmClient() {
    Shutdown();
}

core::Result<std::shared_ptr<IAsyncLlmOperation>> OpenAiAsyncLlmClient::CompleteAsync(
    ChatCompletionRequest request,
    Callback callback) {
    if (request.stream) {
        if (auto status = impl_->options.protocol->ValidateRequest(request); !status.ok()) return status;
        return core::Status::Error(core::ErrorCode::InvalidArgument, "use CompleteStreamingAsync for streaming requests");
    }
    return SubmitCompletion(std::move(request), {}, std::move(callback));
}

core::Result<std::shared_ptr<IAsyncLlmOperation>> OpenAiAsyncLlmClient::CompleteStreamingAsync(
    ChatCompletionRequest request, LlmEventSink sink, Callback callback) {
    if (!sink) return core::Status::Error(core::ErrorCode::InvalidArgument, "LLM stream sink is required");
    request.stream = true;
    return SubmitCompletion(std::move(request), std::move(sink), std::move(callback));
}

core::Result<std::shared_ptr<IAsyncLlmOperation>> OpenAiAsyncLlmClient::SubmitCompletion(
    ChatCompletionRequest request, LlmEventSink sink, Callback callback) {
    if (!callback) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "async LLM callback is required");
    }
    try {
        if (auto status = impl_->options.protocol->ValidateRequest(request); !status.ok()) {
            impl_->logger.warn("LLM request rejected: {}", status.message());
            return status;
        }
        auto built = BuildHttpRequest(impl_->options, request);
        if (!built.ok()) return built.status();
        std::unique_ptr<ILlmStreamDecoder> decoder;
        if (request.stream) {
            if (!dynamic_cast<net::IAsyncStreamingHttpClient*>(&impl_->http_client))
                return core::Status::Error(core::ErrorCode::Unimplemented, "HTTP client has no streaming capability");
            LlmProtocolContext context{impl_->options.default_model, impl_->options.response_validation,
                                       impl_->options.stream_limits, std::move(sink)};
            auto created = impl_->options.protocol->CreateStreamDecoder(request, context);
            if (!created.ok()) return created.status();
            decoder = std::move(created).value();
        }
        auto operation = std::make_shared<AsyncOpenAiOperation>(
            impl_, std::move(request), std::move(built).value(), std::move(callback), std::move(decoder));
        if (auto status = impl_->Register(operation); !status.ok()) {
            return status;
        }
        operation->Start();
        return std::static_pointer_cast<IAsyncLlmOperation>(std::move(operation));
    } catch (const std::exception& error) {
        return core::Status::Error(
            core::ErrorCode::InternalError,
            std::string("failed to submit async LLM completion: ") + error.what());
    }
}

void OpenAiAsyncLlmClient::Shutdown() noexcept {
    if (impl_) {
        impl_->Shutdown();
    }
}


}
