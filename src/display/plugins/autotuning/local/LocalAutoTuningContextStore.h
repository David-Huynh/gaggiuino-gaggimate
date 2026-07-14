#ifndef LOCALAUTOTUNINGCONTEXTSTORE_H
#define LOCALAUTOTUNINGCONTEXTSTORE_H

#include <Arduino.h>
#include <cstddef>

class Settings;

class LocalAutoTuningContextStore {
  public:
    bool begin() const;
    bool save(Settings const &settings) const;
    bool clear() const;
    size_t bytes() const;
};

#endif // LOCALAUTOTUNINGCONTEXTSTORE_H
