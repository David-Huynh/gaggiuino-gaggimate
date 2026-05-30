#ifndef GAGGIMATE_HEADLESS

#include "RatingPlugin.h"
#include "../core/Controller.h"
#include "../core/Event.h"
#include "../core/PluginManager.h"
#include <cstdint>
#include <cmath>

static constexpr lv_coord_t CARD_W = 390;
static constexpr lv_coord_t RATING_CARD_H = 210;
static constexpr lv_coord_t TAG_CARD_H = 300;
static constexpr lv_coord_t REC_CARD_H = 250;
static constexpr lv_coord_t BTN_W = 56;
static constexpr lv_coord_t BTN_H = 56;
static constexpr lv_coord_t BTN_SPACING = 8;

struct TasteTagOption {
    const char *value;
    const char *label;
};

static constexpr TasteTagOption TASTE_TAG_OPTIONS[] = {
    {"sour", "Sour"},
    {"bitter", "Bitter"},
    {"weak", "Weak"},
    {"harsh", "Harsh"},
    {"thin", "Thin"},
    {"astringent", "Astringent"},
    {"channeling_suspected", "Channeling"},
    {"balanced", "Balanced"},
};
static constexpr int TASTE_TAG_COUNT = sizeof(TASTE_TAG_OPTIONS) / sizeof(TASTE_TAG_OPTIONS[0]);

static void star_btn_event_cb(lv_event_t *e);
static void skip_btn_event_cb(lv_event_t *e);
static void tag_btn_event_cb(lv_event_t *e);
static void done_tags_btn_event_cb(lv_event_t *e);
static void no_tags_btn_event_cb(lv_event_t *e);
static void use_btn_event_cb(lv_event_t *e);
static void ignore_btn_event_cb(lv_event_t *e);
static void later_btn_event_cb(lv_event_t *e);

void RatingPlugin::setup(Controller *ctrl, PluginManager *pm) {
    controller = ctrl;
    pluginManager = pm;

    pm->on("rl:recommendation:received", [this](Event const &event) {
        storeRecommendation(event);
        if (!feedbackOverlayActive() && shouldPromptRecommendation()) {
            showRecommendationOverlay();
        }
    });

    pm->on("rl:shot:complete", [this](Event const &event) {
        if (!controller || !controller->getSettings().isRLRatingEnabled())
            return;
        String shotId = event.getString("shot_id");
        if (!shotId.isEmpty()) {
            String recommendationId = event.getString("recommendation_id");
            showRatingOverlay(shotId);
            _pendingShotRecommendationId = recommendationId;
        }
    });

    pm->on("controller:brew:start", [this](Event const &) { closeOverlay(); });
}

void RatingPlugin::loop() {
    if (_overlay && _overlayMode == RLOverlayMode::RATING && (millis() - _shownAtMs > RATING_POPUP_TIMEOUT_MS)) {
        dismissOverlay();
    } else if (_overlay && _overlayMode == RLOverlayMode::TASTE_TAGS &&
               (millis() - _shownAtMs > RATING_POPUP_TIMEOUT_MS)) {
        submitFeedback();
    }
}

void RatingPlugin::storeRecommendation(Event const &event) {
    _pendingRecommendationId = event.getString("recommendation_id");
    if (_pendingRecommendationId.isEmpty())
        return;

    _pendingRecommendationStatus = event.getString("status");
    _pendingMode = event.getString("mode");
    _pendingGrindDeltaSteps = event.getInt("grind_delta_steps");
    _pendingNextDoseG = event.getFloat("next_dose_g");
    _pendingTargetYieldG = event.getFloat("target_yield_g");
    _pendingTargetRatio = event.getFloat("target_ratio");
    _hasPendingRecommendation = true;
}

bool RatingPlugin::shouldPromptRecommendation() const {
    if (!controller || !controller->getSettings().isRLRatingEnabled())
        return false;
    if (!_hasPendingRecommendation || _pendingRecommendationId.isEmpty())
        return false;
    return _pendingRecommendationStatus == "pending" || _pendingRecommendationStatus == "shown" || _pendingRecommendationStatus.isEmpty();
}

bool RatingPlugin::feedbackOverlayActive() const {
    return _overlayMode == RLOverlayMode::RATING || _overlayMode == RLOverlayMode::TASTE_TAGS;
}

void RatingPlugin::showRatingOverlay(const String &shotId) {
    closeOverlay();
    _pendingShotId = shotId;
    _pendingRating = 0;
    _selectedTasteTagMask = 0;
    _overlayMode = RLOverlayMode::RATING;
    _shownAtMs = millis();

    _overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(_overlay, 0, 0);
    lv_obj_set_style_bg_color(_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_overlay, 0, 0);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(_overlay);
    lv_obj_set_size(card, CARD_W, RATING_CARD_H);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Rate your shot");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *subtitle = lv_label_create(card);
    lv_label_set_text(subtitle, "Taste?");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 52);

    const lv_coord_t totalW = 5 * BTN_W + 4 * BTN_SPACING;
    const lv_coord_t startX = -totalW / 2;
    for (int i = 1; i <= 5; i++) {
        lv_obj_t *btn = lv_btn_create(card);
        lv_obj_set_size(btn, BTN_W, BTN_H);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, startX + CARD_W / 2 + (i - 1) * (BTN_W + BTN_SPACING), 92);
        lv_obj_set_style_radius(btn, BTN_W / 2, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xC8860A), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xE8A020), LV_STATE_PRESSED);
        lv_obj_set_user_data(btn, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(btn, star_btn_event_cb, LV_EVENT_CLICKED, this);

        char label[3];
        snprintf(label, sizeof(label), "%d", i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_center(lbl);
    }

    lv_obj_t *skipBtn = lv_btn_create(card);
    lv_obj_set_size(skipBtn, 80, 28);
    lv_obj_align(skipBtn, LV_ALIGN_TOP_MID, 0, 168);
    lv_obj_set_style_bg_color(skipBtn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(skipBtn, lv_color_hex(0x555555), LV_STATE_PRESSED);
    lv_obj_set_style_radius(skipBtn, 6, 0);
    lv_obj_add_event_cb(skipBtn, skip_btn_event_cb, LV_EVENT_CLICKED, this);

    lv_obj_t *skipLbl = lv_label_create(skipBtn);
    lv_label_set_text(skipLbl, "Skip");
    lv_obj_set_style_text_color(skipLbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_center(skipLbl);
}

void RatingPlugin::showTasteTagOverlay() {
    clearOverlay(false);
    _overlayMode = RLOverlayMode::TASTE_TAGS;
    _shownAtMs = millis();

    _overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(_overlay, 0, 0);
    lv_obj_set_style_bg_color(_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_overlay, 0, 0);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(_overlay);
    lv_obj_set_size(card, CARD_W, TAG_CARD_H);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "What was off?");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t *subtitle = lv_label_create(card);
    lv_label_set_text(subtitle, "Optional taste tags");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 48);

    for (int i = 0; i < TASTE_TAG_COUNT; i++) {
        const int col = i % 2;
        const int row = i / 2;
        lv_obj_t *btn = lv_btn_create(card);
        lv_obj_set_size(btn, 148, 34);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 38 + col * 166, 82 + row * 42);
        lv_obj_set_style_radius(btn, 7, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xC8860A), LV_STATE_CHECKED);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_user_data(btn, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(btn, tag_btn_event_cb, LV_EVENT_CLICKED, this);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, TASTE_TAG_OPTIONS[i].label);
        lv_obj_center(lbl);
    }

    lv_obj_t *doneBtn = lv_btn_create(card);
    lv_obj_set_size(doneBtn, 140, 38);
    lv_obj_align(doneBtn, LV_ALIGN_BOTTOM_LEFT, 48, -20);
    lv_obj_set_style_bg_color(doneBtn, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_radius(doneBtn, 8, 0);
    lv_obj_add_event_cb(doneBtn, done_tags_btn_event_cb, LV_EVENT_CLICKED, this);
    lv_obj_t *doneLbl = lv_label_create(doneBtn);
    lv_label_set_text(doneLbl, "Done");
    lv_obj_center(doneLbl);

    lv_obj_t *noTagsBtn = lv_btn_create(card);
    lv_obj_set_size(noTagsBtn, 140, 38);
    lv_obj_align(noTagsBtn, LV_ALIGN_BOTTOM_RIGHT, -48, -20);
    lv_obj_set_style_bg_color(noTagsBtn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(noTagsBtn, 8, 0);
    lv_obj_add_event_cb(noTagsBtn, no_tags_btn_event_cb, LV_EVENT_CLICKED, this);
    lv_obj_t *noTagsLbl = lv_label_create(noTagsBtn);
    lv_label_set_text(noTagsLbl, "No tags");
    lv_obj_center(noTagsLbl);
}

void RatingPlugin::showRecommendationOverlay() {
    closeOverlay();
    _overlayMode = RLOverlayMode::RECOMMENDATION;
    _shownAtMs = millis();

    _overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(_overlay, 0, 0);
    lv_obj_set_style_bg_color(_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_overlay, 0, 0);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(_overlay);
    lv_obj_set_size(card, CARD_W, REC_CARD_H);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Next shot");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    char recipe[128];
    const char *grindText = "Keep grind";
    char grindBuf[32];
    if (_pendingGrindDeltaSteps != 0) {
        snprintf(grindBuf, sizeof(grindBuf), "%+d grind step%s", _pendingGrindDeltaSteps,
                 std::abs(_pendingGrindDeltaSteps) == 1 ? "" : "s");
        grindText = grindBuf;
    }
    snprintf(recipe, sizeof(recipe), "%s\nGrind %.1fg dose\nYield %.1fg", grindText, _pendingNextDoseG, _pendingTargetYieldG);

    lv_obj_t *details = lv_label_create(card);
    lv_label_set_text(details, recipe);
    lv_label_set_long_mode(details, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(details, CARD_W - 56);
    lv_obj_set_style_text_color(details, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_text_align(details, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(details, LV_ALIGN_TOP_MID, 0, 52);

    lv_obj_t *useBtn = lv_btn_create(card);
    lv_obj_set_size(useBtn, 104, 42);
    lv_obj_align(useBtn, LV_ALIGN_BOTTOM_LEFT, 24, -22);
    lv_obj_set_style_bg_color(useBtn, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_radius(useBtn, 8, 0);
    lv_obj_add_event_cb(useBtn, use_btn_event_cb, LV_EVENT_CLICKED, this);
    lv_obj_t *useLbl = lv_label_create(useBtn);
    lv_label_set_text(useLbl, "Use");
    lv_obj_center(useLbl);

    lv_obj_t *laterBtn = lv_btn_create(card);
    lv_obj_set_size(laterBtn, 104, 42);
    lv_obj_align(laterBtn, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_style_bg_color(laterBtn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(laterBtn, 8, 0);
    lv_obj_add_event_cb(laterBtn, later_btn_event_cb, LV_EVENT_CLICKED, this);
    lv_obj_t *laterLbl = lv_label_create(laterBtn);
    lv_label_set_text(laterLbl, "Later");
    lv_obj_center(laterLbl);

    lv_obj_t *ignoreBtn = lv_btn_create(card);
    lv_obj_set_size(ignoreBtn, 104, 42);
    lv_obj_align(ignoreBtn, LV_ALIGN_BOTTOM_RIGHT, -24, -22);
    lv_obj_set_style_bg_color(ignoreBtn, lv_color_hex(0x7A2424), 0);
    lv_obj_set_style_radius(ignoreBtn, 8, 0);
    lv_obj_add_event_cb(ignoreBtn, ignore_btn_event_cb, LV_EVENT_CLICKED, this);
    lv_obj_t *ignoreLbl = lv_label_create(ignoreBtn);
    lv_label_set_text(ignoreLbl, "Ignore");
    lv_obj_center(ignoreLbl);
}

void RatingPlugin::clearOverlay(bool clearShotContext) {
    if (_overlay) {
        lv_obj_del(_overlay);
        _overlay = nullptr;
    }
    _overlayMode = RLOverlayMode::NONE;
    if (clearShotContext) {
        _pendingShotId = "";
        _pendingShotRecommendationId = "";
        _pendingRating = 0;
        _selectedTasteTagMask = 0;
    }
}

void RatingPlugin::closeOverlay() {
    clearOverlay(true);
}

void RatingPlugin::dismissOverlay() {
    closeOverlay();
    if (shouldPromptRecommendation()) {
        showRecommendationOverlay();
    }
}

void RatingPlugin::selectRating(int rating) {
    if (_pendingShotId.isEmpty())
        return;
    _pendingRating = rating;
    _selectedTasteTagMask = 0;
    showTasteTagOverlay();
}

bool RatingPlugin::toggleTasteTag(int index) {
    if (index < 0 || index >= TASTE_TAG_COUNT)
        return false;
    const uint16_t mask = static_cast<uint16_t>(1U << index);
    _selectedTasteTagMask ^= mask;
    return (_selectedTasteTagMask & mask) != 0;
}

void RatingPlugin::clearTasteTags() {
    _selectedTasteTagMask = 0;
}

String RatingPlugin::selectedTasteTagsCsv() const {
    String tags;
    for (int i = 0; i < TASTE_TAG_COUNT; i++) {
        const uint16_t mask = static_cast<uint16_t>(1U << i);
        if ((_selectedTasteTagMask & mask) == 0)
            continue;
        if (!tags.isEmpty())
            tags += ",";
        tags += TASTE_TAG_OPTIONS[i].value;
    }
    return tags;
}

void RatingPlugin::submitFeedback() {
    if (_pendingShotId.isEmpty())
        return;

    Event event;
    event.id = "rl:rating";
    event.setString("shot_id", _pendingShotId);
    event.setString("recommendation_id", _pendingShotRecommendationId);
    event.setInt("rating", _pendingRating);
    event.setString("taste_tags", selectedTasteTagsCsv());
    pluginManager->trigger(event);

    dismissOverlay();
}

void RatingPlugin::skipRating() {
    if (_pendingShotId.isEmpty())
        return;

    Event event;
    event.id = "rl:rating";
    event.setString("shot_id", _pendingShotId);
    event.setString("recommendation_id", _pendingShotRecommendationId);
    event.setInt("skipped", 1);
    pluginManager->trigger(event);

    dismissOverlay();
}

void RatingPlugin::useRecommendation() {
    if (_pendingRecommendationId.isEmpty())
        return;
    Event event;
    event.id = "rl:recommendation:apply";
    event.setString("recommendation_id", _pendingRecommendationId);
    pluginManager->trigger(event);
    _hasPendingRecommendation = false;
    closeOverlay();
}

void RatingPlugin::ignoreRecommendation() {
    if (_pendingRecommendationId.isEmpty())
        return;
    Event event;
    event.id = "rl:recommendation:ignore";
    event.setString("recommendation_id", _pendingRecommendationId);
    pluginManager->trigger(event);
    _hasPendingRecommendation = false;
    closeOverlay();
}

static void star_btn_event_cb(lv_event_t *e) {
    auto *plugin = static_cast<RatingPlugin *>(lv_event_get_user_data(e));
    auto *btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
    int rating = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(btn)));
    plugin->selectRating(rating);
}

static void skip_btn_event_cb(lv_event_t *e) {
    auto *plugin = static_cast<RatingPlugin *>(lv_event_get_user_data(e));
    plugin->skipRating();
}

static void tag_btn_event_cb(lv_event_t *e) {
    auto *plugin = static_cast<RatingPlugin *>(lv_event_get_user_data(e));
    auto *btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
    int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(btn)));
    plugin->toggleTasteTag(index);
}

static void done_tags_btn_event_cb(lv_event_t *e) {
    auto *plugin = static_cast<RatingPlugin *>(lv_event_get_user_data(e));
    plugin->submitFeedback();
}

static void no_tags_btn_event_cb(lv_event_t *e) {
    auto *plugin = static_cast<RatingPlugin *>(lv_event_get_user_data(e));
    plugin->clearTasteTags();
    plugin->submitFeedback();
}

static void use_btn_event_cb(lv_event_t *e) {
    auto *plugin = static_cast<RatingPlugin *>(lv_event_get_user_data(e));
    plugin->useRecommendation();
}

static void ignore_btn_event_cb(lv_event_t *e) {
    auto *plugin = static_cast<RatingPlugin *>(lv_event_get_user_data(e));
    plugin->ignoreRecommendation();
}

static void later_btn_event_cb(lv_event_t *e) {
    auto *plugin = static_cast<RatingPlugin *>(lv_event_get_user_data(e));
    plugin->closeOverlay();
}

#endif // GAGGIMATE_HEADLESS
