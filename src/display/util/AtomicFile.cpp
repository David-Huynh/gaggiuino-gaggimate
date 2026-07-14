#include "AtomicFile.h"

#include <LittleFS.h>

namespace AtomicFile {

String temporaryPath(const String &path) { return path + ".tmp"; }

String backupPath(const String &path) { return path + ".bak"; }

bool restoreBackup(const String &path) {
    const String backup = backupPath(path);
    if (!LittleFS.exists(backup)) {
        return false;
    }
    if (LittleFS.exists(path) && !LittleFS.remove(path)) {
        return false;
    }
    return LittleFS.rename(backup, path);
}

void discardBackup(const String &path) { LittleFS.remove(backupPath(path)); }

bool commit(const String &path) {
    const String temporary = temporaryPath(path);
    const String backup = backupPath(path);
    if (!LittleFS.exists(temporary)) {
        return false;
    }

    if (LittleFS.exists(path)) {
        LittleFS.remove(backup);
        if (!LittleFS.rename(path, backup)) {
            return false;
        }
    }
    if (LittleFS.rename(temporary, path)) {
        LittleFS.remove(backup);
        return true;
    }
    if (!LittleFS.exists(path) && LittleFS.exists(backup)) {
        LittleFS.rename(backup, path);
    }
    return false;
}

bool recoverPending(const String &path, bool temporaryFileValid) {
    const String temporary = temporaryPath(path);
    if (LittleFS.exists(temporary)) {
        if (temporaryFileValid) {
            if (commit(path)) {
                return true;
            }
        } else {
            LittleFS.remove(temporary);
        }
    }
    if (LittleFS.exists(path)) {
        discardBackup(path);
        return true;
    }
    return restoreBackup(path);
}

} // namespace AtomicFile
