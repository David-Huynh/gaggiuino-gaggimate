#include "LocalAutoTuningFiles.h"

#include <display/util/AtomicFile.h>

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

static bool readJsonPath(const String &path, JsonDocument &document) {
    File file = LittleFS.open(path, FILE_READ);
    if (!file) {
        return false;
    }
    const DeserializationError error = deserializeJson(document, file);
    file.close();
    return !error && document.is<JsonObject>();
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
    if (LittleFS.exists(path)) {
        return true;
    }
    return LittleFS.mkdir(path);
}

String recordPath(const char *directory, const String &recordId) {
    return String(directory) + "/" + safeIdentifier(recordId) + ".json";
}

bool writeJson(const String &path, const JsonDocument &document) {
    const String temporaryPath = AtomicFile::temporaryPath(path);
    LittleFS.remove(temporaryPath);
    File file = LittleFS.open(temporaryPath, FILE_WRITE);
    if (!file) {
        return false;
    }
    const size_t expected = measureJson(document);
    const size_t written = serializeJson(document, file);
    file.flush();
    file.close();

    JsonDocument verification(&psramAllocator);
    if (written != expected || !readJsonPath(temporaryPath, verification)) {
        LittleFS.remove(temporaryPath);
        return false;
    }
    return AtomicFile::commit(path);
}

bool readJson(const String &path, JsonDocument &document) {
    const String temporaryPath = AtomicFile::temporaryPath(path);
    if (LittleFS.exists(temporaryPath)) {
        JsonDocument pending(&psramAllocator);
        AtomicFile::recoverPending(path, readJsonPath(temporaryPath, pending));
    }
    if (readJsonPath(path, document)) {
        AtomicFile::discardBackup(path);
        return true;
    }
    if (readJsonPath(AtomicFile::backupPath(path), document)) {
        AtomicFile::restoreBackup(path);
        return true;
    }
    return false;
}

void recoverDirectory(const char *directory) {
    File root = LittleFS.open(directory);
    if (!root || !root.isDirectory()) {
        return;
    }
    std::vector<String> temporaryPaths;
    std::vector<String> backupPaths;
    File file = root.openNextFile();
    while (file) {
        const String path = file.name();
        const bool regularFile = !file.isDirectory();
        file.close();
        if (regularFile && path.endsWith(".json.tmp")) {
            temporaryPaths.push_back(path);
        } else if (regularFile && path.endsWith(".json.bak")) {
            backupPaths.push_back(path);
        }
        file = root.openNextFile();
    }
    root.close();

    for (const String &temporaryPath : temporaryPaths) {
        JsonDocument pending(&psramAllocator);
        const String finalPath = temporaryPath.substring(0, temporaryPath.length() - 4);
        AtomicFile::recoverPending(finalPath, readJsonPath(temporaryPath, pending));
    }
    for (const String &backupPath : backupPaths) {
        const String finalPath = backupPath.substring(0, backupPath.length() - 4);
        if (LittleFS.exists(finalPath)) {
            AtomicFile::discardBackup(finalPath);
        } else {
            AtomicFile::restoreBackup(finalPath);
        }
    }
}

DirectoryStats directoryStats(const char *directory) {
    DirectoryStats result;
    File root = LittleFS.open(directory);
    if (!root || !root.isDirectory()) {
        return result;
    }
    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory() && String(file.name()).endsWith(".json")) {
            ++result.count;
            result.bytes += file.size();
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    return result;
}

std::int64_t fileTimestamp(const String &path) {
    JsonDocument document(&psramAllocator);
    if (!readJson(path, document)) {
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
    File root = LittleFS.open(directory);
    if (!root || !root.isDirectory()) {
        return false;
    }
    bool found = false;
    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory() && String(file.name()).endsWith(".json")) {
            const String path = file.name();
            file.close();
            const std::int64_t timestamp = fileTimestamp(path);
            if (!found || timestamp < oldestTimestamp) {
                found = true;
                oldestPath = path;
                oldestTimestamp = timestamp;
            }
        } else {
            file.close();
        }
        file = root.openNextFile();
    }
    root.close();
    return found;
}

bool removeOldestFile(const char *directory) {
    String oldestPath;
    std::int64_t oldestTimestamp = std::numeric_limits<std::int64_t>::max();
    return findOldestFile(directory, oldestPath, oldestTimestamp) && LittleFS.remove(oldestPath);
}

bool clearDirectory(const char *directory) {
    File root = LittleFS.open(directory);
    if (!root || !root.isDirectory()) {
        return true;
    }
    bool success = true;
    File file = root.openNextFile();
    while (file) {
        const String path = file.name();
        const bool directoryEntry = file.isDirectory();
        file.close();
        if (!directoryEntry) {
            success = LittleFS.remove(path) && success;
        }
        file = root.openNextFile();
    }
    root.close();
    return success;
}

} // namespace LocalAutoTuningFiles
