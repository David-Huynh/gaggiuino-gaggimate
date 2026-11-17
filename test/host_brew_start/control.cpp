#include <atomic>
#include <mutex>
#include <vector>
#include <functional>
#include <cassert>
#include <cstdio>
#include "GaggiMateComm.h"

unsigned long nowMs = 0;
unsigned long millis() { return nowMs; }
enum class CtrlCmd { CHANGE_MODE };
constexpr int MODE_STANDBY = 0, MODE_BREW = 1, MODE_STEAM = 2, MODE_GRIND = 4, ALT_RELAY_GRIND = 1;
enum class PumpTarget { PUMP_TARGET_PRESSURE };
struct Process {
    bool isActive() const { return false; }
    bool isAltRelayActive() const { return false; }
    bool isRelayActive() const { return false; }
    int getType() const { return MODE_BREW; }
    float getPumpValue() const { return 0; }
};
struct BrewProcess : Process {
    bool isAdvancedPump() const { return false; }
    PumpTarget getPumpTarget() const { return PumpTarget::PUMP_TARGET_PRESSURE; }
    float getPumpPressure() const { return 9; }
    float getPumpFlow() const { return 2; }
};
namespace gm { struct Payload { int kind; float value; }; }
struct Controller {
    struct Comms {
        bool connected = true;
        std::vector<gm::Payload> sent;
        std::function<void()> duringSend;
        bool isConnected() const { return connected; }
        gm::Payload buildBoilerControl(int, BoilerControlMode, float value) { return {1, value}; }
        gm::Payload buildPumpControl(int, PumpControlMode, float power, float, float) { return {2, power}; }
        gm::Payload buildRelayControl(int index, bool value) { return {3 + index, float(value)}; }
        void sendBatch(gm::Payload const *values, size_t count) {
            sent.insert(sent.end(), values, values + count);
            if (duringSend) duringSend();
        }
        void sendUrgentBatch(gm::Payload const *values, size_t count) { sendBatch(values, count); }
    } comms;
    struct Settings {
        int getTemperatureOffset() const { return 0; }
        int getAltRelayFunction() const { return ALT_RELAY_GRIND; }
        float getSteamPumpCutoff() const { return 9; }
        float getPressureOffset() const { return 0; }
    } settings;
    struct Info { bool protocolMismatch = false; struct { bool pressure = true; } capabilities; } systemInfo;
    std::atomic<bool> infoApplied{false}, controllerHeaterStatusKnown{false};
    bool initialized = true, waitingForController = false;
    unsigned standbyCommands = 0;
    struct Plugins { void trigger(char const *) {} } plugins;
    Plugins *pluginManager = &plugins;
    void applyConnectionPriority(bool) {}
    void postCommand(CtrlCmd, int mode) { assert(mode == MODE_STANDBY); ++standbyCommands; }
    void onControllerConnection(bool);
    std::recursive_mutex processMutex;
    Process *currentProcess = nullptr;
    std::atomic<uint32_t> controlRevision{1};
    uint32_t sentControlRevision = 0;
    unsigned long lastFullControlSend = 0;
    static constexpr unsigned long CONTROL_REFRESH_MS = 1000;
    BoilerCommand lastBoiler{};
    PumpCommand lastPump{};
    RelayCommand lastRelay{};
    bool lastAlt = false;
    float targetPressure = 0, targetFlow = 0, temperature = 93;
    float getTargetTemp() const { return temperature; }
    void updateControl(bool urgent = false);
};
#include "controller_control.inc"

int main() {
    Controller c;
    c.onControllerConnection(false);
    assert(c.standbyCommands == 0);
    c.infoApplied = true;
    c.onControllerConnection(false);
    assert(c.standbyCommands == 1);
    c.onControllerConnection(false);
    assert(c.standbyCommands == 1);
    c.updateControl();
    assert(c.comms.sent.empty()); // A configured UART port is not controller readiness.
    c.infoApplied = true;
    c.updateControl();
    assert(c.comms.sent.size() == 4 && c.comms.sent[0].value == 93);
    c.comms.sent.clear();
    nowMs = 100;
    c.updateControl();
    assert(c.comms.sent.empty());
    ++c.controlRevision; // INFO after STM32 restart, even with the same setpoint.
    c.updateControl();
    assert(c.comms.sent.size() == 4);
    c.comms.sent.clear();
    nowMs += 1000;
    c.updateControl(); // A dropped/expired initial command is retried without user mode changes.
    assert(c.comms.sent.size() == 4 && c.comms.sent[0].value == 93);
    c.comms.sent.clear();
    ++c.controlRevision;
    c.comms.duringSend = [&] { ++c.controlRevision; };
    c.updateControl();
    c.comms.duringSend = nullptr;
    c.comms.sent.clear();
    c.updateControl(); // A simultaneous session reset cannot be marked as already sent.
    assert(c.comms.sent.size() == 4);
    c.comms.sent.clear();
    c.systemInfo.protocolMismatch = true;
    c.updateControl(true);
    assert(c.comms.sent.empty());
    c.systemInfo.protocolMismatch = false;
    c.comms.connected = false;
    c.updateControl(true);
    assert(c.comms.sent.empty());
    puts("PASS control startup: handshake gating, same-target reset, dropped-command refresh, concurrent reset and protocol mismatch");
}
