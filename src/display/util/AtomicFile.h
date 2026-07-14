#ifndef ATOMICFILE_H
#define ATOMICFILE_H

#include <Arduino.h>

namespace AtomicFile {

String temporaryPath(const String &path);
String backupPath(const String &path);

// Commits an already-written and validated .tmp file using a recoverable backup.
bool commit(const String &path);
bool recoverPending(const String &path, bool temporaryFileValid);
bool restoreBackup(const String &path);
void discardBackup(const String &path);

} // namespace AtomicFile

#endif // ATOMICFILE_H
