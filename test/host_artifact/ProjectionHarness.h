#pragma once
#include <LittleFS.h>
#include <display/core/AutoTuningPorts.h>
#include <display/core/StorageCoordinator.h>
#include <display/models/shot_log_format.h>
#include <display/util/PsramAllocator.h>
#include <display/util/PsramStlAllocator.h>
#include <map>

// Compile the production projection method with fake index/notes collaborators.
// The .slog encoding, chunked writes and recovery use the real method unchanged.
class ShotHistoryPlugin {
  public:
    MemoryFS *fs = &LittleFS;
    std::map<uint32_t, ShotIndexEntry> index;
    String savedNotes;
    bool ensureProjection(AutoTuning::CompletedShotArtifact const &artifact);
    bool appendToIndex(ShotIndexEntry const &entry) {
        index[entry.id] = entry;
        return true;
    }
    void loadNotes(String const &, JsonDocument &notes) {
        if (!savedNotes.isEmpty()) deserializeJson(notes, savedNotes);
    }
    void saveNotes(String const &, JsonDocument const &notes) {
        savedNotes = "";
        serializeJson(notes, savedNotes);
    }
};
