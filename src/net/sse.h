#pragma once

#include "result.h"

#include <functional>
#include <string>
#include <string_view>

namespace agent::net {

struct SseEvent {
    std::string data;
    std::string event;
    std::string id;
};

// HTTP body 边界与 SSE 行无关；解析器逐请求持有状态，回调期间只借用事件。
class SseDecoder {
public:
    using Sink = std::function<core::Status(const SseEvent&)>;
    explicit SseDecoder(Sink sink, std::size_t max_event_bytes = 256 * 1024);
    core::Status Feed(std::string_view bytes);
    core::Status Finish();

private:
    core::Status Line();
    Sink sink_;
    std::size_t limit_;
    std::size_t event_bytes_ = 0;
    std::string line_;
    SseEvent event_;
    bool has_data_ = false;
    bool skip_lf_ = false;
    bool first_line_ = true;
    core::Status status_;
};

}
