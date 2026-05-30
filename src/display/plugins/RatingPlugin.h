#ifndef RATINGPLUGIN_H
#define RATINGPLUGIN_H

#ifndef GAGGIMATE_HEADLESS

#include "../core/Event.h"
#include "../core/Plugin.h"
#include <lvgl.h>

constexpr unsigned long RATING_POPUP_TIMEOUT_MS = 30000;

enum class RLOverlayMode { NONE, RATING, RECOMMENDATION };

class RatingPlugin : public Plugin {
  public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;

    void submitRating(int rating);
    void useRecommendation();
    void ignoreRecommendation();
    void dismissOverlay();
    void closeOverlay();

  private:
    void showRatingOverlay(const String &shotId);
    void storeRecommendation(Event const &event);
    void showRecommendationOverlay();
    bool shouldPromptRecommendation() const;

    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;

    lv_obj_t *_overlay = nullptr;
    RLOverlayMode _overlayMode = RLOverlayMode::NONE;
    unsigned long _shownAtMs = 0;

    String _pendingShotId;
    String _pendingShotRecommendationId;
    String _pendingRecommendationId;
    String _pendingRecommendationStatus;
    String _pendingMode;
    int _pendingGrindDeltaSteps = 0;
    float _pendingNextDoseG = 0.0f;
    float _pendingTargetYieldG = 0.0f;
    float _pendingTargetRatio = 0.0f;
    bool _hasPendingRecommendation = false;
};

#endif // GAGGIMATE_HEADLESS
#endif // RATINGPLUGIN_H
