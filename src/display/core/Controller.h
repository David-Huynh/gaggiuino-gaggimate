#ifndef CONTROLLER_H
#define CONTROLLER_H

#include "GaggiMateClient.h"

#include <atomic>
#include <cstdint>
#include "PluginManager.h"
#include "Settings.h"
#include "SystemInfo.h"
#include <WiFi.h>
#include <display/core/ProfileManager.h>
#include <display/core/process/Process.h>
#ifndef GAGGIMATE_HEADLESS
#include <display/drivers/Driver.h>
#include <display/ui/default/DefaultUI.h>
#endif

const IPAddress WIFI_AP_IP(4, 4, 4, 1); // the IP address the web server, Samsung requires the IP to be in public space
const IPAddress WIFI_SUBNET_MASK(255, 255, 255, 0); // no need to change: https://avinetworks.com/glossary/subnet-mask/

enum class VolumetricMeasurementSource { INACTIVE, FLOW_ESTIMATION, BLUETOOTH, HARDWARE_SCALE };

// Commands posted from any task; drained on the Arduino loop task at the top of
// Controller::loop(). External callers (WebUI, LVGL, plugins) MUST use
// postCommand() rather than calling the corresponding mutator directly so that
// process-state mutation only happens on a single task.
enum class CtrlCmd : uint8_t {
    ACTIVATE,
    DEACTIVATE,
    CLEAR,
    ACTIVATE_GRIND,
    DEACTIVATE_GRIND,
    ACTIVATE_STANDBY,
    DEACTIVATE_STANDBY,
    SET_MODE,
    RAISE_TEMP,
    LOWER_TEMP,
    RAISE_GRIND_TARGET,
    LOWER_GRIND_TARGET,
};

class Controller {
  public:
    Controller() = default;

    // Thread-safe: enqueue a command from any task. Non-blocking.
    void postCommand(CtrlCmd cmd, int32_t arg = 0);

    void setup();
    void connect();
    void loop();
    void loopLogic();
    void loopControl();

    void setMode(int newMode);
    void setTargetTemp(float temperature);
    void setPressureScale();
    void setPumpModelCoeffs();
    void setTargetGrindDuration(int duration);
    void setTargetGrindVolume(double volume);

    int getMode() const;

    float getTargetTemp() const;
    int getTargetGrindDuration() const;
    virtual float getCurrentTemp() const { return currentTemp.load(std::memory_order_relaxed); }
    bool isActive() const;
    bool isGrindActive() const;
    bool isUpdating() const;
    bool isAutotuning() const;
    bool isReady() const;
    bool isVolumetricAvailable() const;
    bool isSDCard() const { return sdcard; }
    virtual float getTargetPressure() const { return targetPressure; }
    virtual float getTargetFlow() const { return targetFlow; }
    virtual float getCurrentPressure() const { return pressure.load(std::memory_order_relaxed); }
    virtual float getCurrentPuckFlow() const { return currentPuckFlow; }
    virtual float getCurrentPumpFlow() const { return currentPumpFlow.load(std::memory_order_relaxed); }

    bool isTaskHealthy() const { return is_task_healthy(eTaskGetState(taskHandle)); }

    void autotune(int testTime, int samples, int heaterWattage);
    void startProcess(Process *process);
    Process *getProcess() const { return currentProcess; }
    Process *getLastProcess() const { return lastProcess; }
    Settings &getSettings() { return settings; }
    ProfileManager *getProfileManager() { return profileManager; }
#ifndef GAGGIMATE_HEADLESS
    DefaultUI *getUI() const { return ui; }
#endif
    bool isErrorState() const { return error > 0; }
    int getError() const { return error; }

    // Event callback methods
    void updateLastAction();
    void raiseTemp();
    void lowerTemp();
    void raiseBrewTarget();
    void lowerBrewTarget();
    void raiseGrindTarget();
    void lowerGrindTarget();
    void activate();
    void deactivate();
    void clear();
    void activateGrind();
    void deactivateGrind();
    void activateStandby();
    void deactivateStandby();
    void onOTAUpdate();
    void onScreenReady();
    void onTargetToggle();
    void onTargetChange(ProcessTarget target);
    void onProfileSave() const;
    void onProfileSaveAsNew();
    void onVolumetricMeasurement(double measurement, VolumetricMeasurementSource source);
    void setVolumetricOverride(bool override) { volumetricOverride = override; }
    void setHardwareScalePresent(bool present) { hardwareScalePresent = present; }
    bool isHardwareScalePresent() const { return hardwareScalePresent; }
    bool isHardwareScaleShotBaselineActive() const { return hardwareScaleShotBaselineActive; }
    float getHardwareScaleShotBaseline() const { return hardwareScaleShotBaseline; }
    void scaleTare();
    void sendScaleCalibration(float c1, float c2);
    bool isBluetoothScaleHealthy() const;
    // Most-recent ScaleSample snapshot (rich health/stddev info), copied under mutex.
    ScaleSample getScaleSample() const;
    void onFlush();
    int getWaterLevel() const {
        int emptyDist = settings.getEmptyTankDistance();
        int fullDist  = settings.getFullTankDistance();
        if (emptyDist <= fullDist) return 0;
        float reversedLevel = static_cast<float>(emptyDist) -
                              static_cast<float>(std::min(emptyDist, tofDistance));
        int level = static_cast<int>((reversedLevel - fullDist) /
                                     static_cast<float>(emptyDist - fullDist) * 100.0f);
        return std::max(0, std::min(100, level));
    };
    int getTofDistance() const { return tofDistance; }

    void onVolumetricDelete();
    bool isLowWaterLevel() const { return getWaterLevel() < 20; };

    SystemInfo getSystemInfo() const { return systemInfo; }

    GaggiMateClient *getClientController() { return &comms; }

  private:
    // Initialization methods
#ifndef GAGGIMATE_HEADLESS
    void setupPanel();
#endif
    void setupBluetooth();
    void onSystemInfo(const char *hardware, const char *version, uint32_t protocolVersion, bool dimming, bool pressure,
                      bool ledControl, bool tof, bool scale);
    // Connected to a controller too old to speak the framed protocol: drive the
    // same path as a protocol-version mismatch (OTA recovery only). infoJson is
    // the legacy INFO characteristic contents (hardware/version/capabilities).
    void onIncompatibleController(const String &infoJson);
    void setupWifi();

    // Functional methods
    void updateControl();
    // Switch the BLE connection interval based on whether a process is running.
    // force re-applies even if the desired state is unchanged (use on connect).
    void applyConnectionPriority(bool force = false);
    void drainCommandQueue();
    bool captureHardwareScaleShotBaseline();
    void resetHardwareScaleShotBaseline();
    void recordHardwareScaleBaselineSample(const ScaleSample &sample);
    bool isHardwareScaleSampleHealthy(const ScaleSample &sample) const;

    // Event handlers
    void onTempRead(float temperature);

    void handleBrewButton(int brewButtonStatus);
    void handleSteamButton(int steamButtonStatus);
    void handleWaterButton(int buttonStatus);
    void handleProfileButton(int buttonStatus, String id);
    void handleProfileUpdate();

    // Private Attributes
#ifndef GAGGIMATE_HEADLESS
    DefaultUI *ui = nullptr;
    Driver *driver = nullptr;
#endif
    GaggiMateClient comms;
    hw_timer_t *timer = nullptr;
    Settings settings;
    PluginManager *pluginManager{};
    ProfileManager *profileManager{};

    int mode = MODE_BREW;
    // Sensor scalars written from the UART poll callback (Arduino loop task) and
    // read from loopTask on Core 1; atomic to avoid torn reads / race-y glitches
    // in the brew control loop.
    std::atomic<float> currentTemp{0.0f};
    std::atomic<float> pressure{0.0f};
    float targetPressure = 0.0f;
    float currentPuckFlow = 0.0f;
    std::atomic<float> currentPumpFlow{0.0f};
    float targetFlow = 0.0f;
    int tofDistance = 0;

    SystemInfo systemInfo{};

    // Last control values sent to the controller. updateControl() only
    // transmits components that differ from these (the controller is stateful
    // and delivery is acknowledged). Reset on (re)connect to force a full resend.
    BoilerCommand lastBoiler{};
    PumpCommand lastPump{};
    RelayCommand lastRelay{};
    bool lastAlt = false;
    bool controlStateSent = false;

    // BLE connection-interval priority: tight while a process runs, relaxed when
    // idle (frees radio airtime for Wi-Fi). Tracks the last requested state.
    bool connLowLatency = false;

    Process *currentProcess = nullptr;
    Process *lastProcess = nullptr;
    // Serializes mutation and dereference of currentProcess / lastProcess across
    // tasks (Arduino loop, Core 1 loopTask, AsyncTCP, LVGL). Recursive so that
    // activate() can call clear()/startProcess() while already holding it.
    SemaphoreHandle_t processMutex = nullptr;

    // External-mutation queue, drained on the Arduino loop task.
    QueueHandle_t cmdQueue = nullptr;

    // Most-recent ScaleSample, written from UART poll callback, read by getters.
    mutable SemaphoreHandle_t scaleSampleMutex = nullptr;
    ScaleSample lastScaleSample{};
    static constexpr uint8_t HARDWARE_SCALE_BASELINE_SAMPLE_COUNT = 8;
    float hardwareScaleBaselineSamples[HARDWARE_SCALE_BASELINE_SAMPLE_COUNT]{};
    uint8_t hardwareScaleBaselineSampleIndex = 0;
    uint8_t hardwareScaleBaselineSampleCount = 0;
    bool hardwareScaleShotBaselineActive = false;
    float hardwareScaleShotBaseline = 0.0f;

    unsigned long grindActiveUntil = 0;
    unsigned long lastPing = 0;
    unsigned long lastProgress = 0;
    unsigned long lastInfoRequest = 0;
    unsigned long lastAction = 0;
    bool loaded = false;
    bool updating = false;
    bool autotuning = false;
    bool isApConnection = false;
    // WiFi up/down is signalled (flag only) from the Arduino WiFi event task and
    // acted on in loop(): doing server/socket/mDNS start-stop in that small-stack
    // callback corrupted the heap under load. See setupWifi() + loop().
    volatile bool wifiConnectedPending = false;
    volatile bool wifiDisconnectedPending = false;
    bool initialized = false;
    bool screenReady = false;
    bool waitingForController = false;
    unsigned long connectStartTime = 0;
    bool volumetricOverride = false;
    bool hardwareScalePresent = false;
    bool processCompleted = false;
    bool steamReady = false;
    bool sdcard = false;
    int error = 0;

    // Bluetooth scale connection monitoring
    VolumetricMeasurementSource currentVolumetricSource = VolumetricMeasurementSource::INACTIVE;
    unsigned long lastBluetoothMeasurement = 0;
    static const unsigned long BLUETOOTH_GRACE_PERIOD_MS = 1500; // 1.5 second grace period
    static const unsigned long CONTROLLER_WAITING_TIMEOUT_MS = 10000;

    xTaskHandle taskHandle;
    xTaskHandle logicTaskHandle;

    static void loopTask(void *arg);
    static void loopLogicTask(void *arg);
};

#endif // CONTROLLER_H
