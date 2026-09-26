#include "skill_vision_event_sink.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

using agent::service::persona::SkillSessionManager;
using agent::service::persona::SkillSessionState;
using agent::service::persona::SkillVisionEventSink;
using agent::service::persona::SkillVisionEventSinkOptions;

TEST(SkillVisionEventSinkTest, RecordsVisionEventAsSkillObservation) {
    auto manager = std::make_shared<SkillSessionManager>();
    SkillVisionEventSink sink(manager);

    media::VisionEvent event;
    event.event_id = "vision-event-1";
    event.session_id = "session-runtime";
    event.trace_id = "trace-vision-event";
    event.peak_frame_id = 7;
    event.representative_frame_id = 7;
    event.peak_score = 0.81;
    media::VisionAnalysis analysis;
    analysis.agent_hint = "画面中检测到明显移动";
    analysis.confidence = 0.76;
    analysis.facts = {"画面中有移动"};
    event.analysis = analysis;

    auto status = sink.Publish(event);

    ASSERT_TRUE(status.ok()) << status.message();
    auto session = manager->Get("session-runtime", "vision.observe");
    ASSERT_TRUE(session.ok()) << session.status().message();
    ASSERT_TRUE(session.value().has_value());
    EXPECT_EQ(session.value()->state, SkillSessionState::Running);
    EXPECT_EQ(session.value()->last_observation, "画面中检测到明显移动");
    ASSERT_EQ(session.value()->recent_observations.size(), 1u);
    EXPECT_EQ(session.value()->recent_observations[0].confidence, 0.76);
    EXPECT_NE(
        session.value()->recent_observations[0].metadata_json.find("vision-event-1"),
        std::string::npos);
}

TEST(SkillVisionEventSinkTest, DuplicateVisionEventIsRecordedButNotInjected) {
    auto manager = std::make_shared<SkillSessionManager>();
    SkillVisionEventSinkOptions options;
    options.min_prompt_confidence = 0.1;
    SkillVisionEventSink sink(manager, options);

    media::VisionEvent event;
    event.event_id = "vision-event-duplicate";
    event.session_id = "session-runtime";
    event.trace_id = "trace-vision-event";
    event.peak_score = 0.91;
    event.duplicate = true;
    event.analysis = media::VisionAnalysis{.agent_hint = "重复视觉事件", .confidence = 0.8};

    auto status = sink.Publish(event);

    ASSERT_TRUE(status.ok()) << status.message();
    auto session = manager->Get("session-runtime", "vision.observe");
    ASSERT_TRUE(session.ok()) << session.status().message();
    ASSERT_TRUE(session.value().has_value());
    EXPECT_EQ(session.value()->state, SkillSessionState::Running);
    EXPECT_TRUE(session.value()->last_observation.empty());
    ASSERT_EQ(session.value()->recent_observations.size(), 1u);
    EXPECT_TRUE(session.value()->recent_observations[0].stale);
    EXPECT_FALSE(session.value()->recent_observations[0].should_inject_prompt);
}

} // namespace
