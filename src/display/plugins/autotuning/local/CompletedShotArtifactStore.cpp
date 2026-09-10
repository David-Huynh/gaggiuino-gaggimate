#include <cmath>
#include "CompletedShotArtifactStore.h"

#include "../AutoTuningJsonCodec.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <algorithm>
#include <cstring>
#include <display/core/StorageCoordinator.h>
#include <display/util/AtomicFile.h>
#include <display/util/LittleFSUtil.h>
#include <display/util/PsramAllocator.h>
#include <display/util/PsramStlAllocator.h>
#include <esp_log.h>
#if !defined(GAGGIMATE_SIM)
#include <mbedtls/sha256.h>
#endif

namespace {
constexpr const char *LOG_TAG = "CompletedShotStore";
constexpr const char *ARTIFACT_DIR = "/rll/c";
constexpr int ARTIFACT_SCHEMA_VERSION = 1;
constexpr size_t MAX_ARTIFACT_BYTES = 512 * 1024;

using ByteBuffer = std::vector<std::uint8_t, PsramStlAllocator<std::uint8_t>>;

class VectorPrint : public Print {
  public:
    explicit VectorPrint(ByteBuffer &target) : target(target) {}

    size_t write(std::uint8_t value) override {
        target.push_back(value);
        return 1;
    }

    size_t write(const std::uint8_t *buffer, size_t size) override {
        target.insert(target.end(), buffer, buffer + size);
        return size;
    }

  private:
    ByteBuffer &target;
};

String safeIdentifier(String value) {
    value.trim();
    String output;
    output.reserve(std::min<size_t>(value.length(), 180));
    for (size_t index = 0; index < value.length() && output.length() < 180; ++index) {
        const char character = value.charAt(index);
        const bool valid = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9') || character == '_' || character == '-' ||
                           character == '.';
        output += valid ? character : '_';
    }
    return output.isEmpty() ? String("shot") : output;
}

String hexByte(std::uint8_t value) {
    constexpr char HEX_DIGITS[] = "0123456789abcdef";
    String result;
    result += HEX_DIGITS[(value >> 4) & 0x0F];
    result += HEX_DIGITS[value & 0x0F];
    return result;
}

String payloadHash(const std::uint8_t *data, size_t size) {
#if defined(GAGGIMATE_SIM)
    std::uint64_t hash = 1469598103934665603ULL;
    for (size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 1099511628211ULL;
    }
    char chunk[17];
    snprintf(chunk, sizeof(chunk), "%016llx", static_cast<unsigned long long>(hash));
    String result;
    result.reserve(64);
    for (int index = 0; index < 4; ++index) {
        result += chunk;
    }
    return result;
#else
    std::uint8_t digest[32]{};
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    const bool success = mbedtls_sha256_starts_ret(&context, 0) == 0 &&
                         mbedtls_sha256_update_ret(&context, data, size) == 0 &&
                         mbedtls_sha256_finish_ret(&context, digest) == 0;
    mbedtls_sha256_free(&context);
    if (!success) {
        return "";
    }
    String result;
    result.reserve(64);
    for (std::uint8_t byte : digest) {
        result += hexByte(byte);
    }
    return result;
#endif
}

bool writeHistoryMetadata(AutoTuning::ShotRecord const &record, JsonObject output) {
    AutoTuning::ShotHistoryMetadata const &history = record.history;
    if (!history.reserved || history.phaseTransitionCount > history.phaseTransitions.size()) {
        return false;
    }
    output["id"] = history.id;
    output["reserved"] = history.reserved;
    output["started_volumetric"] = history.startedVolumetric;
    output["brew_delay_ms"] = history.brewDelayMs;
    output["final_exit_reason"] = history.finalExitReason;
    output["final_measured_weight_g"] = history.finalMeasuredWeightG;
    JsonArray transitions = output["phase_transitions"].to<JsonArray>();
    for (size_t index = 0; index < history.phaseTransitionCount; ++index) {
        AutoTuning::ShotPhaseTransition const &source = history.phaseTransitions[index];
        JsonObject transition = transitions.add<JsonObject>();
        transition["sample_index"] = source.sampleIndex;
        transition["phase_number"] = source.phaseNumber;
        transition["exit_reason"] = source.exitReason;
        transition["phase_name"] = source.phaseName.c_str();
    }
    return true;
}

bool writeHistorySamples(AutoTuning::ShotRecord const &record, JsonObject output) {
    JsonArray measuredWeight = output["measured_weight"].to<JsonArray>();
    JsonArray estimatedWeight = output["estimated_weight"].to<JsonArray>();
    JsonArray measuredFlow = output["measured_flow"].to<JsonArray>();
    JsonArray puckFlow = output["puck_flow"].to<JsonArray>();
    JsonArray puckResistance = output["puck_resistance"].to<JsonArray>();
    JsonArray waterPumped = output["water_pumped"].to<JsonArray>();
    JsonArray systemInfo = output["system_info"].to<JsonArray>();
    for (AutoTuning::ShotSample const &sample : record.samples) {
        measuredWeight.add(sample.measuredWeight);
        estimatedWeight.add(sample.estimatedWeight);
        measuredFlow.add(sample.measuredFlow);
        puckFlow.add(sample.puckFlow);
        puckResistance.add(sample.puckResistance);
        if (sample.waterPumped && (!std::isfinite(*sample.waterPumped) || *sample.waterPumped < 0.0f))
            return false;
        if (sample.waterPumped)
            waterPumped.add(*sample.waterPumped);
        else
            waterPumped.add(nullptr);
        systemInfo.add(sample.systemInfo);
    }
    return true;
}

bool encodeArtifact(AutoTuning::CompletedShotArtifact const &artifact, ByteBuffer &encoded) {
    JsonDocument shot(&psramAllocator);
    JsonDocument completion(&psramAllocator);
    if (!AutoTuningJsonCodec::writeShotRecord(artifact.record, shot) ||
        !AutoTuningJsonCodec::writeShotCompletion(artifact.completion, completion)) {
        return false;
    }

    JsonDocument document(&psramAllocator);
    JsonObject root = document.to<JsonObject>();
    root["artifact_schema_version"] = ARTIFACT_SCHEMA_VERSION;
    root["revision"] = artifact.revision;
    root["committed_at"] = artifact.committedAt;
    root["shot"].set(shot.as<JsonObjectConst>());
    root["completion"].set(completion.as<JsonObjectConst>());
    JsonObject disposition = root["disposition"].to<JsonObject>();
    disposition["dose_confirmation_required"] = artifact.disposition.doseConfirmationRequired;
    disposition["optimizer_delivery_required"] = artifact.disposition.optimizerDeliveryRequired;
    disposition["community_upload_required"] = artifact.disposition.communityUploadRequired;
    if (!writeHistoryMetadata(artifact.record, root["history"].to<JsonObject>()) ||
        !writeHistorySamples(artifact.record, root["history_samples"].to<JsonObject>())) {
        return false;
    }

    const size_t expected = measureMsgPack(document);
    if (expected == 0 || expected > MAX_ARTIFACT_BYTES) {
        return false;
    }
    encoded.clear();
    encoded.reserve(expected);
    VectorPrint output(encoded);
    return serializeMsgPack(document, output) == expected && encoded.size() == expected;
}

bool readFile(const String &path, ByteBuffer &bytes) {
    size_t size = 0;
    {
        auto lease = StorageCoordinator::instance().acquireFlash();
        File file = LittleFS.open(path, FILE_READ);
        if (!file) {
            return false;
        }
        size = file.size();
        file.close();
    }
    if (size == 0 || size > MAX_ARTIFACT_BYTES) {
        return false;
    }
    bytes.resize(size);
    for (size_t offset = 0; offset < size; offset += StorageCoordinator::MAX_FLASH_QUANTUM_BYTES) {
        const size_t chunk = std::min(StorageCoordinator::MAX_FLASH_QUANTUM_BYTES, size - offset);
        auto lease = StorageCoordinator::instance().acquireFlash();
        File file = LittleFS.open(path, FILE_READ);
        if (!file || !file.seek(offset) || file.read(bytes.data() + offset, chunk) != chunk) {
            if (file) {
                file.close();
            }
            return false;
        }
        file.close();
    }
    return true;
}

bool exactUint16(JsonVariantConst value, std::uint16_t &output) {
    if (!value.is<unsigned int>()) {
        return false;
    }
    const unsigned int parsed = value.as<unsigned int>();
    if (parsed > UINT16_MAX) {
        return false;
    }
    output = static_cast<std::uint16_t>(parsed);
    return true;
}

bool decodeArtifact(const ByteBuffer &encoded, AutoTuning::CompletedShotArtifact &artifact, String &error) {
    JsonDocument document(&psramAllocator);
    const DeserializationError parseError = deserializeMsgPack(document, encoded.data(), encoded.size());
    if (parseError || !document.is<JsonObjectConst>()) {
        error = "Completed shot artifact is not valid MessagePack";
        return false;
    }
    JsonObjectConst root = document.as<JsonObjectConst>();
    if (!root["artifact_schema_version"].is<int>() ||
        root["artifact_schema_version"].as<int>() != ARTIFACT_SCHEMA_VERSION ||
        !root["revision"].is<unsigned int>() || root["revision"].as<unsigned int>() == 0 ||
        root["committed_at"].is<bool>() || !root["committed_at"].is<std::int64_t>()) {
        error = "Completed shot artifact envelope is invalid";
        return false;
    }

    auto decodedStorage = makePsramUnique<AutoTuningJsonCodec::DecodedShotRecord>();
    auto &decoded = *decodedStorage;
    if (!AutoTuningJsonCodec::parseShotRecord(root["shot"], decoded, error)) {
        return false;
    }
    AutoTuning::ShotCompletion completion;
    if (!AutoTuningJsonCodec::parseShotCompletion(root["completion"], completion, error) ||
        completion.shotId != decoded.record.shotId) {
        error = "Completed shot artifact completion does not match its shot";
        return false;
    }

    JsonObjectConst disposition = root["disposition"].as<JsonObjectConst>();
    if (disposition.isNull() || !disposition["dose_confirmation_required"].is<bool>() ||
        !disposition["optimizer_delivery_required"].is<bool>() ||
        !disposition["community_upload_required"].is<bool>()) {
        error = "Completed shot artifact disposition is invalid";
        return false;
    }

    JsonObjectConst history = root["history"].as<JsonObjectConst>();
    JsonArrayConst transitions = history["phase_transitions"].as<JsonArrayConst>();
    if (history.isNull() || !history["id"].is<unsigned int>() || !history["reserved"].is<bool>() ||
        !history["started_volumetric"].is<bool>() || !history["brew_delay_ms"].is<unsigned int>() ||
        !history["final_exit_reason"].is<unsigned int>() || !history["final_measured_weight_g"].is<float>() ||
        transitions.isNull() || transitions.size() > decoded.record.history.phaseTransitions.size()) {
        error = "Completed shot history metadata is invalid";
        return false;
    }
    AutoTuning::ShotHistoryMetadata metadata;
    metadata.id = history["id"].as<std::uint32_t>();
    metadata.reserved = history["reserved"].as<bool>();
    metadata.startedVolumetric = history["started_volumetric"].as<bool>();
    if (!exactUint16(history["brew_delay_ms"], metadata.brewDelayMs) ||
        history["final_exit_reason"].as<unsigned int>() > UINT8_MAX) {
        error = "Completed shot history bounds are invalid";
        return false;
    }
    metadata.finalExitReason = static_cast<std::uint8_t>(history["final_exit_reason"].as<unsigned int>());
    metadata.finalMeasuredWeightG = history["final_measured_weight_g"].as<float>();
    for (JsonObjectConst source : transitions) {
        if (source.isNull() || !source["phase_number"].is<unsigned int>() || !source["exit_reason"].is<unsigned int>() ||
            source["phase_number"].as<unsigned int>() > UINT8_MAX ||
            source["exit_reason"].as<unsigned int>() > UINT8_MAX || !source["phase_name"].is<const char *>()) {
            error = "Completed shot phase transition is invalid";
            return false;
        }
        AutoTuning::ShotPhaseTransition &target = metadata.phaseTransitions[metadata.phaseTransitionCount];
        if (!exactUint16(source["sample_index"], target.sampleIndex)) {
            error = "Completed shot phase index is invalid";
            return false;
        }
        target.phaseNumber = static_cast<std::uint8_t>(source["phase_number"].as<unsigned int>());
        target.exitReason = static_cast<std::uint8_t>(source["exit_reason"].as<unsigned int>());
        target.phaseName = source["phase_name"].as<const char *>();
        ++metadata.phaseTransitionCount;
    }

    JsonObjectConst historySamples = root["history_samples"].as<JsonObjectConst>();
    constexpr const char *CHANNELS[] = {
        "measured_weight", "estimated_weight", "measured_flow", "puck_flow", "puck_resistance", "system_info",
    };
    const size_t sampleCount = decoded.samples.size();
    for (const char *channel : CHANNELS) {
        if (!historySamples[channel].is<JsonArrayConst>() ||
            historySamples[channel].as<JsonArrayConst>().size() != sampleCount) {
            error = "Completed shot history channels are invalid";
            return false;
        }
    }
    const bool hasWater = !historySamples["water_pumped"].isNull();
    if (hasWater && (!historySamples["water_pumped"].is<JsonArrayConst>() ||
                     historySamples["water_pumped"].size() != sampleCount)) {
        error = "Completed shot pumped-water channel is invalid";
        return false;
    }
    for (size_t index = 0; index < sampleCount; ++index) {
        AutoTuning::ShotSample &sample = decoded.samples[index];
        JsonVariantConst measuredWeight = historySamples["measured_weight"][index];
        JsonVariantConst estimatedWeight = historySamples["estimated_weight"][index];
        JsonVariantConst measuredFlow = historySamples["measured_flow"][index];
        JsonVariantConst puckFlow = historySamples["puck_flow"][index];
        JsonVariantConst puckResistance = historySamples["puck_resistance"][index];
        std::uint16_t systemInfo = 0;
        if (!measuredWeight.is<float>() || !estimatedWeight.is<float>() || !measuredFlow.is<float>() ||
            !puckFlow.is<float>() || !puckResistance.is<float>() ||
            !exactUint16(historySamples["system_info"][index], systemInfo)) {
            error = "Completed shot history sample is invalid";
            return false;
        }
        sample.measuredWeight = measuredWeight.as<float>();
        sample.estimatedWeight = estimatedWeight.as<float>();
        sample.measuredFlow = measuredFlow.as<float>();
        sample.puckFlow = puckFlow.as<float>();
        sample.puckResistance = puckResistance.as<float>();
        sample.systemInfo = systemInfo;
        JsonVariantConst water = historySamples["water_pumped"][index];
        if (hasWater && !water.isNull()) {
            if (!water.is<float>() || !std::isfinite(water.as<float>()) || water.as<float>() < 0.0f) {
                error = "Completed shot pumped-water sample is invalid";
                return false;
            }
            sample.waterPumped = water.as<float>();
        }
    }

    // Every field is replaced below. Avoid constructing a multi-kilobyte reset
    // temporary on the caller's stack during startup recovery or write validation.
    artifact.payloadHash.clear();
    artifact.record = std::move(decoded.record);
    artifact.record.history = std::move(metadata);
    artifact.completion = std::move(completion);
    artifact.disposition.doseConfirmationRequired = disposition["dose_confirmation_required"].as<bool>();
    artifact.disposition.optimizerDeliveryRequired = disposition["optimizer_delivery_required"].as<bool>();
    artifact.disposition.communityUploadRequired = disposition["community_upload_required"].as<bool>();
    artifact.samples.assign(decoded.samples.begin(), decoded.samples.end());
    artifact.revision = root["revision"].as<std::uint32_t>();
    artifact.committedAt = root["committed_at"].as<std::int64_t>();
    artifact.bindSamples();
    return true;
}

bool validArtifactFile(const String &path) {
    ByteBuffer bytes;
    auto artifact = makePsramUnique<AutoTuning::CompletedShotArtifact>();
    String error;
    return readFile(path, bytes) && decodeArtifact(bytes, *artifact, error);
}

} // namespace

String CompletedShotArtifactStore::pathFor(const String &shotId) {
    return String(ARTIFACT_DIR) + "/" + safeIdentifier(shotId) + ".mpk";
}

bool CompletedShotArtifactStore::begin() {
    {
        auto lease = StorageCoordinator::instance().acquireFlash();
        if (!LittleFSUtil::existsQuietly("/rll") && !LittleFS.mkdir("/rll")) {
            return false;
        }
        if (!LittleFSUtil::existsQuietly(ARTIFACT_DIR) && !LittleFS.mkdir(ARTIFACT_DIR)) {
            return false;
        }
    }
    return recover();
}

bool CompletedShotArtifactStore::write(AutoTuning::CompletedShotArtifact &artifact) {
    if (artifact.record.shotId.empty() || artifact.record.samples.empty() ||
        artifact.completion.shotId != artifact.record.shotId || artifact.revision == 0) {
        return false;
    }
    ByteBuffer encoded;
    if (!encodeArtifact(artifact, encoded)) {
        return false;
    }
    const String hash = payloadHash(encoded.data(), encoded.size());
    if (hash.length() != 64) {
        return false;
    }

    const String path = pathFor(artifact.record.shotId.c_str());
    if (LittleFSUtil::existsQuietly(path)) {
        ByteBuffer existingBytes;
        auto existing = makePsramUnique<AutoTuning::CompletedShotArtifact>();
        String error;
        if (readFile(path, existingBytes) &&
            decodeArtifact(existingBytes, *existing, error)) {
            const String existingHash =
                payloadHash(existingBytes.data(), existingBytes.size());
            if (existingHash == hash) {
                artifact.payloadHash = hash.c_str();
                return true;
            }
            if (artifact.revision <= existing->revision) {
                ESP_LOGE(LOG_TAG,
                         "Refusing conflicting or regressive artifact revision for %s",
                         artifact.record.shotId.c_str());
                return false;
            }
        }
    }
    const String temporary = AtomicFile::temporaryPath(path);
    {
        auto lease = StorageCoordinator::instance().acquireFlash();
        if (!LittleFSUtil::removeIfExists(temporary)) {
            return false;
        }
    }
    for (size_t offset = 0; offset < encoded.size(); offset += StorageCoordinator::MAX_FLASH_QUANTUM_BYTES) {
        const size_t chunk = std::min(StorageCoordinator::MAX_FLASH_QUANTUM_BYTES, encoded.size() - offset);
        auto lease = StorageCoordinator::instance().acquireFlash();
        File file = LittleFS.open(temporary, offset == 0 ? FILE_WRITE : FILE_APPEND);
        if (!file || file.write(encoded.data() + offset, chunk) != chunk) {
            if (file) {
                file.close();
            }
            LittleFSUtil::removeIfExists(temporary);
            return false;
        }
        file.close();
    }

    ByteBuffer verification;
    if (!readFile(temporary, verification) || verification.size() != encoded.size() ||
        payloadHash(verification.data(), verification.size()) != hash || !validArtifactFile(temporary) ||
        !AtomicFile::commit(path)) {
        LittleFSUtil::removeIfExists(temporary);
        return false;
    }
    artifact.payloadHash = hash.c_str();
    return true;
}

bool CompletedShotArtifactStore::load(const String &shotId, AutoTuning::CompletedShotArtifact &artifact) const {
    const String path = pathFor(shotId);
    ByteBuffer encoded;
    String error;
    if (!readFile(path, encoded) || !decodeArtifact(encoded, artifact, error)) {
        if (!error.isEmpty()) {
            ESP_LOGW(LOG_TAG, "Unable to decode %s: %s", path.c_str(), error.c_str());
        }
        return false;
    }
    artifact.payloadHash = payloadHash(encoded.data(), encoded.size()).c_str();
    return artifact.payloadHash.size() == 64;
}

bool CompletedShotArtifactStore::exists(const String &shotId) const {
    auto lease = StorageCoordinator::instance().acquireFlash();
    return LittleFSUtil::existsQuietly(pathFor(shotId));
}

bool CompletedShotArtifactStore::remove(const String &shotId) {
    auto lease = StorageCoordinator::instance().acquireFlash();
    const String path = pathFor(shotId);
    const bool removed = !LittleFSUtil::existsQuietly(path) || LittleFS.remove(path);
    LittleFSUtil::removeIfExists(AtomicFile::temporaryPath(path));
    LittleFSUtil::removeIfExists(AtomicFile::backupPath(path));
    return removed;
}

bool CompletedShotArtifactStore::listShotIds(std::vector<String> &shotIds) const {
    auto lease = StorageCoordinator::instance().acquireFlash();
    File directory = LittleFS.open(ARTIFACT_DIR);
    if (!directory || !directory.isDirectory()) {
        return false;
    }
    File entry = directory.openNextFile();
    size_t scanned = 0;
    while (entry) {
        String path = LittleFSUtil::pathFromEntry(ARTIFACT_DIR, entry.name());
        const bool regular = !entry.isDirectory();
        entry.close();
        if (regular && path.endsWith(".mpk")) {
            const int slash = path.lastIndexOf('/');
            shotIds.push_back(path.substring(slash + 1, path.length() - 4));
        }
        if (++scanned % 8 == 0) {
            lease.checkpoint();
        }
        entry = directory.openNextFile();
    }
    directory.close();
    return true;
}

size_t CompletedShotArtifactStore::bytes() const {
    auto lease = StorageCoordinator::instance().acquireFlash();
    size_t total = 0;
    File directory = LittleFS.open(ARTIFACT_DIR);
    if (!directory || !directory.isDirectory()) {
        return 0;
    }
    File entry = directory.openNextFile();
    size_t scanned = 0;
    while (entry) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            if (name.endsWith(".mpk")) {
                total += entry.size();
            }
        }
        entry.close();
        if (++scanned % 8 == 0) {
            lease.checkpoint();
        }
        entry = directory.openNextFile();
    }
    directory.close();
    return total;
}

bool CompletedShotArtifactStore::recover() {
    std::vector<String> temporary;
    std::vector<String> backup;
    {
        auto lease = StorageCoordinator::instance().acquireFlash();
        File directory = LittleFS.open(ARTIFACT_DIR);
        if (!directory || !directory.isDirectory()) {
            return false;
        }
        File entry = directory.openNextFile();
        size_t scanned = 0;
        while (entry) {
            const String path = LittleFSUtil::pathFromEntry(ARTIFACT_DIR, entry.name());
            const bool regular = !entry.isDirectory();
            entry.close();
            if (regular && path.endsWith(".mpk.tmp")) {
                temporary.push_back(path);
            } else if (regular && path.endsWith(".mpk.bak")) {
                backup.push_back(path);
            }
            if (++scanned % 8 == 0) {
                lease.checkpoint();
            }
            entry = directory.openNextFile();
        }
        directory.close();
    }
    for (const String &path : temporary) {
        const String finalPath = path.substring(0, path.length() - 4);
        AtomicFile::recoverPending(finalPath, validArtifactFile(path));
    }
    for (const String &path : backup) {
        const String finalPath = path.substring(0, path.length() - 4);
        if (LittleFSUtil::existsQuietly(finalPath)) {
            AtomicFile::discardBackup(finalPath);
        } else {
            AtomicFile::restoreBackup(finalPath);
        }
    }
    return true;
}
