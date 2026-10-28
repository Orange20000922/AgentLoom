#pragma once

#include "../core/result.h"
#include "../core/thread_pool.h"
#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <functional>
#include <memory>

namespace agent::skill {

class ISkillDeadline {
public:
    virtual ~ISkillDeadline() = default;
    virtual void Cancel() noexcept = 0;
};

// 复用宿主的事件循环与线程池，不创建第二套调度线程。
class ISkillDeadlineScheduler {
public:
    virtual ~ISkillDeadlineScheduler() = default;
    virtual core::Result<std::shared_ptr<ISkillDeadline>> Schedule(
        std::chrono::steady_clock::time_point deadline,
        std::function<void()> completion) = 0;
};

std::shared_ptr<ISkillDeadlineScheduler> MakeSkillDeadlineScheduler(
    boost::asio::any_io_executor executor, core::ThreadPool& completion_pool);

}
