#ifndef COMPLETEDSHOTARTIFACTSTORE_H
#define COMPLETEDSHOTARTIFACTSTORE_H

#include <Arduino.h>
#include <display/core/AutoTuningModels.h>
#include <vector>

class CompletedShotArtifactStore {
  public:
    bool begin();
    bool write(AutoTuning::CompletedShotArtifact &artifact);
    bool load(const String &shotId, AutoTuning::CompletedShotArtifact &artifact) const;
    bool exists(const String &shotId) const;
    bool remove(const String &shotId);
    bool listShotIds(std::vector<String> &shotIds) const;
    size_t bytes() const;

    static String pathFor(const String &shotId);

  private:
    bool recover();
};

#endif // COMPLETEDSHOTARTIFACTSTORE_H
