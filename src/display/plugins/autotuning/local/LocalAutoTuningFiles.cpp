#include "LocalAutoTuningFiles.h"

#include <display/util/AtomicFile.h>
#include <display/util/LittleFSUtil.h>
#include <display/core/StorageCoordinator.h>

#include <LittleFS.h>
#include <algorithm>
#include <display/util/PsramAllocator.h>
#include <limits>
#include <vector>

namespace LocalAutoTuningFiles {
namespace {

static String safeIdentifier(String value, size_t maxLength = 120) {
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

static bool listRegularPaths(const char *directory, std::vector<String> &paths,
                             StorageCoordinator::FlashLease &flashLease) {
    StorageCoordinator::instance().assertFlashLease();
    if (!LittleFSUtil::existsQuietly(directory)) {
        return false;
    }
    File root = LittleFS.open(directory);
    if (!root || !root.isDirectory()) {
        return false;
    }
    File file = root.openNextFile();
    size_t scanned = 0;
    while (file) {
        const String path = LittleFSUtil::pathFromEntry(directory, file.name());
        const bool regularFile = !file.isDirectory();
        file.close();
        if (regularFile) {
            paths.push_back(path);
        }
        if (++scanned % 8 == 0) {
            flashLease.checkpoint();
        }
        file = root.openNextFile();
    }
    root.close();
    return true;
}

static bool readJsonPath(const String &path, JsonDocument &document,
                         StorageCoordinator::FlashLease &flashLease) {
    (void)flashLease;
    StorageCoordinator::instance().assertFlashLease();
    File file = LittleFS.open(path, FILE_READ);
    if (!file) {
        return false;
    }
    const DeserializationError error = deserializeJson(document, file);
    file.close();
    return !error && document.is<JsonObject>();
}

static bool listRecordPathsUnlocked(const char *directory,
                                    std::vector<String> &paths,
                                    StorageCoordinator::FlashLease &flashLease) {
    std::vector<String> allPaths;
    if (!listRegularPaths(directory, allPaths, flashLease)) {
        return false;
    }
    for (const String &path : allPaths) {
        if (path.endsWith(".json")) {
            paths.push_back(path);
        }
    }
    return true;
}

static bool readJsonUnlocked(const String &path, JsonDocument &document,
                             StorageCoordinator::FlashLease &flashLease) {
    const String temporaryPath = AtomicFile::temporaryPath(path);
    if (LittleFSUtil::existsQuietly(temporaryPath)) {
        JsonDocument pending(&psramAllocator);
        AtomicFile::recoverPending(
            path, readJsonPath(temporaryPath, pending, flashLease));
    }
    if (readJsonPath(path, document, flashLease)) {
        AtomicFile::discardBackup(path);
        return true;
    }
    const String backupPath = AtomicFile::backupPath(path);
    if (LittleFSUtil::existsQuietly(backupPath) &&
        readJsonPath(backupPath, document, flashLease)) {
        AtomicFile::restoreBackup(path);
        return true;
    }
    return false;
}

static bool jsonEpoch(JsonVariantConst value, std::int64_t &output) {
    if (value.is<bool>() || !value.is<std::int64_t>()) {
        return false;
    }
    output = value.as<std::int64_t>();
    return true;
}

} // namespace

bool ensureDirectory(const char *path) {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    if (LittleFSUtil::existsQuietly(path)) {
        return true;
    }
    return LittleFS.mkdir(path);
}

String recordPath(const char *directory, const String &recordId) {
    return String(directory) + "/" + safeIdentifier(recordId) + ".json";
}

bool writeJson(const String &path, const JsonDocument &document) {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    const String temporaryPath = AtomicFile::temporaryPath(path);
    if (!LittleFSUtil::removeIfExists(temporaryPath)) {
        return false;
    }
    File file = LittleFS.open(temporaryPath, FILE_WRITE);
    if (!file) {
        return false;
    }
    const size_t expected = measureJson(document);
    const size_t written = serializeJson(document, file);
    file.flush();
    file.close();

    JsonDocument verification(&psramAllocator);
    if (written != expected ||
        !readJsonPath(temporaryPath, verification, flashLease)) {
        LittleFSUtil::removeIfExists(temporaryPath);
        return false;
    }
    return AtomicFile::commit(path);
}

bool readJson(const String &path, JsonDocument &document) {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    return readJsonUnlocked(path, document, flashLease);
}

void recoverDirectory(const char *directory) {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    std::vector<String> paths;
    if (!listRegularPaths(directory, paths, flashLease)) {
        return;
    }
    std::vector<String> temporaryPaths;
    std::vector<String> backupPaths;
    for (const String &path : paths) {
        if (path.endsWith(".json.tmp")) {
            temporaryPaths.push_back(path);
        } else if (path.endsWith(".json.bak")) {
            backupPaths.push_back(path);
        }
    }

    for (const String &temporaryPath : temporaryPaths) {
        JsonDocument pending(&psramAllocator);
        const String finalPath = temporaryPath.substring(0, temporaryPath.length() - 4);
        AtomicFile::recoverPending(
            finalPath, readJsonPath(temporaryPath, pending, flashLease));
        flashLease.checkpoint();
    }
    for (const String &backupPath : backupPaths) {
        const String finalPath = backupPath.substring(0, backupPath.length() - 4);
        if (LittleFSUtil::existsQuietly(finalPath)) {
            AtomicFile::discardBackup(finalPath);
        } else {
            AtomicFile::restoreBackup(finalPath);
        }
        flashLease.checkpoint();
    }
}

bool listRecordPaths(const char *directory, std::vector<String> &paths) {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    return listRecordPathsUnlocked(directory, paths, flashLease);
}

DirectoryStats directoryStats(const char *directory) {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    DirectoryStats result;
    std::vector<String> paths;
    if (!listRecordPathsUnlocked(directory, paths, flashLease)) {
        return result;
    }
    size_t scanned = 0;
    for (const String &path : paths) {
        File file = LittleFS.open(path, FILE_READ);
        if (file) {
            ++result.count;
            result.bytes += file.size();
            file.close();
        }
        if (++scanned % 8 == 0) {
            flashLease.checkpoint();
        }
    }
    return result;
}

std::int64_t fileTimestamp(const String &path) {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    JsonDocument document(&psramAllocator);
    if (!readJsonUnlocked(path, document, flashLease)) {
        return 0;
    }
    JsonObjectConst root = document.as<JsonObjectConst>();
    std::int64_t timestamp = 0;
    if (jsonEpoch(root["updated_at"], timestamp) || jsonEpoch(root["timestamp"], timestamp) ||
        jsonEpoch(root["created_at"], timestamp)) {
        return timestamp;
    }
    return 0;
}

bool findOldestFile(const char *directory, String &oldestPath, std::int64_t &oldestTimestamp) {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    std::vector<String> paths;
    if (!listRecordPathsUnlocked(directory, paths, flashLease)) {
        return false;
    }
    bool found = false;
    size_t scanned = 0;
    for (const String &path : paths) {
        JsonDocument document(&psramAllocator);
        std::int64_t timestamp = 0;
        if (readJsonUnlocked(path, document, flashLease)) {
            JsonObjectConst root = document.as<JsonObjectConst>();
            jsonEpoch(root["updated_at"], timestamp) ||
                jsonEpoch(root["timestamp"], timestamp) ||
                jsonEpoch(root["created_at"], timestamp);
        }
        if (!found || timestamp < oldestTimestamp) {
            found = true;
            oldestPath = path;
            oldestTimestamp = timestamp;
        }
        if (++scanned % 8 == 0) {
            flashLease.checkpoint();
        }
    }
    return found;
}

bool removeOldestFile(const char *directory) {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    String oldestPath;
    std::int64_t oldestTimestamp = std::numeric_limits<std::int64_t>::max();
    std::vector<String> paths;
    if (!listRecordPathsUnlocked(directory, paths, flashLease)) {
        return false;
    }
    bool found = false;
    for (const String &path : paths) {
        JsonDocument document(&psramAllocator);
        std::int64_t timestamp = 0;
        if (readJsonUnlocked(path, document, flashLease)) {
            JsonObjectConst root = document.as<JsonObjectConst>();
            jsonEpoch(root["updated_at"], timestamp) ||
                jsonEpoch(root["timestamp"], timestamp) ||
                jsonEpoch(root["created_at"], timestamp);
        }
        if (!found || timestamp < oldestTimestamp) {
            found = true;
            oldestPath = path;
            oldestTimestamp = timestamp;
        }
        flashLease.checkpoint();
    }
    return found && LittleFS.remove(oldestPath);
}

bool clearDirectory(const char *directory) {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    std::vector<String> paths;
    if (!listRegularPaths(directory, paths, flashLease)) {
        return true;
    }
    bool success = true;
    size_t removed = 0;
    for (const String &path : paths) {
        success = LittleFS.remove(path) && success;
        if (++removed % 8 == 0) {
            flashLease.checkpoint();
        }
    }
    return success;
}

} // namespace LocalAutoTuningFiles
