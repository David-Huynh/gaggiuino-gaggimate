#include "HWScalePlugin.h"
#include <cmath>
#include <display/core/Controller.h>

HWScalePlugin HWScale;

void HWScalePlugin::setup(Controller *_controller, PluginManager *_pluginManager) {
    this->controller = _controller;
    this->pluginManager = _pluginManager;

    _pluginManager->on("controller:weight:change", [this](Event const &event) { onMeasurement(event.getFloat("value")); });

    _pluginManager->on("controller:brew:prestart", [this](Event const &) { onProcessStart(); });

    _pluginManager->on("controller:mode:change", [this](Event const &event) { active = event.getInt("value") != MODE_STANDBY; });

    _pluginManager->on("controller:ready", [this](Event const &) { active = controller->getMode() != MODE_STANDBY; });
}

void HWScalePlugin::onMeasurement(float weight) {
    unsigned long now = millis();
    if (now - lastMeasurementTime < MIN_MEASUREMENT_INTERVAL_MS) {
        return;
    }
    lastMeasurementTime = now;

    if (!isfinite(weight) || weight < -1000.0f || weight > 10000.0f) {
        ESP_LOGW("HWScalePlugin", "Invalid weight measurement: %f, ignoring", weight);
        return;
    }

    if (!present) {
        present = true;
        controller->setHardwareScalePresent(true);
        controller->setVolumetricOverride(true);
        ESP_LOGI("HWScalePlugin", "Hardware scale detected, enabling volumetric override");
    }

    currentWeight = weight;

    if (!active)
        return;

    controller->onVolumetricMeasurement(weight, VolumetricMeasurementSource::HARDWARE_SCALE);
}

void HWScalePlugin::onProcessStart() {
    if (!present)
        return;
    controller->scaleTare();
}

void HWScalePlugin::tare() {
    if (!present)
        return;
    controller->scaleTare();
}
