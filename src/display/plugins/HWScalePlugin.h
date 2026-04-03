#ifndef HWSCALEPLUGIN_H
#define HWSCALEPLUGIN_H

#include "../core/Plugin.h"

class HWScalePlugin : public Plugin {
  public:
    HWScalePlugin() = default;

    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override {}

    bool isPresent() const { return present; }
    float getWeight() const { return currentWeight; }
    void tare();

  private:
    void onMeasurement(float weight);
    void onProcessStart();

    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;
    bool present = false;
    float currentWeight = 0.0f;
    bool active = false;

    // Rate limiting
    mutable unsigned long lastMeasurementTime = 0;
    static constexpr unsigned long MIN_MEASUREMENT_INTERVAL_MS = 10;
};

extern HWScalePlugin HWScale;

#endif // HWSCALEPLUGIN_H
