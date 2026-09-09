#include "CommunityUploadPlugin.h"
#include <display/core/Controller.h>
#include <display/core/EpochTime.h>
#include <display/core/StorageCoordinator.h>
#include <display/plugins/community/CommunityPayloadValidator.h>
#include <ArduinoJson.h>
#include <display/util/PsramAllocator.h>

void CommunityUploadPlugin::setup(Controller *ctrl, PluginManager *pm) {
    controller = ctrl;
    backlog.begin();
    backlog.recover();
    const auto stats = backlog.stats();
    if (stats.pending || stats.failed) {
        xTaskCreatePinnedToCore(worker, "CloudHandoff", 6144, this, 1, nullptr, 0);
    }
}

void CommunityUploadPlugin::loop() {}

void CommunityUploadPlugin::worker(void *argument) {
    auto *plugin = static_cast<CommunityUploadPlugin *>(argument);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        plugin->relayPending();
        const auto stats = plugin->backlog.stats();
        if (!stats.pending && !stats.failed) vTaskDelete(nullptr);
    }
}

void CommunityUploadPlugin::relayPending() {
    // No network or filesystem work during a process. Transfer at most one old
    // record per cycle; new shots have only the existing completed-shot copy.
    auto flashLease = StorageCoordinator::instance().tryAcquireFlash();
    if (!flashLease || !controller->getSettings().isRLCommunityUploadEnabled()) return;
    auto *transport = controller->getOptimizerTransport();
    if (!transport || !transport->connected()) return;
    CommunityUploadQueue::Item item;
    String payload;
    if (!backlog.selectReady("", EpochTime::now(), item, payload)) return;
    JsonDocument document(&psramAllocator);
    String error;
    if (deserializeJson(document, payload) ||
        !CommunityPayloadValidator::validateRecord(document.as<JsonObjectConst>(), item.recordType, item.recordId, error)) {
        // Preserve invalid old records for diagnosis, without blocking others.
        auto rejected = item;
        rejected.status = CommunityUploadQueue::Status::Rejected;
        backlog.replaceIfCurrent(item, payload, rejected, payload);
        return;
    }
    if (transport->enqueueCommunityHandoff(payload.c_str())) {
        // The MQTT outbox now owns these exact bytes until application receipt.
        backlog.removeIfCurrent(item, payload);
    }
}
