#include "CommunityUploadQueue.h"

#include "CommunityPayloadValidator.h"
#include <display/util/AtomicFile.h>

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <algorithm>
#include <display/core/AutoTuningModels.h>
#include <display/core/EpochTime.h>
#include <display/util/PsramAllocator.h>
#include <vector>

namespace {

constexpr const char *LEGACY_QUEUE_DIR = "/rlu";
constexpr const char *QUEUE_DIR = "/rlu2";
constexpr size_t MAX_QUEUE_ITEMS = 24;
constexpr size_t MAX_QUEUE_BYTES = 384 * 1024;

struct QueueLock {
    SemaphoreHandle_t mutex;
    bool locked;

    explicit QueueLock(SemaphoreHandle_t value)
        : mutex(value), locked(value && xSemaphoreTakeRecursive(value, portMAX_DELAY) == pdTRUE) {}

    ~QueueLock() {
        if (locked) {
            xSemaphoreGiveRecursive(mutex);
        }
    }
};

static String safeIdentifier(String value, size_t maxLength = 220) {
    value.trim();
    String output;
    output.reserve(std::min(value.length(), maxLength));
    for (size_t index = 0; index < value.length() && output.length() < maxLength; ++index) {
        const char character = value.charAt(index);
        const bool valid = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.' ||
                           character == ':' || character == '@';
        output += valid ? character : '_';
    }
    return output.isEmpty() ? "record" : output;
}

static String pathFromEntry(const char *directory, String name) {
    if (name.startsWith("/")) {
        return name;
    }
    return String(directory) + "/" + name;
}

static String pathFromEntry(String name) { return pathFromEntry(QUEUE_DIR, name); }

static String canonicalPath(const String &uploadId) { return String(QUEUE_DIR) + "/" + safeIdentifier(uploadId) + ".upl"; }

static String legacyJsonPath(const String &uploadId) { return String(QUEUE_DIR) + "/" + safeIdentifier(uploadId) + ".json"; }

static bool isQueuePath(const String &path) { return path.endsWith(".upl") || path.endsWith(".json"); }

static bool isTemporaryQueuePath(const String &path) { return path.endsWith(".upl.tmp") || path.endsWith(".json.tmp"); }

static bool isBackupQueuePath(const String &path) { return path.endsWith(".upl.bak") || path.endsWith(".json.bak"); }

static const char *statusKey(CommunityUploadQueue::Status status) {
    switch (status) {
    case CommunityUploadQueue::Status::Failed:
        return "failed";
    case CommunityUploadQueue::Status::Rejected:
        return "rejected";
    case CommunityUploadQueue::Status::Pending:
    default:
        return "pending";
    }
}

static CommunityUploadQueue::Status statusFromKey(const String &status) {
    if (status == "failed") {
        return CommunityUploadQueue::Status::Failed;
    }
    if (status == "rejected") {
        return CommunityUploadQueue::Status::Rejected;
    }
    return CommunityUploadQueue::Status::Pending;
}

static bool jsonEpoch(JsonVariantConst value, std::int64_t &output) {
    if (value.is<bool>() || !value.is<std::int64_t>()) {
        return false;
    }
    output = value.as<std::int64_t>();
    return true;
}

static std::int64_t jsonEpochOrZero(JsonVariantConst value) {
    std::int64_t parsed = 0;
    return jsonEpoch(value, parsed) ? parsed : 0;
}

static std::optional<bool> optionalBool(JsonVariantConst value) {
    return value.is<bool>() ? std::optional<bool>(value.as<bool>()) : std::nullopt;
}

} // namespace

CommunityUploadQueue::~CommunityUploadQueue() {
    if (mutex) {
        vSemaphoreDelete(mutex);
    }
}

bool CommunityUploadQueue::begin() {
    if (!mutex) {
        mutex = xSemaphoreCreateRecursiveMutex();
    }
    QueueLock lock(mutex);
    return lock.locked && ensureDirectoryUnlocked();
}

bool CommunityUploadQueue::storageAvailable() const {
    QueueLock lock(mutex);
    return lock.locked && ensureDirectoryUnlocked();
}

bool CommunityUploadQueue::ensureDirectoryUnlocked() const {
    if (LittleFS.exists(QUEUE_DIR)) {
        return true;
    }
    return LittleFS.mkdir(QUEUE_DIR);
}

void CommunityUploadQueue::recover() {
    QueueLock lock(mutex);
    if (!lock.locked || !ensureDirectoryUnlocked()) {
        return;
    }

    File root = LittleFS.open(QUEUE_DIR);
    if (!root || !root.isDirectory()) {
        return;
    }
    std::vector<String> temporaryPaths;
    std::vector<String> backupPaths;
    File file = root.openNextFile();
    while (file) {
        const String path = pathFromEntry(file.name());
        const bool regularFile = !file.isDirectory();
        file.close();
        if (regularFile && isTemporaryQueuePath(path)) {
            temporaryPaths.push_back(path);
        } else if (regularFile && isBackupQueuePath(path)) {
            backupPaths.push_back(path);
        }
        file = root.openNextFile();
    }
    root.close();

    for (const String &temporaryPath : temporaryPaths) {
        Item pending;
        String payload;
        const String finalPath = temporaryPath.substring(0, temporaryPath.length() - 4);
        const bool valid = readItemUnlocked(temporaryPath, pending, &payload) && !payload.isEmpty();
        AtomicFile::recoverPending(finalPath, valid);
    }
    for (const String &backup : backupPaths) {
        const String finalPath = backup.substring(0, backup.length() - 4);
        if (LittleFS.exists(finalPath)) {
            AtomicFile::discardBackup(finalPath);
        } else {
            AtomicFile::restoreBackup(finalPath);
        }
    }
}

bool CommunityUploadQueue::readItemUnlocked(const String &path, Item &item, String *payloadJson) const {
    File file = LittleFS.open(path, FILE_READ);
    if (!file) {
        return false;
    }
    const String metadataLine = file.readStringUntil('\n');
    JsonDocument metadata(&psramAllocator);
    if (deserializeJson(metadata, metadataLine) || !metadata.is<JsonObject>()) {
        file.close();
        return false;
    }

    item.path = path;
    item.uploadId = metadata["upload_id"].as<String>();
    item.recordType = metadata["record_type"].as<String>();
    item.recordId = metadata["record_id"].as<String>();
    item.status = statusFromKey(metadata["status"].as<String>());
    item.endpoint = metadata["endpoint"].as<String>();
    item.createdAt = jsonEpochOrZero(metadata["created_at"]);
    item.nextRetryAt = jsonEpochOrZero(metadata["next_retry_at"]);
    item.attemptCount = metadata["attempt_count"] | 0;
    item.bytes = file.size();
    if (payloadJson) {
        *payloadJson = file.readString();
    }
    file.close();
    return !item.uploadId.isEmpty() && !item.recordType.isEmpty() && !item.recordId.isEmpty();
}

bool CommunityUploadQueue::writeItemUnlocked(const Item &item, const String &payloadJson) const {
    JsonDocument metadata(&psramAllocator);
    metadata["upload_id"] = item.uploadId;
    metadata["record_type"] = item.recordType;
    metadata["record_id"] = item.recordId;
    metadata["status"] = statusKey(item.status);
    metadata["endpoint"] = item.endpoint;
    metadata["created_at"] = item.createdAt;
    metadata["next_retry_at"] = item.nextRetryAt;
    metadata["attempt_count"] = item.attemptCount;

    String metadataJson;
    serializeJson(metadata, metadataJson);
    const String temporaryPath = AtomicFile::temporaryPath(item.path);
    LittleFS.remove(temporaryPath);
    File file = LittleFS.open(temporaryPath, FILE_WRITE);
    if (!file) {
        return false;
    }
    const size_t metadataBytes = file.println(metadataJson);
    const size_t payloadBytes = file.print(payloadJson);
    file.flush();
    file.close();
    if (metadataBytes == 0 || payloadBytes != payloadJson.length()) {
        LittleFS.remove(temporaryPath);
        return false;
    }

    Item verification;
    String verifiedPayload;
    if (!readItemUnlocked(temporaryPath, verification, &verifiedPayload) || verifiedPayload != payloadJson ||
        verification.uploadId != item.uploadId) {
        LittleFS.remove(temporaryPath);
        return false;
    }
    return AtomicFile::commit(item.path);
}

bool CommunityUploadQueue::removeRecordUnlocked(const String &recordType, const String &recordId, const String &exceptPath) {
    if (!ensureDirectoryUnlocked()) {
        return false;
    }
    File root = LittleFS.open(QUEUE_DIR);
    if (!root || !root.isDirectory()) {
        return false;
    }
    std::vector<String> matchingPaths;
    File file = root.openNextFile();
    while (file) {
        const String path = pathFromEntry(file.name());
        file.close();
        if (isQueuePath(path) && path != exceptPath) {
            Item item;
            if (readItemUnlocked(path, item) && item.recordType == recordType && item.recordId == recordId) {
                matchingPaths.push_back(path);
            }
        }
        file = root.openNextFile();
    }
    root.close();

    bool removed = true;
    for (const String &path : matchingPaths) {
        removed = LittleFS.remove(path) && removed;
    }
    return removed;
}

bool CommunityUploadQueue::enqueue(const String &uploadId, const String &recordType, const String &recordId,
                                   const String &payloadJson, const String &endpoint, std::int64_t createdAt,
                                   std::int64_t nextRetryAt, bool replaceRecord) {
    QueueLock lock(mutex);
    if (!lock.locked || !ensureDirectoryUnlocked() || uploadId.isEmpty() || recordType.isEmpty() || recordId.isEmpty() ||
        payloadJson.isEmpty() || payloadJson.length() > CommunityPayloadValidator::MAX_PAYLOAD_BYTES) {
        return false;
    }

    const String path = canonicalPath(uploadId);
    const String legacyPath = legacyJsonPath(uploadId);
    const String existingPath = LittleFS.exists(path) ? path : (LittleFS.exists(legacyPath) ? legacyPath : String());
    if (!existingPath.isEmpty()) {
        Item existing;
        if (!readItemUnlocked(existingPath, existing) || existing.uploadId != uploadId || existing.recordType != recordType ||
            existing.recordId != recordId) {
            return false;
        }
        return (!replaceRecord || removeRecordUnlocked(recordType, recordId, existingPath)) && pruneUnlocked();
    }

    Item item;
    item.path = path;
    item.uploadId = uploadId;
    item.recordType = recordType;
    item.recordId = recordId;
    item.endpoint = endpoint;
    item.createdAt = createdAt;
    item.nextRetryAt = nextRetryAt;
    if (!writeItemUnlocked(item, payloadJson)) {
        return false;
    }
    if (replaceRecord && !removeRecordUnlocked(recordType, recordId, path)) {
        return false;
    }
    if (pruneUnlocked()) {
        return true;
    }
    if (!replaceRecord) {
        LittleFS.remove(path);
    }
    return false;
}

bool CommunityUploadQueue::patchShotCorrection(const String &shotId, bool hasGrindFollowed, bool grindFollowed,
                                               bool hasDoseFollowed, bool doseFollowed, bool hasYieldFollowed, bool yieldFollowed,
                                               std::int64_t updatedAt, std::int64_t nextRetryAt) {
    QueueLock lock(mutex);
    if (!lock.locked || !ensureDirectoryUnlocked() || (!hasGrindFollowed && !hasDoseFollowed && !hasYieldFollowed)) {
        return false;
    }

    File root = LittleFS.open(QUEUE_DIR);
    if (!root || !root.isDirectory()) {
        return false;
    }
    bool patched = false;
    File file = root.openNextFile();
    while (file) {
        const String path = pathFromEntry(file.name());
        file.close();
        Item item;
        String payloadJson;
        if (isQueuePath(path) && readItemUnlocked(path, item, &payloadJson) && item.recordType == "shot" &&
            item.recordId == shotId) {
            JsonDocument document(&psramAllocator);
            if (!deserializeJson(document, payloadJson) && document.is<JsonObject>()) {
                JsonObject payload = document.as<JsonObject>();
                std::optional<bool> storedGrind = optionalBool(payload["grind_followed"]);
                std::optional<bool> storedDose = optionalBool(payload["dose_followed"]);
                std::optional<bool> storedYield = optionalBool(payload["yield_followed"]);
                if (hasGrindFollowed) {
                    payload["grind_followed"] = grindFollowed;
                    storedGrind = grindFollowed;
                }
                if (hasDoseFollowed) {
                    payload["dose_followed"] = doseFollowed;
                    storedDose = doseFollowed;
                }
                if (hasYieldFollowed) {
                    payload["yield_followed"] = yieldFollowed;
                    storedYield = yieldFollowed;
                }
                const AutoTuning::FollowThroughStatus followThrough =
                    AutoTuning::deriveFollowThrough(storedGrind, storedDose, storedYield);
                if (followThrough == AutoTuning::FollowThroughStatus::Unknown) {
                    payload.remove("recommendation_followed");
                } else {
                    payload["recommendation_followed"] = AutoTuning::followThroughStatusKey(followThrough);
                }
                payload["updated_at"] = updatedAt;
                String updatedPayload;
                serializeJson(document, updatedPayload);
                item.status = Status::Pending;
                item.nextRetryAt = nextRetryAt;
                item.attemptCount = 0;
                patched = writeItemUnlocked(item, updatedPayload);
                break;
            }
        }
        file = root.openNextFile();
    }
    root.close();
    return patched;
}

bool CommunityUploadQueue::selectReady(const String &endpoint, std::int64_t now, Item &item, String &payloadJson) const {
    QueueLock lock(mutex);
    if (!lock.locked || !ensureDirectoryUnlocked()) {
        return false;
    }
    File root = LittleFS.open(QUEUE_DIR);
    if (!root || !root.isDirectory()) {
        return false;
    }

    bool found = false;
    File file = root.openNextFile();
    while (file) {
        const String path = pathFromEntry(file.name());
        file.close();
        Item candidate;
        if (isQueuePath(path) && readItemUnlocked(path, candidate) && candidate.endpoint == endpoint &&
            (candidate.status == Status::Pending || candidate.status == Status::Failed) && candidate.nextRetryAt <= now &&
            (!found || candidate.createdAt < item.createdAt)) {
            item = candidate;
            found = true;
        }
        file = root.openNextFile();
    }
    root.close();
    return found && readItemUnlocked(item.path, item, &payloadJson);
}

CommunityUploadQueue::MutationResult CommunityUploadQueue::replaceIfCurrent(Item const &expected, const String &expectedPayload,
                                                                            Item replacement, const String &replacementPayload) {
    QueueLock lock(mutex);
    if (!lock.locked) {
        return MutationResult::Failed;
    }
    Item current;
    String currentPayload;
    if (!readItemUnlocked(expected.path, current, &currentPayload)) {
        return MutationResult::Stale;
    }
    if (current.uploadId != expected.uploadId || currentPayload != expectedPayload) {
        return MutationResult::Stale;
    }
    replacement.path = expected.path;
    return writeItemUnlocked(replacement, replacementPayload) ? MutationResult::Applied : MutationResult::Failed;
}

CommunityUploadQueue::MutationResult CommunityUploadQueue::removeIfCurrent(Item const &expected, const String &expectedPayload) {
    QueueLock lock(mutex);
    if (!lock.locked) {
        return MutationResult::Failed;
    }
    Item current;
    String currentPayload;
    if (!readItemUnlocked(expected.path, current, &currentPayload)) {
        return MutationResult::Stale;
    }
    if (current.uploadId != expected.uploadId || currentPayload != expectedPayload) {
        return MutationResult::Stale;
    }
    return LittleFS.remove(expected.path) ? MutationResult::Applied : MutationResult::Failed;
}

CommunityUploadQueue::Stats CommunityUploadQueue::stats() const {
    QueueLock lock(mutex);
    Stats result;
    if (!lock.locked || !ensureDirectoryUnlocked()) {
        return result;
    }
    File root = LittleFS.open(QUEUE_DIR);
    if (!root || !root.isDirectory()) {
        return result;
    }
    File file = root.openNextFile();
    while (file) {
        const String path = pathFromEntry(file.name());
        file.close();
        Item item;
        if (isQueuePath(path) && readItemUnlocked(path, item)) {
            result.bytes += item.bytes;
            if (item.status == Status::Failed) {
                ++result.failed;
            } else if (item.status == Status::Rejected) {
                ++result.rejected;
            } else {
                ++result.pending;
            }
        }
        file = root.openNextFile();
    }
    root.close();
    return result;
}

bool CommunityUploadQueue::discardMismatched(const String &endpoint, bool hasCredentials, const String &installId) {
    QueueLock lock(mutex);
    if (!lock.locked || !ensureDirectoryUnlocked() || endpoint.isEmpty()) {
        return false;
    }
    bool removed = false;
    File root = LittleFS.open(QUEUE_DIR);
    if (!root || !root.isDirectory()) {
        return false;
    }
    File file = root.openNextFile();
    while (file) {
        const String path = pathFromEntry(file.name());
        file.close();
        if (isQueuePath(path)) {
            Item item;
            String payloadJson;
            if (!readItemUnlocked(path, item, &payloadJson)) {
                LittleFS.remove(path);
                removed = true;
            } else if (item.endpoint.isEmpty()) {
                JsonDocument payload(&psramAllocator);
                const String queuedInstallId =
                    !deserializeJson(payload, payloadJson) ? payload["install_id"].as<String>() : String();
                if (hasCredentials && queuedInstallId == installId) {
                    item.endpoint = endpoint;
                    writeItemUnlocked(item, payloadJson);
                } else {
                    LittleFS.remove(path);
                    removed = true;
                }
            } else if (item.endpoint != endpoint) {
                LittleFS.remove(path);
                removed = true;
            }
        }
        file = root.openNextFile();
    }
    root.close();
    return removed;
}

bool CommunityUploadQueue::pruneUnlocked() {
    if (!ensureDirectoryUnlocked()) {
        return false;
    }
    std::vector<Item> items;
    size_t totalBytes = 0;
    File root = LittleFS.open(QUEUE_DIR);
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
            const String path = pathFromEntry(file.name());
            file.close();
            Item item;
            if (isQueuePath(path) && readItemUnlocked(path, item)) {
                items.push_back(item);
                totalBytes += item.bytes;
            } else if (isQueuePath(path)) {
                LittleFS.remove(path);
            }
            file = root.openNextFile();
        }
        root.close();
    }

    std::sort(items.begin(), items.end(), [](Item const &left, Item const &right) { return left.createdAt < right.createdAt; });
    while (!items.empty() && (items.size() > MAX_QUEUE_ITEMS || totalBytes > MAX_QUEUE_BYTES)) {
        const auto removable =
            std::find_if(items.begin(), items.end(), [](Item const &item) { return item.status == Status::Rejected; });
        if (removable == items.end()) {
            return false;
        }
        const Item item = *removable;
        if (LittleFS.remove(item.path)) {
            totalBytes = totalBytes > item.bytes ? totalBytes - item.bytes : 0;
        } else {
            return false;
        }
        items.erase(removable);
    }
    return items.size() <= MAX_QUEUE_ITEMS && totalBytes <= MAX_QUEUE_BYTES;
}

void CommunityUploadQueue::prune() {
    QueueLock lock(mutex);
    if (lock.locked) {
        pruneUnlocked();
    }
}

void CommunityUploadQueue::removeOneLegacyItem() {
    QueueLock lock(mutex);
    if (!lock.locked) {
        return;
    }
    File root = LittleFS.open(LEGACY_QUEUE_DIR);
    if (!root || !root.isDirectory()) {
        return;
    }
    File file = root.openNextFile();
    while (file) {
        const String path = pathFromEntry(LEGACY_QUEUE_DIR, file.name());
        const bool directory = file.isDirectory();
        file.close();
        if (!directory) {
            LittleFS.remove(path);
            break;
        }
        file = root.openNextFile();
    }
    root.close();
}
