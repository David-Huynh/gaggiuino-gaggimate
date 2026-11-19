#pragma once

#include <display/core/AutoTuningPorts.h>
#include <display/core/EpochTime.h>
#include <display/plugins/autotuning/AutoTuningJsonCodec.h>
#include <display/plugins/autotuning/local/CompletedShotArtifactStore.h>
#include <display/plugins/autotuning/local/LocalAutoTuningFiles.h>
#include <display/plugins/autotuning/local/LocalAutoTuningSummaryStore.h>
#include <display/util/PsramAllocator.h>
#include <display/util/PsramStlAllocator.h>
#include <mutex>

// Exercise the device's recovery methods with real stores and a fake controller.
class LocalAutoTuningStorePlugin {
  public:
    struct Controller {
        AutoTuning::CompletedShotProjectionPort *projection = nullptr;
        AutoTuning::CompletedShotProjectionPort *getCompletedShotProjection() const { return projection; }
    } machine;
    Controller *controller = &machine;
    CompletedShotArtifactStore artifactStore;
    bool recoverCommittedArtifacts();

  private:
    struct ContextStore {
        bool begin() const { return true; }
    } contextStore;
    LocalAutoTuningSummaryStore summaryStore;
    std::recursive_mutex storeMutex;
    bool artifactStoreReady = false;

    bool ensureDirectories();
    bool saveReplaySnapshot(AutoTuning::CompletedShotArtifact const &artifact);
    bool loadReplaySnapshot(const String &shotId, JsonDocument &out) const;
    bool updateReplayArtifactIdentity(JsonObject root, AutoTuning::CompletedShotArtifact const &artifact) const;
    bool upsertArtifactSummary(AutoTuning::CompletedShotArtifact const &artifact);
    void requestStatusRefresh() {}
};
