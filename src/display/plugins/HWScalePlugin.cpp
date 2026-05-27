#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
#include "HWScalePlugin.h"
#include <cmath>
#include <display/core/Controller.h>

HWScalePlugin HWScale;

void HWScalePlugin::setup(Controller *_controller, PluginManager *_pluginManager) {
    this->controller = _controller;
    this->pluginManager = _pluginManager;

    _pluginManager->on("controller:scale:sample", [this](Event const &) { onSample(controller->getScaleSample()); });

    _pluginManager->on("controller:mode:change", [this](Event const &event) { active = event.getInt("value") != MODE_STANDBY; });

    _pluginManager->on("controller:ready", [this](Event const &) { active = controller->getMode() != MODE_STANDBY; });
}

void HWScalePlugin::onSample(const ScaleSample &s) {
    lastSample = s;

    // First non-zero seq proves the STM32 is publishing HX711 samples. On a
    // first-run, uncalibrated scale the gram readings are intentionally NaN.
    if (!present && s.sampleSeq > 0) {
        present = true;
        controller->setHardwareScalePresent(true);
        controller->setVolumetricOverride(true);
        ESP_LOGI("HWScalePlugin", "Hardware scale detected, enabling volumetric override");
    }

    if (!active || !present) {
        return;
    }

    // Block the brew controller from acting on suspect samples. The user feels
    // a stuck brew worse than a brew that doesn't auto-stop, so when in doubt:
    // suppress.
    if (s.healthBits & BREW_BLOCKING_HEALTH) {
        return;
    }
    if (!std::isfinite(s.weightG)) {
        return;
    }

    controller->onVolumetricMeasurement(s.weightG, VolumetricMeasurementSource::HARDWARE_SCALE);
}

void HWScalePlugin::onProcessStart() {
    // Brew starts use an ESP32-side software baseline so HX711 offsets are not
    // changed during a shot. Manual tare still goes through tare().
}

void HWScalePlugin::tare() {
    if (!present)
        return;
    controller->scaleTare();
}
#endif
