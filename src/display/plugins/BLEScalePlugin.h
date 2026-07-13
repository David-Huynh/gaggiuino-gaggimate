#ifndef BLESCALEPLUGIN_H
#define BLESCALEPLUGIN_H
#include "../core/Plugin.h"
#include "../core/ScaleSourceResolver.h"
#include "remote_scales.h"
#include "remote_scales_plugin_registry.h"

void on_ble_measurement(float value);
void on_ble_brew_measurement(float value);
void on_ble_grind_measurement(float value);

constexpr unsigned long UPDATE_INTERVAL_MS = 1000;
constexpr unsigned int RECONNECTION_TRIES = 15;

class BLEScalePlugin : public Plugin {
  public:
    BLEScalePlugin();
    ~BLEScalePlugin();

    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;
    ;

    void connect(const std::string &uuid);
    void connect(const std::string &uuid, ScaleRole role);
    void scan() const;
    void disconnect();
    void disconnect(ScaleRole role);
    void onMeasurement(float value) const;
    void onMeasurement(ScaleRole role, float value) const;
    bool isConnected() const;
    bool isConnected(ScaleRole role) const;
    std::string getName() const;
    std::string getName(ScaleRole role) const;
    std::string getUUID() const;
    std::string getUUID(ScaleRole role) const;
    int getRSSI() const;
    int getRSSI(ScaleRole role) const;

    std::vector<DiscoveredDevice> getDiscoveredScales() const;
    void tare() const;

    // Accessors for the native scale fields that drivers optionally expose
    // (see RemoteScales). Each returns a sentinel value if not supported.
    float getFlowRate() const;
    bool hasFlowRate() const;
    uint8_t getBatteryLevel() const;
    bool hasBatteryLevel() const;
    ScaleWeightUnit getWeightUnit() const;
    bool hasWeightUnit() const;
    uint32_t getScaleTimerMs() const;
    bool hasScaleTimer() const;

  private:
    struct ScaleSlot {
        std::unique_ptr<RemoteScales> scale = nullptr;
        std::string uuid;
        bool doConnect = false;
        unsigned int reconnectionTries = 0;
        uint8_t lastBatteryLevel = REMOTE_SCALES_BATTERY_UNKNOWN;
        ScaleWeightUnit lastWeightUnit = ScaleWeightUnit::UNKNOWN;
        bool warnedOunceMidBrew = false;
        unsigned long lastMeasurementTime = 0;
    };

    void update();
    void updateSlot(ScaleRole role);
    void onProcessStart(ScaleRole role) const;
    void pollScaleMetadata(ScaleRole role);

    void establishConnection(ScaleRole role);
    ScaleSlot &slotFor(ScaleRole role);
    const ScaleSlot &slotFor(ScaleRole role) const;
    ScaleSlot *activeSlotFor(ScaleRole role);
    const ScaleSlot *activeSlotFor(ScaleRole role) const;
    bool sameScaleAssigned() const;
    std::string savedScaleFor(ScaleRole role) const;
    bool shouldUseOtherSlot(ScaleRole role) const;
    void connectSavedScale(ScaleRole role);

    bool active = false;

    unsigned long lastUpdate = 0;
    ScaleSlot brewSlot;
    ScaleSlot grindSlot;
    static constexpr unsigned long MIN_MEASUREMENT_INTERVAL_MS = 10; // Max 100 measurements per second

    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;
    RemoteScalesPluginRegistry *pluginRegistry = nullptr;
    RemoteScalesScanner *scanner = nullptr;
};

extern BLEScalePlugin BLEScales;

#endif // BLESCALEPLUGIN_H
