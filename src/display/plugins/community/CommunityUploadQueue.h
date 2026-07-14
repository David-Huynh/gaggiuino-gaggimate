#ifndef COMMUNITYUPLOADQUEUE_H
#define COMMUNITYUPLOADQUEUE_H

#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class CommunityUploadQueue {
  public:
    enum class Status : std::uint8_t {
        Pending,
        Failed,
        Rejected,
    };

    enum class MutationResult : std::uint8_t {
        Applied,
        Stale,
        Failed,
    };

    struct Item {
        String path;
        String uploadId;
        String recordType;
        String recordId;
        Status status = Status::Pending;
        String endpoint;
        std::int64_t createdAt = 0;
        std::int64_t nextRetryAt = 0;
        int attemptCount = 0;
        size_t bytes = 0;
    };

    struct Stats {
        int pending = 0;
        int failed = 0;
        int rejected = 0;
        size_t bytes = 0;
    };

    CommunityUploadQueue() = default;
    ~CommunityUploadQueue();
    CommunityUploadQueue(CommunityUploadQueue const &) = delete;
    CommunityUploadQueue &operator=(CommunityUploadQueue const &) = delete;

    bool begin();
    bool storageAvailable() const;
    void recover();
    bool enqueue(const String &uploadId, const String &recordType, const String &recordId, const String &payloadJson,
                 const String &endpoint, std::int64_t createdAt, std::int64_t nextRetryAt, bool replaceRecord);
    bool patchShotCorrection(const String &shotId, bool hasGrindFollowed, bool grindFollowed, bool hasDoseFollowed,
                             bool doseFollowed, bool hasYieldFollowed, bool yieldFollowed, std::int64_t updatedAt,
                             std::int64_t nextRetryAt);
    bool selectReady(const String &endpoint, std::int64_t now, Item &item, String &payloadJson) const;
    MutationResult replaceIfCurrent(Item const &expected, const String &expectedPayload, Item replacement,
                                    const String &replacementPayload);
    MutationResult removeIfCurrent(Item const &expected, const String &expectedPayload);
    Stats stats() const;
    bool discardMismatched(const String &endpoint, bool hasCredentials, const String &installId);
    void prune();
    void removeOneLegacyItem();

  private:
    bool ensureDirectoryUnlocked() const;
    bool readItemUnlocked(const String &path, Item &item, String *payloadJson = nullptr) const;
    bool writeItemUnlocked(const Item &item, const String &payloadJson) const;
    bool removeRecordUnlocked(const String &recordType, const String &recordId, const String &exceptPath = "");
    bool pruneUnlocked();

    mutable SemaphoreHandle_t mutex = nullptr;
};

#endif // COMMUNITYUPLOADQUEUE_H
