#include "Controller.h"
#include "ArduinoJson.h"
#include "esp_coexist.h"
#include "esp_sntp.h"
#include "esp_task_wdt.h"
#include <LittleFS.h>
#include <SD_MMC.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <display/config.h>
#include <display/core/constants.h>
#include <display/core/process/BrewProcess.h>
#include <display/core/process/GrindProcess.h>
#include <display/core/process/PumpProcess.h>
#include <display/core/process/SteamProcess.h>
#include <display/core/static_profiles.h>
#include <display/core/zones.h>
#include <display/plugins/AutoWakeupPlugin.h>
#include <display/plugins/BLEScalePlugin.h>
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
#include <display/plugins/HWScalePlugin.h>
#endif
#include <display/plugins/BoilerFillPlugin.h>
#include <display/plugins/HomekitPlugin.h>
#include <display/plugins/LedControlPlugin.h>
#include <display/plugins/MQTTPlugin.h>
#include <display/plugins/NetworkWatchdogPlugin.h>
#ifndef GAGGIMATE_HEADLESS
#include <display/plugins/RatingPlugin.h>
#endif
#include <display/plugins/ShotHistoryPlugin.h>
#include <display/plugins/SmartGrindPlugin.h>
#include <display/plugins/WebUIPlugin.h>
#include <display/plugins/mDNSPlugin.h>
#include <display/util/PsramAllocator.h>
#ifndef GAGGIMATE_HEADLESS
#include <display/drivers/AmoledDisplayDriver.h>
#include <display/drivers/LilyGoDriver.h>
#include <display/drivers/WaveshareDriver.h>
#endif

const String LOG_TAG = F("Controller");

namespace {
// RAII guard for the process mutex. Recursive take, give on scope exit.
struct ProcessLock {
    SemaphoreHandle_t m;
    explicit ProcessLock(SemaphoreHandle_t mtx) : m(mtx) {
        if (m != nullptr) {
            xSemaphoreTakeRecursive(m, portMAX_DELAY);
        }
    }
    ~ProcessLock() {
        if (m != nullptr) {
            xSemaphoreGiveRecursive(m);
        }
    }
    ProcessLock(const ProcessLock &) = delete;
    ProcessLock &operator=(const ProcessLock &) = delete;
};
} // namespace

struct CtrlCmdMsg {
    CtrlCmd type;
    int32_t arg;
};

void Controller::postCommand(CtrlCmd cmd, int32_t arg) {
    if (cmdQueue == nullptr) {
        return;
    }
    CtrlCmdMsg msg{cmd, arg};
    if (xQueueSend(cmdQueue, &msg, 0) != pdTRUE) {
        ESP_LOGW(LOG_TAG.c_str(), "Controller cmdQueue full, dropped cmd=%u", static_cast<unsigned>(cmd));
    }
}

void Controller::drainCommandQueue() {
    if (cmdQueue == nullptr) {
        return;
    }
    CtrlCmdMsg msg;
    while (xQueueReceive(cmdQueue, &msg, 0) == pdTRUE) {
        switch (msg.type) {
        case CtrlCmd::ACTIVATE:
            activate();
            break;
        case CtrlCmd::DEACTIVATE:
            deactivate();
            break;
        case CtrlCmd::CLEAR:
            clear();
            break;
        case CtrlCmd::ACTIVATE_GRIND:
            activateGrind();
            break;
        case CtrlCmd::DEACTIVATE_GRIND:
            deactivateGrind();
            break;
        case CtrlCmd::ACTIVATE_STANDBY:
            activateStandby();
            break;
        case CtrlCmd::DEACTIVATE_STANDBY:
            deactivateStandby();
            break;
        case CtrlCmd::SET_MODE:
            setMode(static_cast<int>(msg.arg));
            break;
        case CtrlCmd::RAISE_TEMP:
            raiseTemp();
            break;
        case CtrlCmd::LOWER_TEMP:
            lowerTemp();
            break;
        case CtrlCmd::RAISE_GRIND_TARGET:
            raiseGrindTarget();
            break;
        case CtrlCmd::LOWER_GRIND_TARGET:
            lowerGrindTarget();
            break;
        }
    }
}

void Controller::setup() {
    mode = settings.getStartupMode();
    processMutex = xSemaphoreCreateRecursiveMutex();
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    scaleSampleMutex = xSemaphoreCreateMutex();
#endif
    cmdQueue = xQueueCreate(16, sizeof(CtrlCmdMsg));

    // Web assets are served from this partition. LittleFS (not SPIFFS): SPIFFS
    // has no directory tree, so stat()/exists() is O(whole filesystem) and a
    // miss scans every page -- the web handler does that synchronously in the
    // async_tcp task for every request, which under a multi-tab load burst
    // pegged CPU0 for >5s and tripped the task watchdog (reboot). LittleFS
    // lookups are O(path). maxOpenFiles 16 for concurrent asset serving. [GM-90]
    if (!LittleFS.begin(true, "/littlefs", 16)) {
        Serial.println(F("An Error has occurred while mounting LittleFS"));
    }

#ifndef GAGGIMATE_HEADLESS
    setupPanel();
#endif

    pluginManager = new PluginManager();
#ifndef GAGGIMATE_HEADLESS
    ui = new DefaultUI(this, driver, pluginManager);
    if (driver->supportsSDCard() && driver->installSDCard()) {
        sdcard = true;
        ESP_LOGI(LOG_TAG, "SD Card detected and mounted");
        ESP_LOGI(LOG_TAG, "Used: %lluMB, Capacity: %lluMB", SD_MMC.usedBytes() / 1024 / 1024, SD_MMC.cardSize() / 1024 / 1024);
    }
#endif
    FS *fs = &LittleFS;
    if (sdcard) {
        fs = &SD_MMC;
    }
    profileManager = new ProfileManager(fs, "/p", settings, pluginManager);
    profileManager->setup();
    if (settings.isHomekit())
        pluginManager->registerPlugin(new HomekitPlugin(settings.getWifiSsid(), settings.getWifiPassword()));
    else
        pluginManager->registerPlugin(new mDNSPlugin());
    if (settings.isBoilerFillActive()) {
        pluginManager->registerPlugin(new BoilerFillPlugin());
    }
    if (settings.isSmartGrindActive()) {
        pluginManager->registerPlugin(new SmartGrindPlugin());
    }
    if (settings.isHomeAssistant()) {
        pluginManager->registerPlugin(new MQTTPlugin());
    }
    if (settings.isHomeAssistant() && settings.isRLRatingEnabled()) {
#ifndef GAGGIMATE_HEADLESS
        pluginManager->registerPlugin(new RatingPlugin());
#endif
    }
    pluginManager->registerPlugin(new WebUIPlugin());
    pluginManager->registerPlugin(new NetworkWatchdogPlugin());
    pluginManager->registerPlugin(&ShotHistory);
    pluginManager->registerPlugin(&BLEScales);
#if defined(GAGGIMATE_UART_COMMS) && !defined(GAGGIMATE_DISABLE_HARDWARE_SCALE)
    pluginManager->registerPlugin(&HWScale);
#endif
    pluginManager->registerPlugin(new LedControlPlugin());
    pluginManager->registerPlugin(new AutoWakeupPlugin());
    pluginManager->setup(this);

    pluginManager->on("profiles:profile:save", [this](Event const &event) {
        String id = event.getString("id");
        if (id == profileManager->getSelectedProfile().id) {
            this->handleProfileUpdate();
        }
    });

    pluginManager->on("profiles:profile:select", [this](Event const &event) { this->handleProfileUpdate(); });

    // When the user cycles the brew scale source from the UI/WebUI we tear down
    // any in-flight process so the next start resolves the source cleanly.
    // Without this, Controller::currentVolumetricSource can hold a value that no
    // longer matches the selected source ("brew button does nothing" after
    // cycling away from Predictive).
    settings.setOnScaleSourceChange([this](int) {
        postCommand(CtrlCmd::DEACTIVATE);
        postCommand(CtrlCmd::CLEAR);
    });

#ifndef GAGGIMATE_HEADLESS
    ui->init();
#endif
    this->onScreenReady();

    updateLastAction();
    xTaskCreatePinnedToCore(loopTask, "Controller::loopControl", configMINIMAL_STACK_SIZE * 6, this, 2, &taskHandle, 0);
    xTaskCreatePinnedToCore(loopLogicTask, "Controller::loopLogic", configMINIMAL_STACK_SIZE * 6, this, 3, &logicTaskHandle, 0);
    esp_task_wdt_add(taskHandle);
}

void Controller::onScreenReady() { screenReady = true; }

void Controller::onTargetToggle() { settings.setVolumetricTarget(!settings.isVolumetricTarget()); }

void Controller::onTargetChange(ProcessTarget target) { settings.setVolumetricTarget(target == ProcessTarget::VOLUMETRIC); }

void Controller::connect() {
    lastPing = millis();
    connectStartTime = millis();
    pluginManager->trigger("controller:startup");

    setupWifi();
    setupBluetooth();
    pluginManager->on("ota:update:start", [this](Event const &) { this->updating = true; });
    pluginManager->on("ota:update:end", [this](Event const &) { this->updating = false; });

    updateLastAction();
    initialized = true;
}

#ifndef GAGGIMATE_HEADLESS
void Controller::setupPanel() {
    if (LilyGoDriver::getInstance()->isCompatible()) {
        driver = LilyGoDriver::getInstance();
    } else if (AmoledDisplayDriver::getInstance()->isCompatible()) {
        driver = AmoledDisplayDriver::getInstance();
    } else if (WaveshareDriver::getInstance()->isCompatible()) {
        driver = WaveshareDriver::getInstance();
    } else {
        Serial.println("No compatible display driver found");
        delay(10000);
        ESP.restart();
    }
    driver->init();
}
#endif

// Parse a comma-separated float string ("a,b,c,d") into `out`. Missing fields
// are left at `def` -- used so pump-model coeffs can carry NaN to signal
// two-point flow-measurement mode, and an absent PID Kf defaults to 0.
static void parseFloatCsv(const String &csv, float *out, size_t count, float def) {
    for (size_t i = 0; i < count; i++)
        out[i] = def;
    int start = 0;
    for (size_t i = 0; i < count; i++) {
        if (start > csv.length())
            break;
        int comma = csv.indexOf(',', start);
        String token = (comma < 0) ? csv.substring(start) : csv.substring(start, comma);
        token.trim();
        if (token.length() > 0)
            out[i] = token.toFloat();
        if (comma < 0)
            break;
        start = comma + 1;
    }
}

void Controller::setupBluetooth() {
#ifdef GAGGIMATE_UART_COMMS
#ifndef GAGGIMATE_UART_BAUD
#define GAGGIMATE_UART_BAUD 115200
#endif
#ifndef GAGGIMATE_UART_RX_PIN
#define GAGGIMATE_UART_RX_PIN 44
#endif
#ifndef GAGGIMATE_UART_TX_PIN
#define GAGGIMATE_UART_TX_PIN 43
#endif
    Serial2.begin(GAGGIMATE_UART_BAUD, SERIAL_8N1, GAGGIMATE_UART_RX_PIN, GAGGIMATE_UART_TX_PIN);
    ESP_LOGI(LOG_TAG, "UART controller link initialized at %d baud (RX=%d, TX=%d)", GAGGIMATE_UART_BAUD,
             GAGGIMATE_UART_RX_PIN, GAGGIMATE_UART_TX_PIN);
#endif
    comms.init("GPBLC");
    comms.onConnectionChanged([this](bool connected) {
        // Force a full control resend after any (re)connect -- the controller
        // starts with no state and updateControl() otherwise only sends deltas.
        controlStateSent = false;
        if (connected) {
            // Re-assert the connection interval for the fresh link (e.g. tight
            // again if we reconnected mid-shot).
            applyConnectionPriority(true);
        } else if (initialized) {
            pluginManager->trigger("controller:bluetooth:disconnect");
            waitingForController = true;
            setMode(MODE_STANDBY);
        }
    });
    comms.onSystemInfo(
        [this](const char *hardware, const char *version, uint32_t protocolVersion, bool dimming, bool pressure, bool ledControl,
               bool tof, bool scale) { onSystemInfo(hardware, version, protocolVersion, dimming, pressure, ledControl, tof, scale); });
    comms.onIncompatibleController([this](const String &info) { onIncompatibleController(info); });
    // A controller OTA streams the firmware over this BLE link; the relaxed idle
    // interval makes that crawl. Force a low-latency interval for the duration of
    // a controller flash, then restore. (A display OTA is Wi-Fi-bound, so leave
    // BLE relaxed to keep radio airtime for the download.)
    pluginManager->on("ota:update:start", [this](Event const &event) {
        if (event.getString("component") != "display") {
            connLowLatency = true;
            comms.setLowLatency(true);
            // Streaming firmware over BLE -> BLE must win the shared radio, same
            // as during a shot. Without this it would run against the new
            // idle WiFi-preference and crawl. Restored by applyConnectionPriority
            // on ota:update:end. [GM-90]
            esp_coex_preference_set(ESP_COEX_PREFER_BT);
        }
    });
    pluginManager->on("ota:update:end", [this](Event const &) { applyConnectionPriority(true); });
    comms.onSensorData([this](float temp, float pressure, float puckFlow, float pumpFlow, float puckResistance) {
        onTempRead(temp);
        this->pressure.store(pressure, std::memory_order_relaxed);
        this->currentPuckFlow = puckFlow;
        this->currentPumpFlow.store(pumpFlow, std::memory_order_relaxed);
        pluginManager->trigger("boiler:pressure:change", "value", pressure);
        pluginManager->trigger("pump:puck-flow:change", "value", puckFlow);
        pluginManager->trigger("pump:flow:change", "value", pumpFlow);
        pluginManager->trigger("pump:puck-resistance:change", "value", puckResistance);
    });
    comms.onButtonState([this](uint8_t index, bool pressed) {
        const int status = pressed ? 1 : 0;
        String behavior = settings.getButtonBehavior(index);
        ESP_LOGV("Controller", "Button %d changed to %d, behavior: %s", index, status, behavior);
        if (behavior == "" || behavior == "none") {
            return;
        }
        if (behavior == "brew") {
            handleBrewButton(status);
            return;
        }
        if (behavior == "steam") {
            handleSteamButton(status);
            return;
        }
        if (behavior == "water") {
            handleWaterButton(status);
            return;
        }
        if (behavior == "flush") {
            // Flush is a one-shot fixed-duration BrewProcess. Trigger on
            // press only; release does nothing so the user can't
            // accidentally cancel mid-flush by letting go (push button)
            // or flipping the rocker back. onFlush() itself is a no-op
            // if a process is already active, so rapid presses don't
            // queue.
            //
            // Ensure we land in MODE_BREW so the flush UI renders, but
            // only when no other process is currently running. Mutating
            // mode mid-process would orphan the active mode's UI while
            // onFlush() silently no-ops on the re-entrancy guard. The
            // setMode guard mirrors the pattern other button handlers
            // use when they need to switch modes safely.
            if (status) {
                if (getMode() == MODE_STANDBY) {
                    deactivateStandby();
                }
                if (getMode() != MODE_BREW && !isActive()) {
                    setMode(MODE_BREW);
                }
                onFlush();
            }
            return;
        }
        handleProfileButton(status, behavior);
    });
    comms.onError([this](int error) {
        // Autotune timeout = info-level, not runaway. Controller already
        // preserved NVS PID. Clear autotuning flag, fire dedicated Web UI
        // event. Don't latch this->error (would gate future setupBluetooth).
        if (error == ERROR_CODE_AUTOTUNE_TIMEOUT) {
            ESP_LOGW(LOG_TAG, "Autotune timed out — previous PID preserved");
            autotuning = false;
            pluginManager->trigger("controller:autotune:failed");
            return;
        }
        if (error != ERROR_CODE_TIMEOUT && error != this->error) {
            this->error = error;
            deactivate();
            setMode(MODE_STANDBY);
            pluginManager->trigger(F("controller:error"));
            ESP_LOGE(LOG_TAG, "Received error %d", error);
        }
    });
    comms.onAutotuneResult([this](float Kp, float Ki, float Kd, float Kf) {
        ESP_LOGI(LOG_TAG, "Received autotune values: Kp=%.3f, Ki=%.3f, Kd=%.3f, Kf=%.3f (combined)", Kp, Ki, Kd, Kf);
        // Guard: older controller firmware could emit zero/NaN gains (#672
        // class). Reject — keep existing PID, surface as "Autotune Failed".
        if (!std::isfinite(Kp) || !std::isfinite(Ki) || !std::isfinite(Kd) || !std::isfinite(Kf) || Kp <= 0.0f ||
            (Kp + Ki + Kd) <= 0.0f) {
            ESP_LOGW(LOG_TAG, "Rejecting autotune result: invalid gains, preserving existing PID");
            autotuning = false;
            pluginManager->trigger("controller:autotune:failed");
            return;
        }
        char pid[64];
        // Store in simplified format with combined Kf
        snprintf(pid, sizeof(pid), "%.3f,%.3f,%.3f,%.3f", Kp, Ki, Kd, Kf);
        settings.setPid(String(pid));
        pluginManager->trigger("controller:autotune:result");
        autotuning = false;
    });
    comms.onVolumetricMeasurement(
        [this](float value) { onVolumetricMeasurement(value, VolumetricMeasurementSource::FLOW_ESTIMATION); });
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    comms.onWeightMeasurement([this](float weight) {
        ScaleSample sample{};
        sample.weightG = weight;
        sample.stddevG = 0.0f;
        sample.ch1G = weight;
        sample.ch2G = 0.0f;
        sample.ch1StdG = 0.0f;
        sample.ch2StdG = 0.0f;
        sample.healthBits = SCALE_HEALTH_OK;
        if (scaleSampleMutex && xSemaphoreTake(scaleSampleMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            sample.sampleSeq = lastScaleSample.sampleSeq + 1;
            lastScaleSample = sample;
            lastHardwareScaleSampleMs = millis();
            xSemaphoreGive(scaleSampleMutex);
        }
        recordHardwareScaleBaselineSample(sample);
        pluginManager->trigger("controller:scale:sample", "value", weight);
        pluginManager->trigger("controller:weight:change", "value", weight);
    });
    comms.onScaleOffsets([this](long offset1, long offset2) {
        settings.setScaleOffset1(offset1);
        settings.setScaleOffset2(offset2);
        ESP_LOGI(LOG_TAG, "Scale offsets received and saved: %ld, %ld", offset1, offset2);
        Event ev{"controller:scale:tare:done"};
        ev.setInt("success", 1);
        ev.setFloat("offset1", static_cast<float>(offset1));
        ev.setFloat("offset2", static_cast<float>(offset2));
        ev.setFloat("std1", 0.0f);
        ev.setFloat("std2", 0.0f);
        ev.setInt("healthBits", SCALE_HEALTH_OK);
        pluginManager->trigger(ev);
    });
    comms.onScaleCalibrationResult([this](uint8_t channel, float calibration) {
        const long now = static_cast<long>(time(nullptr));
        if (channel == 1) {
            settings.setScaleCalibration1(calibration);
            settings.setScaleCalTimestamp1(now);
            settings.setScaleCalStddev1(0.0f);
        } else if (channel == 2) {
            settings.setScaleCalibration2(calibration);
            settings.setScaleCalTimestamp2(now);
            settings.setScaleCalStddev2(0.0f);
        }
        pluginManager->trigger("controller:scale:calibrated", "channel", static_cast<int>(channel));
        Event ev{"controller:scale:cal:done"};
        ev.setInt("channel", static_cast<int>(channel));
        ev.setFloat("factor", calibration);
        ev.setFloat("stddevG", 0.0f);
        ev.setInt("success", 1);
        ev.setInt("errorCode", 0);
        pluginManager->trigger(ev);
        ESP_LOGI(LOG_TAG, "Scale ch%d calibration result: %.6f", channel, calibration);
    });
#endif
    comms.onTofMeasurement([this](uint32_t value) {
        tofDistance = static_cast<int>(value);
        ESP_LOGV(LOG_TAG, "Received new TOF distance: %d", tofDistance);
        pluginManager->trigger("controller:tof:change", "value", tofDistance);
    });
    pluginManager->on("settings:changed", [this](Event const &) {
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
        if (comms.isConnected() && systemInfo.capabilities.scale) {
            comms.sendScaleCalibration(settings.getScaleCalibration1(), settings.getScaleCalibration2(), settings.getScaleOffset1(),
                                       settings.getScaleOffset2());
        }
#endif
    });
#ifdef GAGGIMATE_UART_COMMS
    pluginManager->trigger("controller:uart:init");
#else
    pluginManager->trigger("controller:bluetooth:init");
#endif
}

void Controller::onSystemInfo(const char *hardware, const char *version, uint32_t protocolVersion, bool dimming, bool pressure,
                              bool ledControl, bool tof, bool scale) {
    const bool mismatch = protocolVersion != gm_proto::PROTOCOL_VERSION;
    systemInfo = SystemInfo{.hardware = String(hardware),
                            .version = String(version),
                            .capabilities =
                                SystemCapabilities{
                                    .dimming = dimming,
                                    .pressure = pressure,
                                    .ledControl = ledControl,
                                    .tof = tof,
                                    .scale = scale,
                                },
                            .protocolVersion = protocolVersion,
                            .protocolMismatch = mismatch};
    ESP_LOGI(LOG_TAG, "System info: %s %s (proto=%u local=%u dm=%d ps=%d led=%d tof=%d scale=%d)", hardware, version,
             protocolVersion, gm_proto::PROTOCOL_VERSION, dimming, pressure, ledControl, tof, scale);
    if (mismatch) {
        // Mixed-firmware links are not wire-compatible, so don't push config and
        // don't drive control (updateControl() also bails on protocolMismatch).
        // We still fire controller:ready below so OTA can init -- that's the
        // recovery path to update the out-of-date side.
        ESP_LOGW(LOG_TAG, "Protocol version mismatch: controller=%u display=%u -- control inhibited, OTA only", protocolVersion,
                 gm_proto::PROTOCOL_VERSION);
        pluginManager->trigger("controller:protocol:mismatch", "value", static_cast<int>(protocolVersion));
    } else {
        // Capability-dependent setup that the old protocol ran synchronously right
        // after connect, now driven by the asynchronous SystemInfo push.
        setPressureScale();
        float pid[4];
        parseFloatCsv(settings.getPid(), pid, 4, 0.0f);
        comms.sendPidSettings(pid[0], pid[1], pid[2], pid[3]);
        setPumpModelCoeffs();
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
        if (scale) {
            comms.sendScaleCalibration(settings.getScaleCalibration1(), settings.getScaleCalibration2(), settings.getScaleOffset1(),
                                       settings.getScaleOffset2());
        }
#endif
    }
#ifdef GAGGIMATE_DISABLE_HARDWARE_SCALE
    systemInfo.capabilities.scale = false;
#endif

    if (!loaded) {
        loaded = true;
        if (!mismatch && settings.getStartupMode() == MODE_STANDBY)
            activateStandby();
        pluginManager->trigger("controller:ready");
    }
    pluginManager->trigger("controller:bluetooth:connect");
}

void Controller::onIncompatibleController(const String &infoJson) {
    // An old controller (no framed-comms characteristics) is, for our purposes,
    // a protocol mismatch: reuse the exact same path. We force protocolVersion 0
    // (it cannot speak the framed protocol), so onSystemInfo() inhibits control
    // but still fires controller:ready so OTA can flash the controller back into
    // compatibility. The real hardware/version/capabilities come from the legacy
    // read-only INFO characteristic the old controller still exposes.
    waitingForController = false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, infoJson);
    if (err) {
        ESP_LOGW(LOG_TAG, "Incompatible controller, no readable info (%s)", err.c_str());
        onSystemInfo("Legacy controller", "0.0.0", 0, false, false, false, false, false);
        return;
    }
    String hardware = doc["hw"].as<String>();
    String version = doc["v"].as<String>();
    if (hardware.isEmpty())
        hardware = "Legacy controller";
    if (version.isEmpty())
        version = "0.0.0";
    onSystemInfo(hardware.c_str(), version.c_str(), 0, doc["cp"]["dm"].as<bool>(), doc["cp"]["ps"].as<bool>(),
                 doc["cp"]["led"].as<bool>(), doc["cp"]["tof"].as<bool>(), doc["cp"]["sc"].as<bool>());
}

void Controller::setupWifi() {
    if (settings.getWifiSsid() != "" && settings.getWifiPassword() != "") {
        WiFi.setHostname(settings.getMdnsName().c_str());
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
        WiFi.begin(settings.getWifiSsid(), settings.getWifiPassword());
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
        for (int attempts = 0; attempts < WIFI_CONNECT_ATTEMPTS; attempts++) {
            if (WiFi.status() == WL_CONNECTED) {
                break;
            }
            delay(500);
            Serial.print(".");
        }
        Serial.println("");
        if (WiFi.status() == WL_CONNECTED) {
            ESP_LOGI(LOG_TAG, "Connected to %s with IP address %s", settings.getWifiSsid().c_str(),
                     WiFi.localIP().toString().c_str());
            // These run in the Arduino WiFi event task (small stack). Only flag
            // the change here; loop() fires the plugin events on the main loop so
            // server/mDNS/socket teardown never runs in this callback context.
            WiFi.onEvent([this](WiFiEvent_t, WiFiEventInfo_t) { wifiConnectedPending = true; },
                         WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
            WiFi.onEvent(
                [this](WiFiEvent_t, WiFiEventInfo_t info) {
                    ESP_LOGI(LOG_TAG, "Lost WiFi connection. Reason: %s",
                             WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(info.wifi_sta_disconnected.reason)));
                    wifiDisconnectedPending = true;
                },
                WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
            configTzTime(resolve_timezone(settings.getTimezone()), NTP_SERVER);
            setenv("TZ", resolve_timezone(settings.getTimezone()), 1);
            tzset();
            sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
            sntp_setservername(0, NTP_SERVER);
            sntp_init();
        } else {
            WiFi.disconnect(true, true);
            ESP_LOGI(LOG_TAG, "Timed out while connecting to WiFi");
            Serial.println("Timed out while connecting to WiFi");
        }
    }
    if (WiFi.status() != WL_CONNECTED) {
        isApConnection = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAPConfig(WIFI_AP_IP, WIFI_AP_IP, WIFI_SUBNET_MASK);
        WiFi.softAP(WIFI_AP_SSID);
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
        ESP_LOGI(LOG_TAG, "Started WiFi AP %s", WIFI_AP_SSID);
    }

    pluginManager->on("ota:update:start", [this](Event const &) { this->updating = true; });
    pluginManager->on("ota:update:end", [this](Event const &) { this->updating = false; });

    pluginManager->trigger("controller:wifi:connect", "AP", isApConnection ? 1 : 0);
}

void Controller::loop() {
    // Drain external commands first so any pending activate/deactivate/setMode
    // from WebUI / LVGL / plugins runs on this task, not on AsyncTCP or LVGL.
    drainCommandQueue();

    // Act on WiFi link-state changes flagged by the (small-stack) event task here
    // on the main loop. Disconnect before connect so a flap is ordered correctly.
    if (wifiDisconnectedPending) {
        wifiDisconnectedPending = false;
        pluginManager->trigger("controller:wifi:disconnect");
    }
    if (wifiConnectedPending) {
        wifiConnectedPending = false;
        pluginManager->trigger("controller:wifi:connect", "AP", isApConnection ? 1 : 0);
    }

    pluginManager->loop();

    if (screenReady && !initialized) {
        connect();
    }

    if (initialized) {
        comms.loop(); // drive the comms send pump + retransmit
    }

    unsigned long now = millis();

    // If BLE scanning has been running for a while without finding the controller,
    // notify the UI so it can update the startup label accordingly.
    if (!waitingForController && initialized && !comms.isConnected() &&
        (now - connectStartTime) > CONTROLLER_WAITING_TIMEOUT_MS) {
        waitingForController = true;
        pluginManager->trigger("controller:bluetooth:waiting");
    }

    if (comms.isReadyForConnection() && comms.connectToServer()) {
        waitingForController = false;
    }

    // Keepalive: updateControl() only sends control deltas now, so a steady-state
    // session would otherwise go silent. A periodic ping keeps the controller's
    // connection watchdog fed (sent in all states, including error). Skip it for
    // an incompatible controller -- it can't parse the frame anyway.
    if (comms.isConnected() && !systemInfo.protocolMismatch && now - lastPing >= PING_INTERVAL) {
        comms.sendPing();
        lastPing = now;
    }
}

void Controller::loopLogic() {
    if (isErrorState()) {
        return;
    }

    unsigned long now = millis();

    if (now - lastProgress > PROGRESS_INTERVAL) {
        // Check if steam is ready
        if (mode == MODE_STEAM && !steamReady && currentTemp.load(std::memory_order_relaxed) + 5.f > getTargetTemp()) {
            activate();
            steamReady = true;
        }

        // Handle current and last process under the process mutex so a concurrent
        // deactivate()/clear() from AsyncTCP or LVGL cannot delete the object
        // we're dereferencing here.
        bool needDeactivate = false;
        {
            ProcessLock lock(processMutex);
            Process *proc = currentProcess;
            if (proc != nullptr) {
                updateLastAction();
                if (proc->getType() == MODE_BREW) {
                    auto brewProcess = static_cast<BrewProcess *>(proc);
                    brewProcess->updatePressure(pressure.load(std::memory_order_relaxed));
                    brewProcess->updateFlow(currentPumpFlow.load(std::memory_order_relaxed));
                }
                proc->progress();
                needDeactivate = !proc->isActive();
            }

            Process *last = lastProcess;
            if (last != nullptr && !last->isComplete()) {
                last->progress();
            }
            if (last != nullptr && last->isComplete() && !processCompleted && settings.isDelayAdjust()) {
                processCompleted = true;
                if (last->getType() == MODE_BREW) {
                    if (auto *brewProcess = static_cast<BrewProcess *>(last); brewProcess->target == ProcessTarget::VOLUMETRIC) {
                        double newDelay = brewProcess->getNewDelayTime();
                        if (newDelay >= 0) {
                            settings.setBrewDelay(newDelay);
                        }
                    }
                } else if (last->getType() == MODE_GRIND) {
                    if (auto *grindProcess = static_cast<GrindProcess *>(last);
                        grindProcess->target == ProcessTarget::VOLUMETRIC) {
                        double newDelay = grindProcess->getNewDelayTime();
                        if (newDelay >= 0) {
                            settings.setGrindDelay(newDelay);
                        }
                    }
                }
            }
        }
        if (needDeactivate) {
            deactivate();
        }
        lastProgress = now;
    }

    if (grindActiveUntil != 0 && now > grindActiveUntil)
        deactivateGrind();
    if (mode != MODE_STANDBY && settings.getStandbyTimeout() > 0 && now > lastAction + settings.getStandbyTimeout())
        activateStandby();
}

void Controller::loopControl() {
    if (initialized) {
        updateControl();
    }
}

bool Controller::isUpdating() const { return updating; }

bool Controller::isAutotuning() const { return autotuning; }

bool Controller::isReady() const { return !isUpdating() && !isErrorState() && !isAutotuning(); }

ScaleAvailability Controller::scaleAvailability() const {
    ScaleAvailability a;
    a.bluetoothConnected = isBluetoothScaleHealthy();
    a.hardwarePresent = hardwareScalePresent;
    a.predictiveAvailable = true;
#ifdef GAGGIMATE_DISABLE_HARDWARE_SCALE
    a.hardwareCapable = false;
#else
    // The hardware scale physically lives on the STM32 controller; capability is
    // advertised in its INFO payload (false for BLE-connected controllers).
    a.hardwareCapable = systemInfo.capabilities.scale;
#endif
    return a;
}

bool Controller::isVolumetricAvailable() const {
    return ScaleSourceResolver::brewVolumetricAvailable(settings.getScaleSource(), scaleAvailability());
}

bool Controller::isGrindVolumetricAvailable() const {
    return ScaleSourceResolver::grindVolumetricAvailable(scaleAvailability());
}

#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
bool Controller::isHardwareScaleSampleHealthy(const ScaleSample &sample) const {
    static constexpr uint16_t BLOCKING_HEALTH =
        SCALE_HEALTH_NOT_CALIBRATED | SCALE_HEALTH_STALE | SCALE_HEALTH_TARE_FAILED | SCALE_HEALTH_SAT_CH1 |
        SCALE_HEALTH_SAT_CH2 | SCALE_HEALTH_TARING | SCALE_HEALTH_CALIBRATING;
    return hardwareScalePresent && std::isfinite(sample.weightG) && std::isfinite(sample.stddevG) &&
           std::fabs(sample.weightG) <= HARDWARE_SCALE_MAX_ABS_G && sample.stddevG <= HARDWARE_SCALE_MAX_STDDEV_G &&
           (sample.healthBits & BLOCKING_HEALTH) == 0;
}

void Controller::recordHardwareScaleBaselineSample(const ScaleSample &sample) {
    if (!isHardwareScaleSampleHealthy(sample)) {
        return;
    }
    hardwareScaleBaselineSamples[hardwareScaleBaselineSampleIndex] = sample.weightG;
    hardwareScaleBaselineSampleIndex =
        static_cast<uint8_t>((hardwareScaleBaselineSampleIndex + 1) % HARDWARE_SCALE_BASELINE_SAMPLE_COUNT);
    if (hardwareScaleBaselineSampleCount < HARDWARE_SCALE_BASELINE_SAMPLE_COUNT) {
        ++hardwareScaleBaselineSampleCount;
    }
}

bool Controller::captureHardwareScaleShotBaseline() {
    resetHardwareScaleShotBaseline();

    auto tryCaptureBaseline = [this](const ScaleSample &latest, bool allowLatestOnly) {
        const uint32_t latestAgeMs = millis() - lastHardwareScaleSampleMs;
        if (!isHardwareScaleSampleHealthy(latest) || latestAgeMs > HARDWARE_SCALE_SAMPLE_FRESH_MS) {
            return false;
        }

        float minSample = latest.weightG;
        float maxSample = latest.weightG;
        float sum = 0.0f;
        uint8_t count = 0;
        for (uint8_t i = 0; i < hardwareScaleBaselineSampleCount; i++) {
            const float sample = hardwareScaleBaselineSamples[i];
            if (std::isfinite(sample) && std::fabs(sample) <= HARDWARE_SCALE_MAX_ABS_G &&
                std::fabs(sample - latest.weightG) <= HARDWARE_SCALE_MAX_BASELINE_SPREAD_G) {
                minSample = std::min(minSample, sample);
                maxSample = std::max(maxSample, sample);
                sum += sample;
                ++count;
            }
        }
        if (count >= HARDWARE_SCALE_PREBREW_MIN_SAMPLES &&
            (maxSample - minSample) <= HARDWARE_SCALE_PREBREW_STABLE_SPREAD_G) {
            hardwareScaleShotBaseline = sum / static_cast<float>(count);
            hardwareScaleShotBaselineActive = true;
            ESP_LOGI(LOG_TAG, "Hardware scale pre-brew baseline captured: %.3f g (%u samples, spread %.3f g)",
                     hardwareScaleShotBaseline, count, maxSample - minSample);
            return true;
        }

        if (allowLatestOnly) {
            hardwareScaleShotBaseline = latest.weightG;
            hardwareScaleShotBaselineActive = true;
            ESP_LOGW(LOG_TAG, "Hardware scale baseline unstable; using latest sample %.3f g (%u samples)",
                     hardwareScaleShotBaseline, count);
            return true;
        }

        return false;
    };

    if (tryCaptureBaseline(getScaleSample(), false)) {
        return true;
    }

    const uint32_t startMs = millis();
    while (millis() - startMs <= HARDWARE_SCALE_PREBREW_STABILIZE_MS) {
#ifdef GAGGIMATE_UART_COMMS
        comms.loop();
#endif
        if (tryCaptureBaseline(getScaleSample(), false)) {
            return true;
        }
        delay(HARDWARE_SCALE_PREBREW_POLL_MS);
    }

    if (tryCaptureBaseline(getScaleSample(), true)) {
        return true;
    }

    if (!isHardwareScaleSampleHealthy(getScaleSample())) {
        ESP_LOGW(LOG_TAG, "Hardware scale shot baseline unavailable; no healthy fresh sample");
        return false;
    }

    ESP_LOGW(LOG_TAG, "Hardware scale shot baseline unavailable; sample is stale");
    return false;
}

void Controller::resetHardwareScaleShotBaseline() {
    hardwareScaleShotBaselineActive = false;
    hardwareScaleShotBaseline = 0.0f;
}
#endif

void Controller::autotune(int testTime, int samples, int heaterWattage) {
    if (isActive() || !isReady()) {
        return;
    }
    if (mode != MODE_STANDBY) {
        activateStandby();
    }
    autotuning = true;
    comms.sendAutotune(testTime, samples, heaterWattage);
    pluginManager->trigger("controller:autotune:start");
}

void Controller::startProcess(Process *process) {
    if (isActive() || !isReady()) {
        delete process;
        return;
    }
    {
        ProcessLock lock(processMutex);
        processCompleted = false;
        this->currentProcess = process;
    }
    applyConnectionPriority(); // shot started -> tight BLE interval
    pluginManager->trigger("controller:process:start");
    updateLastAction();
}

void Controller::applyConnectionPriority(bool force) {
    // A running process needs responsive 10Hz control; idle does not. Track the
    // last requested state so we only renegotiate on transitions.
    const bool lowLatency = currentProcess != nullptr;
    if (force || lowLatency != connLowLatency) {
        connLowLatency = lowLatency;
        comms.setLowLatency(lowLatency);
        // Steer the shared-radio coexistence arbiter to match. WiFi and BLE
        // share one 2.4GHz radio; the arbiter decides who wins on contention.
        // During a shot the BLE control loop (7.5-10ms interval, pressure/flow
        // feedback) must win, so prefer BT. When idle there is no tight BLE
        // deadline, so prefer WiFi to keep the web UI / network responsive --
        // the chronic coex failure mode is WiFi getting starved and the whole
        // IP stack wedging. Default coex preference is BALANCE; nobody set this
        // before. Best-effort: ignore the return (no-op if coex inactive). [GM-90]
#ifndef GAGGIMATE_UART_COMMS
        esp_coex_preference_set(lowLatency ? ESP_COEX_PREFER_BT : ESP_COEX_PREFER_WIFI);
#endif
    }
}

float Controller::getTargetTemp() const {
    Process *proc = currentProcess;
    switch (mode) {
    case MODE_BREW:
    case MODE_GRIND:
        if (proc != nullptr && proc->isActive() && proc->getType() == MODE_BREW) {
            auto brewProcess = static_cast<BrewProcess *>(proc);
            return brewProcess->getTemperature();
        }
        return profileManager->getSelectedProfile().temperature;
    case MODE_STEAM:
        return settings.getTargetSteamTemp();
    case MODE_WATER:
        return settings.getTargetWaterTemp();
    default:
        return 0;
    }
}

void Controller::setTargetTemp(float temperature) {
    pluginManager->trigger("boiler:targetTemperature:change", "value", temperature);
    switch (mode) {
    case MODE_BREW:
    case MODE_GRIND:
        profileManager->getSelectedProfile().temperature = temperature;
        break;
    case MODE_STEAM:
        settings.setTargetSteamTemp(static_cast<int>(temperature));
        break;
    case MODE_WATER:
        settings.setTargetWaterTemp(static_cast<int>(temperature));
        break;
    default:;
    }
    updateLastAction();
}

void Controller::setPressureScale(void) {
    if (systemInfo.capabilities.pressure) {
        comms.sendPressureScale(settings.getPressureScaling());
    }
}

void Controller::setPumpModelCoeffs(void) {
    if (systemInfo.capabilities.dimming) {
        // Default missing coeffs to NaN so a two-value "a,b" string keeps its
        // flow-measurement semantics (c,d NaN) on the controller side.
        float coeffs[4];
        parseFloatCsv(settings.getPumpModelCoeffs(), coeffs, 4, NAN);
        comms.sendPumpModelCoeffs(coeffs[0], coeffs[1], coeffs[2], coeffs[3]);
    }
}

int Controller::getTargetGrindDuration() const { return settings.getTargetGrindDuration(); }

void Controller::setTargetGrindDuration(int duration) {
    Event event = pluginManager->trigger("controller:grindDuration:change", "value", duration);
    settings.setTargetGrindDuration(event.getInt("value"));
    updateLastAction();
}

void Controller::setTargetGrindVolume(double volume) {
    Event event = pluginManager->trigger("controller:grindVolume:change", "value", static_cast<float>(volume));
    settings.setTargetGrindVolume(event.getFloat("value"));
    updateLastAction();
}

void Controller::raiseTemp() {
    float temp = getTargetTemp();
    temp = constrain(temp + 1.0f, MIN_TEMP, MAX_TEMP);
    setTargetTemp(temp);
}

void Controller::lowerTemp() {
    float temp = getTargetTemp();
    temp = constrain(temp - 1.0f, MIN_TEMP, MAX_TEMP);
    setTargetTemp(temp);
}

void Controller::raiseBrewTarget() {
    if (isVolumetricAvailable() && profileManager->getSelectedProfile().isVolumetric()) {
        profileManager->getSelectedProfile().adjustVolumetricTarget(1);
    } else {
        profileManager->getSelectedProfile().adjustDuration(1);
    }
    handleProfileUpdate();
}

void Controller::lowerBrewTarget() {
    if (isVolumetricAvailable() && profileManager->getSelectedProfile().isVolumetric()) {
        profileManager->getSelectedProfile().adjustVolumetricTarget(-1);
    } else {
        profileManager->getSelectedProfile().adjustDuration(-1);
    }
    handleProfileUpdate();
}

void Controller::raiseGrindTarget() {
    if (settings.isVolumetricTarget() && isGrindVolumetricAvailable()) {
        double newTarget = settings.getTargetGrindVolume() + 0.5;
        if (newTarget > BREW_MAX_VOLUMETRIC) {
            newTarget = BREW_MAX_VOLUMETRIC;
        }
        setTargetGrindVolume(newTarget);
    } else {
        int newDuration = getTargetGrindDuration() + 1000;
        if (newDuration > BREW_MAX_DURATION_MS) {
            newDuration = BREW_MAX_DURATION_MS;
        }
        setTargetGrindDuration(newDuration);
    }
}

void Controller::lowerGrindTarget() {
    if (settings.isVolumetricTarget() && isGrindVolumetricAvailable()) {
        double newTarget = settings.getTargetGrindVolume() - 0.5;
        if (newTarget < BREW_MIN_VOLUMETRIC) {
            newTarget = BREW_MIN_VOLUMETRIC;
        }
        setTargetGrindVolume(newTarget);
    } else {
        int newDuration = getTargetGrindDuration() - 1000;
        if (newDuration < BREW_MIN_DURATION_MS) {
            newDuration = BREW_MIN_DURATION_MS;
        }
        setTargetGrindDuration(newDuration);
    }
}

void Controller::updateControl() {
    // Never drive a controller whose protocol version we don't match -- the
    // commands could be misinterpreted (OTA recovery still works; see onSystemInfo).
    if (systemInfo.protocolMismatch) {
        return;
    }

    bool altRelayActive = false;
    bool useAdvancedOutput = false;
    bool outputValve = false;
    bool pressureTarget = false;
    float pumpSetpoint = 0.0f;
    float advancedPressure = 0.0f;
    float advancedFlow = 0.0f;
    float targetTemp = 0.0f;

    // Snapshot everything that needs currentProcess while the mutex is held.
    // UART writes happen after the lock is released, so process start/stop is
    // not blocked behind a potentially slow serial send.
    {
        ProcessLock lock(processMutex);
        Process *proc = currentProcess;
        const bool active = (proc != nullptr) && proc->isActive();

        targetTemp = getTargetTemp();
        if (targetTemp > .0f) {
            targetTemp = targetTemp + static_cast<float>(settings.getTemperatureOffset());
        }

        if (active && proc->isAltRelayActive()) {
            if (proc->getType() == MODE_GRIND && settings.getAltRelayFunction() == ALT_RELAY_GRIND) {
                altRelayActive = true;
            }
        }

        if (active && systemInfo.capabilities.pressure) {
            if (proc->getType() == MODE_STEAM) {
                useAdvancedOutput = true;
                advancedPressure = settings.getSteamPumpCutoff();
                advancedFlow = proc->getPumpValue() * 0.1f;
            } else if (proc->getType() == MODE_BREW) {
                auto *brewProcess = static_cast<BrewProcess *>(proc);
                if (brewProcess->isAdvancedPump()) {
                    useAdvancedOutput = true;
                    outputValve = brewProcess->isRelayActive();
                    pressureTarget = brewProcess->getPumpTarget() == PumpTarget::PUMP_TARGET_PRESSURE;
                    advancedPressure = brewProcess->getPumpPressure();
                    advancedFlow = brewProcess->getPumpFlow();
                }
            }
        }

        if (!useAdvancedOutput) {
            outputValve = active && proc->isRelayActive();
            pumpSetpoint = active ? proc->getPumpValue() : 0.0f;
        }
    }

    // Build the per-component commands, then deliver boiler + pump + valve + alt
    // together in a single batched frame so the controller applies them as one
    // atomic update.
    BoilerCommand boiler;
    boiler.index = 0;
    boiler.setpoint = targetTemp;

    PumpCommand pump;
    pump.index = 0;

    RelayCommand relay;
    relay.index = 0; // brew valve
    relay.open = outputValve;

    if (useAdvancedOutput) {
        targetPressure = advancedPressure;
        targetFlow = advancedFlow;
        pump.mode = pressureTarget ? PumpControlMode::Pressure : PumpControlMode::Flow;
        pump.pressure = advancedPressure;
        pump.flow = advancedFlow;
    } else {
        targetPressure = 0.0f;
        targetFlow = 0.0f;
        pump.mode = PumpControlMode::Power;
        pump.power = pumpSetpoint;
    }

    // Only send components that changed since the last update. The controller is
    // stateful and every message is acknowledged, so re-sending unchanged values
    // each cycle is unnecessary; a periodic ping (see loop()) keeps the watchdog
    // fed when nothing changes. controlStateSent is reset on (re)connect to force
    // a full resend.
    gm::Payload batch[4];
    size_t count = 0;
    if (!controlStateSent || boiler != lastBoiler)
        batch[count++] = comms.buildBoilerControl(boiler.index, boiler.mode, boiler.setpoint);
    if (!controlStateSent || pump != lastPump)
        batch[count++] = comms.buildPumpControl(pump.index, pump.mode, pump.power, pump.pressure, pump.flow);
    if (!controlStateSent || relay != lastRelay)
        batch[count++] = comms.buildRelayControl(relay.index, relay.open); // index 0 = brew valve
    if (!controlStateSent || altRelayActive != lastAlt)
        batch[count++] = comms.buildRelayControl(1, altRelayActive); // index 1 = alt relay

    if (count > 0)
        comms.sendBatch(batch, count);

    lastBoiler = boiler;
    lastPump = pump;
    lastRelay = relay;
    lastAlt = altRelayActive;
    controlStateSent = true;
}

void Controller::activate() {
    const bool wasActive = isActive();
    ESP_LOGI(LOG_TAG, "activate entry: mode=%d isActive=%d src=%d volumetricAvailable=%d hwScalePresent=%d", mode, wasActive,
             settings.getScaleSource(), isVolumetricAvailable(), hardwareScalePresent);
    if (wasActive)
        return;
    clear();
    // clear() already resets this under the process lock, but state in activate()
    // is the operative invariant for the rest of this function — keep it explicit.
    currentVolumetricSource = VolumetricMeasurementSource::INACTIVE;

    // Resolve which source this brew will use. Explicit selection wins when
    // available; otherwise fall back predictably (selected → hardware →
    // predictive) so a brew never blocks on an unavailable scale. Only an
    // explicit OFF (or a non-brew mode) leaves the source INACTIVE → timed brew.
    const int brewSrcSetting = settings.getScaleSource();
    VolumetricMeasurementSource resolved = VolumetricMeasurementSource::INACTIVE;
    if (mode == MODE_BREW) {
        resolved = ScaleSourceResolver::resolveBrewSource(brewSrcSetting, scaleAvailability());
        // A hardware-scale brew needs its software shot baseline (the pre-brew
        // cup weight). If no healthy sample is available, fall back to predictive
        // pump-flow rather than blocking the brew. Done before the tare decision
        // below so the fallback still gets its pump-flow tare.
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
        if (resolved == VolumetricMeasurementSource::HARDWARE_SCALE && !captureHardwareScaleShotBaseline()) {
            ESP_LOGW(LOG_TAG, "HW scale baseline unavailable; falling back to predictive flow for this brew");
            resolved = VolumetricMeasurementSource::FLOW_ESTIMATION;
        }
#endif
    }

    // Tare the pump-flow estimator for every source except the hardware scale.
    // BLE/predictive brews want the estimator zeroed at the start; a HW brew uses
    // the software baseline captured above, and a pump/hardware tare here would
    // race it (post-tare samples read ≈ 0 g while the baseline still holds the
    // cup weight, so weight − baseline clamps to 0 for the whole shot).
    if (resolved == VolumetricMeasurementSource::HARDWARE_SCALE) {
        ESP_LOGI(LOG_TAG, "activate: skipping pump-flow tare (HW scale uses software shot baseline)");
    } else {
        comms.tare();
    }
    currentVolumetricSource = resolved;
    ESP_LOGI(LOG_TAG, "activate: mode=%d brewSrcSetting=%d resolvedSource=%d", mode, brewSrcSetting, static_cast<int>(resolved));

    if (mode == MODE_BREW) {
        // Fire unconditionally so plugins that pre-arm on brew start (e.g.
        // BLE scale auto-tare) run regardless of volumetric availability.
        pluginManager->trigger("controller:brew:prestart");
    }
    delay(200);
    switch (mode) {
    case MODE_BREW:
        startProcess(new BrewProcess(profileManager->getSelectedProfile(),
                                     profileManager->getSelectedProfile().isVolumetric() &&
                                             currentVolumetricSource != VolumetricMeasurementSource::INACTIVE
                                         ? ProcessTarget::VOLUMETRIC
                                         : ProcessTarget::TIME,
                                     settings.getBrewDelay()));
        break;
    case MODE_STEAM:
        startProcess(new SteamProcess(STEAM_SAFETY_DURATION_MS, settings.getSteamPumpPercentage()));
        break;
    case MODE_WATER:
        startProcess(new PumpProcess());
        break;
    default:;
    }
    bool startedBrew = false;
    int procType = -1;
    {
        ProcessLock lock(processMutex);
        Process *proc = currentProcess;
        startedBrew = proc != nullptr && proc->getType() == MODE_BREW;
        procType = proc != nullptr ? proc->getType() : -1;
    }
    ESP_LOGI(LOG_TAG, "activate after startProcess: procType=%d startedBrew=%d", procType, startedBrew);
    if (startedBrew) {
        pluginManager->trigger("controller:brew:start");
    }
}

void Controller::deactivate() {
    int swappedType = -1;
    {
        ProcessLock lock(processMutex);
        if (currentProcess == nullptr) {
            return;
        }
        delete lastProcess;
        lastProcess = currentProcess;
        currentProcess = nullptr;
        swappedType = lastProcess->getType();
    }
    applyConnectionPriority(); // shot ended -> relaxed BLE interval
    if (swappedType == MODE_BREW) {
        currentVolumetricSource = VolumetricMeasurementSource::INACTIVE;
        // Keep the hardware shot baseline alive after pump stop so UI and shot
        // history can continue showing/recording settled shot-relative weight.
        // clear(), mode change, flush, and next activate() reset it.
        pluginManager->trigger("controller:brew:end");
    } else if (swappedType == MODE_GRIND) {
        pluginManager->trigger("controller:grind:end");
    }
    pluginManager->trigger("controller:process:end");
    updateLastAction();
}

void Controller::clear() {
    bool wasBrew = false;
    {
        ProcessLock lock(processMutex);
        processCompleted = true;
        if (lastProcess != nullptr && lastProcess->getType() == MODE_BREW) {
            wasBrew = true;
        }
        delete lastProcess;
        lastProcess = nullptr;
        currentVolumetricSource = VolumetricMeasurementSource::INACTIVE;
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
        resetHardwareScaleShotBaseline();
#endif
    }
    if (wasBrew) {
        pluginManager->trigger("controller:brew:clear");
    }
}

void Controller::activateGrind() {
    if (isGrindActive())
        return;
    clear();
    // Grind-by-weight can only use a movable Bluetooth scale. The hardware scale
    // is fixed in the brew path, and predictive pump-flow is meaningless while
    // grinding.
    VolumetricMeasurementSource resolved = ScaleSourceResolver::resolveGrindSource(scaleAvailability());
    if (settings.isVolumetricTarget() && resolved != VolumetricMeasurementSource::INACTIVE) {
        currentVolumetricSource = resolved;
        startProcess(new GrindProcess(ProcessTarget::VOLUMETRIC, 0, settings.getTargetGrindVolume(), settings.getGrindDelay()));
    } else {
        currentVolumetricSource = VolumetricMeasurementSource::INACTIVE;
        startProcess(
            new GrindProcess(ProcessTarget::TIME, settings.getTargetGrindDuration(), settings.getTargetGrindVolume(), 0.0));
    }
    pluginManager->trigger("controller:grind:start");
}

void Controller::deactivateGrind() {
    deactivate();
    clear();
}

void Controller::activateStandby() {
    setMode(MODE_STANDBY);
    deactivate();
}

void Controller::deactivateStandby() {
    deactivate();
    setMode(MODE_BREW);
}

bool Controller::isActive() const {
    ProcessLock lock(processMutex);
    Process *proc = currentProcess;
    return proc != nullptr && proc->isActive();
}

bool Controller::isGrindActive() const {
    ProcessLock lock(processMutex);
    Process *proc = currentProcess;
    return proc != nullptr && proc->isActive() && proc->getType() == MODE_GRIND;
}

int Controller::getMode() const { return mode; }

void Controller::setMode(int newMode) {
    Event modeEvent = pluginManager->trigger("controller:mode:change", "value", newMode);
    mode = modeEvent.getInt("value");
    steamReady = false;
    if (mode != MODE_BREW) {
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
        resetHardwareScaleShotBaseline();
#endif
    }

    updateLastAction();
    setTargetTemp(getTargetTemp());
}

void Controller::onTempRead(float temperature) {
    float temp = temperature - static_cast<float>(settings.getTemperatureOffset());
    Event event = pluginManager->trigger("boiler:currentTemperature:change", "value", temp);
    currentTemp.store(event.getFloat("value"), std::memory_order_relaxed);
}

void Controller::updateLastAction() { lastAction = millis(); }

void Controller::onOTAUpdate() {
    activateStandby();
    updating = true;
}

void Controller::onProfileSave() const { profileManager->saveProfile(profileManager->getSelectedProfile()); }

void Controller::onProfileSaveAsNew() {
    Profile &profile = profileManager->getSelectedProfile();
    profile.label = "Copy of " + profileManager->getSelectedProfile().label;
    profile.id = generateShortID();
    settings.setSelectedProfile(profile.id);
    profileManager->saveProfile(profileManager->getSelectedProfile());
    profileManager->addFavoritedProfile(profile.id);
}

void Controller::onVolumetricMeasurement(double measurement, VolumetricMeasurementSource source) {
#ifdef GAGGIMATE_DISABLE_HARDWARE_SCALE
    if (source == VolumetricMeasurementSource::HARDWARE_SCALE) {
        return;
    }
#endif
    const __FlashStringHelper *eventName = F("controller:volumetric-measurement:bluetooth:change");
    if (source == VolumetricMeasurementSource::BLUETOOTH) {
        lastBluetoothMeasurement = millis();
    }
    if (source == VolumetricMeasurementSource::FLOW_ESTIMATION) {
        eventName = F("controller:volumetric-measurement:estimation:change");
    } else if (source == VolumetricMeasurementSource::HARDWARE_SCALE) {
        eventName = F("controller:volumetric-measurement:hardware:change");
    }
    pluginManager->trigger(eventName, "value", static_cast<float>(measurement));
    double processMeasurement = measurement;
    if (source == VolumetricMeasurementSource::HARDWARE_SCALE) {
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
        if (!hardwareScaleShotBaselineActive) {
            if (currentVolumetricSource == source) {
                ESP_LOGD(LOG_TAG, "Ignoring hardware scale measurement without shot baseline");
                return;
            }
        } else {
            processMeasurement = std::max(0.0, measurement - static_cast<double>(hardwareScaleShotBaseline));
            if (processMeasurement > HARDWARE_SCALE_MAX_SHOT_G) {
                ESP_LOGW(LOG_TAG, "Ignoring hardware scale shot outlier: raw=%.3f baseline=%.3f shot=%.3f",
                         static_cast<float>(measurement), hardwareScaleShotBaseline, static_cast<float>(processMeasurement));
                return;
            }
            pluginManager->trigger(F("controller:volumetric-measurement:hardware-shot:change"), "value",
                                   static_cast<float>(processMeasurement));
        }
#else
        return;
#endif
    }
    if (currentVolumetricSource != source) {
        ESP_LOGD(LOG_TAG, "Ignoring volumetric measurement, source does not match");
        return;
    }
    ProcessLock lock(processMutex);
    Process *proc = currentProcess;
    Process *last = lastProcess;
    if (proc != nullptr) {
        proc->updateVolume(processMeasurement);
    }
    if (last != nullptr && !last->isComplete()) {
        last->updateVolume(processMeasurement);
    }
}

void Controller::scaleTare() {
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    comms.scaleTare();
#endif
}

void Controller::sendScaleCalibration(float c1, float c2) {
#ifdef GAGGIMATE_DISABLE_HARDWARE_SCALE
    (void)c1;
    (void)c2;
#else
    comms.sendScaleCalibration(c1, c2, settings.getScaleOffset1(), settings.getScaleOffset2());
#endif
}


ScaleSample Controller::getScaleSample() const {
    ScaleSample s{};
    if (scaleSampleMutex && xSemaphoreTake(scaleSampleMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s = lastScaleSample;
        xSemaphoreGive(scaleSampleMutex);
    }
    return s;
}
#endif

bool Controller::isBluetoothScaleHealthy() const {
    // BLE is usable for weight-based routines only while it is actively
    // publishing measurements; a stale connected scale should fall back.
    const unsigned long now = millis();
    return BLEScales.isConnected() && lastBluetoothMeasurement != 0 &&
           now - lastBluetoothMeasurement <= BLUETOOTH_MEASUREMENT_GRACE_MS;
}

void Controller::onFlush() {
    if (isActive()) {
        return;
    }
    clear();
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    resetHardwareScaleShotBaseline();
#endif
    startProcess(new BrewProcess(FLUSH_PROFILE, ProcessTarget::TIME, settings.getBrewDelay()));
    pluginManager->trigger("controller:brew:start");
}

void Controller::onVolumetricDelete() {
    if (profileManager->getSelectedProfile().isVolumetric()) {
        profileManager->getSelectedProfile().removeVolumetricTarget();
    }
}

void Controller::handleBrewButton(int brewButtonStatus) {
    ESP_LOGI(LOG_TAG, "handleBrewButton: mode=%d status=%d isActive=%d src=%d", getMode(), brewButtonStatus, isActive(),
             settings.getScaleSource());
    if (brewButtonStatus) {
        switch (getMode()) {
        case MODE_STANDBY:
            deactivateStandby();
            break;
        case MODE_BREW:
            if (!isActive()) {
                deactivateStandby();
                clear();
                activate();
            } else if (settings.isMomentaryButtons()) {
                deactivate();
                clear();
            }
            break;
        case MODE_WATER:
            activate();
            break;
        case MODE_STEAM:
            deactivate();
            setMode(MODE_BREW);
        default:
            break;
        }
    } else if (!settings.isMomentaryButtons()) {
        if (getMode() == MODE_BREW) {
            if (isActive()) {
                deactivate();
                clear();
            } else {
                clear();
            }
        } else if (getMode() == MODE_WATER) {
            deactivate();
        }
    }
}

void Controller::handleSteamButton(int steamButtonStatus) {
    if (steamButtonStatus) {
        if (getMode() != MODE_STEAM) {
            setMode(MODE_STEAM);
        }
    } else if (!settings.isMomentaryButtons() && getMode() == MODE_STEAM) {
        deactivate();
        setMode(MODE_BREW);
    }
}

void Controller::handleWaterButton(int buttonStatus) {
    if (buttonStatus) {
        switch (getMode()) {
        case MODE_WATER:
            if (!isActive()) {
                activate();
            }
            break;
        default:
            setMode(MODE_WATER);
            break;
        }
    } else if (!settings.isMomentaryButtons() && getMode() == MODE_WATER && isActive()) {
        deactivate();
    }
}

void Controller::handleProfileButton(int buttonStatus, String id) {
    if (buttonStatus && getMode() == MODE_STANDBY) {
        deactivateStandby();
        return;
    }
    if (!buttonStatus && !settings.isMomentaryButtons()) {
        deactivate();
        clear();
    }
    if (buttonStatus) {
        if (getMode() != MODE_BREW) {
            setMode(MODE_BREW);
        }
        if (isActive()) {
            deactivate();
            clear();
            return;
        }
        std::vector<String> profileIds = profileManager->listProfiles();
        if (std::find(profileIds.begin(), profileIds.end(), id) != profileIds.end()) {
            profileManager->selectProfile(id);
            activate();
        }
    }
}

void Controller::handleProfileUpdate() {
    pluginManager->trigger("boiler:targetTemperature:change", "value", profileManager->getSelectedProfile().temperature);
    pluginManager->trigger("controller:targetDuration:change", "value", profileManager->getSelectedProfile().getTotalDuration());
    pluginManager->trigger("controller:targetVolume:change", "value", profileManager->getSelectedProfile().getTotalVolume());
}

void Controller::loopTask(void *arg) {
    TickType_t lastWake = xTaskGetTickCount();
    auto *controller = static_cast<Controller *>(arg);
    while (true) {
        esp_task_wdt_reset();
        controller->loopControl();
        xTaskDelayUntil(&lastWake, pdMS_TO_TICKS(controller->getMode() == MODE_STANDBY ? 1000 : PROGRESS_INTERVAL));
    }
}

void Controller::loopLogicTask(void *arg) {
    TickType_t lastWake = xTaskGetTickCount();
    auto *controller = static_cast<Controller *>(arg);
    while (true) {
        controller->loopLogic();
        xTaskDelayUntil(&lastWake, pdMS_TO_TICKS(controller->getMode() == MODE_STANDBY ? 1000 : PROGRESS_INTERVAL));
    }
}
