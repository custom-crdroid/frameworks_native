/*
 * Copyright 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#undef LOG_TAG
#define LOG_TAG "LayerHistoryIntegrationTest"

#include <Layer.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <log/log.h>

#include <renderengine/mock/FakeExternalTexture.h>

#include "FpsOps.h"
#include "LayerHierarchyTest.h"
#include "Scheduler/LayerHistory.h"
#include "Scheduler/LayerInfo.h"
#include "TestableScheduler.h"
#include "TestableSurfaceFlinger.h"
#include "mock/DisplayHardware/MockDisplayMode.h"
#include "mock/MockSchedulerCallback.h"

namespace android::scheduler {

using android::mock::createDisplayMode;

class LayerHistoryIntegrationTest : public surfaceflinger::frontend::LayerSnapshotTestBase {
protected:
    static constexpr auto PRESENT_TIME_HISTORY_SIZE = LayerInfo::HISTORY_SIZE;
    static constexpr auto MAX_FREQUENT_LAYER_PERIOD_NS = LayerInfo::kMaxPeriodForFrequentLayerNs;
    static constexpr auto FREQUENT_LAYER_WINDOW_SIZE = LayerInfo::kFrequentLayerWindowSize;
    static constexpr auto PRESENT_TIME_HISTORY_DURATION = LayerInfo::HISTORY_DURATION;

    static constexpr Fps LO_FPS = 30_Hz;
    static constexpr auto LO_FPS_PERIOD = LO_FPS.getPeriodNsecs();

    static constexpr Fps HI_FPS = 90_Hz;
    static constexpr auto HI_FPS_PERIOD = HI_FPS.getPeriodNsecs();

    LayerHistoryIntegrationTest() : LayerSnapshotTestBase() {
        mFlinger.resetScheduler(mScheduler);
        mLifecycleManager = {};
        mHierarchyBuilder = {{}};
    }

    void updateLayerSnapshotsAndLayerHistory(nsecs_t now) {
        LayerSnapshotTestBase::update(mFlinger.mutableLayerSnapshotBuilder());
        mFlinger.updateLayerHistory(now);
    }

    void setBufferWithPresentTime(sp<Layer>& layer, nsecs_t time) {
        uint32_t sequence = static_cast<uint32_t>(layer->sequence);
        setBuffer(sequence);
        layer->setDesiredPresentTime(time, false /*autotimestamp*/);
        updateLayerSnapshotsAndLayerHistory(time);
    }

    LayerHistory& history() { return mScheduler->mutableLayerHistory(); }
    const LayerHistory& history() const { return mScheduler->mutableLayerHistory(); }

    LayerHistory::Summary summarizeLayerHistory(nsecs_t now) {
        // LayerHistory::summarize makes no guarantee of the order of the elements in the summary
        // however, for testing only, a stable order is required, therefore we sort the list here.
        // Any tests requiring ordered results must create layers with names.
        auto summary = history().summarize(*mScheduler->refreshRateSelector(), now);
        std::sort(summary.begin(), summary.end(),
                  [](const RefreshRateSelector::LayerRequirement& lhs,
                     const RefreshRateSelector::LayerRequirement& rhs) -> bool {
                      return lhs.name < rhs.name;
                  });
        return summary;
    }

    size_t layerCount() const { return mScheduler->layerHistorySize(); }
    size_t activeLayerCount() const NO_THREAD_SAFETY_ANALYSIS {
        return history().mActiveLayerInfos.size();
    }

    auto frequentLayerCount(nsecs_t now) const NO_THREAD_SAFETY_ANALYSIS {
        const auto& infos = history().mActiveLayerInfos;
        return std::count_if(infos.begin(), infos.end(), [now](const auto& pair) {
            return pair.second.second->isFrequent(now).isFrequent;
        });
    }

    auto animatingLayerCount(nsecs_t now) const NO_THREAD_SAFETY_ANALYSIS {
        const auto& infos = history().mActiveLayerInfos;
        return std::count_if(infos.begin(), infos.end(), [now](const auto& pair) {
            return pair.second.second->isAnimating(now);
        });
    }

    auto clearLayerHistoryCount(nsecs_t now) const NO_THREAD_SAFETY_ANALYSIS {
        const auto& infos = history().mActiveLayerInfos;
        return std::count_if(infos.begin(), infos.end(), [now](const auto& pair) {
            return pair.second.second->isFrequent(now).clearHistory;
        });
    }

    void setDefaultLayerVote(Layer* layer,
                             LayerHistory::LayerVoteType vote) NO_THREAD_SAFETY_ANALYSIS {
        auto [found, layerPair] = history().findLayer(layer->getSequence());
        if (found != LayerHistory::LayerStatus::NotFound) {
            layerPair->second->setDefaultLayerVote(vote);
        }
    }

    auto createLegacyAndFrontedEndLayer(uint32_t sequence) {
        std::string layerName = "test layer:" + std::to_string(sequence);
        const auto layer =
                sp<Layer>::make(LayerCreationArgs{mFlinger.flinger(),
                                                  nullptr,
                                                  layerName,
                                                  0,
                                                  {},
                                                  std::make_optional<uint32_t>(sequence)});
        mFlinger.injectLegacyLayer(layer);
        createRootLayer(sequence);
        return layer;
    }

    auto destroyLayer(sp<Layer>& layer) {
        uint32_t sequence = static_cast<uint32_t>(layer->sequence);
        mFlinger.releaseLegacyLayer(sequence);
        layer.clear();
        destroyLayerHandle(sequence);
    }

    void recordFramesAndExpect(sp<Layer>& layer, nsecs_t& time, Fps frameRate,
                               Fps desiredRefreshRate, int numFrames) {
        LayerHistory::Summary summary;
        for (int i = 0; i < numFrames; i++) {
            setBufferWithPresentTime(layer, time);
            time += frameRate.getPeriodNsecs();

            summary = summarizeLayerHistory(time);
        }

        ASSERT_EQ(1u, summary.size());
        ASSERT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
        ASSERT_EQ(desiredRefreshRate, summary[0].desiredRefreshRate);
    }

    std::shared_ptr<RefreshRateSelector> mSelector =
            std::make_shared<RefreshRateSelector>(makeModes(createDisplayMode(DisplayModeId(0),
                                                                              LO_FPS),
                                                            createDisplayMode(DisplayModeId(1),
                                                                              HI_FPS)),
                                                  DisplayModeId(0));

    mock::SchedulerCallback mSchedulerCallback;

    TestableScheduler* mScheduler = new TestableScheduler(mSelector, mSchedulerCallback);

    TestableSurfaceFlinger mFlinger;
};

namespace {

TEST_F(LayerHistoryIntegrationTest, singleLayerNoVoteDefaultCompatibility) {
    createLegacyAndFrontedEndLayer(1);
    nsecs_t time = systemTime();

    updateLayerSnapshotsAndLayerHistory(time);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    // No layers returned if no layers are active.
    EXPECT_TRUE(summarizeLayerHistory(time).empty());
    EXPECT_EQ(0u, activeLayerCount());

    setBuffer(1);
    setDefaultFrameRateCompatibility(1, ANATIVEWINDOW_FRAME_RATE_NO_VOTE);
    updateLayerSnapshotsAndLayerHistory(time);

    EXPECT_TRUE(summarizeLayerHistory(time).empty());
    EXPECT_EQ(1u, activeLayerCount());
}

TEST_F(LayerHistoryIntegrationTest, singleLayerMinVoteDefaultCompatibility) {
    createLegacyAndFrontedEndLayer(1);
    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    EXPECT_TRUE(summarizeLayerHistory(time).empty());
    EXPECT_EQ(0u, activeLayerCount());

    setBuffer(1);
    setDefaultFrameRateCompatibility(1, ANATIVEWINDOW_FRAME_RATE_MIN);
    updateLayerSnapshotsAndLayerHistory(time);

    auto summary = summarizeLayerHistory(time);
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());

    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
}

TEST_F(LayerHistoryIntegrationTest, oneInvisibleLayer) {
    createLegacyAndFrontedEndLayer(1);
    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);
    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    setBuffer(1);
    updateLayerSnapshotsAndLayerHistory(time);
    auto summary = summarizeLayerHistory(time);
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    // Layer is still considered inactive so we expect to get Min
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());

    hideLayer(1);
    setBuffer(1);
    updateLayerSnapshotsAndLayerHistory(time);

    summary = summarizeLayerHistory(time);
    EXPECT_TRUE(summarizeLayerHistory(time).empty());
    EXPECT_EQ(0u, activeLayerCount());
}

TEST_F(LayerHistoryIntegrationTest, oneLayerExplicitVote) {
    createLegacyAndFrontedEndLayer(1);
    setFrameRate(1, 73.4f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitDefault, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(73.4_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, oneLayerExplicitExactVote) {
    createLegacyAndFrontedEndLayer(1);
    setFrameRate(1, 73.4f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitExactOrMultiple,
              summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(73.4_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, oneLayerExplicitCategory) {
    createLegacyAndFrontedEndLayer(1);
    setFrameRateCategory(1, ANATIVEWINDOW_FRAME_RATE_CATEGORY_HIGH);

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());

    nsecs_t time = systemTime();
    updateLayerSnapshotsAndLayerHistory(time);
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    // First LayerRequirement is the frame rate specification
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitCategory, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(0_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(FrameRateCategory::High, summarizeLayerHistory(time)[0].frameRateCategory);
}

TEST_F(LayerHistoryIntegrationTest, multipleLayers) {
    auto layer1 = createLegacyAndFrontedEndLayer(1);
    auto layer2 = createLegacyAndFrontedEndLayer(2);
    auto layer3 = createLegacyAndFrontedEndLayer(3);

    nsecs_t time = systemTime();

    EXPECT_EQ(3u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));

    LayerHistory::Summary summary;

    // layer1 is active but infrequent.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer1, time);
        time += MAX_FREQUENT_LAYER_PERIOD_NS.count();
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(1u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summary[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));

    // layer2 is frequent and has high refresh rate.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer2, time);
        time += HI_FPS_PERIOD;
        summary = summarizeLayerHistory(time);
    }

    // layer1 is still active but infrequent.
    setBufferWithPresentTime(layer1, time);

    ASSERT_EQ(2u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summary[0].vote);
    ASSERT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[1].vote);
    EXPECT_EQ(HI_FPS, summarizeLayerHistory(time)[1].desiredRefreshRate);

    EXPECT_EQ(2u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));

    // layer1 is no longer active.
    // layer2 is frequent and has low refresh rate.
    for (size_t i = 0; i < 2 * PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer2, time);
        time += LO_FPS_PERIOD;
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(1u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
    EXPECT_EQ(LO_FPS, summary[0].desiredRefreshRate);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));

    // layer2 still has low refresh rate.
    // layer3 has high refresh rate but not enough history.
    constexpr int RATIO = LO_FPS_PERIOD / HI_FPS_PERIOD;
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE - 1; i++) {
        if (i % RATIO == 0) {
            setBufferWithPresentTime(layer2, time);
        }

        setBufferWithPresentTime(layer3, time);
        time += HI_FPS_PERIOD;
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(2u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
    EXPECT_EQ(LO_FPS, summary[0].desiredRefreshRate);
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summary[1].vote);
    EXPECT_EQ(2u, activeLayerCount());
    EXPECT_EQ(2, frequentLayerCount(time));

    // layer3 becomes recently active.
    setBufferWithPresentTime(layer3, time);
    summary = summarizeLayerHistory(time);
    ASSERT_EQ(2u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
    EXPECT_EQ(LO_FPS, summary[0].desiredRefreshRate);
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[1].vote);
    EXPECT_EQ(HI_FPS, summary[1].desiredRefreshRate);
    EXPECT_EQ(2u, activeLayerCount());
    EXPECT_EQ(2, frequentLayerCount(time));

    // layer1 expires.
    destroyLayer(layer1);
    updateLayerSnapshotsAndLayerHistory(time);

    summary = summarizeLayerHistory(time);
    ASSERT_EQ(2u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
    EXPECT_EQ(LO_FPS, summary[0].desiredRefreshRate);
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[1].vote);
    EXPECT_EQ(HI_FPS, summary[1].desiredRefreshRate);
    EXPECT_EQ(2u, layerCount());
    EXPECT_EQ(2u, activeLayerCount());
    EXPECT_EQ(2, frequentLayerCount(time));

    // layer2 still has low refresh rate.
    // layer3 becomes inactive.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer2, time);
        time += LO_FPS_PERIOD;
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(1u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
    EXPECT_EQ(LO_FPS, summary[0].desiredRefreshRate);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));

    // layer2 expires.
    destroyLayer(layer2);
    updateLayerSnapshotsAndLayerHistory(time);
    summary = summarizeLayerHistory(time);
    EXPECT_TRUE(summary.empty());
    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));

    // layer3 becomes active and has high refresh rate.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE + FREQUENT_LAYER_WINDOW_SIZE + 1; i++) {
        setBufferWithPresentTime(layer3, time);
        time += HI_FPS_PERIOD;
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(1u, summary.size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
    EXPECT_EQ(HI_FPS, summary[0].desiredRefreshRate);
    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));

    // layer3 expires.
    destroyLayer(layer3);
    updateLayerSnapshotsAndLayerHistory(time);
    summary = summarizeLayerHistory(time);
    EXPECT_TRUE(summary.empty());
    EXPECT_EQ(0u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, inactiveLayers) {
    auto layer = createLegacyAndFrontedEndLayer(1);
    nsecs_t time = systemTime();

    // the very first updates makes the layer frequent
    for (size_t i = 0; i < FREQUENT_LAYER_WINDOW_SIZE - 1; i++) {
        setBufferWithPresentTime(layer, time);
        time += MAX_FREQUENT_LAYER_PERIOD_NS.count();

        EXPECT_EQ(1u, layerCount());
        ASSERT_EQ(1u, summarizeLayerHistory(time).size());
        EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
        EXPECT_EQ(1u, activeLayerCount());
        EXPECT_EQ(1, frequentLayerCount(time));
    }

    // the next update with the MAX_FREQUENT_LAYER_PERIOD_NS will get us to infrequent
    time += MAX_FREQUENT_LAYER_PERIOD_NS.count();
    setBufferWithPresentTime(layer, time);

    EXPECT_EQ(1u, layerCount());
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));

    // advance the time for the previous frame to be inactive
    time += MAX_ACTIVE_LAYER_PERIOD_NS.count();

    // Now even if we post a quick few frame we should stay infrequent
    for (size_t i = 0; i < FREQUENT_LAYER_WINDOW_SIZE - 1; i++) {
        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;

        EXPECT_EQ(1u, layerCount());
        ASSERT_EQ(1u, summarizeLayerHistory(time).size());
        EXPECT_EQ(LayerHistory::LayerVoteType::Min, summarizeLayerHistory(time)[0].vote);
        EXPECT_EQ(1u, activeLayerCount());
        EXPECT_EQ(0, frequentLayerCount(time));
    }

    // More quick frames will get us to frequent again
    setBufferWithPresentTime(layer, time);
    time += HI_FPS_PERIOD;

    EXPECT_EQ(1u, layerCount());
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, invisibleExplicitLayer) {
    auto explicitVisiblelayer = createLegacyAndFrontedEndLayer(1);
    auto explicitInvisiblelayer = createLegacyAndFrontedEndLayer(2);
    hideLayer(2);
    setFrameRate(1, 60.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    setFrameRate(2, 90.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    nsecs_t time = systemTime();

    // Post a buffer to the layers to make them active
    setBufferWithPresentTime(explicitVisiblelayer, time);
    setBufferWithPresentTime(explicitInvisiblelayer, time);

    EXPECT_EQ(2u, layerCount());
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::ExplicitExactOrMultiple,
              summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(60_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(2u, activeLayerCount());
    EXPECT_EQ(2, frequentLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, infrequentAnimatingLayer) {
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // layer is active but infrequent.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += MAX_FREQUENT_LAYER_PERIOD_NS.count();
    }

    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // another update with the same cadence keep in infrequent
    setBufferWithPresentTime(layer, time);
    time += MAX_FREQUENT_LAYER_PERIOD_NS.count();

    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    mFlinger.mutableLayerSnapshotBuilder().getSnapshot(1)->changes |=
            frontend::RequestedLayerState::Changes::Animation;
    mFlinger.updateLayerHistory(time);
    // an update as animation will immediately vote for Max
    time += MAX_FREQUENT_LAYER_PERIOD_NS.count();

    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(1, animatingLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, frequentLayerBecomingInfrequentAndBack) {
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // Fill up the window with frequent updates
    for (size_t i = 0; i < FREQUENT_LAYER_WINDOW_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += (60_Hz).getPeriodNsecs();

        EXPECT_EQ(1u, layerCount());
        ASSERT_EQ(1u, summarizeLayerHistory(time).size());
        EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
        EXPECT_EQ(1u, activeLayerCount());
        EXPECT_EQ(1, frequentLayerCount(time));
    }

    // posting a buffer after long inactivity should retain the layer as active
    time += std::chrono::nanoseconds(3s).count();
    setBufferWithPresentTime(layer, time);
    EXPECT_EQ(0, clearLayerHistoryCount(time));
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(60_Hz, summarizeLayerHistory(time)[0].desiredRefreshRate);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // posting more infrequent buffer should make the layer infrequent
    time += (MAX_FREQUENT_LAYER_PERIOD_NS + 1ms).count();
    setBufferWithPresentTime(layer, time);
    time += (MAX_FREQUENT_LAYER_PERIOD_NS + 1ms).count();
    setBufferWithPresentTime(layer, time);
    EXPECT_EQ(0, clearLayerHistoryCount(time));
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // posting another buffer should keep the layer infrequent
    setBufferWithPresentTime(layer, time);
    EXPECT_EQ(0, clearLayerHistoryCount(time));
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Min, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // posting more buffers would mean starting of an animation, so making the layer frequent
    setBufferWithPresentTime(layer, time);
    setBufferWithPresentTime(layer, time);
    EXPECT_EQ(1, clearLayerHistoryCount(time));
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // posting a buffer after long inactivity should retain the layer as active
    time += std::chrono::nanoseconds(3s).count();
    setBufferWithPresentTime(layer, time);
    EXPECT_EQ(0, clearLayerHistoryCount(time));
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // posting another buffer should keep the layer frequent
    time += (60_Hz).getPeriodNsecs();
    setBufferWithPresentTime(layer, time);
    EXPECT_EQ(0, clearLayerHistoryCount(time));
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, inconclusiveLayerBecomingFrequent) {
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // Fill up the window with frequent updates
    for (size_t i = 0; i < FREQUENT_LAYER_WINDOW_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += (60_Hz).getPeriodNsecs();

        EXPECT_EQ(1u, layerCount());
        ASSERT_EQ(1u, summarizeLayerHistory(time).size());
        EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
        EXPECT_EQ(1u, activeLayerCount());
        EXPECT_EQ(1, frequentLayerCount(time));
    }

    // posting infrequent buffers after long inactivity should make the layer
    // inconclusive but frequent.
    time += std::chrono::nanoseconds(3s).count();
    setBufferWithPresentTime(layer, time);
    time += (MAX_FREQUENT_LAYER_PERIOD_NS + 1ms).count();
    setBufferWithPresentTime(layer, time);
    EXPECT_EQ(0, clearLayerHistoryCount(time));
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Heuristic, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // posting more buffers should make the layer frequent and switch the refresh rate to max
    // by clearing the history
    setBufferWithPresentTime(layer, time);
    setBufferWithPresentTime(layer, time);
    setBufferWithPresentTime(layer, time);
    EXPECT_EQ(1, clearLayerHistoryCount(time));
    ASSERT_EQ(1u, summarizeLayerHistory(time).size());
    EXPECT_EQ(LayerHistory::LayerVoteType::Max, summarizeLayerHistory(time)[0].vote);
    EXPECT_EQ(1u, activeLayerCount());
    EXPECT_EQ(1, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));
}

TEST_F(LayerHistoryIntegrationTest, getFramerate) {
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));
    EXPECT_EQ(0, animatingLayerCount(time));

    // layer is active but infrequent.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        setBufferWithPresentTime(layer, time);
        time += MAX_FREQUENT_LAYER_PERIOD_NS.count();
    }

    float expectedFramerate = 1e9f / MAX_FREQUENT_LAYER_PERIOD_NS.count();
    EXPECT_FLOAT_EQ(expectedFramerate, history().getLayerFramerate(time, layer->getSequence()));
}

TEST_F(LayerHistoryIntegrationTest, heuristicLayer60Hz) {
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();
    for (float fps = 54.0f; fps < 65.0f; fps += 0.1f) {
        recordFramesAndExpect(layer, time, Fps::fromValue(fps), 60_Hz, PRESENT_TIME_HISTORY_SIZE);
    }
}

TEST_F(LayerHistoryIntegrationTest, heuristicLayer60_30Hz) {
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();
    recordFramesAndExpect(layer, time, 60_Hz, 60_Hz, PRESENT_TIME_HISTORY_SIZE);

    recordFramesAndExpect(layer, time, 60_Hz, 60_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 30_Hz, 60_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 30_Hz, 30_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 60_Hz, 30_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 60_Hz, 60_Hz, PRESENT_TIME_HISTORY_SIZE);
}

TEST_F(LayerHistoryIntegrationTest, heuristicLayerNotOscillating) {
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();

    recordFramesAndExpect(layer, time, 27.1_Hz, 30_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 26.9_Hz, 30_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 26_Hz, 24_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 26.9_Hz, 24_Hz, PRESENT_TIME_HISTORY_SIZE);
    recordFramesAndExpect(layer, time, 27.1_Hz, 30_Hz, PRESENT_TIME_HISTORY_SIZE);
}

TEST_F(LayerHistoryIntegrationTest, smallDirtyLayer) {
    auto layer = createLegacyAndFrontedEndLayer(1);

    nsecs_t time = systemTime();

    EXPECT_EQ(1u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));

    LayerHistory::Summary summary;

    // layer is active but infrequent.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE; i++) {
        auto props = layer->getLayerProps();
        if (i % 3 == 0) {
            props.isSmallDirty = false;
        } else {
            props.isSmallDirty = true;
        }

        setBufferWithPresentTime(layer, time);
        time += HI_FPS_PERIOD;
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(1u, summary.size());
    ASSERT_EQ(LayerHistory::LayerVoteType::Heuristic, summary[0].vote);
    EXPECT_GE(HI_FPS, summary[0].desiredRefreshRate);
}

TEST_F(LayerHistoryIntegrationTest, DISABLED_smallDirtyInMultiLayer) {
    auto uiLayer = createLegacyAndFrontedEndLayer(1);
    auto videoLayer = createLegacyAndFrontedEndLayer(2);
    setFrameRate(2, 30.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,
                 ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);

    nsecs_t time = systemTime();

    EXPECT_EQ(2u, layerCount());
    EXPECT_EQ(0u, activeLayerCount());
    EXPECT_EQ(0, frequentLayerCount(time));

    LayerHistory::Summary summary;

    // uiLayer is updating small dirty.
    for (size_t i = 0; i < PRESENT_TIME_HISTORY_SIZE + FREQUENT_LAYER_WINDOW_SIZE + 1; i++) {
        auto props = uiLayer->getLayerProps();
        props.isSmallDirty = true;
        setBuffer(1);
        uiLayer->setDesiredPresentTime(0, false /*autotimestamp*/);
        updateLayerSnapshotsAndLayerHistory(time);
        setBufferWithPresentTime(videoLayer, time);
        summary = summarizeLayerHistory(time);
    }

    ASSERT_EQ(1u, summary.size());
    ASSERT_EQ(LayerHistory::LayerVoteType::ExplicitDefault, summary[0].vote);
    ASSERT_EQ(30_Hz, summary[0].desiredRefreshRate);
}

class LayerHistoryIntegrationTestParameterized
      : public LayerHistoryIntegrationTest,
        public testing::WithParamInterface<std::chrono::nanoseconds> {};

TEST_P(LayerHistoryIntegrationTestParameterized, HeuristicLayerWithInfrequentLayer) {
    std::chrono::nanoseconds infrequentUpdateDelta = GetParam();
    auto heuristicLayer = createLegacyAndFrontedEndLayer(1);
    auto infrequentLayer = createLegacyAndFrontedEndLayer(2);

    const nsecs_t startTime = systemTime();

    const std::chrono::nanoseconds heuristicUpdateDelta = 41'666'667ns;
    setBufferWithPresentTime(heuristicLayer, startTime);
    setBufferWithPresentTime(infrequentLayer, startTime);

    nsecs_t time = startTime;
    nsecs_t lastInfrequentUpdate = startTime;
    const size_t totalInfrequentLayerUpdates = FREQUENT_LAYER_WINDOW_SIZE * 5;
    size_t infrequentLayerUpdates = 0;
    while (infrequentLayerUpdates <= totalInfrequentLayerUpdates) {
        time += heuristicUpdateDelta.count();
        setBufferWithPresentTime(heuristicLayer, time);

        if (time - lastInfrequentUpdate >= infrequentUpdateDelta.count()) {
            ALOGI("submitting infrequent frame [%zu/%zu]", infrequentLayerUpdates,
                  totalInfrequentLayerUpdates);
            lastInfrequentUpdate = time;
            setBufferWithPresentTime(infrequentLayer, time);
            infrequentLayerUpdates++;
        }

        if (time - startTime > PRESENT_TIME_HISTORY_DURATION.count()) {
            ASSERT_NE(0u, summarizeLayerHistory(time).size());
            ASSERT_GE(2u, summarizeLayerHistory(time).size());

            bool max = false;
            bool min = false;
            Fps heuristic;
            for (const auto& layer : summarizeLayerHistory(time)) {
                if (layer.vote == LayerHistory::LayerVoteType::Heuristic) {
                    heuristic = layer.desiredRefreshRate;
                } else if (layer.vote == LayerHistory::LayerVoteType::Max) {
                    max = true;
                } else if (layer.vote == LayerHistory::LayerVoteType::Min) {
                    min = true;
                }
            }

            if (infrequentLayerUpdates > FREQUENT_LAYER_WINDOW_SIZE) {
                EXPECT_EQ(24_Hz, heuristic);
                EXPECT_FALSE(max);
                if (summarizeLayerHistory(time).size() == 2) {
                    EXPECT_TRUE(min);
                }
            }
        }
    }
}

INSTANTIATE_TEST_CASE_P(LeapYearTests, LayerHistoryIntegrationTestParameterized,
                        ::testing::Values(1s, 2s, 3s, 4s, 5s));

} // namespace
} // namespace android::scheduler