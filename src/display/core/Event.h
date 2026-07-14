#ifndef EVENT_H
#define EVENT_H

#include <Arduino.h>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

enum class EventDataType { EVENT_TYPE_INT, EVENT_TYPE_INT64, EVENT_TYPE_FLOAT, EVENT_TYPE_STRING, EVENT_TYPE_NONE };

struct EventDataEntry {
    String key;
    EventDataType type = EventDataType::EVENT_TYPE_NONE;
    int intValue = 0;
    std::int64_t int64Value = 0;
    float floatValue = 0.0f;
    String stringValue = "";

    EventDataEntry() = default;

    EventDataEntry(const String &k, int value) : key(k), type(EventDataType::EVENT_TYPE_INT), intValue(value) {}

    EventDataEntry(const String &k, std::int64_t value) : key(k), type(EventDataType::EVENT_TYPE_INT64), int64Value(value) {}

    EventDataEntry(const String &k, float value) : key(k), type(EventDataType::EVENT_TYPE_FLOAT), floatValue(value) {}

    EventDataEntry(const String &k, const String &value) : key(k), type(EventDataType::EVENT_TYPE_STRING), stringValue(value) {}
};

using EventData = std::vector<EventDataEntry>;

struct Event {
    String id;
    EventData data;
    bool stopPropagation = false;

    template <typename T> void setPayload(T value) {
        objectPayload = std::make_shared<T>(std::move(value));
        objectPayloadType = payloadTypeToken<T>();
    }

    template <typename T> T const *getPayload() const {
        return objectPayloadType == payloadTypeToken<T>() ? static_cast<T const *>(objectPayload.get()) : nullptr;
    }

    void setInt(const String &key, int value) { data.emplace_back(key, value); }

    void setInt64(const String &key, std::int64_t value) { data.emplace_back(key, value); }

    void setFloat(const String &key, float value) { data.emplace_back(key, value); }

    void setString(const String &key, const String &value) { data.emplace_back(key, value); }

    int getInt(const String &key) const {
        for (const auto &entry : data) {
            if (entry.key == key && entry.type == EventDataType::EVENT_TYPE_INT) {
                return entry.intValue;
            }
        }
        return 0;
    }

    std::int64_t getInt64(const String &key) const {
        for (const auto &entry : data) {
            if (entry.key != key) {
                continue;
            }
            if (entry.type == EventDataType::EVENT_TYPE_INT64) {
                return entry.int64Value;
            }
            if (entry.type == EventDataType::EVENT_TYPE_INT) {
                return entry.intValue;
            }
        }
        return 0;
    }

    float getFloat(const String &key) const {
        for (const auto &entry : data) {
            if (entry.key == key && entry.type == EventDataType::EVENT_TYPE_FLOAT) {
                return entry.floatValue;
            }
        }
        return 0.0f;
    }

    String getString(const String &key) const {
        for (const auto &entry : data) {
            if (entry.key == key && entry.type == EventDataType::EVENT_TYPE_STRING) {
                return entry.stringValue;
            }
        }
        return "";
    }

  private:
    template <typename T> static const void *payloadTypeToken() {
        static const char token = 0;
        return &token;
    }

    std::shared_ptr<const void> objectPayload;
    const void *objectPayloadType = nullptr;
};

#endif // EVENT_H
