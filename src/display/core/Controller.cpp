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
#include <display/core/PredictiveDelayPolicy.h>
#include <display/core/constants.h>
#include <display/core/process/BrewProcess.h>
#include <display/core/process/GrindProcess.h>
#include <display/core/process/PumpProcess.h>
#include <display/core/process/SteamProcess.h>
#include <display/core/static_profiles.h>
#include <display/core/zones.h>
#include <display/plugins/AutoWakeupPlugin.h>
#ifndef GAGGIMATE_SIM
#include <display/plugins/AutoTuningCapturePlugin.h>
#include <display/plugins/CommunityUploadPlugin.h>
#endif
#include <display/plugins/BoilerFillPlugin.h>
#include <display/plugins/LedControlPlugin.h>
#include <display/plugins/LocalAutoTuningStorePlugin.h>
#include <display/plugins/ShotHistoryPlugin.h>
#include <display/plugins/SmartGrindPlugin.h>
#include <display/plugins/WebUIPlugin.h>
#ifndef GAGGIMATE_SIM // network/BLE plugins are device-only
#include <display/plugins/BLEScalePlugin.h>
#include <display/plugins/HomekitPlugin.h>
#include <display/plugins/ImprovPlugin.h>
#include <display/plugins/MQTTPlugin.h>
#include <display/plugins/NetworkWatchdogPlugin.h>
#ifndef GAGGIMATE_HEADLESS
#include <display/plugins/AutoTuningPreferencePlugin.h>
#endif
#include <display/plugins/WifiStaWatchdogPlugin.h>
#include <display/plugins/mDNSPlugin.h>
#endif
#if defined(GAGGIMATE_UART_COMMS) && !defined(GAGGIMATE_DISABLE_HARDWARE_SCALE) && !defined(GAGGIMATE_SIM)
#include <display/plugins/HWScalePlugin.h>
#endif
#include <display/util/PsramAllocator.h>
#ifndef GAGGIMATE_HEADLESS
#ifdef GAGGIMATE_SIM
#include <SdlDriver.h> // desktop SDL panel stands in for the hardware drivers
#else
#include <Preferences.h>
#include <display/drivers/AmoledDisplayDriver.h>
#include <display/drivers/LilyGoDriver.h>
#include <display/drivers/WaveshareDriver.h>
#endif
#endif

const String LOG_TAG = F("Controller");

namespace {
// Only genuine safety/runtime faults latch the controller into an error state
// (which blocks brewing until cleared). The STM32 reports exactly one such fault
// today: thermal runaway. Everything else arriving via EVT,ERR (framing/transport
// glitches, an unknown/garbled command line) is a transient comms hiccup and must
// not stop the machine — see registerRemoteErrorCallback.
inline bool isLatchingError(int code) { return code == static_cast<int>(ERROR_CODE_RUNAWAY); }
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
        return;
    }
    if (logicTaskHandle != nullptr)
        xTaskNotifyGive(logicTaskHandle);
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
        case CtrlCmd::DEACTIVATE_CLEAR:
            deactivate();
            clear();
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
        case CtrlCmd::CHANGE_MODE:
            deactivate();
            clear();
            setMode(static_cast<int>(msg.arg));
            break;
        case CtrlCmd::START_FLUSH:
            onFlush();
            break;
        case CtrlCmd::BUTTON_STATE:
            handleControllerButton(static_cast<uint8_t>((msg.arg >> 1) & 0xFF), (msg.arg & 1) != 0);
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
#ifndef GAGGIMATE_SIM // mDNS/HomeKit are device-only
    if (settings.isHomekit())
        pluginManager->registerPlugin(new HomekitPlugin(settings.getWifiSsid(), settings.getWifiPassword()));
    else
        pluginManager->registerPlugin(new mDNSPlugin());
#endif
    if (settings.isBoilerFillActive()) {
        pluginManager->registerPlugin(new BoilerFillPlugin());
    }
    if (settings.isSmartGrindActive()) {
        pluginManager->registerPlugin(new SmartGrindPlugin());
    }
#ifndef GAGGIMATE_SIM // MQTT/HomeAssistant is device-only
    // The optimizer transport must exist even when legacy Home Assistant MQTT
    // is disabled, including when off-board optimization is enabled at runtime.
    pluginManager->registerPlugin(new MQTTPlugin());
#ifndef GAGGIMATE_HEADLESS
    pluginManager->registerPlugin(new AutoTuningPreferencePlugin());
#endif
#endif
    pluginManager->registerPlugin(new WebUIPlugin());
#ifndef GAGGIMATE_SIM
    pluginManager->registerPlugin(&AutoTuningCapture);
#endif
    pluginManager->registerPlugin(&LocalAutoTuningStore);
#ifndef GAGGIMATE_SIM
    pluginManager->registerPlugin(new CommunityUploadPlugin());
#endif
#ifndef GAGGIMATE_SIM // WiFi watchdogs and BLE scales are device-only
    pluginManager->registerPlugin(new NetworkWatchdogPlugin());
    pluginManager->registerPlugin(new WifiStaWatchdogPlugin());
    pluginManager->registerPlugin(new ImprovPlugin());
#endif
    pluginManager->registerPlugin(&ShotHistory);
#ifndef GAGGIMATE_SIM
    pluginManager->registerPlugin(&BLEScales);
#if defined(GAGGIMATE_UART_COMMS) && !defined(GAGGIMATE_DISABLE_HARDWARE_SCALE)
    pluginManager->registerPlugin(&HWScale);
#endif
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
        postCommand(CtrlCmd::DEACTIVATE_CLEAR);
    });

#ifndef GAGGIMATE_HEADLESS
    ui->init();
#endif
    this->onScreenReady();

    updateLastAction();
    xTaskCreatePinnedToCore(loopLogicTask, "Controller::loopLogic", configMINIMAL_STACK_SIZE * 6, this, 3, &logicTaskHandle, 0);
    esp_task_wdt_add(logicTaskHandle);
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
// NVS values for the cached panel detection result (GM-140) — only append, never renumber
enum PanelModel : uint8_t { PANEL_UNKNOWN = 0, PANEL_LILYGO = 1, PANEL_AMOLED = 2, PANEL_WAVESHARE = 3 };

void Controller::setupPanel() {
#ifdef GAGGIMATE_SIM
    driver = SdlDriver::getInstance(); // desktop SDL panel
    driver->init();
#else
    // The panel can't change after flashing, so cache the detection result in NVS
    // and skip the multi-second probing chain on subsequent boots (GM-140).
    Preferences panelPrefs;
    panelPrefs.begin("panel", false);
    uint8_t model = panelPrefs.getUChar("driver", PANEL_UNKNOWN);
    if (model != PANEL_UNKNOWN) {
        // Drop the cache before init so a crash here falls back to full detection
        panelPrefs.remove("driver");
        switch (model) {
        case PANEL_LILYGO:
            driver = LilyGoDriver::getInstance();
            break;
        case PANEL_AMOLED:
            if (AmoledDisplayDriver::getInstance()->selectVariant(panelPrefs.getChar("variant", -1)))
                driver = AmoledDisplayDriver::getInstance();
            break;
        case PANEL_WAVESHARE:
            driver = WaveshareDriver::getInstance();
            break;
        }
    }
    if (driver == nullptr) {
        if (LilyGoDriver::getInstance()->isCompatible()) {
            driver = LilyGoDriver::getInstance();
            model = PANEL_LILYGO;
        } else if (AmoledDisplayDriver::getInstance()->isCompatible()) {
            driver = AmoledDisplayDriver::getInstance();
            model = PANEL_AMOLED;
            panelPrefs.putChar("variant", AmoledDisplayDriver::getInstance()->getVariant());
        } else if (WaveshareDriver::getInstance()->isCompatible()) {
            driver = WaveshareDriver::getInstance();
            model = PANEL_WAVESHARE;
        } else {
            Serial.println("No compatible display driver found");
            delay(10000);
            ESP.restart();
        }
    }
    driver->init();
    panelPrefs.putUChar("driver", model);
    panelPrefs.end();
#endif
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

    // Size the RX ring generously: the display can be busy with WebUI/MQTT/plugin
    // work between UART drains, so the default 256 B buffer is too easy to
    // overflow during STM32 telemetry bursts. Must be called before begin().
    Serial2.setRxBufferSize(2048);
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
    comms.onSystemInfo([this](const char *hardware, const char *version, uint32_t protocolVersion, bool dimming, bool pressure,
                              bool ledControl, bool tof, bool scale, std::vector<uint32_t> addons) {
        onSystemInfo(hardware, version, protocolVersion, dimming, pressure, ledControl, tof, scale, addons);
    });
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
    comms.onSensorData([this](float temp, float pressure, float puckFlow, float pumpFlow, float puckResistance, float pumpPower,
                              float heaterPower) {
        onTempRead(temp);
        this->pressure.store(pressure, std::memory_order_relaxed);
        this->currentPuckFlow = puckFlow;
        this->currentPumpFlow.store(pumpFlow, std::memory_order_relaxed);
        this->currentPumpPower = pumpPower;
        this->currentHeaterPower = heaterPower;
        this->currentPuckResistance = puckResistance;
        pluginManager->trigger("boiler:pressure:change", "value", pressure);
        pluginManager->trigger("pump:puck-flow:change", "value", puckFlow);
        pluginManager->trigger("pump:flow:change", "value", pumpFlow);
        pluginManager->trigger("pump:puck-resistance:change", "value", puckResistance);
    });
    comms.onButtonState([this](uint8_t index, bool pressed) {
        const int32_t packed = (static_cast<int32_t>(index) << 1) | (pressed ? 1 : 0);
        postCommand(CtrlCmd::BUTTON_STATE, packed);
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
        // Liveness timeouts are handled by the ping logic and never latch.
        if (error == static_cast<int>(ERROR_CODE_TIMEOUT)) {
            return;
        }
        // Transport/protocol glitches (framing errors, an unknown/garbled command
        // line — far more frequent on hardware-scale builds because of the HX711
        // bus timing) must NOT be treated as a latching safety fault. Historically
        // EVT,ERR,UNKNOWN_CMD shared code 4 with RUNAWAY, so one bad command would
        // wedge brewing until a power cycle. Log + count for visibility; do not
        // stop the machine.
        if (!isLatchingError(error)) {
            static uint32_t transportErrCount = 0;
            ++transportErrCount;
            ESP_LOGW(LOG_TAG, "Ignoring non-latching remote error %d (transport glitch; count=%lu)", error,
                     static_cast<unsigned long>(transportErrCount));
            return;
        }
        if (error != this->error) {
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
    comms.onScaleSample([this](const ScaleSample &sample) { onHardwareScaleSample(sample); });
    comms.onWeightMeasurement([this](float weight) {
        ScaleSample sample{};
        sample.weightG = weight;
        sample.stddevG = 0.0f;
        sample.ch1G = weight;
        sample.ch2G = 0.0f;
        sample.ch1StdG = 0.0f;
        sample.ch2StdG = 0.0f;
        sample.healthBits = SCALE_HEALTH_OK;
        onHardwareScaleSample(sample);
    });
    comms.onScaleOffsets([this](long offset1, long offset2) {
        settings.setScaleOffset1(offset1);
        settings.setScaleOffset2(offset2);
        ESP_LOGI(LOG_TAG, "Scale offsets received and saved: %ld, %ld", offset1, offset2);
        Event ev;
        ev.id = "controller:scale:tare:done";
        ev.setInt("success", 1);
        ev.setFloat("offset1", static_cast<float>(offset1));
        ev.setFloat("offset2", static_cast<float>(offset2));
        ev.setFloat("std1", 0.0f);
        ev.setFloat("std2", 0.0f);
        ev.setInt("healthBits", SCALE_HEALTH_OK);
        markHardwareScaleBrewTareDone();
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
        Event ev;
        ev.id = "controller:scale:cal:done";
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
                              bool ledControl, bool tof, bool scale, std::vector<uint32_t> addons) {
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
                                    .addons = addons,
                                },
                            .protocolVersion = protocolVersion,
                            .protocolMismatch = mismatch};
#ifdef GAGGIMATE_DISABLE_HARDWARE_SCALE
    hardwareScalePresent = false;
#else
    hardwareScalePresent = scale;
#endif
    ESP_LOGI(LOG_TAG, "System info: %s %s (proto=%u local=%u dm=%d ps=%d led=%d tof=%d scale=%d)", hardware, version,
             protocolVersion, gm_proto::PROTOCOL_VERSION, dimming, pressure, ledControl, tof, scale);
    if (mismatch) {
        ESP_LOGW(LOG_TAG, "Protocol version mismatch: controller=%u display=%u -- control inhibited, OTA only", protocolVersion,
                 gm_proto::PROTOCOL_VERSION);
        pluginManager->trigger("controller:protocol:mismatch", "value", static_cast<int>(protocolVersion));
    } else {
        setPressureScale();
        setPidSettings();
        setPumpModelCoeffs();
        configResendUntil = millis() + CONFIG_RESEND_WINDOW_MS;
        lastConfigResend = millis();
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
        setMode(settings.getStartupMode());
    }
    pluginManager->trigger("controller:bluetooth:connect");
}

void Controller::onIncompatibleController(const String &infoJson) {
    waitingForController = false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, infoJson);
    if (err) {
        ESP_LOGW(LOG_TAG, "Incompatible controller, no readable info (%s)", err.c_str());
        onSystemInfo("Legacy controller", "0.0.0", 0, false, false, false, false, false, {});
        return;
    }
    String hardware = doc["hw"].as<String>();
    String version = doc["v"].as<String>();
    if (hardware.isEmpty())
        hardware = "Legacy controller";
    if (version.isEmpty())
        version = "0.0.0";
    onSystemInfo(hardware.c_str(), version.c_str(), 0, doc["cp"]["dm"].as<bool>(), doc["cp"]["ps"].as<bool>(),
                 doc["cp"]["led"].as<bool>(), doc["cp"]["tof"].as<bool>(), doc["cp"]["sc"].as<bool>(), {});
}

void Controller::setupWifi() {
    // Generate and persist a WPA2 AP password on first start
    if (settings.getWifiApPassword().isEmpty()) {
        settings.setWifiApPassword(generateShortID(DEFAULT_WIFI_AP_PASSWORD_LENGTH));
    }

    if (settings.getWifiSsid() != "" && settings.getWifiPassword() != "") {
        WiFi.setHostname(settings.getMdnsName().c_str());
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);

        WiFi.onEvent(
            [this](WiFiEvent_t, WiFiEventInfo_t info) {
                const auto &g = info.got_ip.ip_info;
                const uint32_t ip = g.ip.addr;
                const uint32_t gw = g.gw.addr;
                ESP_LOGI(LOG_TAG, "STA got IP: %u.%u.%u.%u gw=%u.%u.%u.%u", (unsigned)(ip & 0xff), (unsigned)((ip >> 8) & 0xff),
                         (unsigned)((ip >> 16) & 0xff), (unsigned)((ip >> 24) & 0xff), (unsigned)(gw & 0xff),
                         (unsigned)((gw >> 8) & 0xff), (unsigned)((gw >> 16) & 0xff), (unsigned)((gw >> 24) & 0xff));
                wifiConnectedPending = true;
            },
            WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
        WiFi.onEvent(
            [](WiFiEvent_t, WiFiEventInfo_t info) {
                const auto &c = info.wifi_sta_connected;
                ESP_LOGI(LOG_TAG, "STA connected: ssid=%.*s bssid=%02x:%02x:%02x:%02x:%02x:%02x ch=%u authmode=%u",
                         (int)c.ssid_len, c.ssid, c.bssid[0], c.bssid[1], c.bssid[2], c.bssid[3], c.bssid[4], c.bssid[5],
                         c.channel, c.authmode);
            },
            WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_CONNECTED);
        WiFi.onEvent(
            [this](WiFiEvent_t, WiFiEventInfo_t info) {
                const auto &d = info.wifi_sta_disconnected;
                const char *name = WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(d.reason));
                ESP_LOGW(LOG_TAG, "STA disconnected: reason=%u (%s) bssid=%02x:%02x:%02x:%02x:%02x:%02x ssid=%.*s", d.reason,
                         name && *name ? name : "vendor/unknown", d.bssid[0], d.bssid[1], d.bssid[2], d.bssid[3], d.bssid[4],
                         d.bssid[5], (int)d.ssid_len, d.ssid);
                wifiDisconnectedPending = true;
            },
            WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
        WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) { ESP_LOGW(LOG_TAG, "STA lost IP"); },
                     WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_LOST_IP);
        WiFi.onEvent(
            [](WiFiEvent_t, WiFiEventInfo_t info) {
                ESP_LOGW(LOG_TAG, "STA authmode changed: %u -> %u", info.wifi_sta_authmode_change.old_mode,
                         info.wifi_sta_authmode_change.new_mode);
            },
            WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE);

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
        const String apPassword = settings.getWifiApPassword();
        // WPA2 requires >= 8 chars; fall back to an open AP if somehow shorter.
        const bool secured = apPassword.length() >= WIFI_AP_PASSWORD_MIN_LENGTH;
        WiFi.mode(WIFI_AP);
        WiFi.softAPConfig(WIFI_AP_IP, WIFI_AP_IP, WIFI_SUBNET_MASK);
        WiFi.softAP(WIFI_AP_SSID, secured ? apPassword.c_str() : nullptr);
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
        // Credentials block so headless users can read the AP login from serial.
        ESP_LOGI(LOG_TAG, "========================================");
        ESP_LOGI(LOG_TAG, "  WiFi Access Point started");
        ESP_LOGI(LOG_TAG, "  SSID:     %s", WIFI_AP_SSID);
        if (secured) {
            ESP_LOGI(LOG_TAG, "  Password: %s", apPassword.c_str());
        } else {
            ESP_LOGI(LOG_TAG, "  Password: <open network>");
        }
        ESP_LOGI(LOG_TAG, "  Web UI:   http://%s/", WIFI_AP_IP.toString().c_str());
        ESP_LOGI(LOG_TAG, "========================================");
    }

    pluginManager->on("ota:update:start", [this](Event const &) { this->updating = true; });
    pluginManager->on("ota:update:end", [this](Event const &) { this->updating = false; });

    // STA path: STA_GOT_IP handler already set wifiConnectedPending; loop()
    // dispatches controller:wifi:connect from there. AP path has no STA_GOT_IP,
    // so it needs the explicit trigger here.
    if (isApConnection) {
        pluginManager->trigger("controller:wifi:connect", "AP", 1);
    }
}

void Controller::loop() {
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

    if (screenReady && !initialized) {
        connect();
    }

    if (initialized) {
        comms.loop(); // drive the comms send pump + retransmit
    }

    pluginManager->loop();

    unsigned long now = millis();

    // A config burst right after a reconnect can be lost in the unstable BLE window,
    // and a spurious ACK then stops the reliable layer retrying. Re-send until it lands.
    if (comms.isConnected() && now < configResendUntil && (now - lastConfigResend) >= CONFIG_RESEND_INTERVAL_MS) {
        setPressureScale();
        setPidSettings();
        setPumpModelCoeffs();
        lastConfigResend = now;
    }

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
}

void Controller::loopLogic() {
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    pollHardwareScaleBrewTare();
#endif
    if (isErrorState()) {
        loopControl();
        return;
    }

    // Check if steam is ready.
    if (mode == MODE_STEAM && !steamReady &&
        currentTemp.load(std::memory_order_relaxed) + 5.f > getTargetTemp()) {
        activate();
        steamReady = true;
    }

    // Process lifecycle under the lock (GM-147); events and NVS writes deferred past unlock.
    DeferredProcessEvents events;
    bool processEnded = false;
    double newBrewDelay = -1.0;
    double newGrindDelay = -1.0;
    {
        std::lock_guard<std::recursive_mutex> guard(processMutex);

        // Handle current process
        if (currentProcess != nullptr) {
            updateLastAction();
            if (currentProcess->getType() == MODE_BREW) {
                auto brewProcess = static_cast<BrewProcess *>(currentProcess);
                brewProcess->updatePressure(pressure.load(std::memory_order_relaxed));
                brewProcess->updateFlow(currentPumpFlow.load(std::memory_order_relaxed));
            }
            currentProcess->progress();
            if (!isActiveLocked()) {
                processEnded = deactivateLocked(events);
            }
        }

        // Handle last process - Calculate auto delay
        if (lastProcess != nullptr && !lastProcess->isComplete()) {
            lastProcess->progress();
        }
        if (lastProcess != nullptr && lastProcess->isComplete() && !processCompleted && settings.isDelayAdjust()) {
            processCompleted = true;
            if (lastProcess->getType() == MODE_BREW) {
                if (auto *brewProcess = static_cast<BrewProcess *>(lastProcess);
                    brewProcess->target == ProcessTarget::VOLUMETRIC) {
                    if (PredictiveDelayPolicy::supportsPostStopLearning(lastVolumetricSource)) {
                        newBrewDelay = brewProcess->getNewDelayTime();
                    }
                }
            } else if (lastProcess->getType() == MODE_GRIND) {
                if (auto *grindProcess = static_cast<GrindProcess *>(lastProcess);
                    grindProcess->target == ProcessTarget::VOLUMETRIC) {
                    newGrindDelay = grindProcess->getNewDelayTime();
                }
            }
        }
    }
    if (processEnded) {
        loopControl(true);
        applyConnectionPriority();
    }
    dispatchEvents(events);
    if (newBrewDelay >= 0) {
        settings.setBrewDelay(newBrewDelay);
    }
    if (newGrindDelay >= 0) {
        settings.setGrindDelay(newGrindDelay);
    }

    unsigned long now = millis();
    if (grindActiveUntil != 0 && now > grindActiveUntil)
        deactivateGrind();
    if (mode != MODE_STANDBY && settings.getStandbyTimeout() > 0 && now > lastAction + settings.getStandbyTimeout())
        activateStandby();

    loopControl();
}

void Controller::loopControl(bool urgent) {
    if (initialized) {
        unsigned long now = millis();

        // Keepalive: updateControl() only sends control deltas now, so a steady-state
        // session would otherwise go silent. A periodic ping keeps the controller's
        // connection watchdog fed (sent in all states, including error). Skip it for
        // an incompatible controller -- it can't parse the frame anyway.
        if (comms.isConnected() && !systemInfo.protocolMismatch && now - lastPing >= PING_INTERVAL) {
            comms.sendPing();
            lastPing = now;
        }

        updateControl(urgent);
    }
}

bool Controller::isUpdating() const { return updating; }

bool Controller::isAutotuning() const { return autotuning; }

bool Controller::isReady() const { return !isUpdating() && !isErrorState() && !isAutotuning(); }

ScaleAvailability Controller::scaleAvailability() const {
    ScaleAvailability a;
    a.bluetoothConnected = isBluetoothScaleHealthy();
    a.brewBluetoothConnected = isBluetoothScaleHealthy(ScaleRole::BREW);
    a.grindBluetoothConnected = isBluetoothScaleHealthy(ScaleRole::GRIND);
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
    DeferredProcessEvents events;
    {
        std::lock_guard<std::recursive_mutex> guard(processMutex);
        startProcessLocked(process, events);
    }
    dispatchEvents(events);
}

void Controller::startProcessLocked(Process *process, DeferredProcessEvents &events) {
    if (isActiveLocked() || !isReady()) {
        delete process;
        return;
    }
    processCompleted = false;
    this->currentProcess = process;
    applyConnectionPriority(); // shot started -> tight BLE interval
    events.push_back({"controller:process:start"});
    updateLastAction();
}

void Controller::dispatchEvents(const DeferredProcessEvents &events) {
    for (const auto &event : events) {
        if (event.utility >= 0) {
            pluginManager->trigger(event.id, "utility", event.utility);
        } else {
            pluginManager->trigger(event.id);
        }
    }
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

void Controller::startBrewProcess() {
    if (mode != MODE_BREW) {
        return;
    }

    // Fire immediately before the process is created so plugins can pre-arm
    // shot capture. Hardware-scale tare is already complete when this helper is
    // called for hardware-scale brews.
    pluginManager->trigger("controller:brew:prestart");
    const double predictiveDelayMs = PredictiveDelayPolicy::brewDelayForSource(
        currentVolumetricSource, settings.getBrewDelay(), settings.getHardwareBrewDelay());
    startProcess(new BrewProcess(profileManager->getSelectedProfile(),
                                 profileManager->getSelectedProfile().isVolumetric() &&
                                         currentVolumetricSource != VolumetricMeasurementSource::INACTIVE
                                     ? ProcessTarget::VOLUMETRIC
                                     : ProcessTarget::TIME,
                                 predictiveDelayMs));

    bool startedBrew = false;
    bool startedUtility = false;
    int procType = -1;
    {
        std::lock_guard<std::recursive_mutex> guard(processMutex);
        Process *proc = currentProcess;
        startedBrew = proc != nullptr && proc->getType() == MODE_BREW;
        startedUtility = proc != nullptr && proc->isUtility();
        procType = proc != nullptr ? proc->getType() : -1;
    }
    ESP_LOGI(LOG_TAG, "startBrewProcess: procType=%d startedBrew=%d", procType, startedBrew);
    if (startedBrew) {
        pluginManager->trigger("controller:brew:start", "utility", startedUtility ? 1 : 0);
    }
}

#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
bool Controller::armHardwareScaleBrewTare() {
    if (pendingHardwareScaleBrewStart.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }

    pendingHardwareScaleBrewStartReady.store(false, std::memory_order_release);
    pendingHardwareScaleBrewTareStartedAt = millis();

    const ScaleSample sample = getScaleSample();
    ESP_LOGI(LOG_TAG, "Waiting for hardware scale tare before brew start: seq=%lu health=0x%04x",
             static_cast<unsigned long>(sample.sampleSeq), sample.healthBits);
    pluginManager->trigger("controller:brew:scale-tare:start");

    if ((sample.healthBits & SCALE_HEALTH_TARING) == 0) {
        scaleTare();
    }
    return true;
}

void Controller::markHardwareScaleBrewTareDone() {
    if (!pendingHardwareScaleBrewStart.load(std::memory_order_acquire))
        return;
    pendingHardwareScaleBrewStartReady.store(true, std::memory_order_release);
    if (logicTaskHandle != nullptr)
        xTaskNotifyGive(logicTaskHandle);
}

void Controller::cancelHardwareScaleBrewTare(const char *reason) {
    if (!pendingHardwareScaleBrewStart.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    pendingHardwareScaleBrewStartReady.store(false, std::memory_order_release);
    pendingHardwareScaleBrewTareStartedAt = 0;
    currentVolumetricSource = VolumetricMeasurementSource::INACTIVE;

    ESP_LOGW(LOG_TAG, "Cancelled hardware-scale brew start: %s", reason != nullptr ? reason : "unknown");
    Event ev;
    ev.id = "controller:brew:scale-tare:failed";
    ev.setString("reason", reason != nullptr ? String(reason) : String("unknown"));
    pluginManager->trigger(ev);
}

void Controller::pollHardwareScaleBrewTare() {
    if (!pendingHardwareScaleBrewStart.load(std::memory_order_acquire)) {
        return;
    }

    if (mode != MODE_BREW) {
        cancelHardwareScaleBrewTare("mode_changed");
        return;
    }
    if (isActive()) {
        cancelHardwareScaleBrewTare("process_started_elsewhere");
        return;
    }
    if (pendingHardwareScaleBrewStartReady.exchange(false, std::memory_order_acq_rel)) {
        pendingHardwareScaleBrewStart.store(false, std::memory_order_release);
        pendingHardwareScaleBrewTareStartedAt = 0;
        ESP_LOGI(LOG_TAG, "Hardware scale tare complete; starting brew");
        startBrewProcess();
        return;
    }

    const ScaleSample sample = getScaleSample();
    if (sample.healthBits & SCALE_HEALTH_TARE_FAILED) {
        cancelHardwareScaleBrewTare("tare_failed");
        return;
    }
    if (millis() - pendingHardwareScaleBrewTareStartedAt > HARDWARE_SCALE_BREW_TARE_TIMEOUT_MS) {
        cancelHardwareScaleBrewTare("tare_timeout");
        return;
    }
}
#endif

float Controller::getTargetTemp() const {
    std::lock_guard<std::recursive_mutex> guard(processMutex);
    Process *proc = currentProcess;
    if (proc != nullptr && proc->isActive()) {
        switch (proc->getType()) {
        case MODE_BREW:
            return static_cast<BrewProcess *>(proc)->getTemperature();
        case MODE_STEAM:
            return settings.getTargetSteamTemp();
        case MODE_WATER:
            return settings.getTargetWaterTemp();
        default:
            break;
        }
    }
    switch (mode) {
    case MODE_BREW:
    case MODE_GRIND:
        return profileManager->getSelectedProfile().temperature;
    case MODE_STEAM:
        return settings.getTargetSteamTemp();
    case MODE_WATER:
        return settings.getTargetWaterTemp();
    default:
        return 0;
    }
}

UartDiagnostics Controller::getUartDiagnostics() const {
#ifdef GAGGIMATE_UART_COMMS
    return comms.getDiagnostics();
#else
    return {};
#endif
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
        bool gearpumpEnabled = systemInfo.capabilities.hasAddon(7);
        // Slip is gear-pump only; send zeros otherwise so it stays a no-op.
        float slip[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (gearpumpEnabled) {
            parseFloatCsv(settings.getPumpSlipCoeffs(), slip, 4, 0.0f);
        }
        comms.sendPumpSettings(coeffs[0], coeffs[1], coeffs[2], coeffs[3],
                               gearpumpEnabled ? settings.getCommutationGain() : DEFAULT_COMMUTATION_GAIN,
                               gearpumpEnabled ? settings.getConvergenceGain() : DEFAULT_CONVERGENCE_GAIN,
                               gearpumpEnabled ? settings.getIntegralGain() : DEFAULT_INTEGRAL_GAIN, settings.getMaxPumpPower(),
                               slip[0], slip[1], slip[2], slip[3]);
    }
}

void Controller::setPidSettings() {
    float pid[4];
    parseFloatCsv(settings.getPid(), pid, 4, 0.0f);
    comms.sendPidSettings(pid[0], pid[1], pid[2], pid[3]);
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

LiveBrewTargetApplyStatus Controller::applyLiveBrewTargetUpdate(const LiveBrewTargetUpdate &update) {
    std::lock_guard<std::recursive_mutex> guard(processMutex);
    Process *proc = currentProcess;
    if (proc == nullptr || proc->getType() != MODE_BREW || !proc->isActive()) {
        return LiveBrewTargetApplyStatus::NOT_ACTIVE_BREW;
    }

    auto *brewProcess = static_cast<BrewProcess *>(proc);
    if (!update.stopRequested && update.hasYieldStopTarget && brewProcess->target != ProcessTarget::VOLUMETRIC) {
        return LiveBrewTargetApplyStatus::YIELD_REQUIRES_VOLUMETRIC;
    }

    brewProcess->applyLiveTargetUpdate(update);
    return LiveBrewTargetApplyStatus::APPLIED;
}

void Controller::updateControl(bool urgent) {
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
        std::lock_guard<std::recursive_mutex> guard(processMutex);
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
    if (urgent || !controlStateSent || boiler != lastBoiler)
        batch[count++] = comms.buildBoilerControl(boiler.index, boiler.mode, boiler.setpoint);
    if (urgent || !controlStateSent || pump != lastPump)
        batch[count++] = comms.buildPumpControl(pump.index, pump.mode, pump.power, pump.pressure, pump.flow);
    if (urgent || !controlStateSent || relay != lastRelay)
        batch[count++] = comms.buildRelayControl(relay.index, relay.open); // index 0 = brew valve
    if (urgent || !controlStateSent || altRelayActive != lastAlt)
        batch[count++] = comms.buildRelayControl(1, altRelayActive); // index 1 = alt relay

    if (count > 0) {
        if (urgent)
            comms.sendUrgentBatch(batch, count);
        else
            comms.sendBatch(batch, count);
    }

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
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    if (pendingHardwareScaleBrewStart.load(std::memory_order_acquire)) {
        ESP_LOGI(LOG_TAG, "Ignoring duplicate brew start while hardware scale tare is pending");
        return;
    }
#endif
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
    }

    // TARE resets the pump/pressure controller and predictive volumetric
    // estimator. It does not tare the HX711 hardware scale; hardware scale tare
    // is a separate scale command path. Always reset pump control at brew start
    // so hardware-scale brews do not inherit stale pressure-controller state.
    comms.tare();
    currentVolumetricSource = resolved;
    ESP_LOGI(LOG_TAG, "activate: mode=%d brewSrcSetting=%d resolvedSource=%d", mode, brewSrcSetting, static_cast<int>(resolved));

    switch (mode) {
    case MODE_BREW:
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
        if (resolved == VolumetricMeasurementSource::HARDWARE_SCALE && armHardwareScaleBrewTare()) {
            return;
        }
#endif
        startBrewProcess();
        break;
    case MODE_STEAM:
        delay(200);
        startProcess(new SteamProcess(STEAM_SAFETY_DURATION_MS, settings.getSteamPumpPercentage()));
        break;
    case MODE_WATER:
        delay(200);
        startProcess(new PumpProcess());
        break;
    default:;
    }
}

void Controller::deactivate() {
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    if (pendingHardwareScaleBrewStart.load(std::memory_order_acquire)) {
        cancelHardwareScaleBrewTare("deactivated");
        return;
    }
#endif
    DeferredProcessEvents events;
    bool processEnded = false;
    {
        std::lock_guard<std::recursive_mutex> guard(processMutex);
        processEnded = deactivateLocked(events);
    }
    if (!processEnded) {
        return;
    }

    // Drive the stopped process state to the controller before any brew-end
    // observer can serialize, persist, or transmit shot data. The urgent batch
    // supersedes any older in-flight control frame; the transport task handles
    // its acknowledgement and retransmission outside the physical shutdown path.
    loopControl(true);
    applyConnectionPriority(); // shot ended -> relaxed BLE interval
    dispatchEvents(events);
}

bool Controller::deactivateLocked(DeferredProcessEvents &events) {
    if (currentProcess == nullptr) {
        return false;
    }
    delete lastProcess;
    lastProcess = currentProcess;
    lastVolumetricSource = currentVolumetricSource;
    currentProcess = nullptr;

    const int swappedType = lastProcess->getType();
    const bool brewWasUtility = lastProcess->isUtility();
    currentVolumetricSource = VolumetricMeasurementSource::INACTIVE;
    if (swappedType == MODE_BREW) {
        events.push_back({"controller:brew:end", brewWasUtility ? 1 : 0});
    } else if (swappedType == MODE_GRIND) {
        events.push_back({"controller:grind:end"});
    }
    events.push_back({"controller:process:end"});
    updateLastAction();
    return true;
}

void Controller::clear() {
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    if (pendingHardwareScaleBrewStart.load(std::memory_order_acquire)) {
        cancelHardwareScaleBrewTare("cleared");
    }
#endif
    DeferredProcessEvents events;
    {
        std::lock_guard<std::recursive_mutex> guard(processMutex);
        clearLocked(events);
    }
    dispatchEvents(events);
}

void Controller::clearLocked(DeferredProcessEvents &events) {
    processCompleted = true;
    if (lastProcess != nullptr && lastProcess->getType() == MODE_BREW) {
        events.push_back({"controller:brew:clear"});
    }
    delete lastProcess;
    lastProcess = nullptr;
    currentVolumetricSource = VolumetricMeasurementSource::INACTIVE;
    lastVolumetricSource = VolumetricMeasurementSource::INACTIVE;
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
    std::lock_guard<std::recursive_mutex> guard(processMutex);
    return isActiveLocked();
}

bool Controller::isGrindActive() const {
    std::lock_guard<std::recursive_mutex> guard(processMutex);
    return currentProcess != nullptr && currentProcess->isActive() && currentProcess->getType() == MODE_GRIND;
}

int Controller::getMode() const { return mode; }

void Controller::setMode(int newMode) {
    Event modeEvent = pluginManager->trigger("controller:mode:change", "value", newMode);
    mode = modeEvent.getInt("value");
    steamReady = false;

    updateLastAction();
    setTargetTemp(getTargetTemp());
    setPidSettings();
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
        currentCoffeeVolume = static_cast<float>(measurement);
        eventName = F("controller:volumetric-measurement:estimation:change");
    } else if (source == VolumetricMeasurementSource::HARDWARE_SCALE) {
        eventName = F("controller:volumetric-measurement:hardware:change");
    }
    pluginManager->trigger(eventName, "value", static_cast<float>(measurement));

    // This callback fires from the NimBLE task on core 0; deactivate()/clear() on
    // other tasks can delete the processes, so hold the lock across the deref (GM-147).
    std::lock_guard<std::recursive_mutex> guard(processMutex);
    Process *proc = currentProcess;
    Process *last = lastProcess;
    bool consumed = false;
    if (proc != nullptr && currentVolumetricSource == source) {
        proc->updateVolume(measurement);
        consumed = true;
    }
    if (last != nullptr && lastVolumetricSource == source && PredictiveDelayPolicy::supportsPostStopLearning(source) &&
        !last->isComplete()) {
        last->updateVolume(measurement);
        consumed = true;
    }
    if (!consumed) {
        ESP_LOGD(LOG_TAG, "Ignoring volumetric measurement, source does not match an eligible process");
    }
}

#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
void Controller::scaleTare() {
    comms.scaleTare();
}

void Controller::sendScaleCalibration(float c1, float c2) {
    comms.sendScaleCalibration(c1, c2, settings.getScaleOffset1(), settings.getScaleOffset2());
}

void Controller::onHardwareScaleSample(const ScaleSample &sample) {
    ScaleSample stored = sample;
    if (scaleSampleMutex && xSemaphoreTake(scaleSampleMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (stored.sampleSeq == 0) {
            stored.sampleSeq = lastScaleSample.sampleSeq + 1;
        }
        lastScaleSample = stored;
        xSemaphoreGive(scaleSampleMutex);
    }

    const float eventWeight = std::isfinite(stored.weightG) ? stored.weightG : 0.0f;
    pluginManager->trigger("controller:scale:sample", "value", eventWeight);
    pluginManager->trigger("controller:weight:change", "value", eventWeight);
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

bool Controller::isBluetoothScaleHealthy(ScaleRole role) const {
    const unsigned long now = millis();
    const unsigned long lastMeasurement =
        role == ScaleRole::BREW ? lastBrewBluetoothMeasurement : lastGrindBluetoothMeasurement;
    return BLEScales.isConnected(role) && lastMeasurement != 0 &&
           now - lastMeasurement <= BLUETOOTH_MEASUREMENT_GRACE_MS;
}

void Controller::noteBluetoothScaleMeasurement(ScaleRole role) {
    const unsigned long now = millis();
    lastBluetoothMeasurement = now;
    if (role == ScaleRole::BREW) {
        lastBrewBluetoothMeasurement = now;
    } else {
        lastGrindBluetoothMeasurement = now;
    }
}

void Controller::onFlush() {
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    if (pendingHardwareScaleBrewStart.load(std::memory_order_acquire)) {
        cancelHardwareScaleBrewTare("flush_started");
    }
#endif
    // Allocate outside the lock; reachable from the UI, AsyncTCP and BLE tasks (GM-147).
    auto *flush = new BrewProcess(FLUSH_PROFILE, ProcessTarget::TIME, settings.getBrewDelay());
    DeferredProcessEvents events;
    {
        std::lock_guard<std::recursive_mutex> guard(processMutex);
        if (isActiveLocked()) {
            delete flush;
            return;
        }
        clearLocked(events);
        startProcessLocked(flush, events);
        events.push_back({"controller:brew:start", 1});
    }
    dispatchEvents(events);
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
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
            if (pendingHardwareScaleBrewStart.load(std::memory_order_acquire)) {
                ESP_LOGI(LOG_TAG, "Ignoring duplicate physical brew start while scale tare is pending");
                break;
            }
#endif
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

void Controller::handleControllerButton(uint8_t index, bool pressed) {
    const int status = pressed ? 1 : 0;
    String behavior = settings.getButtonBehavior(index);
    ESP_LOGV("Controller", "Button %d changed to %d, behavior: %s", index, status, behavior.c_str());
    if (behavior.isEmpty() || behavior == "none")
        return;
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
        if (status) {
            if (getMode() == MODE_STANDBY)
                deactivateStandby();
            if (getMode() != MODE_BREW && !isActive())
                setMode(MODE_BREW);
            onFlush();
        }
        return;
    }
    handleProfileButton(status, behavior);
}

void Controller::handleSteamButton(int steamButtonStatus) {
    {
        std::lock_guard<std::recursive_mutex> guard(processMutex);
        Process *proc = currentProcess;
        if (proc != nullptr && proc->isActive() && proc->getType() == MODE_BREW) {
            ESP_LOGW(LOG_TAG, "Ignoring steam button state %d while brew process is active", steamButtonStatus);
            return;
        }
    }
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

void Controller::loopLogicTask(void *arg) {
    auto *controller = static_cast<Controller *>(arg);
    while (true) {
        esp_task_wdt_reset();
        controller->drainCommandQueue();
        controller->loopLogic();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(controller->getMode() == MODE_STANDBY ? 1000 : PROGRESS_INTERVAL));
    }
}
