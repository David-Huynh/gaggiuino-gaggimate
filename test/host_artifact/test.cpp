#include <Arduino.h>
#include <LittleFS.h>
#include <cassert>
#include <limits>
#include <display/plugins/autotuning/local/CompletedShotArtifactStore.h>
#include <display/plugins/autotuning/AutoTuningJsonCodec.h>
#include <display/util/AtomicFile.h>

extern "C" unsigned long millis() { return 1000; }
extern "C" void delay(uint32_t) {}
extern "C" void yield() {}

static AutoTuning::CompletedShotArtifact fixture() {
    AutoTuning::CompletedShotArtifact artifact;
    artifact.record.shotId = "recovery-fixture";
    artifact.record.machineId = "fixture-machine";
    artifact.record.timestamp = 1720000000;
    artifact.record.beverageOutG = 36;
    artifact.record.shotTimeS = 30;
    artifact.record.weightSource = "hardware_scale";
    artifact.record.flowSource = "hardware_scale";
    artifact.record.excludeFromLocalOptimization = true;
    artifact.record.history.reserved = true;
    artifact.record.history.id = 42;
    artifact.record.history.finalMeasuredWeightG = 36;
    artifact.record.history.phaseTransitionCount = 1;
    artifact.record.history.phaseTransitions[0].phaseName = "extraction";
    artifact.record.history.phaseTransitions[0].exitReason = 7;
    artifact.record.grindFollowed = false;
    artifact.completion.shotId = artifact.record.shotId;
    artifact.committedAt = artifact.record.timestamp + 30;
    for (unsigned i = 0; i < 240; ++i) {
        AutoTuning::ShotSample sample;
        sample.elapsedMs = i * 250;
        sample.pressure = 9;
        sample.temperature = 93;
        sample.weight = sample.measuredWeight = i * 0.15f;
        sample.valveOpen = true;
        artifact.samples.push_back(sample);
    }
    artifact.bindSamples();
    return artifact;
}

int main() {
    CompletedShotArtifactStore store;
    assert(store.begin());
    auto artifact = fixture();
    artifact.bindSamples();
    assert(store.write(artifact));
    auto path = store.pathFor(artifact.record.shotId.c_str());
    assert(LittleFS.open(path).size() > 4096); // exercises chunked IO and validation
    const auto hash = artifact.payloadHash;
    assert(store.write(artifact)); // idempotent write still validates the prior file
    assert(artifact.payloadHash == hash);
    for (unsigned cycle = 0; cycle < 5; ++cycle) {
        // Simulate power loss after writing a complete .tmp but before rename.
        assert(LittleFS.rename(path, AtomicFile::temporaryPath(path)));
        CompletedShotArtifactStore restarted;
        assert(restarted.begin());
        assert(LittleFS.exists(path) && !LittleFS.exists(AtomicFile::temporaryPath(path)));
        AutoTuning::CompletedShotArtifact loaded;
        loaded.payloadHash = "stale";
        assert(restarted.load(artifact.record.shotId.c_str(), loaded));
        assert(loaded.samples.size() == 240 && loaded.record.samples.data() == loaded.samples.data());
        assert(loaded.record.excludeFromLocalOptimization && loaded.record.grindFollowed == false);
        assert(loaded.record.history.id == 42 && loaded.record.history.phaseTransitions[0].phaseName == "extraction");
        assert(loaded.record.history.phaseTransitions[0].exitReason == 7);
        assert(loaded.payloadHash == hash);
    }
    // Invalid pending write must not replace the valid prior artifact.
    assert(LittleFS.rename(path, AtomicFile::backupPath(path)));
    auto corrupt = LittleFS.open(AtomicFile::temporaryPath(path), FILE_WRITE);
    corrupt.write(0xc1); // invalid MessagePack token
    corrupt.close();
    assert(store.begin());
    AutoTuning::CompletedShotArtifact recovered;
    assert(store.load(artifact.record.shotId.c_str(), recovered));
    assert(recovered.payloadHash == hash);
    // Reject a conflicting same revision and malformed sample data.
    artifact.record.beverageOutG = 40;
    assert(!store.write(artifact));
    artifact.revision++;
    assert(store.write(artifact));
    JsonDocument shot;
    assert(AutoTuningJsonCodec::writeShotRecord(artifact.record, shot));
    shot["pressure"][0] = "invalid";
    AutoTuningJsonCodec::DecodedShotRecord decoded;
    String error;
    assert(!AutoTuningJsonCodec::parseShotRecord(shot.as<JsonVariantConst>(), decoded, error));
    assert(!error.isEmpty());
    puts("PASS artifacts: 240 samples, repeated pending-write recovery, invalid temp/backup recovery, revision and input validation");
}
