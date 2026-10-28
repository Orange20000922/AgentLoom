#include "../../src/service/persona/context_plan.h"
#include <algorithm>
#include <chrono>
#include <iostream>

// 仅量化来源装配与 JSON 序列化，不推算真实模型、Redis 或端到端吞吐。
int main() {
    using namespace agent::service::persona;
    std::vector<ContextItem> items;
    for (int i = 0; i < 48; ++i) items.push_back({ContextSource::L3, std::to_string(i), std::string(128, 'a')});
    std::vector<double> samples;
    std::size_t bytes = 0;
    for (int i = 0; i < 3000; ++i) {
        const auto start = std::chrono::steady_clock::now();
        ContextBudgetReport report;
        auto result = CompileContext(items, {}, report);
        if (!result.ok()) return 1;
        bytes += result.value().size();
        samples.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(samples.begin(), samples.end());
    std::cout << "{\"iterations\":3000,\"items_per_context\":48,\"payload_bytes_per_context\":6144,"
        << "\"p50_us\":" << samples[1500] << ",\"p95_us\":" << samples[2850]
        << ",\"p99_us\":" << samples[2970] << ",\"serialized_bytes\":" << bytes << "}\n";
}
