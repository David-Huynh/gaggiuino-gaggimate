#ifndef GAGGIMATE_COMM_H
#define GAGGIMATE_COMM_H

#include <cstdint>

// Public protocol vocabulary shared by GaggiMateClient and GaggiMateServer.
// Firmware code only ever sees these plain types -- never the nanopb structs.

// Pump control mode. Integer values match gaggimate_PumpMode in the schema.
enum class PumpControlMode : uint8_t {
    Power = 0,    // drive at a fixed power percentage
    Pressure = 1, // target a pressure, flow is the limit
    Flow = 2,     // target a flow, pressure is the limit
};

// Boiler control mode. Integer values match gaggimate_BoilerMode in the schema.
enum class BoilerControlMode : uint8_t {
    Temperature = 0, // setpoint is a target temperature in degC
    Pressure = 1,    // setpoint is a target pressure in bar
};

// Per-component commands, used to drive several components atomically in one
// frame and to detect changes (callers can compare against the last value sent
// and only transmit what actually changed).
struct BoilerCommand {
    uint8_t index = 0;
    BoilerControlMode mode = BoilerControlMode::Temperature;
    float setpoint = 0.0f;
    bool operator==(const BoilerCommand &o) const { return index == o.index && mode == o.mode && setpoint == o.setpoint; }
    bool operator!=(const BoilerCommand &o) const { return !(*this == o); }
};
struct PumpCommand {
    uint8_t index = 0;
    PumpControlMode mode = PumpControlMode::Power;
    float power = 0.0f;
    float pressure = 0.0f;
    float flow = 0.0f;
    bool operator==(const PumpCommand &o) const {
        return index == o.index && mode == o.mode && power == o.power && pressure == o.pressure && flow == o.flow;
    }
    bool operator!=(const PumpCommand &o) const { return !(*this == o); }
};
struct RelayCommand {
    uint8_t index = 0;
    bool open = false;
    bool operator==(const RelayCommand &o) const { return index == o.index && open == o.open; }
    bool operator!=(const RelayCommand &o) const { return !(*this == o); }
};

// Hardware scale snapshot shared by the controller and display. This is plain
// protocol vocabulary, not a transport-specific payload.
struct ScaleSample {
    float weightG = 0.0f;
    float stddevG = 0.0f;
    float ch1G = 0.0f;
    float ch2G = 0.0f;
    float ch1StdG = 0.0f;
    float ch2StdG = 0.0f;
    uint16_t healthBits = 0;
    uint32_t sampleSeq = 0;
};

// Numeric UART diagnostics that can be safely exposed in machine/shot payloads.
// Keep this sanitized: no raw frames, no secrets, no user content.
struct UartDiagnostics {
    uint32_t remoteErrorCount = 0;
    uint32_t remoteTimeoutCount = 0;
    uint32_t remoteRunawayCount = 0;
    uint32_t remoteRxOverflowCount = 0;
    uint32_t remoteUnknownCommandCount = 0;
    uint32_t remoteQueueDropCount = 0;
    uint32_t remoteQueueHighWatermark = 0;
    uint32_t remoteOutCommandCount = 0;
    uint32_t remoteAdvCommandCount = 0;
    uint32_t remotePingCommandCount = 0;
    uint32_t remoteValidCommandCount = 0;
    uint32_t remoteLastCommandAgeMs = 0;
    uint32_t remoteLoopLateCount = 0;
    uint32_t remoteLoopMaxLateMs = 0;
    uint32_t lastRemoteErrorCode = 0;
    uint32_t displayTxDropCount = 0;
    uint32_t displayRxOverflowCount = 0;
    uint32_t displayParsedEventCount = 0;
    uint32_t displayTxByteCount = 0;
    uint32_t displayRxByteCount = 0;
    uint32_t displayValidFrameCount = 0;
    uint32_t displayMalformedFrameCount = 0;
    uint32_t displayCrcErrorCount = 0;
    uint32_t displayLinkUpCount = 0;
    uint32_t displayLinkDownCount = 0;
};

struct ControllerDiagnostics {
    uint32_t errorCode = 0;
    bool thermocoupleError = false;
    uint32_t thermocoupleStatus = 0;
    uint32_t thermocoupleErrorCount = 0;
    uint32_t thermocoupleReadCount = 0;
    float thermocoupleRawTemperature = 0.0f;
    float thermocoupleTemperature = 0.0f;
    bool thermocoupleTaskRunning = false;
    float heaterSetpoint = 0.0f;
    float heaterOutput = 0.0f;
    bool heaterRelay = false;
    uint32_t boilerCommandCount = 0;
    uint32_t pumpCommandCount = 0;
    uint32_t relayCommandCount = 0;
    uint32_t pingCommandCount = 0;
    uint32_t tareCommandCount = 0;
    float lastBoilerSetpoint = 0.0f;
    float lastPumpPower = 0.0f;
    bool lastRelayOpen = false;
    uint32_t uartRxBytes = 0;
    uint32_t uartTxBytes = 0;
    uint32_t uartValidFrames = 0;
    uint32_t uartParsedPayloads = 0;
    uint32_t freeHeap = 0;
};

constexpr uint16_t SCALE_HEALTH_OK = 0;
constexpr uint16_t SCALE_HEALTH_NOT_CALIBRATED = 1u << 0;
constexpr uint16_t SCALE_HEALTH_SAT_CH1 = 1u << 1;
constexpr uint16_t SCALE_HEALTH_SAT_CH2 = 1u << 2;
constexpr uint16_t SCALE_HEALTH_STALE = 1u << 3;
constexpr uint16_t SCALE_HEALTH_TARE_FAILED = 1u << 4;
constexpr uint16_t SCALE_HEALTH_TARE_NOISY = 1u << 5;
constexpr uint16_t SCALE_HEALTH_TARING = 1u << 6;
constexpr uint16_t SCALE_HEALTH_CALIBRATING = 1u << 7;

// One LED output's target brightness. Several are packed into a single
// LedControl message so a multi-channel update can't be split (and coalesced)
// into separate frames.
struct LedChannelCommand {
    uint8_t channel = 0;
    uint8_t brightness = 0;
    bool operator==(const LedChannelCommand &o) const { return channel == o.channel && brightness == o.brightness; }
    bool operator!=(const LedChannelCommand &o) const { return !(*this == o); }
};

// Error codes. Values match the gaggimate.ErrorCode enum and the codes the old
// string protocol used, so existing firmware comparisons keep working.
constexpr int ERROR_CODE_NONE = 0;
constexpr int ERROR_CODE_COMM_SEND = 1;
constexpr int ERROR_CODE_COMM_RCV = 2;
constexpr int ERROR_CODE_PROTO_ERR = 3;
constexpr int ERROR_CODE_RUNAWAY = 4;
constexpr int ERROR_CODE_TIMEOUT = 5;
// Autotune hit its test-duration window without detecting a reaction. The
// controller skips the NVS PID persist; the display surfaces it without a
// watchdog-disconnect UX. Distinct from the generic TIMEOUT.
constexpr int ERROR_CODE_AUTOTUNE_TIMEOUT = 6;

#endif // GAGGIMATE_COMM_H
