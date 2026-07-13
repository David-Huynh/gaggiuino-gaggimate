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
        // (Re)send calibration on first detection. On a cold boot the controller
        // may start publishing samples after the display already applied INFO; if
        // calibration was never (re)sent the samples stay flagged NOT_CALIBRATED
        // and never become brew-usable.
        sendCalibration();
        ESP_LOGI("HWScalePlugin", "Hardware scale detected");
    } else if (present && (s.healthBits & SCALE_HEALTH_NOT_CALIBRATED) && !(s.healthBits & SCALE_HEALTH_CALIBRATING) &&
               controller->getSettings().getScaleCalibration1() != 0.0f &&
               controller->getSettings().getScaleCalibration2() != 0.0f &&
               millis() - lastCalResendMs >= CAL_RESEND_INTERVAL_MS) {
        // The controller is publishing but reports NOT_CALIBRATED while the display
        // holds calibration — it lost it (the STM32 boots uncalibrated and does not
        // persist it, so this follows an STM32 reset or a missed push). The display
        // owns calibration, so re-push it. Throttled; gated on the display actually
        // having calibration so a genuine first-run scale is not spammed; and skipped
        // while a calibration is in progress so we never disrupt the cal wizard.
        ESP_LOGW("HWScalePlugin", "HW scale NOT_CALIBRATED but display has calibration; re-sending");
        sendCalibration();
    }

    // Auto-establish a hardware zero once per boot when the scale is calibrated
    // but has no tare offset (offsets 0,0 -> the controller emits raw counts, i.e.
    // the tens-of-thousands-of-grams readings). Only while idle (no running
    // process) so we zero an at-rest platform, and only once; a loaded/noisy
    // platform makes the tare state machine fail safely without writing bad
    // offsets. The display owns offsets, so the result persists across boots.
    if (!autoTareRequested && present && !controller->isActive() &&
        controller->getSettings().getScaleCalibration1() != 0.0f &&
        controller->getSettings().getScaleCalibration2() != 0.0f && controller->getSettings().getScaleOffset1() == 0 &&
        controller->getSettings().getScaleOffset2() == 0 &&
        !(s.healthBits & (SCALE_HEALTH_TARING | SCALE_HEALTH_CALIBRATING))) {
        autoTareRequested = true;
        ESP_LOGI("HWScalePlugin", "Calibrated scale has no tare offset; auto-zeroing once (idle)");
        controller->scaleTare();
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
    if (!controller->isHardwareScaleSampleHealthy(s)) {
        return;
    }

    controller->onVolumetricMeasurement(s.weightG, VolumetricMeasurementSource::HARDWARE_SCALE);
}

void HWScalePlugin::sendCalibration() {
    controller->sendScaleCalibration(controller->getSettings().getScaleCalibration1(),
                                     controller->getSettings().getScaleCalibration2());
    lastCalResendMs = millis();
}

void HWScalePlugin::tare() {
    if (!present)
        return;
    controller->scaleTare();
}
#endif
