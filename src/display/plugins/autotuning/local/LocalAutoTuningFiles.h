#ifndef LOCALAUTOTUNINGFILES_H
#define LOCALAUTOTUNINGFILES_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace LocalAutoTuningFiles {

struct DirectoryStats {
    size_t count = 0;
    size_t bytes = 0;
};

bool ensureDirectory(const char *path);
String recordPath(const char *directory, const String &recordId);
bool readJson(const String &path, JsonDocument &document);
bool writeJson(const String &path, const JsonDocument &document);
void recoverDirectory(const char *directory);
bool listRecordPaths(const char *directory, std::vector<String> &paths);
DirectoryStats directoryStats(const char *directory);
std::int64_t fileTimestamp(const String &path);
bool findOldestFile(const char *directory, String &oldestPath, std::int64_t &oldestTimestamp);
bool removeOldestFile(const char *directory);
bool clearDirectory(const char *directory);

} // namespace LocalAutoTuningFiles

#endif // LOCALAUTOTUNINGFILES_H
