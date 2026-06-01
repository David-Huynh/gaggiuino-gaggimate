#include "WebUIPlugin.h"
#include <DNSServer.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <algorithm>
#include <display/core/Controller.h>
#include <display/core/ProfileManager.h>
#include <display/core/process/BrewProcess.h>
#include <display/core/process/GrindProcess.h>
#include <display/models/profile.h>
#include <cmath>
#include <ctime>
#include <display/plugins/BLEScalePlugin.h>
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
#include <display/plugins/HWScalePlugin.h>
#endif
#include <display/plugins/ShotHistoryPlugin.h>
#include <display/util/PsramStlAllocator.h>
#include <esp_core_dump.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <version.h>

// Incoming WebSocket payloads (profile uploads reserve up to 64 KB) are
// reassembled here. Back the character storage with PSRAM so these large,
// transient buffers don't spike the scarce internal SRAM. The map nodes
// themselves stay on the default heap (tiny: an id + a string handle).
using PsramString = std::basic_string<char, std::char_traits<char>, PsramStlAllocator<char>>;
static std::unordered_map<uint32_t, PsramString> rxBuffers;
static std::unordered_map<uint32_t, unsigned long> rxBufferLastActivity;
static constexpr unsigned long RXBUFFER_IDLE_EVICT_MS = 5UL * 60UL * 1000UL;
static WebUIPlugin *g_webUIPlugin = nullptr;

#if defined(GAGGIMATE_DISABLE_OTA)
static constexpr bool OTA_ENABLED = false;
#else
static constexpr bool OTA_ENABLED = true;
#endif

static String defaultRLContextName() {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "Bean %lu", static_cast<unsigned long>(std::time(nullptr)));
    return String(buffer);
}

static String makeRLContextId(const String &name) {
    String id = name;
    id.toLowerCase();
    id.replace(" ", "_");
    id.replace("/", "_");
    id.replace("\\", "_");
    id.replace(":", "_");
    id.replace("\"", "");
    if (id.isEmpty()) {
        id = "bean";
    }
    return "bean_" + id + "_" + String(static_cast<unsigned long>(std::time(nullptr))) + "_" +
           String(static_cast<unsigned long>(millis()));
}

static JsonArray loadRLContexts(JsonDocument &doc, const String &rawJson) {
    DeserializationError error = deserializeJson(doc, rawJson);
    if (error || !doc.is<JsonArray>()) {
        doc.clear();
        return doc.to<JsonArray>();
    }
    return doc.as<JsonArray>();
}

static int currentBagIndex(JsonArray contexts, const String &name) {
    int bag = 0;
    for (JsonObject context : contexts) {
        if (context["name"].as<String>() == name) {
            bag = std::max(bag, context["bag_index"] | 0);
        }
    }
    return bag;
}

static JsonObject findRLContext(JsonArray contexts, const String &contextId) {
    for (JsonObject context : contexts) {
        if (context["id"].as<String>() == contextId) {
            return context;
        }
    }
    return JsonObject();
}

static void markRLOtherContextsAvailable(JsonArray contexts, const String &activeId) {
    for (JsonObject context : contexts) {
        if (context["id"].as<String>() != activeId && context["status"].as<String>() == "active") {
            context["status"] = "available";
        }
    }
}

static void addRLContext(JsonArray contexts, const String &id, const String &name, int bagIndex, const char *status) {
    JsonObject context = contexts.add<JsonObject>();
    context["id"] = id;
    context["name"] = name;
    context["bag_index"] = bagIndex;
    context["status"] = status;
    context["created_at"] = static_cast<long>(std::time(nullptr));
}

static void persistRLContexts(Settings &settings, JsonDocument &contextsDoc) {
    String contextsJson;
    serializeJson(contextsDoc, contextsJson);
    settings.setRLBeanContextsJson(contextsJson);
}

WebUIPlugin::WebUIPlugin() : server(80), ws("/ws") { g_webUIPlugin = this; }

void WebUIPlugin::setup(Controller *_controller, PluginManager *_pluginManager) {
    this->controller = _controller;
    this->profileManager = _controller->getProfileManager();
    this->pluginManager = _pluginManager;
    this->ota = new GitHubOTA(
        BUILD_GIT_VERSION, controller->getSystemInfo().version,
        RELEASE_URL + (controller->getSettings().getOTAChannel() == "latest" ? "latest" : "tag/nightly"),
        [this](uint8_t phase) {
            pluginManager->trigger("ota:update:phase", "phase", phase);
            updateOTAProgress(phase, 0);
        },
        [this](uint8_t phase, int progress) {
            pluginManager->trigger("ota:update:progress", "progress", progress);
            updateOTAProgress(phase, progress);
        },
        "display-firmware.bin", "display-filesystem.bin", "board-firmware.bin");
    pluginManager->on("controller:wifi:connect", [this](Event const &event) {
        apMode = event.getInt("AP");
        start();
    });
    // Intentionally do NOT stop the server on a WiFi disconnect: the listen
    // socket survives a reconnect, and tearing it down only to rebind moments
    // later races AsyncTCP's async close (bind: -8) and churns sockets in the
    // recovery path. The server keeps listening; clients reconnect on their own.
    pluginManager->on("controller:wifi:disconnect", [this](Event const &) {
        ws.cleanupClients(); // drop dead websocket clients; keep the listener up
    });
    pluginManager->on("controller:ready", [this](Event const &) {
        ota->setControllerVersion(controller->getSystemInfo().version);
#ifndef GAGGIMATE_UART_COMMS
        ota->init(controller->getClientController()->getClient());
#endif
    });
    pluginManager->on("controller:autotune:result", [this](Event const &event) { sendAutotuneResult(); });
    pluginManager->on("controller:autotune:failed", [this](Event const &) { sendAutotuneFailed(); });

    pluginManager->on("rl:recommendation:received", [this](Event const &event) {
        if (!controller->getSettings().isHomeAssistant() || !controller->getSettings().isRLRatingEnabled()) {
            return;
        }

        const String status = event.getString("status");
        JsonDocument doc;
        doc["tp"] = "evt:rl:recommendation";
        doc["recommendation_id"] = event.getString("recommendation_id");
        doc["shot_id"] = event.getString("shot_id");
        doc["status"] = status;
        doc["mode"] = event.getString("mode");
        doc["grind_delta_steps"] = event.getInt("grind_delta_steps");
        doc["grind_delta_um"] = event.getFloat("grind_delta_um");
        doc["next_grind_steps"] = event.getFloat("next_grind_steps");
        doc["next_grind_um"] = event.getFloat("next_grind_um");
        doc["next_dose_g"] = event.getFloat("next_dose_g");
        doc["target_yield_g"] = event.getFloat("target_yield_g");
        doc["target_ratio"] = event.getFloat("target_ratio");
        const String payload = doc.as<String>();
        // Cache as the reopen source of truth only while still promptable; a
        // resolved status (applied/ignored/etc) clears any pending prompt.
        const bool promptable = status.isEmpty() || status == "pending" || status == "shown";
        _pendRecJson = promptable ? payload : String("");
        ws.textAll(payload);
    });

    pluginManager->on("rl:shot:complete", [this](Event const &event) {
        if (!controller->getSettings().isHomeAssistant() || !controller->getSettings().isRLRatingEnabled()) {
            return;
        }

        _pendRateShotId = event.getString("shot_id");
        _pendRateRecId = event.getString("recommendation_id");
        sendRatingPrompt(nullptr); // pending until the shot is rated or skipped
    });

    pluginManager->on("rl:status:received", [this](Event const &event) {
        if (!controller->getSettings().isHomeAssistant() || !controller->getSettings().isRLRatingEnabled()) {
            return;
        }

        rlStatusSeen = event.getInt("seen") > 0;
        rlAddonOnline = event.getInt("addon_online") > 0;
        rlLastStatusAt = event.getInt("timestamp");
        rlLastShotId = event.getString("last_shot_id");
        rlLastShotAt = event.getInt("last_shot_at");
        rlLastRecommendationId = event.getString("last_recommendation_id");
        rlLastRecommendationAt = event.getInt("last_recommendation_at");
        rlRecommendationApplyStatus = event.getString("recommendation_apply_status");
        rlMode = event.getString("mode");
        rlLocalShotCount = event.getInt("local_shot_count");
        rlRatedShotCount = event.getInt("rated_shot_count");
        rlUploadQueueCount = event.getInt("upload_queue_count");
        rlUploadQueueRejectedCount = event.getInt("upload_queue_rejected_count");
        rlUploadQueueLastRejectedId = event.getString("upload_queue_last_rejected_id");
        rlUploadQueueLastRejectedRecordId = event.getString("upload_queue_last_rejected_record_id");
        rlUploadQueueLastRejectedError = event.getString("upload_queue_last_rejected_error");
        rlCommunityUploadEnabled = event.getInt("community_upload_enabled") > 0;
        rlBestKnownRecipe = event.getString("best_known_recipe");

        JsonDocument doc;
        doc["tp"] = "evt:rl:status";
        doc["rlStatusSeen"] = rlStatusSeen;
        doc["rlAddonOnline"] = rlAddonOnline;
        doc["rlLastStatusAt"] = rlLastStatusAt;
        doc["rlLastShotId"] = rlLastShotId;
        doc["rlLastShotAt"] = rlLastShotAt;
        doc["rlLastRecommendationId"] = rlLastRecommendationId;
        doc["rlLastRecommendationAt"] = rlLastRecommendationAt;
        doc["rlRecommendationApplyStatus"] = rlRecommendationApplyStatus;
        doc["rlMode"] = rlMode;
        doc["rlLocalShotCount"] = rlLocalShotCount;
        doc["rlRatedShotCount"] = rlRatedShotCount;
        doc["rlUploadQueueCount"] = rlUploadQueueCount;
        doc["rlUploadQueueRejectedCount"] = rlUploadQueueRejectedCount;
        doc["rlUploadQueueLastRejectedId"] = rlUploadQueueLastRejectedId;
        doc["rlUploadQueueLastRejectedRecordId"] = rlUploadQueueLastRejectedRecordId;
        doc["rlUploadQueueLastRejectedError"] = rlUploadQueueLastRejectedError;
        doc["rlCommunityUploadEnabled"] = rlCommunityUploadEnabled;
        doc["rlBestKnownRecipe"] = rlBestKnownRecipe;
        ws.textAll(doc.as<String>());
    });

    // Forward shot history rebuild progress events to WebSocket clients
    pluginManager->on("evt:history-rebuild-progress", [this](Event const &event) {
        JsonDocument doc(&psramAllocator);
        doc["tp"] = "evt:history-rebuild-progress";
        doc["total"] = event.getInt("total");
        doc["current"] = event.getInt("current");
        doc["status"] = event.getString("status");
        broadcastJson(doc);
    });

    // Subscribe to Bluetooth scale weight updates
    pluginManager->on("controller:volumetric-measurement:bluetooth:change",
                      [this](Event const &event) { this->currentBluetoothWeight = event.getFloat("value"); });
    // Hardware scale and predictive flow estimate are tracked alongside BLE so
    // the WebUI 'cw' field can mirror whichever source the user has selected.
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    pluginManager->on("controller:volumetric-measurement:hardware:change",
                      [this](Event const &event) { this->currentHardwareWeight = event.getFloat("value"); });
#endif
    pluginManager->on("controller:volumetric-measurement:estimation:change",
                      [this](Event const &event) { this->currentEstimatedWeight = event.getFloat("value"); });

#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    // Forward async scale events as WebSocket messages so the calibration page can
    // drive its live progress bars and result toasts directly.
    pluginManager->on("controller:scale:tare:progress", [this](Event const &ev) {
        JsonDocument doc;
        doc["tp"] = "evt:scale:tare:progress";
        doc["samples"] = ev.getInt("samples");
        doc["stddevG"] = ev.getFloat("stddevG");
        ws.textAll(doc.as<String>());
    });
    pluginManager->on("controller:scale:tare:done", [this](Event const &ev) {
        JsonDocument doc;
        doc["tp"] = "evt:scale:tare:done";
        doc["success"] = ev.getInt("success");
        doc["offset1"] = ev.getFloat("offset1");
        doc["offset2"] = ev.getFloat("offset2");
        doc["std1"] = ev.getFloat("std1");
        doc["std2"] = ev.getFloat("std2");
        doc["healthBits"] = ev.getInt("healthBits");
        ws.textAll(doc.as<String>());
    });
    pluginManager->on("controller:scale:cal:progress", [this](Event const &ev) {
        JsonDocument doc;
        doc["tp"] = "evt:scale:cal:progress";
        doc["channel"] = ev.getInt("channel");
        doc["samples"] = ev.getInt("samples");
        doc["stddevG"] = ev.getFloat("stddevG");
        ws.textAll(doc.as<String>());
    });
    pluginManager->on("controller:scale:cal:done", [this](Event const &ev) {
        JsonDocument doc;
        doc["tp"] = "evt:scale:cal:done";
        doc["channel"] = ev.getInt("channel");
        doc["factor"] = ev.getFloat("factor");
        doc["stddevG"] = ev.getFloat("stddevG");
        doc["success"] = ev.getInt("success");
        doc["errorCode"] = ev.getInt("errorCode");
        ws.textAll(doc.as<String>());
    });
    pluginManager->on("controller:scale:not-ready", [this](Event const &ev) {
        JsonDocument doc;
        doc["tp"] = "evt:scale:not-ready";
        doc["source"] = ev.getInt("source");
        ws.textAll(doc.as<String>());
    });
#endif

    setupServer();
}

void WebUIPlugin::loop() {
    if (updating) {
        // Pass which component is being flashed: a controller update streams the
        // firmware over BLE (wants a low-latency link), a display update is over
        // Wi-Fi (wants BLE to stay out of the radio's way). "" = both.
        pluginManager->trigger("ota:update:start", "component", updateComponent);
        ota->update(updateComponent != "display", updateComponent != "controller");
        pluginManager->trigger("ota:update:end");
        updating = false;
    }
    if (!serverRunning) {
        return;
    }
    const long now = millis();
    if ((lastUpdateCheck == 0 || now > lastUpdateCheck + UPDATE_CHECK_INTERVAL)) {
        ota->checkForUpdates();
        pluginManager->trigger("ota:update:status", "value", ota->isUpdateAvailable());
        lastUpdateCheck = now;
        updateOTAStatus(ota->getCurrentVersion());
    }
    if (now > lastStatus + STATUS_PERIOD && !ws.getClients().empty()) {
        lastStatus = now;
        statusDoc.clear();
        statusDoc["tp"] = "evt:status";
        statusDoc["ct"] = controller->getCurrentTemp();
        statusDoc["tt"] = controller->getTargetTemp();
        statusDoc["pr"] = controller->getCurrentPressure();
        statusDoc["fl"] = controller->getCurrentPumpFlow();
        statusDoc["pt"] = controller->getTargetPressure();
        statusDoc["m"] = controller->getMode();
        statusDoc["p"] = controller->getProfileManager()->getSelectedProfile().label;
        statusDoc["puid"] = controller->getProfileManager()->getSelectedProfile().id;
        statusDoc["cp"] = controller->getSystemInfo().capabilities.pressure;
        statusDoc["cd"] = controller->getSystemInfo().capabilities.dimming;
        statusDoc["tw"] = profileManager->getSelectedProfile().getTotalVolume(); // total target weight for the process
        statusDoc["bta"] = controller->isVolumetricAvailable() ? 1 : 0;
        statusDoc["bt"] =
            controller->isVolumetricAvailable() && controller->getProfileManager()->getSelectedProfile().isVolumetric() ? 1 : 0;
        statusDoc["btd"] = profileManager->getSelectedProfile().getTotalDuration();
        statusDoc["led"] = controller->getSystemInfo().capabilities.ledControl;
        statusDoc["gtd"] = controller->getTargetGrindDuration();
        statusDoc["gtv"] = controller->getSettings().getTargetGrindVolume();
        statusDoc["gt"] = controller->isGrindVolumetricAvailable() && controller->getSettings().isVolumetricTarget() ? 1 : 0;
        statusDoc["gact"] = controller->isGrindActive() ? 1 : 0;
        statusDoc["wl"] = controller->getWaterLevel();
        statusDoc["tof"] = controller->getTofDistance();
        statusDoc["rssi"] = 0;
        statusDoc["lat"] = -1; // BLE round-trip latency (ms); -1 = not yet measured

#ifndef GAGGIMATE_UART_COMMS
        if (controller->getClientController()->getClient() != nullptr && controller->getClientController()->getClient()->isConnected()) {
            statusDoc["rssi"] = controller->getClientController()->getClient()->getRssi();
        }
#endif
        if (controller->getClientController()->hasLatency()) {
            statusDoc["lat"] = controller->getClientController()->getLatencyMs();
        }

        const bool bleConnected = BLEScales.isConnected();
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
        const ScaleSample sc = controller->getScaleSample();
        const bool hwScalePresent = controller->isHardwareScalePresent();
        const bool hwHealthy = controller->isHardwareScaleSampleHealthy(sc);

        // During a HW-scale brew, present the shot-relative weight (current −
        // captured baseline) so the WebUI matches the value the brew controller
        // is targeting against. Outside a brew the raw absolute reading is shown.
        const bool useShotRelative = controller->isHardwareScaleShotBaselineActive();
        const float baseline = controller->getHardwareScaleShotBaseline();
        const float displayHwG = (useShotRelative && std::isfinite(sc.weightG))
                                     ? std::max(0.0f, sc.weightG - baseline)
                                     : sc.weightG;

        // 'cw' (currentWeight) mirrors the source the brew controller is acting
        // on (or would, when idle), so the WebUI chart and Process Controls card
        // show the same weight the brew targets against. 'bw' keeps its raw
        // BLE-only meaning.
        const VolumetricMeasurementSource brewSource =
            controller->isActive() ? controller->getCurrentVolumetricSource() : controller->getResolvedBrewSource();
        float cw = 0.0f;
        switch (brewSource) {
        case VolumetricMeasurementSource::HARDWARE_SCALE:
            cw = hwHealthy ? displayHwG : 0.0f;
            break;
        case VolumetricMeasurementSource::BLUETOOTH:
            cw = bleConnected ? this->currentBluetoothWeight : 0.0f;
            break;
        case VolumetricMeasurementSource::FLOW_ESTIMATION:
            cw = this->currentEstimatedWeight;
            break;
        default:
            cw = 0.0f;
            break;
        }

        statusDoc["bw"] = bleConnected ? this->currentBluetoothWeight : 0; // raw BLE weight
        statusDoc["hw"] = hwHealthy ? displayHwG : 0.0f;
        statusDoc["hwc"] = hwScalePresent;
        statusDoc["cw"] = cw;                                              // active-source weight
        statusDoc["bc"] = bleConnected;                                    // bluetooth scale connected status

        // Hardware scale: structured snapshot. Flat hw/hwc stay for existing UI.
        auto sObj = statusDoc["scale"].to<JsonObject>();
        // Never surface an implausible headline weight: an uncalibrated/un-tared
        // controller emits raw counts (tens of thousands of grams). Show 0 when
        // the sample is unhealthy or out of range; raw per-channel c1/c2 below
        // stay unclamped for diagnostics.
        sObj["w"] = (hwHealthy && std::isfinite(displayHwG)) ? displayHwG : 0.0f;
        sObj["sd"] = sc.stddevG;
        sObj["c1"] = sc.ch1G;
        sObj["c2"] = sc.ch2G;
        sObj["sd1"] = sc.ch1StdG;
        sObj["sd2"] = sc.ch2StdG;
        sObj["h"] = sc.healthBits;
        sObj["seq"] = sc.sampleSeq;
        sObj["pr"] = hwScalePresent;
        sObj["bl"] = baseline; // captured shot baseline (0 when no brew active)
#else
        const VolumetricMeasurementSource brewSource =
            controller->isActive() ? controller->getCurrentVolumetricSource() : controller->getResolvedBrewSource();
        float cw = 0.0f;
        switch (brewSource) {
        case VolumetricMeasurementSource::BLUETOOTH:
            cw = bleConnected ? this->currentBluetoothWeight : 0.0f;
            break;
        case VolumetricMeasurementSource::FLOW_ESTIMATION:
            cw = this->currentEstimatedWeight;
            break;
        default:
            cw = 0.0f;
            break;
        }

        statusDoc["bw"] = bleConnected ? this->currentBluetoothWeight : 0; // raw BLE weight
        statusDoc["hw"] = 0.0f;
        statusDoc["hwc"] = false;
        statusDoc["cw"] = cw;                                              // active-source weight
        statusDoc["bc"] = bleConnected;                                    // bluetooth scale connected status
        auto sObj = statusDoc["scale"].to<JsonObject>();
        sObj["w"] = 0.0f;
        sObj["sd"] = 0.0f;
        sObj["c1"] = 0.0f;
        sObj["c2"] = 0.0f;
        sObj["sd1"] = 0.0f;
        sObj["sd2"] = 0.0f;
        sObj["h"] = 0;
        sObj["seq"] = 0;
        sObj["pr"] = false;
        sObj["bl"] = 0.0f;
#endif
        // Scale battery — only surfaced when the driver reports one and the
        // value isn't the UNKNOWN sentinel (255). UI omits the battery pill
        // entirely when `sbat` is absent, so disconnected/unknown scales don't
        // render a stale stub.
        if (bleConnected && BLEScales.hasBatteryLevel()) {
            const uint8_t pct = BLEScales.getBatteryLevel();
            if (pct != REMOTE_SCALES_BATTERY_UNKNOWN) {
                statusDoc["sbat"] = pct;
            }
        }

        // Scale source routing, so the UI can show which source each role uses
        // (enum: 0=none/INACTIVE, 1=predictive, 2=bluetooth, 3=hardware).
        statusDoc["brewSource"] = static_cast<int>(controller->getResolvedBrewSource());
        statusDoc["grindSource"] = static_cast<int>(controller->getResolvedGrindSource());
        statusDoc["activeSource"] = static_cast<int>(controller->getCurrentVolumetricSource());
        statusDoc["scaleCapable"] = controller->scaleAvailability().hardwareCapable;

        Process *process = controller->getProcess();
        if (process == nullptr) {
            process = controller->getLastProcess();
        }
        if (process != nullptr) {
            auto pObj = statusDoc["process"].to<JsonObject>();
            pObj["a"] = controller->isActive() ? 1 : 0;
            if (process->getType() == MODE_BREW) {
                auto *brew = static_cast<BrewProcess *>(process);
                unsigned long ts = brew->isActive() && controller->isActive() ? millis() : brew->finished;
                pObj["s"] = brew->currentPhase.phase == PhaseType::PHASE_TYPE_BREW ? "brew" : "infusion";
                pObj["l"] = brew->isActive() ? brew->currentPhase.name.c_str() : "Finished";
                pObj["e"] = ts - brew->processStarted;
                const bool isVolumetric = brew->target == ProcessTarget::VOLUMETRIC && brew->currentPhase.hasVolumetricTarget() &&
                                          controller->isVolumetricAvailable();
                pObj["tt"] = isVolumetric ? "volumetric" : "time";
                if (isVolumetric) {
                    Target t = brew->currentPhase.getVolumetricTarget();
                    pObj["pt"] = t.value;
                    pObj["pp"] = brew->currentVolume;
                } else {
                    pObj["pt"] = brew->getPhaseDuration();
                    pObj["pp"] = ts - brew->currentPhaseStarted;
                }
            } else if (process->getType() == MODE_GRIND) {
                auto *grind = static_cast<GrindProcess *>(process);
                unsigned long ts = grind->isActive() && controller->isActive() ? millis() : grind->finished;
                pObj["s"] = "grind";
                pObj["l"] = grind->isActive() ? "Grinding" : "Finished";
                pObj["e"] = ts - grind->started;
                const bool isVolumetric = grind->target == ProcessTarget::VOLUMETRIC && controller->isGrindVolumetricAvailable();
                pObj["tt"] = isVolumetric ? "volumetric" : "time";
                if (isVolumetric) {
                    pObj["pt"] = grind->grindVolume;
                    pObj["pp"] = grind->currentVolume;
                } else {
                    pObj["pt"] = grind->time;
                    pObj["pp"] = ts - grind->started;
                }
            }
        }

        // Diagnostics for hang triage: heap usage, largest free block, historical
        // minimum, uptime in seconds. Watch for hl falling while hf stays high
        // (fragmentation) vs steady hf decline (leak).
        statusDoc["hf"] = ESP.getFreeHeap();
        statusDoc["hl"] = ESP.getMaxAllocHeap();
        statusDoc["hm"] = ESP.getMinFreeHeap();
        statusDoc["up"] = millis() / 1000;

        broadcastJson(statusDoc);
    }
    if (now > lastCleanup + CLEANUP_PERIOD) {
        lastCleanup = now;
        ws.cleanupClients();
        // Evict rxBuffers from clients that dropped TCP without a clean WS close
        // (mobile screen-lock, OS killing background tab). Otherwise these leak
        // until reboot.
        const unsigned long nowMs = millis();
        size_t evicted = 0;
        for (auto it = rxBufferLastActivity.begin(); it != rxBufferLastActivity.end();) {
            if (nowMs - it->second > RXBUFFER_IDLE_EVICT_MS) {
                rxBuffers.erase(it->first);
                it = rxBufferLastActivity.erase(it);
                ++evicted;
            } else {
                ++it;
            }
        }
        if (evicted > 0) {
            ESP_LOGI("WebUIPlugin", "Evicted %u idle rxBuffers", static_cast<unsigned>(evicted));
        }
    }
    if (now > lastDns + DNS_PERIOD && dnsServer != nullptr) {
        lastDns = now;
        dnsServer->processNextRequest();
    }
}

void WebUIPlugin::setupServer() {
    server.on("/connecttest.txt", [](AsyncWebServerRequest *request) {
        request->redirect("http://logout.net");
    }); // windows 11 captive portal workaround
    server.on("/wpad.dat", [](AsyncWebServerRequest *request) {
        request->send(404);
    }); // Honestly don't understand what this is but a 404 stops win 10 keep calling this repeatedly and panicking the esp32
        // :)
    server.on("/generate_204",
              [](AsyncWebServerRequest *request) { request->redirect(LOCAL_URL); }); // android captive portal redirect
    server.on("/redirect", [](AsyncWebServerRequest *request) { request->redirect(LOCAL_URL); });            // microsoft redirect
    server.on("/hotspot-detect.html", [](AsyncWebServerRequest *request) { request->redirect(LOCAL_URL); }); // apple call home
    server.on("/library/test/success.html",
              [](AsyncWebServerRequest *request) { request->redirect(LOCAL_URL); }); // apple call home (newer iOS)
    server.on("/canonical.html",
              [](AsyncWebServerRequest *request) { request->redirect(LOCAL_URL); });       // firefox captive portal call home
    server.on("/success.txt", [](AsyncWebServerRequest *request) { request->send(200); }); // firefox captive portal call home
    server.on("/ncsi.txt", [](AsyncWebServerRequest *request) { request->redirect(LOCAL_URL); }); // windows call home
    server.on("/api/settings", [this](AsyncWebServerRequest *request) { handleSettings(request); });
    server.on("/api/status", [this](AsyncWebServerRequest *request) {
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        JsonDocument doc(&psramAllocator);
        doc["mode"] = controller->getMode();
        doc["tt"] = controller->getTargetTemp();
        doc["ct"] = controller->getCurrentTemp();
        serializeJson(doc, *response);
        request->send(response);
    });
    server.on("/api/scales/list", [this](AsyncWebServerRequest *request) { handleBLEScaleList(request); });
    server.on("/api/scales/connect", [this](AsyncWebServerRequest *request) { handleBLEScaleConnect(request); });
    server.on("/api/scales/scan", [this](AsyncWebServerRequest *request) { handleBLEScaleScan(request); });
    server.on("/api/scales/info", [this](AsyncWebServerRequest *request) { handleBLEScaleInfo(request); });
    FS *fs = &LittleFS;
    if (controller->isSDCard()) {
        fs = &SD_MMC;
    }
    server.serveStatic("/api/history/", *fs, "/h/").setCacheControl("no-store");
    server.on("/api/history/index.bin", HTTP_GET, [this, fs](AsyncWebServerRequest *request) {
        // Serve the binary index file directly
        if (fs->exists("/h/index.bin")) {
            request->send(*fs, "/h/index.bin", "application/octet-stream");
        } else {
            request->send(404, "text/plain", "Index not found");
        }
    });
    server.on("/api/core-dump", HTTP_GET, [this](AsyncWebServerRequest *request) { handleCoreDumpDownload(request); });
    server.onNotFound([](AsyncWebServerRequest *request) { request->send(LittleFS, "/w/index.html"); });
    // Content-hashed build assets (Vite emits them under /assets/ with a hash in the filename) never change for a
    // given URL, so let the browser cache them forever and skip the revalidation round-trip entirely. This must be
    // registered before the catch-all "/" handler so it wins for /assets/* requests. [GM-83]
    server.serveStatic("/assets/", LittleFS, "/w/assets/").setCacheControl("public, max-age=31536000, immutable");
    // index.html and other unhashed root files must stay revalidated so a new build (which references freshly
    // hashed assets) is always picked up after an OTA/filesystem update.
    server.serveStatic("/", LittleFS, "/w").setDefaultFile("index.html").setCacheControl("no-cache");
    ws.onEvent(
        [this](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
            if (type == WS_EVT_CONNECT) {
                // Close (and let the browser reconnect) a client whose send
                // queue backs up, instead of keeping it open. With it kept open
                // (false), a client that stalls under load — e.g. while the UI
                // is fetching many shot files for statistics — never has its
                // queued frames / AsyncTCP buffers reclaimed, so they accumulate
                // in internal DRAM until the whole IP stack starves (web + ICMP
                // die, no recovery). Reclaiming via close is the safer failure
                // mode. (Was the v1.8.1 behaviour.)
                client->setCloseClientOnQueueFull(true);
                ESP_LOGI("WebUIPlugin", "WebSocket client connected (%d open connections)", server->getClients().size());
                // Replay pending RL prompts so a reloaded/reconnected client restores
                // its reopen affordance (the WebUI dedupes/minimizes by id).
                if (!_pendRateShotId.isEmpty()) {
                    sendRatingPrompt(client);
                }
                if (!_pendRecJson.isEmpty()) {
                    client->text(_pendRecJson);
                }
            } else if (type == WS_EVT_DISCONNECT) {
                ESP_LOGI("WebUIPlugin", "WebSocket client disconnected (%d open connections)", server->getClients().size());
                rxBuffers.erase(client->id());
            } else if (type == WS_EVT_DATA) {
                handleWebSocketData(server, client, type, arg, data, len);
            }
        });
    server.addHandler(&ws);
}

void WebUIPlugin::start() {
    if (serverRunning) {
        // Already listening. The 0.0.0.0:80 listen socket survives a WiFi
        // reconnect, so re-running end()+begin() only races AsyncTCP's async
        // socket close and fails to rebind ("bind: -8, port in use"). A transient
        // STA reconnect needs nothing done here.
        return;
    }
    server.begin();
    ESP_LOGI("WebUIPlugin", "Started webserver");
    if (apMode) {
        dnsServer = new DNSServer();
        dnsServer->setTTL(3600);
        dnsServer->start(53, "*", WIFI_AP_IP);
        ESP_LOGI("WebUIPlugin", "Started catchall DNS for captive portal");
    }
    lastUpdateCheck = millis();
    serverRunning = true;
}

void WebUIPlugin::stop() {
    if (!serverRunning)
        return;
    ws.closeAll();
    server.end();
    if (dnsServer != nullptr) {
        dnsServer->stop();
        delete dnsServer;
        dnsServer = nullptr;
    }
    serverRunning = false;
    ESP_LOGI("WebUIPlugin", "WebUIPlugin stopped (wifi disconnected)");
}

void WebUIPlugin::sendRatingPrompt(AsyncWebSocketClient *client) {
    if (_pendRateShotId.isEmpty()) {
        return;
    }
    JsonDocument doc;
    doc["tp"] = "evt:rl:shot-complete";
    doc["shot_id"] = _pendRateShotId;
    doc["recommendation_id"] = _pendRateRecId;
    const String payload = doc.as<String>();
    if (client) {
        client->text(payload);
    } else {
        ws.textAll(payload);
    }
}

void WebUIPlugin::handleWebSocketData(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg,
                                      uint8_t *data, size_t len) {

    auto *info = static_cast<AwsFrameInfo *>(arg);
    const uint32_t cid = client->id();

    rxBufferLastActivity[cid] = millis();

    if (info->index == 0) {
        auto &buf = rxBuffers[cid];
        buf.clear();
        if (info->len <= 64 * 1024) {
            buf.reserve(info->len);
        }
    }

    auto &buf = rxBuffers[cid];
    buf.append(reinterpret_cast<const char *>(data), len);
    const bool isFinal = info->final && (info->index + len) == info->len;

    // If this is the final frame of the message, process and clear
    if (isFinal) {
        if (info->opcode == WS_TEXT) {
            ESP_LOGV("WebUIPlugin", "Received request: %.*s", (int)buf.size(), buf.c_str());
            JsonDocument doc(&psramAllocator);
            DeserializationError err = deserializeJson(doc, buf.c_str());
            if (!err) {
                String msgType = doc["tp"].as<String>();
                if (msgType.startsWith("req:profiles:")) {
                    handleProfileRequest(client->id(), doc);
                } else if (msgType == "req:ota-settings") {
                    handleOTASettings(client->id(), doc);
                } else if (msgType == "req:ota-start") {
                    handleOTAStart(client->id(), doc);
                } else if (msgType == "req:autotune-start") {
                    handleAutotuneStart(client->id(), doc);
                } else if (msgType == "req:rl:context:list" || msgType == "req:rl:context:switch" ||
                           msgType == "req:rl:context:start-bean" || msgType == "req:rl:context:start-bag" ||
                           msgType == "req:rl:context:retire" || msgType == "req:rl:context:reset" ||
                           msgType == "req:rl:local-optimization" || msgType == "req:rl:optimization:pause" ||
                           msgType == "req:rl:optimization:resume") {
                    handleRLRequest(client->id(), doc);
                } else if (msgType == "req:rl:recommendation:use") {
                    if (controller->getSettings().isHomeAssistant() && controller->getSettings().isRLRatingEnabled()) {
                        String recommendationId = doc["recommendation_id"].as<String>();
                        if (!recommendationId.isEmpty()) {
                            Event event;
                            event.id = "rl:recommendation:apply";
                            event.setString("recommendation_id", recommendationId);
                            pluginManager->trigger(event);
                            _pendRecJson = ""; // resolved — drop the reopen affordance
                        }
                    }
                } else if (msgType == "req:rl:recommendation:ignore") {
                    if (controller->getSettings().isHomeAssistant() && controller->getSettings().isRLRatingEnabled()) {
                        String recommendationId = doc["recommendation_id"].as<String>();
                        if (!recommendationId.isEmpty()) {
                            Event event;
                            event.id = "rl:recommendation:ignore";
                            event.setString("recommendation_id", recommendationId);
                            pluginManager->trigger(event);
                            _pendRecJson = ""; // resolved — drop the reopen affordance
                        }
                    }
                } else if (msgType == "req:rl:shot:correction") {
                    JsonDocument resp;
                    resp["tp"] = "res:rl:shot:correction";
                    resp["rid"] = doc["rid"];
                    if (!controller->getSettings().isHomeAssistant() || !controller->getSettings().isRLRatingEnabled()) {
                        resp["error"] = F("Auto Tuning is disabled");
                    } else {
                        String shotId = doc["shot_id"].as<String>();
                        if (shotId.isEmpty()) {
                            shotId = rlLastShotId;
                        }
                        if (shotId.isEmpty()) {
                            resp["error"] = F("No shot available to correct");
                        } else {
                            Event event;
                            event.id = "rl:shot:correction";
                            event.setString("shot_id", shotId);
                            event.setString("source", "gaggimate_webui");
                            if (doc["exclude_from_local_optimization"].is<bool>()) {
                                event.setInt("has_exclude_from_local_optimization", 1);
                                event.setInt("exclude_from_local_optimization",
                                             doc["exclude_from_local_optimization"].as<bool>() ? 1 : 0);
                            }
                            if (doc["shot_type"].is<String>()) {
                                event.setString("shot_type", doc["shot_type"].as<String>());
                            }
                            if (doc["grind_followed"].is<bool>()) {
                                event.setInt("has_grind_followed", 1);
                                event.setInt("grind_followed", doc["grind_followed"].as<bool>() ? 1 : 0);
                            }
                            if (doc["dose_followed"].is<bool>()) {
                                event.setInt("has_dose_followed", 1);
                                event.setInt("dose_followed", doc["dose_followed"].as<bool>() ? 1 : 0);
                            }
                            if (doc["yield_followed"].is<bool>()) {
                                event.setInt("has_yield_followed", 1);
                                event.setInt("yield_followed", doc["yield_followed"].as<bool>() ? 1 : 0);
                            }
                            if (doc["correction_tags"].is<JsonArray>()) {
                                String tags;
                                for (JsonVariant tag : doc["correction_tags"].as<JsonArray>()) {
                                    String value = tag.as<String>();
                                    value.trim();
                                    if (value.isEmpty()) {
                                        continue;
                                    }
                                    if (!tags.isEmpty()) {
                                        tags += ",";
                                    }
                                    tags += value;
                                }
                                event.setString("correction_tags", tags);
                            }
                            pluginManager->trigger(event);
                            resp["success"] = true;
                            resp["shot_id"] = shotId;
                        }
                    }
                    String msg;
                    serializeJson(resp, msg);
                    client->text(msg);
                } else if (msgType == "req:rl:upload:requeue") {
                    JsonDocument resp;
                    resp["tp"] = "res:rl:upload:requeue";
                    resp["rid"] = doc["rid"];
                    if (!controller->getSettings().isHomeAssistant() || !controller->getSettings().isRLRatingEnabled()) {
                        resp["error"] = F("Auto Tuning is disabled");
                    } else {
                        Event event;
                        event.id = "rl:upload:requeue";
                        event.setString("source", "gaggimate_webui");
                        event.setInt("limit", doc["limit"] | 50);
                        pluginManager->trigger(event);
                        resp["success"] = true;
                    }
                    String msg;
                    serializeJson(resp, msg);
                    client->text(msg);
                } else if (msgType == "req:rl:rating") {
                    if (controller->getSettings().isHomeAssistant() && controller->getSettings().isRLRatingEnabled()) {
                        String shotId = doc["shot_id"].as<String>();
                        String recommendationId = doc["recommendation_id"].as<String>();
                        const bool skipped = doc["skipped"] | false;
                        const bool hasRating = doc["rating"].is<int>();
                        const int rating = hasRating ? doc["rating"].as<int>() : 0;
                        if (!shotId.isEmpty() && (skipped || (hasRating && rating >= 1 && rating <= 5))) {
                            Event event;
                            event.id = "rl:rating";
                            event.setString("shot_id", shotId);
                            event.setString("recommendation_id", recommendationId);
                            if (hasRating) {
                                event.setInt("rating", rating);
                            }
                            if (skipped) {
                                event.setInt("skipped", 1);
                            }
                            if (doc["taste_tags"].is<JsonArray>()) {
                                String tasteTags;
                                for (JsonVariant tag : doc["taste_tags"].as<JsonArray>()) {
                                    String value = tag.as<String>();
                                    value.trim();
                                    if (value.isEmpty()) {
                                        continue;
                                    }
                                    if (!tasteTags.isEmpty()) {
                                        tasteTags += ",";
                                    }
                                    tasteTags += value;
                                }
                                event.setString("taste_tags", tasteTags);
                            }
                            pluginManager->trigger(event);
                            _pendRateShotId = ""; // rated/skipped — drop the reopen affordance
                            _pendRateRecId = "";
                        }
                    }
                } else if (msgType == "req:process:activate") {
                    controller->postCommand(CtrlCmd::ACTIVATE);
                } else if (msgType == "req:process:deactivate") {
                    controller->postCommand(CtrlCmd::DEACTIVATE);
                    controller->postCommand(CtrlCmd::CLEAR);
                } else if (msgType == "req:process:clear") {
                    controller->postCommand(CtrlCmd::CLEAR);
                } else if (msgType == "req:grind:activate") {
                    controller->postCommand(CtrlCmd::ACTIVATE_GRIND);
                } else if (msgType == "req:grind:deactivate") {
                    controller->postCommand(CtrlCmd::DEACTIVATE_GRIND);
                } else if (msgType == "req:change-grind-target") {
                    if (doc["target"].is<uint8_t>()) {
                        auto target = doc["target"].as<uint8_t>();
                        controller->getSettings().setVolumetricTarget(target);
                    }
                } else if (msgType == "req:raise-temp") {
                    controller->postCommand(CtrlCmd::RAISE_TEMP);
                } else if (msgType == "req:lower-temp") {
                    controller->postCommand(CtrlCmd::LOWER_TEMP);
                } else if (msgType == "req:raise-grind-target") {
                    controller->postCommand(CtrlCmd::RAISE_GRIND_TARGET);
                } else if (msgType == "req:lower-grind-target") {
                    controller->postCommand(CtrlCmd::LOWER_GRIND_TARGET);
                } else if (msgType == "req:change-mode") {
                    if (doc["mode"].is<uint8_t>()) {
                        auto mode = doc["mode"].as<uint8_t>();
                        controller->postCommand(CtrlCmd::DEACTIVATE);
                        controller->postCommand(CtrlCmd::CLEAR);
                        controller->postCommand(CtrlCmd::SET_MODE, mode);
                    }
                } else if (msgType == "req:change-brew-target") {
                    if (doc["target"].is<uint8_t>()) {
                        auto target = doc["target"].as<uint8_t>();
                        controller->getSettings().setVolumetricTarget(target);
                    }
                } else if (msgType == "req:history:rebuild") {
                    // Handle rebuild asynchronously - send immediate ack, progress comes via events
                    JsonDocument resp(&psramAllocator);
                    resp["tp"] = "res:history:rebuild";
                    if (doc["rid"].is<const char *>()) {
                        resp["rid"] = doc["rid"];
                    }
                    resp["msg"] = "Rebuild started";
                    size_t bufferSize = measureJson(resp);
                    auto *buffer = ws.makeBuffer(bufferSize);
                    serializeJson(resp, buffer->get(), bufferSize);
                    client->text(buffer);
                    ShotHistory.startAsyncRebuild();
                } else if (msgType.startsWith("req:history")) {
                    JsonDocument resp(&psramAllocator);
                    ShotHistory.handleRequest(doc, resp);
                    size_t bufferSize = measureJson(resp);
                    auto *buffer = ws.makeBuffer(bufferSize);
                    serializeJson(resp, buffer->get(), bufferSize);
                    client->text(buffer);
                } else if (msgType == "req:flush:start") {
                    handleFlushStart(client->id(), doc);
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
                } else if (msgType == "req:scale:tare") {
                    controller->scaleTare();
                } else if (msgType == "req:scale:cal:start") {
                    uint8_t channel = doc["channel"] | 0;
                    float refWeight = doc["refWeight"] | 0.0f;
                    if ((channel == 1 || channel == 2) && refWeight > 0.0f) {
                        controller->getClientController()->startScaleCalibration(channel, refWeight);
                    }
#endif
                }
            }
        }
        // Done with this message
        rxBuffers.erase(cid);
        rxBufferLastActivity.erase(cid);
    }
}

void WebUIPlugin::handleOTASettings(uint32_t clientId, JsonDocument &request) {
    if (request["update"].as<bool>()) {
        if (!request["channel"].isNull()) {
            controller->getSettings().setOTAChannel(request["channel"].as<String>() == "latest" ? "latest" : "nightly");
            ota->setReleaseUrl(RELEASE_URL + (controller->getSettings().getOTAChannel() == "latest" ? "latest" : "tag/nightly"));
            lastUpdateCheck = 0;
        }
    }
    updateOTAStatus("Checking...");
}

void WebUIPlugin::handleOTAStart(uint32_t clientId, JsonDocument &request) {
    updating = true;
    if (request["cp"].is<String>()) {
        updateComponent = request["cp"].as<String>();
    } else {
        updateComponent = "";
    }
}

void WebUIPlugin::handleAutotuneStart(uint32_t clientId, JsonDocument &request) {
    int testTime = request["time"].as<int>();
    int samples = request["samples"].as<int>();
    // Heater wattage drives combinedKff = TUNER_OUTPUT_SPAN / wattage on the
    // controller. 0 = "skip combinedKff derivation" — happens when older Web
    // UI builds omit the field. WebUI form default is 680 W (Gaggia Classic
    // Pro 2019 / E24, 230 V boiler).
    int heaterWattage = request["wattage"] | 0;
    controller->autotune(testTime, samples, heaterWattage);
}

void WebUIPlugin::handleProfileRequest(uint32_t clientId, JsonDocument &request) {
    // Allocate the response node pool from PSRAM — list responses can be tens
    // of KB and would otherwise fragment the ~300 KB internal heap.
    JsonDocument response(&psramAllocator);
    auto type = request["tp"].as<String>();
    ESP_LOGI("WebUIPlugin", "Handling request: %s", type.c_str());
    response["tp"] = String("res:") + type.substring(4);
    response["rid"] = request["rid"].as<String>();

    if (type == "req:profiles:list") {
        auto arr = response["profiles"].to<JsonArray>();
        for (auto const &id : profileManager->listProfiles()) {
            Profile profile{};
            // Skip entries whose JSON couldn't be opened or failed validation
            // (parseProfile returns false for missing label/type/phases). Without
            // this, corrupt or partial profile files surface as blank cards in
            // the UI — the user reported "blank Simple cards" originating here.
            if (!profileManager->loadProfile(id, profile)) {
                ESP_LOGW("WebUIPlugin", "Skipping unreadable profile %s in list response", id.c_str());
                continue;
            }
            auto p = arr.add<JsonObject>();
            if (request["minimal"].as<bool>()) {
                p["id"] = profile.id;
                p["label"] = profile.label;
            } else {
                writeProfile(p, profile);
            }
        }
    } else if (type == "req:profiles:load") {
        auto id = request["id"].as<String>();
        Profile profile;
        if (profileManager->loadProfile(id, profile)) {
            auto obj = response["profile"].to<JsonObject>();
            writeProfile(obj, profile);
        } else {
            response["error"] = F("Profile not found");
        }
    } else if (type == "req:profiles:save") {
        auto obj = request["profile"].as<JsonObject>();
        Profile profile;
        parseProfile(obj, profile);
        if (!profileManager->saveProfile(profile)) {
            response["error"] = F("Save failed");
        }
        auto respObj = response["profile"].to<JsonObject>();
        writeProfile(respObj, profile);
    } else if (type == "req:profiles:delete") {
        auto id = request["id"].as<String>();
        if (!profileManager->deleteProfile(id)) {
            response["error"] = F("Delete failed");
        }
    } else if (type == "req:profiles:select") {
        auto id = request["id"].as<String>();
        profileManager->selectProfile(id);
    } else if (type == "req:profiles:favorite") {
        auto id = request["id"].as<String>();
        profileManager->addFavoritedProfile(id);
    } else if (type == "req:profiles:unfavorite") {
        auto id = request["id"].as<String>();
        profileManager->removeFavoritedProfile(id);
    } else if (type == "req:profiles:reorder") {
        // Expect an array of profile IDs in desired order
        if (request["order"].is<JsonArray>()) {
            std::vector<String> order;
            for (JsonVariant v : request["order"].as<JsonArray>()) {
                if (v.is<String>()) {
                    String id = v.as<String>();
                    if (!id.isEmpty() && std::find(order.begin(), order.end(), id) == order.end()) {
                        order.emplace_back(std::move(id));
                    }
                }
            }
            controller->getSettings().setProfileOrder(order);
        }
    }

    size_t bufferSize = measureJson(response);
    auto *buffer = ws.makeBuffer(bufferSize);
    serializeJson(response, buffer->get(), bufferSize);
    ws.text(clientId, buffer);
}

void WebUIPlugin::handleRLRequest(uint32_t clientId, JsonDocument &request) {
    JsonDocument response;
    auto type = request["tp"].as<String>();
    response["tp"] = String("res:") + type.substring(4);
    response["rid"] = request["rid"].as<String>();

    Settings &settings = controller->getSettings();
    if (!settings.isHomeAssistant() || !settings.isRLRatingEnabled()) {
        response["error"] = F("Auto Tuning is disabled");
    } else {
        JsonDocument contextsDoc;
        JsonArray contexts = loadRLContexts(contextsDoc, settings.getRLBeanContextsJson());

        if (type == "req:rl:context:start-bean") {
            String name = request["name"].as<String>();
            name.trim();
            if (name.isEmpty()) {
                name = defaultRLContextName();
            }
            JsonObject current = findRLContext(contexts, settings.getRLBeanContextId());
            if (!current.isNull()) {
                current["status"] = "retired";
                current["retired_at"] = static_cast<long>(std::time(nullptr));
            }
            const int bagIndex = currentBagIndex(contexts, name) + 1;
            const String id = makeRLContextId(name);
            markRLOtherContextsAvailable(contexts, id);
            addRLContext(contexts, id, name, bagIndex, "active");
            persistRLContexts(settings, contextsDoc);
            settings.setRLBeanContextId(id);
            settings.setRLBeanContextName(name);
            settings.setRLOptimizationPaused(false);
            settings.setRLLocalOptimizationEnabled(true);
            settings.save(true);
        } else if (type == "req:rl:context:start-bag") {
            String name = settings.getRLBeanContextName();
            if (name.isEmpty()) {
                name = defaultRLContextName();
            }
            JsonObject current = findRLContext(contexts, settings.getRLBeanContextId());
            if (!current.isNull()) {
                current["status"] = "retired";
                current["retired_at"] = static_cast<long>(std::time(nullptr));
            }
            const int bagIndex = currentBagIndex(contexts, name) + 1;
            const String id = makeRLContextId(name);
            markRLOtherContextsAvailable(contexts, id);
            addRLContext(contexts, id, name, bagIndex, "active");
            persistRLContexts(settings, contextsDoc);
            settings.setRLBeanContextId(id);
            settings.setRLBeanContextName(name);
            settings.setRLOptimizationPaused(false);
            settings.setRLLocalOptimizationEnabled(true);
            settings.save(true);
        } else if (type == "req:rl:context:switch") {
            const String id = request["id"].as<String>();
            JsonObject context = findRLContext(contexts, id);
            if (context.isNull()) {
                response["error"] = F("Context not found");
            } else {
                markRLOtherContextsAvailable(contexts, id);
                context["status"] = "active";
                settings.setRLBeanContextId(id);
                settings.setRLBeanContextName(context["name"].as<String>());
                persistRLContexts(settings, contextsDoc);
                settings.save(true);
            }
        } else if (type == "req:rl:context:retire") {
            String id = request["id"].as<String>();
            if (id.isEmpty()) {
                id = settings.getRLBeanContextId();
            }
            JsonObject context = findRLContext(contexts, id);
            if (!context.isNull()) {
                context["status"] = "retired";
                context["retired_at"] = static_cast<long>(std::time(nullptr));
                persistRLContexts(settings, contextsDoc);
            }
            if (settings.getRLBeanContextId() == id) {
                settings.setRLBeanContextId("");
                settings.setRLBeanContextName("");
                settings.setRLLocalOptimizationEnabled(false);
            }
            settings.save(true);
        } else if (type == "req:rl:context:reset") {
            String name = settings.getRLBeanContextName();
            if (name.isEmpty()) {
                name = defaultRLContextName();
            }
            JsonObject current = findRLContext(contexts, settings.getRLBeanContextId());
            if (!current.isNull()) {
                current["status"] = "retired";
                current["retired_at"] = static_cast<long>(std::time(nullptr));
            }
            const int bagIndex = currentBagIndex(contexts, name) + 1;
            const String id = makeRLContextId(name + "_reset");
            markRLOtherContextsAvailable(contexts, id);
            addRLContext(contexts, id, name, bagIndex, "active");
            persistRLContexts(settings, contextsDoc);
            settings.setRLBeanContextId(id);
            settings.setRLBeanContextName(name);
            settings.setRLOptimizationPaused(false);
            settings.setRLLocalOptimizationEnabled(true);
            settings.save(true);
        } else if (type == "req:rl:local-optimization") {
            settings.setRLLocalOptimizationEnabled(request["enabled"] | true);
            settings.save(true);
        } else if (type == "req:rl:optimization:pause") {
            settings.setRLOptimizationPaused(true);
            settings.save(true);
        } else if (type == "req:rl:optimization:resume") {
            settings.setRLOptimizationPaused(false);
            settings.setRLLocalOptimizationEnabled(true);
            settings.save(true);
        }

        JsonArray arr = response["contexts"].to<JsonArray>();
        for (JsonObject context : contexts) {
            JsonObject out = arr.add<JsonObject>();
            out["id"] = context["id"].as<String>();
            out["name"] = context["name"].as<String>();
            out["bag_index"] = context["bag_index"] | 1;
            out["status"] = context["status"].as<String>();
            out["active"] = context["id"].as<String>() == settings.getRLBeanContextId();
        }
        response["active_context_id"] = settings.getRLBeanContextId();
        response["active_context_name"] = settings.getRLBeanContextName();
        response["local_optimization_enabled"] =
            settings.isRLLocalOptimizationEnabled() && !settings.isRLOptimizationPaused() &&
            !settings.getRLBeanContextId().isEmpty();
        response["optimization_paused"] = settings.isRLOptimizationPaused();
    }

    size_t bufferSize = measureJson(response);
    auto *buffer = ws.makeBuffer(bufferSize);
    serializeJson(response, buffer->get(), bufferSize);
    ws.text(clientId, buffer);
}

void WebUIPlugin::handleSettings(AsyncWebServerRequest *request) const {
    if (request->method() == HTTP_POST) {
        controller->getSettings().batchUpdate([request](Settings *settings) {
            if (request->hasArg("startupMode"))
                settings->setStartupMode(request->arg("startupMode") == "brew" ? MODE_BREW : MODE_STANDBY);
            if (request->hasArg("startupProfile"))
                settings->setStartupProfile(request->arg("startupProfile"));
            if (request->hasArg("targetSteamTemp"))
                settings->setTargetSteamTemp(request->arg("targetSteamTemp").toInt());
            if (request->hasArg("targetWaterTemp"))
                settings->setTargetWaterTemp(request->arg("targetWaterTemp").toInt());
            if (request->hasArg("temperatureOffset"))
                settings->setTemperatureOffset(request->arg("temperatureOffset").toInt());
            if (request->hasArg("pressureScaling"))
                settings->setPressureScaling(request->arg("pressureScaling").toFloat());
            if (request->hasArg("pid"))
                settings->setPid(request->arg("pid"));
            if (request->hasArg("pumpModelCoeffs"))
                settings->setPumpModelCoeffs(request->arg("pumpModelCoeffs"));
            if (request->hasArg("wifiSsid"))
                settings->setWifiSsid(request->arg("wifiSsid"));
            if (request->hasArg("mdnsName"))
                settings->setMdnsName(request->arg("mdnsName"));
            if (request->hasArg("wifiPassword") && request->arg("wifiPassword") != "---unchanged---")
                settings->setWifiPassword(request->arg("wifiPassword"));
            settings->setHomekit(request->hasArg("homekit"));
            settings->setBoilerFillActive(request->hasArg("boilerFillActive"));
            if (request->hasArg("startupFillTime"))
                settings->setStartupFillTime(request->arg("startupFillTime").toInt() * 1000);
            if (request->hasArg("steamFillTime"))
                settings->setSteamFillTime(request->arg("steamFillTime").toInt() * 1000);
            settings->setSmartGrindActive(request->hasArg("smartGrindActive"));
            if (request->hasArg("smartGrindIp"))
                settings->setSmartGrindIp(request->arg("smartGrindIp"));
            if (request->hasArg("smartGrindMode"))
                settings->setSmartGrindMode(request->arg("smartGrindMode").toInt());
            const bool homeAssistantEnabled = request->hasArg("homeAssistant");
            settings->setHomeAssistant(homeAssistantEnabled);
            settings->setRLRatingEnabled(homeAssistantEnabled && request->hasArg("rlRatingEnabled"));
            if (!homeAssistantEnabled || !request->hasArg("rlRatingEnabled")) {
                settings->setRLLocalOptimizationEnabled(false);
            }
            if (request->hasArg("haUser"))
                settings->setHomeAssistantUser(request->arg("haUser"));
            if (request->hasArg("haPassword"))
                settings->setHomeAssistantPassword(request->arg("haPassword"));
            if (request->hasArg("haIP"))
                settings->setHomeAssistantIP(request->arg("haIP"));
            if (request->hasArg("haPort"))
                settings->setHomeAssistantPort(request->arg("haPort").toInt());
            if (request->hasArg("haTopic"))
                settings->setHomeAssistantTopic(request->arg("haTopic"));
            settings->setMomentaryButtons(request->hasArg("momentaryButtons"));
            settings->setDelayAdjust(request->hasArg("delayAdjust"));
            if (request->hasArg("brewDelay"))
                settings->setBrewDelay(request->arg("brewDelay").toDouble());
            if (request->hasArg("grindDelay"))
                settings->setGrindDelay(request->arg("grindDelay").toDouble());
            if (request->hasArg("timezone"))
                settings->setTimezone(request->arg("timezone"));
            settings->setClockFormat(request->hasArg("clock24hFormat"));
            if (request->hasArg("standbyTimeout"))
                settings->setStandbyTimeout(request->arg("standbyTimeout").toInt() * 1000);
            if (request->hasArg("mainBrightness"))
                settings->setMainBrightness(request->arg("mainBrightness").toInt());
            if (request->hasArg("standbyBrightness"))
                settings->setStandbyBrightness(request->arg("standbyBrightness").toInt());
            if (request->hasArg("standbyBrightnessTimeout"))
                settings->setStandbyBrightnessTimeout(request->arg("standbyBrightnessTimeout").toInt() * 1000);
            if (request->hasArg("steamPumpPercentage"))
                settings->setSteamPumpPercentage(request->arg("steamPumpPercentage").toFloat());
            if (request->hasArg("steamPumpCutoff"))
                settings->setSteamPumpCutoff(request->arg("steamPumpCutoff").toFloat());
            if (request->hasArg("themeMode"))
                settings->setThemeMode(request->arg("themeMode").toInt());
            if (request->hasArg("sunriseIdle"))
                settings->setSunriseIdle(request->arg("sunriseIdle"));
            if (request->hasArg("sunriseActive"))
                settings->setSunriseActive(request->arg("sunriseActive"));
            if (request->hasArg("sunriseFinished"))
                settings->setSunriseFinished(request->arg("sunriseFinished"));
            if (request->hasArg("sunriseError"))
                settings->setSunriseError(request->arg("sunriseError"));
            if (request->hasArg("sunriseExtBrightness"))
                settings->setSunriseExtBrightness(request->arg("sunriseExtBrightness").toInt());
            if (request->hasArg("emptyTankDistance"))
                settings->setEmptyTankDistance(request->arg("emptyTankDistance").toInt());
            if (request->hasArg("fullTankDistance"))
                settings->setFullTankDistance(request->arg("fullTankDistance").toInt());
            if (request->hasArg("altRelayFunction"))
                settings->setAltRelayFunction(request->arg("altRelayFunction").toInt());
            if (request->hasArg("buttonBehavior"))
                settings->setButtonBehaviorList(explode(request->arg("buttonBehavior"), ','));
            if (request->hasArg("scaleSource"))
                settings->setScaleSource(request->arg("scaleSource").toInt());
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
            if (request->hasArg("scaleCalibration1"))
                settings->setScaleCalibration1(request->arg("scaleCalibration1").toFloat());
            if (request->hasArg("scaleCalibration2"))
                settings->setScaleCalibration2(request->arg("scaleCalibration2").toFloat());
            if (request->hasArg("scaleOffset1"))
                settings->setScaleOffset1(request->arg("scaleOffset1").toInt());
            if (request->hasArg("scaleOffset2"))
                settings->setScaleOffset2(request->arg("scaleOffset2").toInt());
#endif
            settings->setAutoWakeupEnabled(request->hasArg("autowakeupEnabled"));
            if (request->hasArg("autowakeupSchedules")) {
                // Handle schedule format with days
                String schedulesStr = request->arg("autowakeupSchedules");
                std::vector<AutoWakeupSchedule> schedules;

                if (schedulesStr.length() > 0) {
                    // Split semicolon-separated schedules
                    int start = 0;
                    int end = schedulesStr.indexOf(';');

                    while (end != -1 || start < schedulesStr.length()) {
                        String scheduleStr = (end != -1) ? schedulesStr.substring(start, end) : schedulesStr.substring(start);

                        int pipePos = scheduleStr.indexOf('|');
                        if (pipePos != -1) {
                            String timeStr = scheduleStr.substring(0, pipePos);
                            String daysStr = scheduleStr.substring(pipePos + 1);

                            AutoWakeupSchedule schedule;
                            schedule.time = timeStr;

                            if (daysStr.length() == 7) {
                                for (int i = 0; i < 7; i++) {
                                    schedule.days[i] = (daysStr.charAt(i) == '1');
                                }
                            }

                            schedules.push_back(schedule);
                        }

                        if (end == -1)
                            break;
                        start = end + 1;
                        end = schedulesStr.indexOf(';', start);
                    }
                }

                if (schedules.empty()) {
                    schedules.push_back(AutoWakeupSchedule("07:00")); // Default fallback
                }
                settings->setAutoWakeupSchedules(schedules);
            }
            settings->save(true);
        });
        pluginManager->trigger("settings:changed");
        controller->setTargetTemp(controller->getTargetTemp());
        controller->setPumpModelCoeffs();
    }

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    JsonDocument doc(&psramAllocator);
    Settings const &settings = controller->getSettings();
    doc["startupMode"] = settings.getStartupMode() == MODE_BREW ? "brew" : "standby";
    doc["startupProfile"] = settings.getStartupProfile();
    doc["targetSteamTemp"] = settings.getTargetSteamTemp();
    doc["targetWaterTemp"] = settings.getTargetWaterTemp();
    doc["homekit"] = settings.isHomekit();
    doc["rlRatingEnabled"] = settings.isHomeAssistant() && settings.isRLRatingEnabled();
    doc["rlBeanContextId"] = settings.getRLBeanContextId();
    doc["rlBeanContextName"] = settings.getRLBeanContextName();
    doc["rlOptimizationPaused"] = settings.isRLOptimizationPaused();
    doc["rlLocalOptimizationEnabled"] =
        settings.isHomeAssistant() && settings.isRLRatingEnabled() && settings.isRLLocalOptimizationEnabled() &&
        !settings.isRLOptimizationPaused() && !settings.getRLBeanContextId().isEmpty();
    JsonDocument contextsDoc;
    JsonArray contexts = loadRLContexts(contextsDoc, settings.getRLBeanContextsJson());
    JsonArray contextsOut = doc["rlBeanContexts"].to<JsonArray>();
    for (JsonObject context : contexts) {
        JsonObject out = contextsOut.add<JsonObject>();
        out["id"] = context["id"].as<String>();
        out["name"] = context["name"].as<String>();
        out["bag_index"] = context["bag_index"] | 1;
        out["status"] = context["status"].as<String>();
        out["active"] = context["id"].as<String>() == settings.getRLBeanContextId();
    }
    doc["rlStatusSeen"] = rlStatusSeen;
    doc["rlAddonOnline"] = rlAddonOnline;
    doc["rlLastStatusAt"] = rlLastStatusAt;
    doc["rlLastShotId"] = rlLastShotId;
    doc["rlLastShotAt"] = rlLastShotAt;
    doc["rlLastRecommendationId"] = rlLastRecommendationId;
    doc["rlLastRecommendationAt"] = rlLastRecommendationAt;
    doc["rlRecommendationApplyStatus"] = rlRecommendationApplyStatus;
    doc["rlMode"] = rlMode;
    doc["rlLocalShotCount"] = rlLocalShotCount;
    doc["rlRatedShotCount"] = rlRatedShotCount;
    doc["rlUploadQueueCount"] = rlUploadQueueCount;
    doc["rlUploadQueueRejectedCount"] = rlUploadQueueRejectedCount;
    doc["rlUploadQueueLastRejectedId"] = rlUploadQueueLastRejectedId;
    doc["rlUploadQueueLastRejectedRecordId"] = rlUploadQueueLastRejectedRecordId;
    doc["rlUploadQueueLastRejectedError"] = rlUploadQueueLastRejectedError;
    doc["rlCommunityUploadEnabled"] = rlCommunityUploadEnabled;
    doc["rlBestKnownRecipe"] = rlBestKnownRecipe;
    doc["homeAssistant"] = settings.isHomeAssistant();
    doc["haUser"] = settings.getHomeAssistantUser();
    doc["haPassword"] = settings.getHomeAssistantPassword();
    doc["haIP"] = settings.getHomeAssistantIP();
    doc["haPort"] = settings.getHomeAssistantPort();
    doc["haTopic"] = settings.getHomeAssistantTopic();
    doc["pid"] = settings.getPid();
    doc["pumpModelCoeffs"] = settings.getPumpModelCoeffs();
    doc["wifiSsid"] = settings.getWifiSsid();
    doc["wifiPassword"] = apMode ? "---unchanged---" : settings.getWifiPassword();
    doc["mdnsName"] = settings.getMdnsName();
    doc["temperatureOffset"] = String(settings.getTemperatureOffset());
    doc["pressureScaling"] = String(settings.getPressureScaling());
    doc["boilerFillActive"] = settings.isBoilerFillActive();
    doc["startupFillTime"] = settings.getStartupFillTime() / 1000;
    doc["steamFillTime"] = settings.getSteamFillTime() / 1000;
    doc["smartGrindActive"] = settings.isSmartGrindActive();
    doc["smartGrindIp"] = settings.getSmartGrindIp();
    doc["smartGrindMode"] = settings.getSmartGrindMode();
    doc["momentaryButtons"] = settings.isMomentaryButtons();
    doc["brewDelay"] = settings.getBrewDelay();
    doc["grindDelay"] = settings.getGrindDelay();
    doc["delayAdjust"] = settings.isDelayAdjust();
    doc["timezone"] = settings.getTimezone();
    doc["clock24hFormat"] = settings.isClock24hFormat();
    doc["standbyTimeout"] = settings.getStandbyTimeout() / 1000;
    doc["mainBrightness"] = settings.getMainBrightness();
    doc["standbyBrightness"] = settings.getStandbyBrightness();
    doc["standbyBrightnessTimeout"] = settings.getStandbyBrightnessTimeout() / 1000;
    doc["steamPumpPercentage"] = settings.getSteamPumpPercentage();
    doc["steamPumpCutoff"] = settings.getSteamPumpCutoff();
    doc["themeMode"] = settings.getThemeMode();
    doc["sunriseIdle"] = settings.getSunriseIdle();
    doc["sunriseActive"] = settings.getSunriseActive();
    doc["sunriseFinished"] = settings.getSunriseFinished();
    doc["sunriseError"] = settings.getSunriseError();
    doc["sunriseExtBrightness"] = settings.getSunriseExtBrightness();
    doc["emptyTankDistance"] = settings.getEmptyTankDistance();
    doc["fullTankDistance"] = settings.getFullTankDistance();
    doc["altRelayFunction"] = settings.getAltRelayFunction();
    doc["scaleSource"] = settings.getScaleSource();
    doc["scaleCalibration1"] = settings.getScaleCalibration1();
    doc["scaleCalibration2"] = settings.getScaleCalibration2();
    doc["scaleOffset1"] = settings.getScaleOffset1();
    doc["scaleOffset2"] = settings.getScaleOffset2();
    doc["scaleCalTimestamp1"] = settings.getScaleCalTimestamp1();
    doc["scaleCalTimestamp2"] = settings.getScaleCalTimestamp2();
    doc["scaleCalStddev1"] = settings.getScaleCalStddev1();
    doc["scaleCalStddev2"] = settings.getScaleCalStddev2();
    doc["hardwareScaleDisabled"] =
#ifdef GAGGIMATE_DISABLE_HARDWARE_SCALE
        true;
#else
        false;
#endif
    // Runtime truth for whether the connected controller actually has a hardware
    // scale (STM32 + HX711 over UART). The web UI gates the "Hardware" source
    // option on this rather than only the build-time hardwareScaleDisabled flag.
    doc["scaleCapable"] = controller->scaleAvailability().hardwareCapable;
    // Add auto-wakeup settings to response
    doc["autowakeupEnabled"] = settings.isAutoWakeupEnabled();
    doc["buttonBehavior"] = implode(settings.getButtonBehaviorList(), ",");

    // Add schedule format with days
    std::vector<AutoWakeupSchedule> autowakeupSchedules = settings.getAutoWakeupSchedules();
    String schedulesStr = "";
    for (size_t i = 0; i < autowakeupSchedules.size(); i++) {
        if (i > 0)
            schedulesStr += ";";
        schedulesStr += autowakeupSchedules[i].time + "|";

        // Convert days array to 7-bit string
        for (int j = 0; j < 7; j++) {
            schedulesStr += autowakeupSchedules[i].days[j] ? "1" : "0";
        }
    }
    doc["autowakeupSchedules"] = schedulesStr;
    serializeJson(doc, *response);
    request->send(response);

    if (request->method() == HTTP_POST && request->hasArg("restart"))
        ESP.restart();
}

void WebUIPlugin::handleBLEScaleList(AsyncWebServerRequest *request) {
    JsonDocument doc(&psramAllocator);
    JsonArray scalesArray = doc.to<JsonArray>();
    std::vector<DiscoveredDevice> devices = BLEScales.getDiscoveredScales();
    for (const DiscoveredDevice &device : BLEScales.getDiscoveredScales()) {
        JsonDocument scale(&psramAllocator);
        scale["uuid"] = device.getAddress().toString();
        scale["name"] = device.getName();
        scale["rssi"] = device.getRSSI();
        scalesArray.add(scale);
    }
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
}

void WebUIPlugin::handleBLEScaleScan(AsyncWebServerRequest *request) {
    if (request->method() != HTTP_POST) {
        request->send(404);
        return;
    }
    BLEScales.scan();
    JsonDocument doc(&psramAllocator);
    doc["success"] = true;
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
}

void WebUIPlugin::handleBLEScaleConnect(AsyncWebServerRequest *request) {
    if (request->method() != HTTP_POST) {
        request->send(404);
        return;
    }
    BLEScales.connect(request->arg("uuid").c_str());
    JsonDocument doc(&psramAllocator);
    doc["success"] = true;
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
}

void WebUIPlugin::handleBLEScaleInfo(AsyncWebServerRequest *request) {
    JsonDocument doc(&psramAllocator);
    doc["connected"] = BLEScales.isConnected();
    doc["name"] = BLEScales.getName();
    doc["uuid"] = BLEScales.getUUID();
    doc["rssi"] = BLEScales.getRSSI();
    doc["hasBattery"] = BLEScales.hasBatteryLevel();
    // Only surface the numeric when the scale reports one — a 255 sentinel
    // (REMOTE_SCALES_BATTERY_UNKNOWN) would otherwise render as a fake "255%".
    if (BLEScales.hasBatteryLevel()) {
        const uint8_t pct = BLEScales.getBatteryLevel();
        if (pct != REMOTE_SCALES_BATTERY_UNKNOWN) {
            doc["battery"] = pct;
        }
    }
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
}

void WebUIPlugin::updateOTAStatus(const String &version) {
    if (ws.getClients().empty()) {
        return;
    }
    Settings const &settings = controller->getSettings();
    JsonDocument doc(&psramAllocator);
    doc["latestVersion"] = ota->getCurrentVersion();
    doc["tp"] = "res:ota-settings";
    doc["displayUpdateAvailable"] = ota->isUpdateAvailable(false);
    doc["controllerUpdateAvailable"] = ota->isUpdateAvailable(true);
    doc["displayVersion"] = BUILD_GIT_VERSION;
    doc["controllerVersion"] = controller->getSystemInfo().version;
    doc["hardware"] = controller->getSystemInfo().hardware;
    doc["latestVersion"] = ota->getCurrentVersion();
    doc["channel"] = settings.getOTAChannel();
    doc["updating"] = updating;
    // LittleFS usage metrics
    {
        size_t total = LittleFS.totalBytes();
        size_t used = LittleFS.usedBytes();
        size_t freeBytes = total > used ? (total - used) : 0;
        doc["spiffsTotal"] = static_cast<uint32_t>(total);
        doc["spiffsUsed"] = static_cast<uint32_t>(used);
        doc["spiffsFree"] = static_cast<uint32_t>(freeBytes);
        if (total > 0) {
            doc["spiffsUsedPct"] = static_cast<uint8_t>((used * 100) / total);
        }
    }
    // Memory usage metrics
    {
        size_t free = heap_caps_get_free_size(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
        size_t total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
        doc["heapFree"] = static_cast<uint32_t>(free);
        doc["heapLargest"] = static_cast<uint32_t>(largest);
        doc["heapTotal"] = static_cast<uint32_t>(total);
    }
    doc["controllerTaskHealth"] = controller->isTaskHealthy();
#ifndef GAGGIMATE_HEADLESS
    doc["uiTaskHealth"] = controller->getUI()->isTaskHealthy();
#endif
    if (controller->isSDCard()) {
        const uint64_t total = SD_MMC.cardSize();
        const uint64_t used = SD_MMC.usedBytes();
        const uint64_t freeBytes = total > used ? (total - used) : 0;
        doc["sdTotal"] = total;
        doc["sdUsed"] = used;
        doc["sdFree"] = freeBytes;
        if (total > 0) {
            // Provide integer percentage to avoid float JSON
            doc["sdUsedPct"] = static_cast<uint8_t>((used * 100) / total);
        }
    }
    broadcastJson(doc);
}

void WebUIPlugin::updateOTAProgress(uint8_t phase, int progress) {
    if (ws.getClients().empty()) {
        return;
    }
    JsonDocument doc(&psramAllocator);
    doc["tp"] = "evt:ota-progress";
    doc["phase"] = phase;
    doc["progress"] = progress;
    broadcastJson(doc);
}

void WebUIPlugin::broadcastJson(JsonDocument &doc) {
    if (ws.getClients().empty()) {
        return;
    }
    const size_t len = measureJson(doc);
    auto *buffer = ws.makeBuffer(len);
    if (buffer == nullptr) {
        return; // out of buffers; drop this broadcast rather than churn the heap
    }
    serializeJson(doc, buffer->get(), len);
    ws.textAll(buffer);
}

void WebUIPlugin::sendAutotuneResult() {
    JsonDocument doc(&psramAllocator);
    doc["tp"] = "evt:autotune-result";
    doc["pid"] = controller->getSettings().getPid();
    broadcastJson(doc);
}

void WebUIPlugin::sendAutotuneFailed() {
    // Distinct WS event — Autotune page renders "timed out" error card
    // instead of stuck spinner. Fires on ERROR_CODE_AUTOTUNE_TIMEOUT.
    JsonDocument doc(&psramAllocator);
    doc["tp"] = "evt:autotune-failed";
    broadcastJson(doc);
}

void WebUIPlugin::handleFlushStart(uint32_t clientId, JsonDocument &request) {
    controller->onFlush();

    JsonDocument response(&psramAllocator);
    response["tp"] = "res:flush:start";
    response["rid"] = request["rid"];
    response["success"] = true;

    String msg;
    serializeJson(response, msg);
    ws.text(clientId, msg);
}

void WebUIPlugin::handleCoreDumpDownload(AsyncWebServerRequest *request) {
    // Check if core dump is available
    size_t coreAddr, coreSize;
    if (esp_core_dump_image_get(&coreAddr, &coreSize) != ESP_OK || coreSize == 0) {
        request->send(404, "text/plain", "No core dump available");
        return;
    }

    // Find the coredump partition
    const esp_partition_t *coredump_partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (coredump_partition == NULL) {
        request->send(500, "text/plain", "Core dump partition not found");
        return;
    }

    ESP_LOGI("WebUIPlugin", "Streaming core dump: %d bytes from 0x%x", coreSize, coreAddr);

    // Create a streaming response
    AsyncWebServerResponse *response =
        request->beginResponse("application/octet-stream", coreSize,
                               [coredump_partition, coreSize](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
                                   // Calculate how much to read
                                   size_t remaining = coreSize - index;
                                   size_t toRead = (remaining < maxLen) ? remaining : maxLen;

                                   if (toRead == 0)
                                       return 0;

                                   // Read from partition
                                   esp_err_t err = esp_partition_read(coredump_partition, index, buffer, toRead);
                                   if (err != ESP_OK) {
                                       ESP_LOGE("WebUIPlugin", "Failed to read core dump: %s", esp_err_to_name(err));
                                       return 0;
                                   }

                                   return toRead;
                               });

    // Set appropriate headers
    response->addHeader("Content-Disposition", "attachment; filename=\"coredump.bin\"");
    response->addHeader("Cache-Control", "no-cache");

    request->send(response);
}
