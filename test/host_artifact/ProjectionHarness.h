#pragma once
#include <LittleFS.h>
#include <display/core/AutoTuningPorts.h>
#include <display/core/StorageCoordinator.h>
#include <display/models/shot_log_format.h>
#include <display/util/LittleFSUtil.h>
#include <display/util/PsramAllocator.h>
#include <display/util/PsramStlAllocator.h>

// Run the production projection, index and notes methods against memory-backed flash.
class ShotHistoryPlugin : public AutoTuning::CompletedShotProjectionPort {
  public:
    struct Controller {
        int clearedBefore = 0;
        const Controller &getSettings() const { return *this; }
        int getHistoryClearedBefore() const { return clearedBefore; }
        uint32_t getHistoryIndex() const { return 1; }
    } machine;
    Controller *controller = &machine;
    MemoryFS *fs = &LittleFS;
    bool ensureProjection(AutoTuning::CompletedShotArtifact const &artifact) override;
    bool removeProjection(std::uint32_t) override { return false; }
    bool appendToIndex(const ShotIndexEntry &entry, bool preserveCompleted = false);
    bool ensureIndexExists();
    bool readIndexHeader(File &indexFile, ShotIndexHeader &header);
    int findEntryPosition(File &indexFile, const ShotIndexHeader &header, uint32_t shotId);
    bool readEntryAtPosition(File &indexFile, size_t position, ShotIndexEntry &entry);
    bool writeEntryAtPosition(File &indexFile, size_t position, const ShotIndexEntry &entry);
    bool loadNotes(const String &id, JsonDocument &notes);
    bool saveNotes(const String &id, const JsonDocument &notes);
    String resolveNotesPath(uint32_t id);
};
