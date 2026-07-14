#ifndef LITTLEFSUTIL_H
#define LITTLEFSUTIL_H

#include <Arduino.h>

namespace LittleFSUtil {

// File::name() differs across Arduino-ESP32 versions: it may return either an
// absolute path or only the directory-entry name. Normalize both forms.
String pathFromEntry(const char *directory, const String &entryName);

// Arduino-ESP32 2.x implements LittleFS.exists() through a logging read-open.
// Keep expected missing recovery files out of the error log.
bool existsQuietly(const String &path);

// Missing temporary and backup files are an expected atomic-write state.
bool removeIfExists(const String &path);

} // namespace LittleFSUtil

#endif // LITTLEFSUTIL_H
