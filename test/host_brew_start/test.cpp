#include <cassert>
#include <cstdio>
#include <atomic>
#include <string>
#include <vector>
#include <optional>
#include <limits>
#include "HX711Scale.h"
#include "BrewTareOperation.h"
#include "StorageCoordinator.h"
#include "ScaleSourceResolver.h"
#include "logging.h"

unsigned long clockMs = 100;
bool driverReady = true, driverSaturated = false;
bool driverReadValid = true;
long driverRaw1 = 1000, driverRaw2 = 2000;
void HX711Dual::begin(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t) {}
void HX711Dual::power_up() {}
void HX711Dual::power_down() {}
void HX711Dual::set_gain(uint8_t) {}
bool HX711Dual::is_ready() const { return driverReady; }
bool HX711Dual::wait_ready_timeout(unsigned long timeout, unsigned long) {
    if (!driverReady) clockMs += timeout;
    return driverReady;
}
uint16_t HX711Dual::detect_rate() { return 10; }
HxResult HX711Dual::try_read(long raw[2], bool valid[2], bool sat[2]) {
    raw[0] = driverRaw1; raw[1] = driverRaw2;
    valid[0] = valid[1] = driverReadValid;
    sat[0] = sat[1] = driverSaturated;
    return HxResult::OK;
}

// Only external dependencies of the extracted Controller methods are faked.
using String = std::string;
constexpr int MODE_STANDBY = 0, MODE_BREW = 1, MODE_STEAM = 2, MODE_WATER = 3, MODE_GRIND = 4;
constexpr int STEAM_SAFETY_DURATION_MS = 120000;
enum class ProcessTarget { TIME, VOLUMETRIC };
struct Event { String id; void setString(const char *, const String &) {} };
struct Plugins { void trigger(const char *) {} void trigger(const Event &) {} };
struct Settings {
    bool momentary = false;
    bool isDelayAdjust() const { return false; }
    bool isMomentaryButtons() const { return momentary; }
    int getScaleSource() const { return SCALE_SOURCE_HARDWARE; }
    int getSteamPumpPercentage() const { return 20; }
    void setBrewDelay(double) {} void setGrindDelay(double) {}
    unsigned long getStandbyTimeout() const { return 0; }
};
struct Process {
    virtual ~Process() = default;
    virtual int getType() const { return MODE_BREW; }
    bool active = true;
    bool isUtility() const { return false; }
    bool isComplete() const { return true; }
    void progress() { active = false; }
};
struct BrewProcess : Process {
    ProcessTarget target = ProcessTarget::TIME;
    void updatePressure(float) {} void updateFlow(float) {} void updateWaterPumped(float) {}
    double getNewDelayTime() const { return -1; }
};
struct GrindProcess : Process {
    ProcessTarget target = ProcessTarget::TIME;
    double getNewDelayTime() const { return -1; }
};
struct SteamProcess : Process { SteamProcess(int, int) {} };
struct PumpProcess : Process {};
namespace PredictiveDelayPolicy {
bool supportsPostStopLearning(VolumetricMeasurementSource) { return false; }
}
struct Controller {
    struct DeferredEvent { const char *id; int utility = -1; };
    using DeferredProcessEvents = std::vector<DeferredEvent>;
    BrewTareOperation brewTare;
    std::atomic<const char *> brewStartError{""};
    std::recursive_mutex processMutex;
    std::optional<StorageCoordinator::ProcessLease> pendingProcessStorageLease, processStorageLease;
    VolumetricMeasurementSource currentVolumetricSource = VolumetricMeasurementSource::INACTIVE;
    VolumetricMeasurementSource lastVolumetricSource = VolumetricMeasurementSource::INACTIVE;
    struct Comms {
        uint32_t lastRequest = 0;
        void scaleTare(uint32_t id) { lastRequest = id; }
        void tare() {}
    } comms;
    Plugins plugins;
    Plugins *pluginManager = &plugins;
    Settings settings;
    struct Warnings { bool error = false; bool hasError() const { return error; } } warnings;
    bool flushPending = false;
    Process *currentProcess = nullptr, *lastProcess = nullptr;
    bool ready = true, started = false, steamReady = false, processCompleted = false;
    bool hardwareScalePresent = true;
    int mode = MODE_BREW;
    unsigned urgentStops = 0, endEvents = 0;
    unsigned long grindActiveUntil = 0, lastAction = 0;
    std::atomic<float> currentTemp{0}, pressure{0}, currentPumpFlow{0}, currentWaterPumped{0};
    bool isReady() const { return ready; }
    bool isErrorState() const { return false; }
    bool isActiveLocked() const { return currentProcess && currentProcess->active; }
    bool isActive() const { return isActiveLocked(); }
    float getTargetTemp() const { return 93; }
    void updateLastAction() {} void deactivateGrind() {} void activateStandby() {}
    void setMode(int value) { mode = value; }
    int getMode() const { return mode; }
    ScaleAvailability scaleAvailability() const {
        ScaleAvailability availability;
        availability.hardwarePresent = availability.hardwareCapable = true;
        return availability;
    }
    bool isVolumetricAvailable() const { return true; }
    void applyConnectionPriority() {}
    void startBrewProcess() {
        assert(brewTare.pending()); // manual tare remains excluded until process creation
        started = true;
        startProcess(new BrewProcess);
    }
    void loopControl(bool urgent = false) {
        if (urgent) {
            // Stop outputs before allowing flash work to resume.
            assert(StorageCoordinator::instance().processActive());
            ++urgentStops;
        }
    }
    void dispatchEvents(const DeferredProcessEvents &events) {
        for (const auto &event : events) {
            if (String(event.id) == "controller:process:end") {
                assert(!StorageCoordinator::instance().processActive());
                auto flash = StorageCoordinator::instance().tryAcquireFlash();
                assert(flash); // completion observers may persist before another shot
                ++endEvents;
            }
        }
    }
    bool armHardwareScaleBrewTare();
    void cancelHardwareScaleBrewTare(const char *reason);
    void pollHardwareScaleBrewTare();
    bool deactivateLocked(DeferredProcessEvents &events);
    void loopLogic();
    void activate(bool ignoreWarnings = false);
    void deactivate();
    void startProcess(Process *process);
    bool startProcessLocked(Process *process, DeferredProcessEvents &events);
    void clear();
    void clearLocked(DeferredProcessEvents &events);
    void handleBrewButton(bool pressed);
    void deactivateStandby();
    ~Controller() { delete currentProcess; delete lastProcess; }
};
#include "controller_methods.inc"

static void testOperation() {
    static_assert(BrewTareOperation::TIMEOUT_MS == 7000);
    BrewTareOperation op;
    const auto a = op.begin(100);
    assert(op.snapshot().requestId == a && op.snapshot().startedAt == 100);
    assert(op.snapshot().state == BrewTareOperation::Outcome::WAITING);
    assert(a && !op.begin(101));
    assert(!op.complete(0, true, 102) && !op.complete(a + 1, true, 102));
    assert(op.outcome(7099) == BrewTareOperation::Outcome::WAITING);
    assert(op.outcome(7100) == BrewTareOperation::Outcome::TIMED_OUT);
    assert(!op.complete(a, true, 7101));
    op.cancel();
    const auto b = op.begin(8000);
    assert(a != b && !op.complete(a, true, 14000));
    assert(op.complete(b, true, 14999)); // Accept a delayed result before seven seconds.
    assert(!op.complete(b, false, 14999));
    assert(op.outcome(14999) == BrewTareOperation::Outcome::SUCCEEDED);
    op.cancel();
    op.seed(UINT32_MAX);
    const auto wrapped = op.begin(UINT32_MAX - 100);
    assert(wrapped == 1);
    assert(op.outcome(6898) == BrewTareOperation::Outcome::WAITING);
    assert(op.outcome(6899) == BrewTareOperation::Outcome::TIMED_OUT);
    op.cancel();
    const auto c = op.begin(10);
    op.cancel();
    assert(!op.complete(c, true, 11));
    const auto d = op.begin(20);
    assert(op.complete(d, true, 7020));
    assert(op.outcome(7020) == BrewTareOperation::Outcome::TIMED_OUT);
    puts("PASS operation: correlation, duplicate/late replies, cancellation, deadlines, counter/clock wrap");
}

static void testResultValidation() {
    ScaleTareResult result{1, true, 100, 200, 0, 0, 0};
    assert(result.validSuccess());
    result.stddev1 = std::numeric_limits<float>::quiet_NaN(); assert(!result.validSuccess());
    result.stddev1 = -1; assert(!result.validSuccess());
    result.stddev1 = 0; result.offset1 = 8388608; assert(!result.validSuccess());
    result.offset1 = 0; result.healthBits = SCALE_HEALTH_TARE_FAILED; assert(!result.validSuccess());
    result.healthBits = SCALE_HEALTH_TARING; assert(!result.validSuccess());
    result.healthBits = 0x100; assert(!result.validSuccess());
    result.healthBits = 0; result.success = false; assert(!result.validSuccess());
    puts("PASS result validation: invalid offsets, noise, health and failure rejected");
}

static void testDriver() {
    static_assert(HX711Scale::TARE_TIMEOUT_MS == 6000);
    static_assert(BrewTareOperation::TIMEOUT_MS > HX711Scale::TARE_TIMEOUT_MS);
    std::vector<ScaleTareResult> results;
    HX711Scale scale(1, 2, 3, [](const ScaleSnapshot &) {}, 100, 100);
    scale.setTareDoneCallback([&](const ScaleTareResult &r) { results.push_back(r); });
    scale.setup(); scale.loop();
    scale.requestTare(10); scale.loop();
    assert(scale.snapshot().healthBits & SCALE_HEALTH_TARING);
    scale.requestTare(11);
    assert(results.size() == 1 && results.back().requestId == 11 && !results.back().success);
    driverReady = false;
    for (int i = 0; i < 39; ++i) scale.loop();
    assert(results.size() == 1); // 5850 ms without a ready ADC is still pending.
    scale.loop(); // Exactly six seconds: explicit failure, never stuck busy.
    assert(results.size() == 2 && results.back().requestId == 10 && !results.back().success);
    assert(!(scale.snapshot().healthBits & SCALE_HEALTH_TARING));
    driverReady = true;
    const auto normalStart = clockMs;
    scale.requestTare(12);
    for (int i = 0; i < 9; ++i) {
        driverRaw1 = 1000 + i * 2; driverRaw2 = -2000 - i * 4;
        clockMs += 100; scale.loop();
    }
    assert(results.size() == 2); // Nine fresh pairs cannot authorize a shot.
    driverRaw1 = 1018; driverRaw2 = -2036;
    clockMs += 100; scale.loop();
    assert(results.size() == 3 && results.back().requestId == 12 && results.back().validSuccess());
    assert(clockMs - normalStart == 1000);
    assert(results.back().offset1 == 1009 && results.back().offset2 == -2018);
    driverRaw1 = 1000; driverRaw2 = 2000;
    scale.requestTare(); // manual tare remains supported
    for (int i = 0; i < 9; ++i) { clockMs += 100; scale.loop(); }
    assert(results.size() == 3);
    clockMs += 100; scale.loop();
    assert(results.back().requestId == 0 && results.back().validSuccess());
    assert(results.back().offset1 == 1000 && results.back().offset2 == 2000);
    driverSaturated = true;
    scale.requestTare(13);
    for (int i = 0; i < 60; ++i) { clockMs += 100; scale.loop(); }
    assert(results.back().requestId == 13 && !results.back().success);
    driverSaturated = false;
    scale.requestTare(14); // Deadline also applies before the first successful read.
    driverReady = false;
    for (int i = 0; i < 40; ++i) scale.loop();
    assert(results.back().requestId == 14 && !results.back().success);
    driverReady = true;
    scale.requestTare(15);
    for (int i = 0; i < 9; ++i) { clockMs += 100; scale.loop(); }
    clockMs += 5100; // The tenth read exactly at the deadline must not succeed.
    scale.loop();
    assert(results.back().requestId == 15 && !results.back().success);
    // Delayed valid conversions must not force a manual tare or second attempt:
    // 4.5 s of invalid reads plus ten good pairs finishes before six seconds.
    scale.requestTare(16);
    driverReadValid = false;
    const auto beforeSettling = results.size();
    for (int i = 0; i < 45; ++i) { clockMs += 100; scale.loop(); }
    assert(results.size() == beforeSettling);
    driverReadValid = true;
    for (int i = 0; i < 9; ++i) { clockMs += 100; scale.loop(); }
    assert(results.size() == beforeSettling); // Invalid conversions were not counted.
    clockMs += 100; scale.loop();
    assert(results.back().requestId == 16 && results.back().validSuccess());
    puts("PASS actual HX711 driver: ten-pair mean, one-second tare, six-second timeout, delayed reads and recovery");
}

static void testController() {
    auto &storage = StorageCoordinator::instance();
    Controller c;
    c.pendingProcessStorageLease.emplace(storage.acquireProcess());
    c.armHardwareScaleBrewTare();
    const auto old = c.comms.lastRequest;
    c.cancelHardwareScaleBrewTare("deactivated");
    assert(!storage.processActive());
    assert(!c.brewTare.complete(old, true, clockMs));
    c.pendingProcessStorageLease.emplace(storage.acquireProcess());
    c.armHardwareScaleBrewTare();
    assert(!c.brewTare.complete(old, true, clockMs));
    c.ready = false; c.pollHardwareScaleBrewTare();
    assert(!c.started && !storage.processActive());
    c.ready = true;
    c.pendingProcessStorageLease.emplace(storage.acquireProcess());
    c.armHardwareScaleBrewTare();
    c.mode = MODE_STEAM; c.pollHardwareScaleBrewTare();
    assert(!c.started && !storage.processActive());
    c.mode = MODE_BREW;
    c.pendingProcessStorageLease.emplace(storage.acquireProcess());
    c.armHardwareScaleBrewTare();
    clockMs += BrewTareOperation::TIMEOUT_MS;
    c.pollHardwareScaleBrewTare();
    assert(!c.started && !storage.processActive());
    c.pendingProcessStorageLease.emplace(storage.acquireProcess());
    c.armHardwareScaleBrewTare();
    c.brewTare.complete(c.comms.lastRequest, false, clockMs);
    c.pollHardwareScaleBrewTare();
    assert(!c.started && !storage.processActive());
    for (int shot = 0; shot < 100; ++shot) {
        c.started = false;
        if (shot % 2 == 0)
            c.activate(); // the method reached by a web Start command
        else
            c.handleBrewButton(1); // physical latching button
        assert(c.brewTare.pending() && c.pendingProcessStorageLease && *c.pendingProcessStorageLease);
        const auto request = c.comms.lastRequest;
        c.activate(); // duplicate web/physical commands preserve this tare
        c.handleBrewButton(1);
        assert(c.comms.lastRequest == request);
        c.brewTare.complete(c.comms.lastRequest, true, clockMs);
        c.pollHardwareScaleBrewTare();
        assert(c.started && c.currentProcess && storage.processActive());
        if (shot % 4 == 0)
            c.deactivate(); // web Stop
        else if (shot % 4 == 1)
            c.handleBrewButton(0); // physical Stop
        else
            c.loopLogic(); // automatic completion
        assert(!storage.processActive() && !c.currentProcess && !c.brewTare.pending());
        assert(!c.pendingProcessStorageLease && !c.processStorageLease);
        auto flash = storage.tryAcquireFlash();
        assert(flash);
    }
    assert(c.urgentStops == 100 && c.endEvents == 100);
    c.warnings.error = true;
    c.activate();
    assert(!c.brewTare.pending() && !storage.processActive());
    c.ready = false;
    c.activate(true); // warning override cannot bypass controller readiness
    assert(!c.brewTare.pending() && !storage.processActive());
    c.ready = true;
    c.activate(true);
    assert(c.brewTare.pending() && storage.processActive());
    c.handleBrewButton(0); // physical release cancels a pending tare
    assert(!c.brewTare.pending() && !storage.processActive());
    puts("PASS actual Controller methods: cancellation/recovery; 100 web/button starts with automatic/manual stops");
}

int main() { testOperation(); testResultValidation(); testDriver(); testController(); }
