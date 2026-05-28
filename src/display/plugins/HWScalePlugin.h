#ifndef HWSCALEPLUGIN_H
#define HWSCALEPLUGIN_H

#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
#include "../core/Plugin.h"
#include "GaggiMateComm.h"

class HWScalePlugin : public Plugin {
  public:
    HWScalePlugin() = default;

    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override {}

    bool isPresent() const { return present; }
    float getWeight() const { return lastSample.weightG; }
    const ScaleSample &lastReading() const { return lastSample; }
    void tare();

    // Health bits that disqualify the sample from feeding the brew controller.
    static constexpr uint16_t BREW_BLOCKING_HEALTH = SCALE_HEALTH_NOT_CALIBRATED | SCALE_HEALTH_STALE | SCALE_HEALTH_TARE_FAILED |
                                                     SCALE_HEALTH_SAT_CH1 | SCALE_HEALTH_SAT_CH2 | SCALE_HEALTH_TARING |
                                                     SCALE_HEALTH_CALIBRATING;

  private:
    void onSample(const ScaleSample &s);
    void onProcessStart();
    // Push the display-held calibration to the controller and arm the re-send throttle.
    void sendCalibration();

    // The STM32 boots uncalibrated and does not persist calibration, so if it ever
    // reports NOT_CALIBRATED while the display holds calibration we re-push it — but
    // no more often than this, to avoid spamming the UART on a persistent fault.
    static constexpr unsigned long CAL_RESEND_INTERVAL_MS = 2000;

    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;
    bool present = false;
    bool active = false;
    unsigned long lastCalResendMs = 0;
    ScaleSample lastSample{};
};

extern HWScalePlugin HWScale;

#endif
#endif // HWSCALEPLUGIN_H
