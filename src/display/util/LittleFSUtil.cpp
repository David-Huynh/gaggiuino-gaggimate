#include "LittleFSUtil.h"

#include <LittleFS.h>
#ifndef GAGGIMATE_SIM
#include <sys/stat.h>
#endif

namespace LittleFSUtil {

String pathFromEntry(const char *directory, const String &entryName) {
    if (entryName.startsWith("/")) {
        return entryName;
    }

    String normalizedDirectory = directory;
    if (!normalizedDirectory.startsWith("/")) {
        normalizedDirectory = "/" + normalizedDirectory;
    }
    while (normalizedDirectory.length() > 1 && normalizedDirectory.endsWith("/")) {
        normalizedDirectory.remove(normalizedDirectory.length() - 1);
    }

    const String relativeDirectory = normalizedDirectory.substring(1);
    if (entryName == relativeDirectory || entryName.startsWith(relativeDirectory + "/")) {
        return "/" + entryName;
    }
    return normalizedDirectory == "/" ? "/" + entryName : normalizedDirectory + "/" + entryName;
}

bool existsQuietly(const String &path) {
#ifdef GAGGIMATE_SIM
    return LittleFS.exists(path);
#else
    const String vfsPath = path.startsWith("/littlefs/") || path == "/littlefs"
                               ? path
                               : (path.startsWith("/") ? "/littlefs" + path : "/littlefs/" + path);
    struct stat info {};
    return ::stat(vfsPath.c_str(), &info) == 0;
#endif
}

bool removeIfExists(const String &path) { return !existsQuietly(path) || LittleFS.remove(path); }

} // namespace LittleFSUtil
