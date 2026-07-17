#ifndef SHOTHISTORYPLUGIN_H
#define SHOTHISTORYPLUGIN_H

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <display/core/Controller.h>
#include <display/core/Event.h>
#include <display/core/Plugin.h>
#include <display/core/utils.h>
#include <display/models/shot_log_format.h>
#include <atomic>
#include <deque>
#include <mutex>

constexpr size_t SHOT_HISTORY_INTERVAL = 100;
constexpr size_t MIN_FREE_SPACE_BYTES = 500 * 1024;         // 500 KB reserved free space
constexpr unsigned long EXTENDED_RECORDING_DURATION = 3000; // 3 seconds
constexpr unsigned long WEIGHT_STABILIZATION_TIME = 1000;   // 1 second
constexpr float WEIGHT_STABILIZATION_THRESHOLD = 0.1f;      // 0.1g threshold

class ShotHistoryPlugin : public Plugin {
  public:
    ShotHistoryPlugin() = default;

    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;

    void record();

    void handleRequest(JsonDocument &request, JsonDocument &response);

    // Index management methods
    bool appendToIndex(const ShotIndexEntry &entry);
    void updateIndexMetadata(uint32_t shotId, uint8_t rating, uint16_t volume);
    void markIndexDeleted(uint32_t shotId);
    void rebuildIndex();
    void startAsyncRebuild();
    bool ensureIndexExists();

    // Read up to maxCount most recent non-deleted index entries, newest first.
    // Returns the number of entries written to outEntries.
    size_t readRecentEntries(ShotIndexEntry *outEntries, size_t maxCount);

  private:
    // Index helper functions
    bool readIndexHeader(File &indexFile, ShotIndexHeader &header);
    int findEntryPosition(File &indexFile, const ShotIndexHeader &header, uint32_t shotId);
    bool readEntryAtPosition(File &indexFile, size_t position, ShotIndexEntry &entry);
    bool writeEntryAtPosition(File &indexFile, size_t position, const ShotIndexEntry &entry);
    bool createEarlyIndexEntry();

    void saveNotes(const String &id, const JsonDocument &notes);
    void loadNotes(const String &id, JsonDocument &notes);
    void startRecording();

    uint16_t getSystemInfo(); // Helper to pack system state bits

    unsigned long getTime();

    void endRecording();
    void endExtendedRecording();
    void cleanupHistory();
    size_t getFreeSpace();

    void recordPhaseTransition(uint8_t phaseNumber, uint16_t sampleIndex,
                               uint8_t reason); // Helper for phase transitions
    void rememberRLShotHistoryMapping(Event const &event);
    void persistNextRLShotHistoryMapping();
    void attachAutoTuningSummary(JsonDocument &response, JsonObjectConst notes) const;
    uint8_t ratingFromNotes(const String &id);

    // Live weight for a given source from the cached per-source values below.
    float sourceWeight(VolumetricMeasurementSource source) const;

    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;
    FS *fs = &LittleFS;
    String currentId = "";
    struct PendingRLShotHistoryMapping {
        String historyId;
        String shotId;
        String recommendationId;
    };
    std::mutex pendingRLMappingMutex;
    std::deque<PendingRLShotHistoryMapping> pendingRLMappings;
    std::atomic<bool> isFileOpen{false};
    File currentFile;
    ShotLogHeader header{};
    uint32_t sampleCount = 0;
    uint8_t ioBuffer[4096];
    size_t ioBufferPos = 0; // bytes used

    bool recording = false;
    bool extendedRecording = false;
    // Source the active shot is recording weight from, latched at brew start so
    // the logged weight stays consistent through the post-shot settling window
    // (Controller::currentVolumetricSource resets to INACTIVE on brew end).
    VolumetricMeasurementSource shotSource = VolumetricMeasurementSource::INACTIVE;
    bool indexEntryCreated = false;     // Track if early index entry was created
    bool shotStartedVolumetric = false; // Track initial volumetric mode
    double currentBrewDelay = 0.0;      // Brew delay (ms) the active shot was started with
    unsigned long shotStart = 0;
    unsigned long extendedRecordingStart = 0;
    unsigned long lastWeightChangeTime = 0;
    float currentTemperature = 0.0f;
    float currentBluetoothWeight = 0.0f;
    float lastStableWeight = 0.0f;
    // Trailing-sample weight and EMA flow for the active volumetric source.
    float lastScaleWeight = 0.0f;
    float currentScaleFlow = 0.0f;
    float lastBluetoothWeight = 0.0f;
    float currentBluetoothFlow = 0.0f;
    float currentEstimatedWeight = 0.0f;
    float currentPuckResistance = 0.0f;
    unsigned long lastLoggedElapsedMs = 0;
    String currentProfileName;

    // Phase transition tracking (v5+)
    uint8_t lastRecordedPhase = 0xFF; // Invalid initial value to detect first phase
    uint8_t finalExitReason = 0;      // Reason the shot ended (PhaseExitReason); captured at brew end

    // Running aggregates for the current shot, used to populate the index
    // entry at completion without re-parsing the .slog file (see record()).
    uint32_t tempSumScaled = 0; // sum of sample.ct (°C * 10)
    uint32_t tempSampleCount = 0;
    uint16_t maxPressureScaled = 0; // max of sample.cp (bar * 10)
    uint32_t flowSumScaled = 0;     // sum of positive sample.fl (ml/s * 100)
    uint32_t positiveFlowCount = 0;

    // Async rebuild state
    bool rebuildInProgress = false;

    xTaskHandle taskHandle;
    void flushBuffer();
    static void loopTask(void *arg);
};

extern ShotHistoryPlugin ShotHistory;

#endif // SHOTHISTORYPLUGIN_H
