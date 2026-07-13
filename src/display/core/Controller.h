#ifndef CONTROLLER_H
#define CONTROLLER_H

#include "AutoTuning.h"
#include "GaggiMateClient.h"

#include <atomic>
#include <cstdint>
#include "LiveBrewTargetUpdate.h"
#include "PluginManager.h"
#include "ScaleSourceResolver.h"
#include "Settings.h"
#include "SystemInfo.h"
#include <WiFi.h>
#include <display/core/ProfileManager.h>
#include <display/core/process/Process.h>
#include <mutex>
#include <vector>
#ifndef GAGGIMATE_HEADLESS
#include <display/drivers/Driver.h>
#include <display/ui/default/DefaultUI.h>
#endif

const IPAddress WIFI_AP_IP(4, 4, 4, 1); // the IP address the web server, Samsung requires the IP to be in public space
const IPAddress WIFI_SUBNET_MASK(255, 255, 255, 0); // no need to change: https://avinetworks.com/glossary/subnet-mask/

// VolumetricMeasurementSource and the scale-source role resolver live in
// ScaleSourceResolver.h (included above) so the routing logic stays a small,
// Arduino-free unit.

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
    void setPidSettings();
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
    virtual float getCurrentPumpPower() const { return currentPumpPower; }
    virtual float getCurrentHeaterPower() const { return currentHeaterPower; }
    virtual float getCurrentPuckResistance() const { return currentPuckResistance; }
    virtual float getCurrentCoffeeVolume() const { return currentCoffeeVolume; }

    bool isTaskHealthy() const { return is_task_healthy(eTaskGetState(logicTaskHandle)); }

    void autotune(int testTime, int samples, int heaterWattage);
    void startProcess(Process *process);
    // Thread-safe live brew target mutation for adapter-driven adaptive profiles.
    LiveBrewTargetApplyStatus applyLiveBrewTargetUpdate(const LiveBrewTargetUpdate &update);
    // Dereferencing the returned pointers requires holding getProcessLock(); the
    // logic task and control entry points can delete them at any time (GM-147).
    Process *getProcess() const { return currentProcess; }
    Process *getLastProcess() const { return lastProcess; }
    std::recursive_mutex &getProcessLock() const { return processMutex; }
    Settings &getSettings() { return settings; }
    void setOptimizerTransport(AutoTuning::OptimizerTransportPort *transport) { optimizerTransport = transport; }
    AutoTuning::OptimizerTransportPort *getOptimizerTransport() { return optimizerTransport; }
    AutoTuning::OptimizerTransportPort const *getOptimizerTransport() const { return optimizerTransport; }
    void setAutoTuningRecordStore(AutoTuning::AutoTuningRecordStorePort *store) { autoTuningRecordStore = store; }
    AutoTuning::AutoTuningRecordStorePort *getAutoTuningRecordStore() const { return autoTuningRecordStore; }
    void setCommunityUpload(AutoTuning::CommunityUploadPort *upload) { communityUpload = upload; }
    AutoTuning::CommunityUploadPort *getCommunityUpload() const { return communityUpload; }
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

    // --- Scale source roles --------------------------------------------------
    // Live snapshot of which sources are currently usable (BLE connected, HW
    // present/capable, predictive). Cheap: no mutex, just flag reads.
    ScaleAvailability scaleAvailability() const;
    // Source the active process is currently consuming weight from.
    VolumetricMeasurementSource getCurrentVolumetricSource() const { return currentVolumetricSource; }
    // What the brew / grind roles would resolve to right now (for display).
    VolumetricMeasurementSource getResolvedBrewSource() const {
        return ScaleSourceResolver::resolveBrewSource(settings.getScaleSource(), scaleAvailability());
    }
    VolumetricMeasurementSource getResolvedGrindSource() const { return ScaleSourceResolver::resolveGrindSource(scaleAvailability()); }
    // Volumetric availability for the grind-by-weight role (brew uses isVolumetricAvailable()).
    bool isGrindVolumetricAvailable() const;
#ifdef GAGGIMATE_DISABLE_HARDWARE_SCALE
    void setHardwareScalePresent(bool present) {
        (void)present;
        hardwareScalePresent = false;
    }
    bool isHardwareScalePresent() const { return false; }
#else
    void setHardwareScalePresent(bool present) { hardwareScalePresent = present; }
    bool isHardwareScalePresent() const { return hardwareScalePresent; }
    void scaleTare();
    void sendScaleCalibration(float c1, float c2);
    // Most-recent ScaleSample snapshot (rich health/stddev info), copied under mutex.
    ScaleSample getScaleSample() const;
    bool isHardwareScaleSampleHealthy(const ScaleSample &sample) const;
#endif
    bool isBluetoothScaleHealthy() const;
    bool isBluetoothScaleHealthy(ScaleRole role) const;
    void noteBluetoothScaleMeasurement(ScaleRole role);
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
    UartDiagnostics getUartDiagnostics() const;
    ControllerDiagnostics getControllerDiagnostics() const { return comms.getControllerDiagnostics(); }

    GaggiMateClient *getClientController() { return &comms; }

  private:
    // Initialization methods
#ifndef GAGGIMATE_HEADLESS
    void setupPanel();
#endif
    void setupBluetooth();
    void onSystemInfo(const char *hardware, const char *version, uint32_t protocolVersion, bool dimming, bool pressure,
                      bool ledControl, bool tof, bool scale, std::vector<uint32_t> addons);
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
    void startBrewProcess();
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    void onHardwareScaleSample(const ScaleSample &sample);
    bool armHardwareScaleBrewTare();
    void markHardwareScaleBrewTareDone();
    void pollHardwareScaleBrewTare();
    void cancelHardwareScaleBrewTare(const char *reason);
#endif

    struct DeferredProcessEvent {
        const char *id;
        int utility = -1;
    };
    using DeferredProcessEvents = std::vector<DeferredProcessEvent>;

    // Process lifecycle (GM-147): the *Locked helpers assume processMutex is held
    // and collect events for dispatch after unlocking.
    bool isActiveLocked() const { return currentProcess != nullptr && currentProcess->isActive(); }
    void startProcessLocked(Process *process, DeferredProcessEvents &events);
    bool deactivateLocked(DeferredProcessEvents &events);
    void clearLocked(DeferredProcessEvents &events);
    void dispatchEvents(const DeferredProcessEvents &events);

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
    AutoTuning::OptimizerTransportPort *optimizerTransport{};
    AutoTuning::AutoTuningRecordStorePort *autoTuningRecordStore{};
    AutoTuning::CommunityUploadPort *communityUpload{};

    int mode = MODE_BREW;
    // Sensor scalars written from the UART poll callback (Arduino loop task) and
    // read from loopTask on Core 1; atomic to avoid torn reads / race-y glitches
    // in the brew control loop.
    std::atomic<float> currentTemp{0.0f};
    std::atomic<float> pressure{0.0f};
    float targetPressure = 0.0f;
    float currentPuckFlow = 0.0f;
    std::atomic<float> currentPumpFlow{0.0f};
    float currentPumpPower = 0.0f;
    float currentHeaterPower = 0.0f;
    float currentPuckResistance = 0.0f;
    float currentCoffeeVolume = 0.0f;
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

    // Guards currentProcess/lastProcess lifecycle across tasks (UI, AsyncTCP, BLE
    // callbacks, logic task). Recursive: locked composites call locked primitives.
    mutable std::recursive_mutex processMutex;
    Process *currentProcess = nullptr;
    Process *lastProcess = nullptr;

    // External-mutation queue, drained on the Arduino loop task.
    QueueHandle_t cmdQueue = nullptr;

#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    // Most-recent ScaleSample, written from UART poll callback, read by getters.
    mutable SemaphoreHandle_t scaleSampleMutex = nullptr;
    ScaleSample lastScaleSample{};
    static constexpr float HARDWARE_SCALE_MAX_ABS_G = 5000.0f;
    static constexpr float HARDWARE_SCALE_MAX_STDDEV_G = 5.0f;
    static constexpr unsigned long HARDWARE_SCALE_BREW_TARE_TIMEOUT_MS = 4000;
    bool pendingHardwareScaleBrewStart = false;
    bool pendingHardwareScaleBrewStartReady = false;
    unsigned long pendingHardwareScaleBrewTareStartedAt = 0;
#endif

    unsigned long grindActiveUntil = 0;
    unsigned long lastBluetoothMeasurement = 0;
    unsigned long lastBrewBluetoothMeasurement = 0;
    unsigned long lastGrindBluetoothMeasurement = 0;
    unsigned long lastPing = 0;
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
    // Re-send the config burst for a few seconds after a (re)connect (see loop()).
    unsigned long configResendUntil = 0;
    unsigned long lastConfigResend = 0;
    static const unsigned long CONFIG_RESEND_WINDOW_MS = 8000;
    static const unsigned long CONFIG_RESEND_INTERVAL_MS = 1000;
    bool volumetricOverride = false;
    bool hardwareScalePresent = false;
    // INFO/capability payload has been read and applied (capability latched,
    // calibration sent, controller:ready fired). Decoupled from `connected` so a
    // PING that latches the connection early can't skip capability detection.
    bool infoApplied = false;
    // controller:bluetooth:connect has been announced for this connection.
    bool connectionAnnounced = false;
    bool processCompleted = false;
    bool steamReady = false;
    bool sdcard = false;
    int error = 0;

    // Source the active process consumes weight from; set per-process in
    // activate()/activateGrind() via the role resolver.
    VolumetricMeasurementSource currentVolumetricSource = VolumetricMeasurementSource::INACTIVE;
    // Source retained with lastProcess so eligible movable scales can provide
    // post-stop carryover measurements without reopening the active source.
    VolumetricMeasurementSource lastVolumetricSource = VolumetricMeasurementSource::INACTIVE;
    static const unsigned long BLUETOOTH_MEASUREMENT_GRACE_MS = 2000;
    static const unsigned long CONTROLLER_WAITING_TIMEOUT_MS = 10000;

    xTaskHandle logicTaskHandle;

    static void loopLogicTask(void *arg);
};

#endif // CONTROLLER_H
