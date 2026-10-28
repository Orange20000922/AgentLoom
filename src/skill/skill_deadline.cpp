#include "skill_deadline.h"
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <atomic>

namespace agent::skill {
namespace {
class Deadline final : public ISkillDeadline, public std::enable_shared_from_this<Deadline> {
public:
    Deadline(boost::asio::any_io_executor executor, core::ThreadPool& pool)
        : timer_(boost::asio::make_strand(std::move(executor))), pool_(pool) {}

    void Start(std::chrono::steady_clock::time_point deadline, std::function<void()> callback) {
        callback_ = std::move(callback);
        timer_.expires_at(deadline);
        timer_.async_wait([self = shared_from_this()](const boost::system::error_code& error) {
            if (error || self->cancelled_.load()) return;
            auto callback = std::move(self->callback_);
            auto task = std::make_shared<std::function<void()>>(std::move(callback));
            auto status = self->pool_.Submit([task] { (*task)(); }, {}, "skill-deadline");
            // 队列拒绝不能吞掉 deadline；此处只通知工具状态机结束，不执行正常业务。
            if (!status.ok()) (*task)();
        });
    }

    void Cancel() noexcept override {
        cancelled_.store(true);
        try {
            boost::asio::post(timer_.get_executor(), [self = shared_from_this()] {
                self->timer_.cancel();
                self->callback_ = {};
            });
        } catch (...) {
            // executor 停止后仍以原子标记抑制回调；宿主负责先 drain 工具再停事件循环。
        }
    }
private:
    boost::asio::steady_timer timer_;
    core::ThreadPool& pool_;
    std::atomic<bool> cancelled_{false};
    std::function<void()> callback_;
};

class Scheduler final : public ISkillDeadlineScheduler {
public:
    Scheduler(boost::asio::any_io_executor executor, core::ThreadPool& pool)
        : executor_(std::move(executor)), pool_(pool) {}
    core::Result<std::shared_ptr<ISkillDeadline>> Schedule(
        std::chrono::steady_clock::time_point deadline, std::function<void()> callback) override {
        try {
            auto timer = std::make_shared<Deadline>(executor_, pool_);
            timer->Start(deadline, std::move(callback));
            return std::shared_ptr<ISkillDeadline>(std::move(timer));
        } catch (const std::exception& error) {
            return core::Status::Error(core::ErrorCode::InternalError, error.what());
        }
    }
private:
    boost::asio::any_io_executor executor_;
    core::ThreadPool& pool_;
};
}

std::shared_ptr<ISkillDeadlineScheduler> MakeSkillDeadlineScheduler(
    boost::asio::any_io_executor executor, core::ThreadPool& pool) {
    return std::make_shared<Scheduler>(std::move(executor), pool);
}
}
