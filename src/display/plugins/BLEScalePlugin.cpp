#include "BLEScalePlugin.h"
#include "remote_scales.h"
#include "remote_scales_plugin_registry.h"
#include <cctype>
#include <cmath> // For isfinite()
#include <display/core/Controller.h>
#include <NimBLEDevice.h>
#include <scales/acaia.h>
#include <scales/bookoo.h>
#include <scales/decent.h>
#include <scales/difluid.h>
#include <scales/dot.h>
#include <scales/eclair.h>
#include <scales/eureka.h>
#include <scales/felicitaScale.h>
#include <scales/myscale.h>
#include <scales/timemore.h>
#include <scales/varia.h>
#include <scales/weighmybru.h>

namespace {

bool startsWithIgnoreCase(const std::string &value, const char *prefix) {
    for (size_t i = 0; prefix[i] != '\0'; ++i) {
        if (i >= value.size()) {
            return false;
        }
        const auto lhs = static_cast<unsigned char>(value[i]);
        const auto rhs = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

bool handlesWeighMyBrewCompatibilityName(const DiscoveredDevice &device) {
    return startsWithIgnoreCase(device.getName(), "weighmybr");
}

std::unique_ptr<RemoteScales> createWeighMyBrewScale(const DiscoveredDevice &device) {
    return std::make_unique<WeighMyBrewScales>(device);
}

void applyWeighMyBrewCompatibilityPlugin() {
    RemoteScalesPlugin plugin = RemoteScalesPlugin{
        .id = "plugin-weighmybrew-compat",
        .handles = handlesWeighMyBrewCompatibilityName,
        .initialise = createWeighMyBrewScale,
    };
    RemoteScalesPluginRegistry::getInstance()->registerPlugin(plugin);
}

const char *roleName(ScaleRole role) { return role == ScaleRole::BREW ? "brew" : "grind"; }

} // namespace

void on_ble_measurement(float value) {
    if (&BLEScales != nullptr) {
        BLEScales.onMeasurement(value);
    }
}

void on_ble_brew_measurement(float value) {
    if (xPortInIsrContext()) {
        return;
    }
    if (&BLEScales != nullptr) {
        BLEScales.onMeasurement(ScaleRole::BREW, value);
    }
}

void on_ble_grind_measurement(float value) {
    if (xPortInIsrContext()) {
        return;
    }
    if (&BLEScales != nullptr) {
        BLEScales.onMeasurement(ScaleRole::GRIND, value);
    }
}

BLEScalePlugin BLEScales;

BLEScalePlugin::BLEScalePlugin() = default;

BLEScalePlugin::~BLEScalePlugin() noexcept {
    try {
        // Disable active flag first to stop processing
        active = false;

        // Give any running callbacks time to complete
        delay(100);

        // Ensure proper cleanup
        disconnect();

        if (scanner != nullptr) {
            // Stop scanning first
            scanner->stopAsyncScan();
            // Give it time to actually stop
            delay(50);
            delete scanner;
            scanner = nullptr;
        }
    } catch (...) {
        // Swallow: destructors must not propagate exceptions.
        // NimBLE + Arduino delay() calls don't throw in practice; belt-and-braces.
    }
}

void BLEScalePlugin::setup(Controller *controller, PluginManager *manager) {
    if (controller == nullptr || manager == nullptr) {
        ESP_LOGE("BLEScalePlugin", "Invalid controller or manager passed to setup");
        return;
    }

    this->controller = controller;
    this->pluginManager = manager;
    this->pluginRegistry = RemoteScalesPluginRegistry::getInstance();

    if (!NimBLEDevice::getInitialized()) {
        NimBLEDevice::init("GaggiMate");
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);
        NimBLEDevice::setMTU(256);
    }

    // Apply scale plugins with error checking
    AcaiaScalesPlugin::apply();
    BookooScalesPlugin::apply();
    DecentScalesPlugin::apply();
    DifluidScalesPlugin::apply();
    EclairScalesPlugin::apply();
    EurekaScalesPlugin::apply();
    FelicitaScalePlugin::apply();
    TimemoreScalesPlugin::apply();
    VariaScalesPlugin::apply();
    WeighMyBrewScalePlugin::apply();
    applyWeighMyBrewCompatibilityPlugin();
    myscalePlugin::apply();
    TimemoreDotScalesPlugin::apply();

    // Initialize scanner with error handling
    this->scanner = new (std::nothrow) RemoteScalesScanner();
    if (this->scanner == nullptr) {
        ESP_LOGE("BLEScalePlugin", "Failed to create RemoteScalesScanner - out of memory");
        return;
    }

    manager->on("controller:bluetooth:connect", [this](Event const &) {
        if (this->controller != nullptr && this->controller->getMode() != MODE_STANDBY) {
            ESP_LOGI("BLEScalePlugin", "Resuming scanning");
            scan();
            active = true;
        }
    });
    manager->on("controller:bluetooth:disconnect", [this](Event const &) {
        ESP_LOGW("BLEScalePlugin", "Controller disconnected, stopping BLE scan");
        active = false;
    });
    manager->on("controller:brew:prestart", [this](Event const &) {
        if (this->controller != nullptr &&
            this->controller->getCurrentVolumetricSource() == VolumetricMeasurementSource::BLUETOOTH) {
            onProcessStart(ScaleRole::BREW);
        }
    });
    manager->on("controller:brew:end", [this](Event const &) {
        const ScaleSlot *slot = activeSlotFor(ScaleRole::BREW);
        if (slot != nullptr && slot->scale != nullptr && slot->scale->isConnected() && slot->scale->hasTimerControl()) {
            slot->scale->stopTimer();
        }
    });
    manager->on("controller:grind:start", [this](Event const &) {
        if (this->controller != nullptr &&
            this->controller->getCurrentVolumetricSource() == VolumetricMeasurementSource::BLUETOOTH) {
            onProcessStart(ScaleRole::GRIND);
        }
    });
    manager->on("controller:mode:change", [this](Event const &event) {
        if (event.getInt("value") != MODE_STANDBY) {
            ESP_LOGI("BLEScalePlugin", "Resuming scanning");
            scan();
            active = true;
        } else {
            active = false;
        }
    });
}

void BLEScalePlugin::loop() {
    if (brewSlot.doConnect && brewSlot.scale == nullptr) {
        establishConnection(ScaleRole::BREW);
    }
    if (grindSlot.doConnect && grindSlot.scale == nullptr) {
        establishConnection(ScaleRole::GRIND);
    }
    if (!active) {
        if (brewSlot.scale != nullptr || grindSlot.scale != nullptr) {
            disconnect();
        }
        if (scanner != nullptr && scanner->isScanRunning()) {
            scanner->stopAsyncScan();
        }
    }
    const unsigned long now = millis();
    if (now - lastUpdate > UPDATE_INTERVAL_MS) {
        lastUpdate = now;
        update();
    }
}

void BLEScalePlugin::update() {
    // Graceful failure - if controller is null, just disable ourselves
    if (controller == nullptr) {
        ESP_LOGW("BLEScalePlugin", "Controller is null, disabling BLE scale");
        active = false;
        return;
    }

    if (!active)
        return;

    updateSlot(ScaleRole::BREW);
    updateSlot(ScaleRole::GRIND);
}

void BLEScalePlugin::updateSlot(ScaleRole role) {
    if (shouldUseOtherSlot(role)) {
        return;
    }

    ScaleSlot &slot = slotFor(role);
    const bool hasConnectedScale = slot.scale != nullptr && slot.scale->isConnected();
    if (slot.scale != nullptr) {
        slot.scale->update();
        if (!hasConnectedScale) {
            slot.reconnectionTries++;
            if (slot.reconnectionTries > RECONNECTION_TRIES) {
                ESP_LOGW("BLEScalePlugin", "Max reconnection attempts reached, disconnecting %s scale",
                         role == ScaleRole::BREW ? "brew" : "grind");
                disconnect(role);
                if (scanner != nullptr) {
                    scanner->initializeAsyncScan();
                }
            }
        } else {
            pollScaleMetadata(role);
        }
        return;
    }

    connectSavedScale(role);
}

void BLEScalePlugin::connect(const std::string &uuid) {
    connect(uuid, ScaleRole::BREW);
    connect(uuid, ScaleRole::GRIND);
    if (controller != nullptr) {
        controller->getSettings().setSavedScale(uuid.data());
    }
}

void BLEScalePlugin::connect(const std::string &uuid, ScaleRole role) {
    if (uuid.empty()) {
        ESP_LOGE("BLEScalePlugin", "Cannot connect %s scale with empty UUID", roleName(role));
        return;
    }
    if (controller == nullptr) {
        ESP_LOGE("BLEScalePlugin", "Controller is null, cannot save %s scale setting", roleName(role));
        return;
    }

    ScaleSlot &slot = slotFor(role);
    if (slot.uuid != uuid) {
        disconnect(role);
    }
    slot.uuid = uuid;
    slot.doConnect = true;
    if (role == ScaleRole::BREW) {
        controller->getSettings().setSavedBrewScale(uuid.data());
    } else {
        controller->getSettings().setSavedGrindScale(uuid.data());
    }

    if (shouldUseOtherSlot(role)) {
        slot.doConnect = false;
    }
}

void BLEScalePlugin::scan() const {
    if (isConnected(ScaleRole::BREW) && isConnected(ScaleRole::GRIND)) {
        return;
    }
    if (scanner == nullptr) {
        ESP_LOGE("BLEScalePlugin", "Scanner not initialized, cannot start scan");
        return;
    }
    scanner->initializeAsyncScan();
}

void BLEScalePlugin::disconnect() {
    disconnect(ScaleRole::BREW);
    disconnect(ScaleRole::GRIND);
}

void BLEScalePlugin::disconnect(ScaleRole role) {
    ScaleSlot &slot = slotFor(role);
    if (slot.scale != nullptr) {
        delay(50);
        if (slot.scale) {
            slot.scale->disconnect();
        }
    }
    slot.scale = nullptr;
    slot.uuid = "";
    slot.doConnect = false;
    slot.reconnectionTries = 0;
    slot.lastBatteryLevel = REMOTE_SCALES_BATTERY_UNKNOWN;
    slot.lastWeightUnit = ScaleWeightUnit::UNKNOWN;
    slot.warnedOunceMidBrew = false;
    slot.lastMeasurementTime = 0;
}

void BLEScalePlugin::onProcessStart(ScaleRole role) const {
    const ScaleSlot *slot = activeSlotFor(role);
    if (slot != nullptr && slot->scale != nullptr && slot->scale->isConnected()) {
        slot->scale->tare();
        delay(50);
        if (slot->scale != nullptr && slot->scale->isConnected()) {
            slot->scale->tare();
        }
    }
}

void BLEScalePlugin::pollScaleMetadata(ScaleRole role) {
    ScaleSlot &slot = slotFor(role);
    const ScaleSlot *activeSlot = activeSlotFor(role);
    if (activeSlot == nullptr || activeSlot->scale == nullptr || !activeSlot->scale->isConnected() || pluginManager == nullptr) {
        return;
    }
    auto *pm = pluginManager;

    if (activeSlot->scale->hasBatteryLevel()) {
        const uint8_t pct = activeSlot->scale->getBatteryLevel();
        if (pct != slot.lastBatteryLevel && pct != REMOTE_SCALES_BATTERY_UNKNOWN) {
            slot.lastBatteryLevel = pct;
            pm->trigger("scale:battery:change", "value", static_cast<int>(pct));
        }
    }
}

void BLEScalePlugin::tare() const {
    onProcessStart(ScaleRole::BREW);
    if (!sameScaleAssigned()) {
        onProcessStart(ScaleRole::GRIND);
    }
}

void BLEScalePlugin::establishConnection(ScaleRole role) {
    if (shouldUseOtherSlot(role)) {
        slotFor(role).doConnect = false;
        return;
    }

    ScaleSlot &slot = slotFor(role);
    if (slot.uuid.empty()) {
        ESP_LOGE("BLEScalePlugin", "Cannot establish %s scale connection with empty UUID", roleName(role));
        return;
    }

    ESP_LOGI("BLEScalePlugin", "Connecting %s scale to %s", roleName(role), slot.uuid.c_str());
    if (scanner == nullptr) {
        ESP_LOGE("BLEScalePlugin", "Scanner not initialized, cannot establish connection");
        return;
    }

    scanner->stopAsyncScan();

    auto discoveredScales = scanner->getDiscoveredScales();
    bool deviceFound = false;

    for (const auto &d : discoveredScales) {
        if (d.getAddress().toString() == slot.uuid) {
            deviceFound = true;
            slot.reconnectionTries = 0;

            auto factory = RemoteScalesFactory::getInstance();
            if (factory == nullptr) {
                ESP_LOGE("BLEScalePlugin", "RemoteScalesFactory instance is null");
                return;
            }

            slot.scale = factory->create(d);
            if (!slot.scale) {
                ESP_LOGE("BLEScalePlugin", "Connection to device %s failed", d.getName().c_str());
                return;
            }

            slot.scale->setLogCallback([](std::string message) {
                if (!message.empty()) {
                    Serial.print(message.c_str());
                }
            });

            slot.scale->setWeightUpdatedCallback(role == ScaleRole::BREW ? on_ble_brew_measurement : on_ble_grind_measurement);

            bool connectResult = slot.scale->connect();
            if (!connectResult) {
                ESP_LOGW("BLEScalePlugin", "Failed to connect %s scale, retrying scan", roleName(role));
                disconnect(role);
                if (scanner != nullptr) {
                    scanner->initializeAsyncScan();
                }
            } else {
                slot.doConnect = false;
            }
            break;
        }
    }

    if (!deviceFound) {
        ESP_LOGW("BLEScalePlugin", "%s scale device %s not found in discovered scales", roleName(role), slot.uuid.c_str());
        if (scanner != nullptr) {
            scanner->initializeAsyncScan();
        }
    }
}

void BLEScalePlugin::onMeasurement(float value) const { onMeasurement(ScaleRole::BREW, value); }

void BLEScalePlugin::onMeasurement(ScaleRole role, float value) const {
    ScaleSlot &slot = const_cast<BLEScalePlugin *>(this)->slotFor(role);
    unsigned long now = millis();
    if (now - slot.lastMeasurementTime < MIN_MEASUREMENT_INTERVAL_MS) {
        return;
    }
    slot.lastMeasurementTime = now;

    if (controller == nullptr || !active) {
        return;
    }

    if (!isfinite(value) || value < -1000.0f || value > 10000.0f) {
        ESP_LOGW("BLEScalePlugin", "Invalid %s scale measurement value: %f, ignoring", roleName(role), value);
        return;
    }

    controller->noteBluetoothScaleMeasurement(role);
    if (sameScaleAssigned()) {
        controller->noteBluetoothScaleMeasurement(role == ScaleRole::BREW ? ScaleRole::GRIND : ScaleRole::BREW);
    }

    if (controller->isActive() && controller->getCurrentVolumetricSource() == VolumetricMeasurementSource::BLUETOOTH &&
        !sameScaleAssigned()) {
        const bool roleMatchesActiveProcess =
            controller->isGrindActive() ? role == ScaleRole::GRIND : role == ScaleRole::BREW;
        if (!roleMatchesActiveProcess) {
            return;
        }
    }

    controller->onVolumetricMeasurement(value, VolumetricMeasurementSource::BLUETOOTH);

    const ScaleSlot *activeSlot = activeSlotFor(role);
    if (activeSlot != nullptr && activeSlot->scale != nullptr && activeSlot->scale->hasFlowRate() && pluginManager != nullptr) {
        pluginManager->trigger("controller:volumetric-measurement:scale-flow:change", "value", activeSlot->scale->getFlowRate());
    }
}

BLEScalePlugin::ScaleSlot &BLEScalePlugin::slotFor(ScaleRole role) {
    return role == ScaleRole::BREW ? brewSlot : grindSlot;
}

const BLEScalePlugin::ScaleSlot &BLEScalePlugin::slotFor(ScaleRole role) const {
    return role == ScaleRole::BREW ? brewSlot : grindSlot;
}

BLEScalePlugin::ScaleSlot *BLEScalePlugin::activeSlotFor(ScaleRole role) {
    if (shouldUseOtherSlot(role)) {
        return &slotFor(role == ScaleRole::BREW ? ScaleRole::GRIND : ScaleRole::BREW);
    }
    return &slotFor(role);
}

const BLEScalePlugin::ScaleSlot *BLEScalePlugin::activeSlotFor(ScaleRole role) const {
    if (shouldUseOtherSlot(role)) {
        return &slotFor(role == ScaleRole::BREW ? ScaleRole::GRIND : ScaleRole::BREW);
    }
    return &slotFor(role);
}

std::string BLEScalePlugin::savedScaleFor(ScaleRole role) const {
    if (controller == nullptr) {
        return "";
    }
    const Settings &settings = controller->getSettings();
    String configured = role == ScaleRole::BREW ? settings.getSavedBrewScale() : settings.getSavedGrindScale();
    if (configured.isEmpty()) {
        configured = settings.getSavedScale();
    }
    return configured.c_str();
}

bool BLEScalePlugin::sameScaleAssigned() const {
    const std::string brewUuid = !brewSlot.uuid.empty() ? brewSlot.uuid : savedScaleFor(ScaleRole::BREW);
    const std::string grindUuid = !grindSlot.uuid.empty() ? grindSlot.uuid : savedScaleFor(ScaleRole::GRIND);
    return !brewUuid.empty() && brewUuid == grindUuid;
}

bool BLEScalePlugin::shouldUseOtherSlot(ScaleRole role) const {
    if (!sameScaleAssigned()) {
        return false;
    }
    const ScaleRole otherRole = role == ScaleRole::BREW ? ScaleRole::GRIND : ScaleRole::BREW;
    const ScaleSlot &other = slotFor(otherRole);
    return other.scale != nullptr || other.doConnect;
}

void BLEScalePlugin::connectSavedScale(ScaleRole role) {
    if (scanner == nullptr) {
        return;
    }
    if (shouldUseOtherSlot(role)) {
        return;
    }
    const std::string savedUuid = savedScaleFor(role);
    if (savedUuid.empty()) {
        return;
    }
    ScaleSlot &slot = slotFor(role);
    if (slot.uuid != savedUuid) {
        slot.uuid = savedUuid;
        slot.doConnect = false;
    }

    auto discoveredScales = scanner->getDiscoveredScales();
    for (const auto &d : discoveredScales) {
        if (d.getAddress().toString() == slot.uuid) {
            ESP_LOGI("BLEScalePlugin", "Connecting to last known %s scale", roleName(role));
            slot.doConnect = true;
            return;
        }
    }
    scanner->initializeAsyncScan();
}

bool BLEScalePlugin::isConnected() const { return isConnected(ScaleRole::BREW) || isConnected(ScaleRole::GRIND); }

bool BLEScalePlugin::isConnected(ScaleRole role) const {
    const ScaleSlot *slot = activeSlotFor(role);
    return slot != nullptr && slot->scale != nullptr && slot->scale->isConnected();
}

std::string BLEScalePlugin::getName() const {
    if (isConnected(ScaleRole::BREW)) {
        return getName(ScaleRole::BREW);
    }
    return getName(ScaleRole::GRIND);
}

std::string BLEScalePlugin::getName(ScaleRole role) const {
    const ScaleSlot *slot = activeSlotFor(role);
    return slot != nullptr && slot->scale != nullptr && slot->scale->isConnected() ? slot->scale->getDeviceName() : "";
}

std::string BLEScalePlugin::getUUID() const {
    if (isConnected(ScaleRole::BREW)) {
        return getUUID(ScaleRole::BREW);
    }
    return getUUID(ScaleRole::GRIND);
}

std::string BLEScalePlugin::getUUID(ScaleRole role) const {
    const ScaleSlot *slot = activeSlotFor(role);
    return slot != nullptr && slot->scale != nullptr && slot->scale->isConnected() ? slot->scale->getDeviceAddress() : "";
}

int BLEScalePlugin::getRSSI() const {
    if (isConnected(ScaleRole::BREW)) {
        return getRSSI(ScaleRole::BREW);
    }
    return getRSSI(ScaleRole::GRIND);
}

int BLEScalePlugin::getRSSI(ScaleRole role) const {
    const ScaleSlot *slot = activeSlotFor(role);
    return slot != nullptr && slot->scale != nullptr && slot->scale->isConnected() ? slot->scale->getRSSI() : 0;
}

float BLEScalePlugin::getFlowRate() const {
    const ScaleSlot *slot = activeSlotFor(ScaleRole::BREW);
    if (slot == nullptr || slot->scale == nullptr || !slot->scale->hasFlowRate()) {
        slot = activeSlotFor(ScaleRole::GRIND);
    }
    return slot != nullptr && slot->scale != nullptr && slot->scale->hasFlowRate() ? slot->scale->getFlowRate() : 0.0f;
}

bool BLEScalePlugin::hasFlowRate() const {
    const ScaleSlot *brew = activeSlotFor(ScaleRole::BREW);
    const ScaleSlot *grind = activeSlotFor(ScaleRole::GRIND);
    return (brew != nullptr && brew->scale != nullptr && brew->scale->hasFlowRate()) ||
           (grind != nullptr && grind->scale != nullptr && grind->scale->hasFlowRate());
}

uint8_t BLEScalePlugin::getBatteryLevel() const {
    const ScaleSlot *slot = activeSlotFor(ScaleRole::BREW);
    if (slot == nullptr || slot->scale == nullptr || !slot->scale->hasBatteryLevel()) {
        slot = activeSlotFor(ScaleRole::GRIND);
    }
    return slot != nullptr && slot->scale != nullptr && slot->scale->hasBatteryLevel() ? slot->scale->getBatteryLevel()
                                                                                       : REMOTE_SCALES_BATTERY_UNKNOWN;
}

bool BLEScalePlugin::hasBatteryLevel() const {
    const ScaleSlot *brew = activeSlotFor(ScaleRole::BREW);
    const ScaleSlot *grind = activeSlotFor(ScaleRole::GRIND);
    return (brew != nullptr && brew->scale != nullptr && brew->scale->hasBatteryLevel()) ||
           (grind != nullptr && grind->scale != nullptr && grind->scale->hasBatteryLevel());
}

ScaleWeightUnit BLEScalePlugin::getWeightUnit() const {
    const ScaleSlot *slot = activeSlotFor(ScaleRole::BREW);
    if (slot == nullptr || slot->scale == nullptr || !slot->scale->hasWeightUnit()) {
        slot = activeSlotFor(ScaleRole::GRIND);
    }
    return slot != nullptr && slot->scale != nullptr && slot->scale->hasWeightUnit() ? slot->scale->getWeightUnit()
                                                                                    : ScaleWeightUnit::UNKNOWN;
}

bool BLEScalePlugin::hasWeightUnit() const {
    const ScaleSlot *brew = activeSlotFor(ScaleRole::BREW);
    const ScaleSlot *grind = activeSlotFor(ScaleRole::GRIND);
    return (brew != nullptr && brew->scale != nullptr && brew->scale->hasWeightUnit()) ||
           (grind != nullptr && grind->scale != nullptr && grind->scale->hasWeightUnit());
}

uint32_t BLEScalePlugin::getScaleTimerMs() const {
    const ScaleSlot *slot = activeSlotFor(ScaleRole::BREW);
    if (slot == nullptr || slot->scale == nullptr || !slot->scale->hasScaleTimer()) {
        slot = activeSlotFor(ScaleRole::GRIND);
    }
    return slot != nullptr && slot->scale != nullptr && slot->scale->hasScaleTimer() ? slot->scale->getScaleTimerMs() : 0;
}

bool BLEScalePlugin::hasScaleTimer() const {
    const ScaleSlot *brew = activeSlotFor(ScaleRole::BREW);
    const ScaleSlot *grind = activeSlotFor(ScaleRole::GRIND);
    return (brew != nullptr && brew->scale != nullptr && brew->scale->hasScaleTimer()) ||
           (grind != nullptr && grind->scale != nullptr && grind->scale->hasScaleTimer());
}

std::vector<DiscoveredDevice> BLEScalePlugin::getDiscoveredScales() const {
    if (scanner == nullptr) {
        ESP_LOGW("BLEScalePlugin", "Scanner not initialized, returning empty device list");
        return std::vector<DiscoveredDevice>();
    }
    return scanner->getDiscoveredScales();
}
