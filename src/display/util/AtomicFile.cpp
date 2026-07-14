#include "AtomicFile.h"
#include "LittleFSUtil.h"

#include <LittleFS.h>

namespace AtomicFile {

String temporaryPath(const String &path) { return path + ".tmp"; }

String backupPath(const String &path) { return path + ".bak"; }

bool restoreBackup(const String &path) {
    const String backup = backupPath(path);
    if (!LittleFSUtil::existsQuietly(backup)) {
        return false;
    }
    if (LittleFSUtil::existsQuietly(path) && !LittleFS.remove(path)) {
        return false;
    }
    return LittleFS.rename(backup, path);
}

void discardBackup(const String &path) { LittleFSUtil::removeIfExists(backupPath(path)); }

bool commit(const String &path) {
    const String temporary = temporaryPath(path);
    const String backup = backupPath(path);
    if (!LittleFSUtil::existsQuietly(temporary)) {
        return false;
    }

    if (LittleFSUtil::existsQuietly(path)) {
        if (!LittleFSUtil::removeIfExists(backup)) {
            return false;
        }
        if (!LittleFS.rename(path, backup)) {
            return false;
        }
    }
    if (LittleFS.rename(temporary, path)) {
        LittleFSUtil::removeIfExists(backup);
        return true;
    }
    if (!LittleFSUtil::existsQuietly(path) && LittleFSUtil::existsQuietly(backup)) {
        LittleFS.rename(backup, path);
    }
    return false;
}

bool recoverPending(const String &path, bool temporaryFileValid) {
    const String temporary = temporaryPath(path);
    if (LittleFSUtil::existsQuietly(temporary)) {
        if (temporaryFileValid) {
            if (commit(path)) {
                return true;
            }
        } else {
            LittleFSUtil::removeIfExists(temporary);
        }
    }
    if (LittleFSUtil::existsQuietly(path)) {
        discardBackup(path);
        return true;
    }
    return restoreBackup(path);
}

} // namespace AtomicFile
