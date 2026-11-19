#include "DefaultUI.h"
#include <display/plugins/autotuning/AutoTuningTasteGoalJson.h>

#include <WiFi.h>
#include <display/core/AutoTuning.h>
#include <display/core/Controller.h>
#include <display/core/ProfileImage.h>
#include <display/core/process/BrewProcess.h>
#include <display/core/process/Process.h>
#include <display/core/zones.h>
#ifndef GAGGIMATE_SIM // hardware panel drivers are device-only
#include <display/drivers/AmoledDisplayDriver.h>
#include <display/drivers/LilyGoDriver.h>
#include <display/drivers/WaveshareDriver.h>
#include <display/drivers/common/LV_Helper.h>
#endif
#include <cmath>
#include <display/main.h>
#include <display/plugins/SmartDosePlugin.h>
#include <display/ui/utils/effects.h>
#include <esp32-hal-psram.h>
#include <utility>

#include "esp_sntp.h"

#include <display/ui/default/eez/images.h>
#include <display/ui/default/eez/ui.h>

static EffectManager effect_mgr;

static constexpr uint32_t STARTUP_FADE_MS = 1000; // start screen fade-in duration on power-up

static constexpr int32_t GAUGE_TICK_LONG = 25;      // meter tick length on most screens
static constexpr int32_t GAUGE_TICK_SHORT = 10;     // shortened tick length on profile / new-menu screens
static constexpr uint32_t GAUGE_TICK_ANIM_MS = 300; // tick length transition duration
static constexpr int16_t GAUGE_SETPOINT_INSIDE_PIVOT_X = -195;
static constexpr const char *TASTE_GOAL_LEVEL_LABELS[] = {"Any", "Low", "Medium", "High"};
static constexpr lv_coord_t AUTO_TUNING_CARD_MAX_WIDTH = 360;
static constexpr lv_coord_t AUTO_TUNING_CARD_MAX_HEIGHT = 410;

static lv_coord_t autoTuningCardWidth() {
    const lv_coord_t available = LV_HOR_RES - 96;
    return available < AUTO_TUNING_CARD_MAX_WIDTH ? available : AUTO_TUNING_CARD_MAX_WIDTH;
}

static lv_coord_t autoTuningCardHeight() {
    const lv_coord_t available = LV_VER_RES - 48;
    return available < AUTO_TUNING_CARD_MAX_HEIGHT ? available : AUTO_TUNING_CARD_MAX_HEIGHT;
}

static uint8_t tasteGoalLevelIndex(const String &level) {
    if (level == "low")
        return 1;
    if (level == "medium")
        return 2;
    if (level == "high")
        return 3;
    return 0;
}

static const char *tasteGoalLevelValue(const uint8_t index) {
    static constexpr const char *VALUES[] = {"", "low", "medium", "high"};
    return index < 4 ? VALUES[index] : "";
}

static void applyGaugeSetpointVisibility(lv_obj_t *obj, bool inside) {
    if (lv_obj_check_type(obj, &lv_meter_class)) {
        auto *meter = reinterpret_cast<lv_meter_t *>(obj);
        for (auto *indicator = static_cast<lv_meter_indicator_t *>(_lv_ll_get_head(&meter->indicator_ll)); indicator != nullptr;
             indicator = static_cast<lv_meter_indicator_t *>(_lv_ll_get_next(&meter->indicator_ll, indicator))) {
            if (indicator->type == LV_METER_INDICATOR_TYPE_NEEDLE_IMG) {
                const bool isInsideIndicator = indicator->type_data.needle_img.src == &img_indicator_small_inside;
                indicator->opa = inside == isInsideIndicator ? LV_OPA_COVER : LV_OPA_TRANSP;
                lv_obj_invalidate(obj);
            }
        }
    }

    const uint32_t childCount = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < childCount; i++)
        applyGaugeSetpointVisibility(lv_obj_get_child(obj, i), inside);
}

// Profile, menu and info screens, plus the status screen in chart mode, show shortened meter ticks.
static bool isShortTickScreen(ScreensEnum s, bool chartMode) {
    return s == SCREEN_ID_MENU_SCREEN_NEW || s == SCREEN_ID_INFO_SCREEN || s == SCREEN_ID_NEW_PROFILE_SCREEN ||
           (chartMode && s == SCREEN_ID_STATUS_SCREEN);
}

// Format a millisecond duration as "m:ss" for the brew/profile time labels.
static void formatDuration(unsigned long ms, char *buf, size_t len) {
    const double seconds = ms / 1000.0;
    const int minutes = static_cast<int>(seconds / 60.0);
    const int secs = static_cast<int>(seconds) % 60;
    snprintf(buf, len, "%d:%02d", minutes, secs);
}

static float clampPercentage(float pct) { return pct < 0.0f ? 0.0f : (pct > 100.0f ? 100.0f : pct); }

// Shot chart styling, after the community concept UI: thin lines, endpoint dot, no grid or fill.
// Theme color slots (Pressure, Progress, Temperature, Warning) so the chart matches the rest of the UI.
static constexpr int SHOT_THEME_COLORS[SHOT_SERIES_COUNT] = {7, 7, 2, 2, 6, 8};
static constexpr int SHOT_GUIDE_COLOR = 3; // theme SemiDark, for the baseline and phase markers
static constexpr lv_coord_t SHOT_DOT_RADIUS = 3;
static constexpr bool SHOT_IS_TARGET[SHOT_SERIES_COUNT] = {false, true, false, true, false, false};
static constexpr lv_coord_t SHOT_CHART_SCALE = 1000;    // every series is normalised to 0..1000 of its own range
static constexpr float SHOT_FLOW_RANGE_DEFAULT = 12.0f; // bar / ml/s
static constexpr float SHOT_WEIGHT_RANGE_DEFAULT = 50.0f;
static constexpr float SHOT_TEMP_RANGE = 10.0f; // profile temperature centred, ±10 °C
static constexpr uint32_t SHOT_ZOOM_ANIM_MS = 400;
static constexpr int32_t SHOT_ZOOM_ANIM_STEPS = 1000;

// Pressure teal and flow pink as in the web UI's profile chart, deepened where the profile card is light (dark themes).
static constexpr uint32_t PROFILE_COLORS[3][2] = {{0x2a8f8f, 0xe05a5a}, {0x4bc0c0, 0xffc0c0}, {0x2a8f8f, 0xe05a5a}};
static constexpr lv_coord_t PROFILE_LINE_WIDTH = 4;     // limit lines draw dashed and thinner
static constexpr float PROFILE_CHART_STEP = 0.1f;       // s, matches the web UI profile chart
static constexpr float PROFILE_CHART_MIN_RANGE = 10.0f; // bar / ml/s

static float applyEasing(float t, TransitionType type) {
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    switch (type) {
    case TransitionType::LINEAR:
        return t;
    case TransitionType::EASE_IN:
        return t * t;
    case TransitionType::EASE_OUT:
        return 1.0f - (1.0f - t) * (1.0f - t);
    case TransitionType::EASE_IN_OUT:
        return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
    default:
        return 1.0f;
    }
}

static lv_coord_t scaleShotValue(float value, float min, float max) {
    const int32_t scaled = lroundf((value - min) / (max - min) * SHOT_CHART_SCALE);
    return static_cast<lv_coord_t>(LV_CLAMP(0, scaled, SHOT_CHART_SCALE));
}

// Grow a range in whole steps until it fits value.
static float growShotRange(float range, float value, float step) {
    while (value > range)
        range += step;
    return range;
}

// Shared look of the live shot chart and the profile preview chart: thin lines, no grid, baseline and phase markers.
static void styleTrendChart(lv_obj_t *chart, uint16_t points) {
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, points);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, SHOT_CHART_SCALE);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_obj_set_style_border_opa(chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 0, LV_PART_MAIN);
    // Inset the plot by the dot radius so endpoint dots at the edges aren't clipped by the chart bounds.
    lv_obj_set_style_pad_all(chart, SHOT_DOT_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_left(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);
}

// The chart shares one line descriptor across all series, so set every field each segment.
static void styleTrendLine(lv_draw_line_dsc_t *line, bool secondary, lv_coord_t width = 2) {
    line->dash_width = secondary ? width + 1 : 0;
    line->dash_gap = secondary ? width + 1 : 0;
    line->opa = secondary ? LV_OPA_60 : LV_OPA_COVER;
    line->width = secondary ? width * 3 / 4 : width;
}

// Drawn before the series so the guides sit behind the lines.
static void drawTrendGuides(lv_event_t *e, int theme, const bool *phaseMarks, int markCount, int pointCount) {
    lv_obj_t *chart = lv_event_get_target(e);
    lv_draw_ctx_t *drawCtx = lv_event_get_draw_ctx(e);
    lv_area_t content;
    lv_obj_get_content_coords(chart, &content);
    const lv_coord_t width = lv_obj_get_content_width(chart);
    const lv_coord_t bottom = content.y1 + lv_obj_get_content_height(chart);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(theme_colors[theme][SHOT_GUIDE_COLOR]);
    line.width = 1;
    line.opa = LV_OPA_40;
    const lv_point_t baseStart = {content.x1, bottom};
    const lv_point_t baseEnd = {content.x2, bottom};
    lv_draw_line(drawCtx, &line, &baseStart, &baseEnd);

    line.opa = LV_OPA_30;
    line.dash_width = 2;
    line.dash_gap = 3;
    for (int i = 0; i < markCount; i++) {
        if (!phaseMarks[i])
            continue;
        const lv_coord_t x = static_cast<lv_coord_t>(content.x1 + width * i / (pointCount - 1));
        const lv_point_t top = {x, content.y1};
        const lv_point_t base = {x, bottom};
        lv_draw_line(drawCtx, &line, &top, &base);
    }
}

// EEZ string setters allocate a fresh StringRef on the LVGL heap each call; skip unchanged text to cut churn.
static bool stringChanged(const char *current, const char *next) {
    return current == nullptr || next == nullptr || strcmp(current, next) != 0;
}

static lv_color_t themeColor(const int index) { return lv_color_hex(theme_colors[eez_flow_get_selected_theme_index()][index]); }

static const char *autoTuningProviderLabel(const String &mode) {
    if (mode == AutoTuning::PROVIDER_OFF_BOARD) {
        return "Off-board";
    }
    if (mode == AutoTuning::PROVIDER_ON_BOARD) {
        return "On-board";
    }
    return "Disabled";
}

static uint16_t autoTuningProviderIndex(const String &mode) {
    if (mode == AutoTuning::PROVIDER_ON_BOARD) {
        return 1;
    }
    return 0;
}

static String autoTuningProviderModeFromIndex(const uint16_t index) {
    if (index == 1) {
        return AutoTuning::PROVIDER_ON_BOARD;
    }
    return AutoTuning::PROVIDER_OFF_BOARD;
}

static lv_obj_t *createAutoTuningLabel(lv_obj_t *parent, const char *text, const lv_coord_t x, const lv_coord_t y,
                                       const lv_coord_t w, const lv_font_t *font, const lv_color_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, LV_SIZE_CONTENT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, text);
    return label;
}

static void recipeDomainDecrementClicked(lv_event_t *event) {
    lv_obj_t *spinbox = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    lv_spinbox_decrement(spinbox);
    lv_event_send(spinbox, LV_EVENT_VALUE_CHANGED, nullptr);
}

static void recipeDomainIncrementClicked(lv_event_t *event) {
    lv_obj_t *spinbox = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    lv_spinbox_increment(spinbox);
    lv_event_send(spinbox, LV_EVENT_VALUE_CHANGED, nullptr);
}

static lv_obj_t *createRecipeDomainSpinbox(lv_obj_t *parent, const char *label, const lv_coord_t y, const lv_coord_t bodyWidth,
                                           const float value, const float minimum = 0.1f, const float maximum = 1000.0f) {
    createAutoTuningLabel(parent, label, 4, y + 8, bodyWidth - 184, &lv_font_montserrat_14, themeColor(0));

    lv_obj_t *spinbox = lv_spinbox_create(parent);
    lv_obj_set_pos(spinbox, bodyWidth - 128, y);
    lv_obj_set_size(spinbox, 78, 38);
    lv_spinbox_set_range(spinbox, static_cast<int32_t>(std::lround(minimum * 10.0f)),
                         static_cast<int32_t>(std::lround(maximum * 10.0f)));
    const uint8_t digits = maximum >= 1000.0f ? 5 : maximum >= 100.0f ? 4 : maximum >= 10.0f ? 3 : 2;
    lv_spinbox_set_digit_format(spinbox, digits, 1);
    lv_spinbox_set_step(spinbox, 1);
    lv_spinbox_set_value(spinbox, static_cast<int32_t>(std::lround(value * 10.0f)));
    lv_obj_set_style_text_align(spinbox, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(spinbox, themeColor(1), 0);
    lv_obj_set_style_text_color(spinbox, themeColor(0), 0);
    lv_obj_set_style_border_color(spinbox, themeColor(0), 0);
    lv_obj_set_style_radius(spinbox, 6, 0);

    lv_obj_t *minus = lv_btn_create(parent);
    lv_obj_set_pos(minus, bodyWidth - 176, y);
    lv_obj_set_size(minus, 40, 38);
    lv_obj_set_style_radius(minus, 6, 0);
    lv_obj_add_event_cb(minus, recipeDomainDecrementClicked, LV_EVENT_CLICKED, spinbox);
    lv_obj_t *minusLabel = lv_label_create(minus);
    lv_label_set_text(minusLabel, LV_SYMBOL_MINUS);
    lv_obj_center(minusLabel);

    lv_obj_t *plus = lv_btn_create(parent);
    lv_obj_set_pos(plus, bodyWidth - 42, y);
    lv_obj_set_size(plus, 40, 38);
    lv_obj_set_style_radius(plus, 6, 0);
    lv_obj_add_event_cb(plus, recipeDomainIncrementClicked, LV_EVENT_CLICKED, spinbox);
    lv_obj_t *plusLabel = lv_label_create(plus);
    lv_label_set_text(plusLabel, LV_SYMBOL_PLUS);
    lv_obj_center(plusLabel);
    return spinbox;
}

static void setAutoTuningSwitchState(lv_obj_t *sw, const bool checked, const bool enabled) {
    if (!sw) {
        return;
    }
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(sw, LV_STATE_CHECKED);
    }
    if (enabled) {
        lv_obj_clear_state(sw, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(sw, LV_STATE_DISABLED);
    }
}

static void autoTuningProviderChanged(lv_event_t *event) {
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(event));
    ui->setAutoTuningProviderFromIndex(lv_dropdown_get_selected(lv_event_get_target(event)));
}

static void autoTuningEnabledChanged(lv_event_t *event) {
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(event));
    ui->setAutoTuningEnabled(lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED));
}

static void autoTuningLocalChanged(lv_event_t *event) {
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(event));
    ui->setAutoTuningLocalOptimization(lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED));
}

static void autoTuningCommunityChanged(lv_event_t *event) {
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(event));
    ui->setAutoTuningCommunityUpload(lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED));
}

static void autoTuningCloseClicked(lv_event_t *event) {
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(event));
    ui->closeAutoTuningSettings();
}

static void autoTuningTasteGoalClicked(lv_event_t *event) {
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(event));
    ui->showTasteGoalSettings();
}

static void autoTuningDoseTargetChanged(lv_event_t *event) {
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(event));
    ui->setAutoTuningDoseTarget(lv_spinbox_get_value(lv_event_get_target(event)) / 10.0f);
}

static void autoTuningAdvancedClicked(lv_event_t *event) {
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(event));
    ui->showRecipeDomainSettings();
}

static void recipeDomainCloseClicked(lv_event_t *event) {
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(event));
    ui->closeRecipeDomainSettings();
}

static void recipeDomainSaveClicked(lv_event_t *event) {
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(event));
    ui->saveRecipeDomainSettings();
}

static void tasteGoalCloseClicked(lv_event_t *event) {
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(event));
    ui->closeTasteGoalSettings();
}

static void tasteGoalModeChanged(lv_event_t *event) {
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(event));
    ui->setTasteGoalModeFromIndex(lv_dropdown_get_selected(lv_event_get_target(event)));
}

static void tasteGoalLevelClicked(lv_event_t *event) {
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(event));
    ui->cycleTasteGoalLevel(lv_event_get_target(event));
}

static void tasteGoalSaveClicked(lv_event_t *event) {
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(event));
    ui->saveTasteGoalSettings();
}

int16_t calculate_angle(int set_temp, int range, int offset) {
    const double percentage = static_cast<double>(set_temp) / static_cast<double>(MAX_TEMP);
    return (percentage * ((double)range)) - range / 2 - offset;
}

void DefaultUI::reloadProfiles() { profileLoaded = 0; }

DefaultUI::DefaultUI(Controller *controller, Driver *driver, PluginManager *pluginManager)
    : controller(controller), panelDriver(driver), pluginManager(pluginManager) {
    setupPanel();
    xTaskCreatePinnedToCore(loopTask, "DefaultUI::loop", configMINIMAL_STACK_SIZE * 6, this, 1, &taskHandle, 1);
}

void DefaultUI::init() {
    profileManager = controller->getProfileManager();
    auto triggerRender = [this](Event const &) { rerender = true; };
    pluginManager->on("boiler:currentTemperature:change", [=](Event const &event) {
        int newTemp = static_cast<int>(event.getFloat("value"));
        if (newTemp != currentTemp) {
            currentTemp = newTemp;
            rerender = true;
        }
    });
    pluginManager->on("boiler:pressure:change", [=](Event const &event) {
        float newPressure = event.getFloat("value");
        if (round(newPressure * 10.0f) != round(pressure * 10.0f)) {
            pressure = newPressure;
            rerender = true;
        }
    });
    pluginManager->on("boiler:targetTemperature:change", [=](Event const &event) {
        int newTemp = static_cast<int>(event.getFloat("value"));
        if (newTemp != targetTemp) {
            targetTemp = newTemp;
            rerender = true;
        }
    });
    pluginManager->on("boiler:targetSteamTemperature:change", [=](Event const &event) {
        int newTemp = static_cast<int>(event.getFloat("value"));
        if (newTemp != targetSteamTemp) {
            targetTemp = newTemp;
            rerender = true;
        }
    });
    pluginManager->on("controller:targetVolume:change", [this](Event const &event) { rerender = true; });
    pluginManager->on("controller:targetDuration:change", [this](Event const &event) { rerender = true; });
    pluginManager->on("controller:grindDuration:change", [this](Event const &event) { rerender = true; });
    pluginManager->on("controller:grindVolume:change", [this](Event const &event) { rerender = true; });
    pluginManager->on("controller:grind:end", triggerRender);
    pluginManager->on("controller:grind:start", triggerRender);
    pluginManager->on("controller:process:end", triggerRender);
    pluginManager->on("controller:process:start", triggerRender);
    pluginManager->on("boiler:steamHeater:activate", triggerRender);
    pluginManager->on("boiler:steamHeater:deactivate", triggerRender);
    pluginManager->on("controller:mode:change", [this](Event const &event) {
        mode = event.getInt("value");
        if (mode >= MODE_STANDBY && mode <= MODE_GRIND)
            changeScreen(screenForMode(mode));
    });
    pluginManager->on("settings:changed", [this](Event const &) {
        if (welcomeActive && controller->getSettings().isSetupDone())
            onWelcomeContinue();
    });
    pluginManager->on("controller:brew:start", [this](Event const &event) { changeScreen(SCREEN_ID_STATUS_SCREEN); });
    pluginManager->on("controller:brew:clear", [this](Event const &event) {
        if (eez_flow_get_current_screen() == SCREEN_ID_STATUS_SCREEN) {
            changeScreen(SCREEN_ID_BREW_SCREEN);
        }
    });
    pluginManager->on("controller:bluetooth:waiting", [this](Event const &) {
        waitingForController = true;
        rerender = true;
    });
    pluginManager->on("controller:brew:confirm", [this](Event const &) {
        if (eez_flow_get_current_screen() != SCREEN_ID_BREW_SCREEN)
            changeScreen(SCREEN_ID_BREW_SCREEN);
        setBrewConfirmVisible(true);
    });
    // Answered elsewhere (web UI, other client): drop the overlay.
    pluginManager->on("controller:brew:confirm:cancel", [this](Event const &) { setBrewConfirmVisible(false); });
    pluginManager->on("controller:brew:start", [this](Event const &) { setBrewConfirmVisible(false); });
    pluginManager->on("controller:bluetooth:connect", [this](Event const &) {
        waitingForController = false;
        if (eez_flow_get_current_screen() == SCREEN_ID_STANDBY_SCREEN && !controller->getSystemInfo().protocolMismatch &&
            !initialized) {
            ::Settings &settings = controller->getSettings();
            if (settings.getStartupMode() == MODE_BREW) {
                changeScreen(SCREEN_ID_BREW_SCREEN);
            } else {
                standbyEnterTime = ::millis();
            }
        }
        initialized = true;
        pressureAvailable = controller->getSystemInfo().capabilities.pressure;
        rerender = true;
    });
    pluginManager->on("controller:bluetooth:disconnect", [this](Event const &) {
        waitingForController = true;
        rerender = true;
    });
    pluginManager->on("controller:wifi:connect", [this](Event const &event) {
        rerender = true;
        apActive = event.getInt("AP");
    });
    pluginManager->on("ota:update:start", [this](Event const &) {
        rerender = true;
        changeScreen(SCREEN_ID_STANDBY_SCREEN);
    });
    pluginManager->on("ota:update:end", [this](Event const &) {
        rerender = true;
        changeScreen(SCREEN_ID_STANDBY_SCREEN);
    });
    pluginManager->on("ota:update:status", [this](Event const &event) {
        rerender = true;
        updateAvailable = event.getInt("value");
    });
    pluginManager->on("controller:error", [this](Event const &) {
        rerender = true;
        changeScreen(SCREEN_ID_STANDBY_SCREEN);
    });
    pluginManager->on("controller:protocol:mismatch", [this](Event const &) {
        // Incompatible firmware on the other end: control is inhibited (OTA only),
        // so surface it on the standby screen like a runaway error.
        rerender = true;
        changeScreen(SCREEN_ID_STANDBY_SCREEN);
    });
    pluginManager->on("controller:autotune:start", [this](Event const &) { changeScreen(SCREEN_ID_STANDBY_SCREEN); });
    pluginManager->on("controller:autotune:result", [this](Event const &) { changeScreen(SCREEN_ID_STANDBY_SCREEN); });

    pluginManager->on("profiles:profile:select", [this](Event const &event) {
        reloadProfiles();
        rerender = true;
    });
    pluginManager->on("profiles:profile:favorite", [this](Event const &event) { reloadProfiles(); });
    pluginManager->on("profiles:profile:unfavorite", [this](Event const &event) { reloadProfiles(); });
    pluginManager->on("profiles:profile:save", [this](Event const &event) { reloadProfiles(); });
    pluginManager->on("profiles:image:change", [this](Event const &) {
        imageGeneration++;
        reloadProfiles();
        rerender = true;
    });
    pluginManager->on("controller:volumetric-measurement:active:change", [=](Event const &event) {
        double newWeight = event.getFloat("value");
        if (round(newWeight * 10.0) != round(activeWeight * 10.0)) {
            activeWeight = newWeight;
            rerender = true;
        }
    });
    // Task names cap at 15 chars; "DefaultUI::loopProfiles" read as "DefaultUI::loop" in panics. SD + JSON need the headroom.
    xTaskCreatePinnedToCore(profileLoopTask, "UI::profiles", configMINIMAL_STACK_SIZE * 6, this, 1, &profileTaskHandle, 0);
    pluginManager->on("controller:volumetric-measurement:estimation:change", [=](Event const &event) {
        double newWeight = event.getFloat("value");
        if (round(newWeight * 10.0) != round(estimatedWeight * 10.0)) {
            estimatedWeight = newWeight;
            rerender = true;
        }
    });
    pluginManager->on("controller:brew:prestart", [=](Event const &) {
        estimatedWeight = 0.0;
        rerender = true;
    });
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    pluginManager->on("controller:scale:sample", [=](Event const &event) {
        double newWeight = event.getFloat("value");
        if (round(newWeight * 10.0) != round(hardwareWeight * 10.0)) {
            hardwareWeight = newWeight;
            rerender = true;
        }
    });
#endif
    xTaskCreatePinnedToCore(profileLoopTask, "DefaultUI::loopProfiles", configMINIMAL_STACK_SIZE * 8, this, 1, &profileTaskHandle,
                            0);
}

// True while any pointer input device (the touch panel) is pressed.
static bool pointerPressed() {
    for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev != nullptr; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER && indev->proc.state == LV_INDEV_STATE_PRESSED)
            return true;
    }
    return false;
}

void DefaultUI::loop() {
    const unsigned long now = ::millis();
    const unsigned long diff = now - lastRender;

    if (touchFlushHeld) {
        if (!controller->isActive()) {
            touchFlushHeld = false;
        } else if (!pointerPressed()) {
            touchFlushHeld = false;
            controller->onFlushRelease();
        }
    }

    if (now - lastTempLog > TEMP_HISTORY_INTERVAL) {
        lastTempLog = now;
        if (++heatingFlashTick % 4 == 0) {
            heatingFlash = !heatingFlash;
            rerender = true;
        }
    }

    if ((controller->isActive() && diff > RERENDER_INTERVAL_ACTIVE) || diff > RERENDER_INTERVAL_IDLE) {
        rerender = true;
    }

    if (rerender) {
        updateAutoTuningSettingsModal();
        rerender = false;
        lastRender = now;
        applyTheme();
        if (controller->isErrorState()) {
            changeScreen(SCREEN_ID_STANDBY_SCREEN);
        }
        updateState();
        // Fill the EEZ data models before handleScreenChange() creates/ticks a screen (undefined fields abort the flow).
        updateSystemStatus();
        updateWarnings();
        updateProfileInfo();
        updateBoiler();
        updateBrewProcess();
    updateShotChart();
        const auto weightSource =
            controller->isActive() ? controller->getCurrentVolumetricSource() : controller->getResolvedBrewSource();
        double displayWeight = 0.0;
        if (weightSource == VolumetricMeasurementSource::HARDWARE_SCALE) {
            displayWeight = hardwareWeight;
        } else if (weightSource == VolumetricMeasurementSource::BLUETOOTH) {
            displayWeight = bluetoothWeight;
        } else if (weightSource == VolumetricMeasurementSource::FLOW_ESTIMATION) {
            displayWeight = estimatedWeight;
        }
        currentWeight = FloatValue(displayWeight);
        eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_SCALE_WEIGHT_CURRENT, currentWeight);

        char timeBuf[12];
        formatDuration(controller->getSettings().getTargetGrindDuration(), timeBuf, sizeof(timeBuf));
        if (stringChanged(grindTimeTarget.getString(), timeBuf)) {
            grindTimeTarget = StringValue(timeBuf);
            eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_GRIND_TIME_TARGET, grindTimeTarget);
        }
        grindWeightTarget = FloatValue(controller->getSettings().getTargetGrindVolume());
        eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_GRIND_WEIGHT_TARGET, grindWeightTarget);

        handleScreenChange();
        currentScreen = static_cast<ScreensEnum>(eez_flow_get_current_screen());
        updateProfilePreview();
        effect_mgr.evaluate_all();

        if (currentScreen == SCREEN_ID_STANDBY_SCREEN) {
            if (standbyEnterTime > 0) {
                const Settings &settings = controller->getSettings();
                const unsigned long now = millis();
                if (now - standbyEnterTime >= settings.getStandbyBrightnessTimeout()) {
                    setBrightness(settings.getStandbyBrightness());
                }
            }
        }
    }

    ui_tick();
    // EEZ owns visibility. Pulse only the selected Automagic icon while not ready.
    // Restore every other icon so a state change cannot leave it dimmed.
    static uint32_t lastDoseBlinkUpdate = 0;
    if (currentScreen == SCREEN_ID_BREW_SCREEN && millis() - lastDoseBlinkUpdate >= 100u) {
        lastDoseBlinkUpdate = millis();
        const auto dosing = SmartDose.getUIState();
        lv_obj_t *const icons[] = {objects.smart_dose_cup, objects.smart_dose_manual, objects.smart_dose_portafilter};
        for (int i = 0; i < 3; ++i) {
            lv_obj_t *icon = icons[i];
            if (icon == nullptr)
                continue;
            const lv_opa_t opacity = dosing.state == i + 1 && dosing.blinking && (millis() / 400u) % 2u ? 64 : LV_OPA_COVER;
            if (lv_obj_get_style_img_opa(icon, LV_PART_MAIN) != opacity)
                lv_obj_set_style_img_opa(icon, opacity, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
    lv_task_handler();
}

void DefaultUI::loopProfiles() {
    if (!profileLoaded) {
        // Build into locals and swap under the lock — the UI task reads these concurrently (GM-147).
        const auto favoritedIds = profileManager->getFavoritedProfiles();
        std::vector<String> ids;
        ids.reserve(favoritedIds.size() + 1);
        ids.emplace_back(controller->getSettings().getSelectedProfile());
        for (const auto &id : favoritedIds) {
            if (std::find(ids.begin(), ids.end(), id) == ids.end())
                ids.emplace_back(id);
        }
        std::vector<Profile> profiles;
        std::vector<bool> hasImage;
        std::vector<String> validIds;
        profiles.reserve(ids.size());
        hasImage.reserve(ids.size());
        validIds.reserve(ids.size());
        const std::vector<String> imageIds = controller->isSDCard() ? profileManager->listProfileImages() : std::vector<String>{};
        for (const auto &profileId : ids) {
            Profile profile{};
            if (!profileManager->loadProfile(profileId, profile)) {
                ESP_LOGW("DefaultUI", "Skipping unreadable profile %s", profileId.c_str());
                continue;
            }
            profiles.emplace_back(std::move(profile));
            validIds.emplace_back(profileId);
            hasImage.push_back(std::find(imageIds.begin(), imageIds.end(), profileId) != imageIds.end());
        }
        {
            std::lock_guard<std::mutex> guard(profilesMutex);
            favoritedProfileIds = std::move(validIds);
            favoritedProfiles = std::move(profiles);
            favoritedHasImage = std::move(hasImage);
            const int count = static_cast<int>(favoritedProfileIds.size());
            currentProfileIdx = count == 0 ? 0 : std::min(currentProfileIdx, count - 1);
            profilesGeneration++;
        }
        profileLoaded = 1;
    }
    loadRequestedImage();
}

// SD reads stay on this task: a slow or failing card must not block rendering or overflow the UI task's stack.
void DefaultUI::loadRequestedImage() {
    String wanted;
    int request;
    {
        std::lock_guard<std::mutex> guard(imageMutex);
        wanted = imageWantedId;
        request = imageRequest;
    }
    const int generation = imageGeneration;
    if (wanted.isEmpty() || (request == imageLoadedRequest && generation == imageLoadedGeneration))
        return;
    uint8_t *pixels = profileManager->loadProfileImage(wanted);
    imageLoadedRequest = request;
    imageLoadedGeneration = generation;
    if (pixels == nullptr)
        return;
    {
        std::lock_guard<std::mutex> guard(imageMutex);
        free(imagePending);
        imagePending = pixels;
        imagePendingId = wanted;
    }
    rerender = true;
}

ScreensEnum DefaultUI::screenForMode(int mode) {
    switch (mode) {
    case MODE_BREW:
        return SCREEN_ID_BREW_SCREEN;
    case MODE_GRIND:
        return SCREEN_ID_GRIND_SCREEN;
    case MODE_STEAM:
        return SCREEN_ID_STEAM_SCREEN;
    case MODE_WATER:
        return SCREEN_ID_WATER_SCREEN;
    default:
        return SCREEN_ID_STANDBY_SCREEN;
    }
}

void DefaultUI::onWelcomeContinue() {
    welcomeActive = false;
    changeScreen(screenForMode(mode));
}

void DefaultUI::changeScreen(ScreensEnum screen) {
    targetScreen = welcomeActive ? SCREEN_ID_WELCOME_SCREEN : screen;
    brewScreenState = BrewScreenState::Brew;
    rerender = true;
    // Reset some submenus
}

void DefaultUI::changeBrewScreenMode(BrewScreenState state) {
    brewScreenState = state;
    rerender = true;
}

void DefaultUI::onProfileSwitch() {
    currentProfileIdx = 0;
    profileDetailsVisible = false;
    changeScreen(SCREEN_ID_NEW_PROFILE_SCREEN);
}

void DefaultUI::onNextProfile() {
    std::lock_guard<std::mutex> guard(profilesMutex);
    if (currentProfileIdx + 1 < static_cast<int>(favoritedProfileIds.size())) {
        currentProfileIdx++;
        profileDetailsVisible = false;
    }
    rerender = true;
}

void DefaultUI::onPreviousProfile() {
    if (currentProfileIdx > 0) {
        currentProfileIdx--;
        profileDetailsVisible = false;
    }
    rerender = true;
}

void DefaultUI::onProfileDetailToggle() {
    profileDetailsVisible = !profileDetailsVisible;
    rerender = true;
}

void DefaultUI::onProfileSelect() {
    String id;
    {
        std::lock_guard<std::mutex> guard(profilesMutex);
        if (currentProfileIdx >= 0 && currentProfileIdx < static_cast<int>(favoritedProfileIds.size())) {
            id = favoritedProfileIds[currentProfileIdx];
        }
    }
    if (id.isEmpty() || !profileManager->selectProfile(id)) {
        reloadProfiles();
        return;
    }
    profileDirty = false;
    changeScreen(SCREEN_ID_BREW_SCREEN);
}

void DefaultUI::onVolumetricDelete() {
    controller->onVolumetricDelete();
    profileDirty = true;
}

void DefaultUI::showAutoTuningSettings() {
    if (autoTuningModal != nullptr) {
        updateAutoTuningSettingsModal();
        return;
    }

    const lv_coord_t cardWidth = autoTuningCardWidth();
    const lv_coord_t cardHeight = autoTuningCardHeight();
    const lv_coord_t bodyWidth = cardWidth - 40;

    autoTuningModal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(autoTuningModal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(autoTuningModal, 0, 0);
    lv_obj_set_style_bg_color(autoTuningModal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(autoTuningModal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(autoTuningModal, 0, 0);
    lv_obj_clear_flag(autoTuningModal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(autoTuningModal);
    lv_obj_set_size(card, cardWidth, cardHeight);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, themeColor(1), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, themeColor(0), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = createAutoTuningLabel(card, "Auto Tuning", 0, 14, cardWidth, &lv_font_montserrat_24, themeColor(0));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *closeBtn = lv_btn_create(card);
    lv_obj_set_size(closeBtn, 36, 32);
    lv_obj_align(closeBtn, LV_ALIGN_TOP_RIGHT, -28, 12);
    lv_obj_set_style_radius(closeBtn, 8, 0);
    lv_obj_set_style_bg_color(closeBtn, themeColor(0), 0);
    lv_obj_set_style_bg_color(closeBtn, themeColor(2), LV_STATE_PRESSED);
    lv_obj_add_event_cb(closeBtn, autoTuningCloseClicked, LV_EVENT_CLICKED, this);
    lv_obj_t *closeLabel = lv_label_create(closeBtn);
    lv_label_set_text(closeLabel, "X");
    lv_obj_set_style_text_color(closeLabel, themeColor(1), 0);
    lv_obj_center(closeLabel);

    lv_obj_t *body = lv_obj_create(card);
    lv_obj_set_pos(body, 20, 58);
    lv_obj_set_size(body, bodyWidth, cardHeight - 76);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_bottom(body, 18, 0);

    createAutoTuningLabel(body, "Auto Tuning", 4, 10, bodyWidth - 84, &lv_font_montserrat_16, themeColor(0));
    autoTuningEnabledSwitch = lv_switch_create(body);
    lv_obj_set_pos(autoTuningEnabledSwitch, bodyWidth - 66, 3);
    lv_obj_set_size(autoTuningEnabledSwitch, 58, 32);
    lv_obj_add_event_cb(autoTuningEnabledSwitch, autoTuningEnabledChanged, LV_EVENT_VALUE_CHANGED, this);

    createAutoTuningLabel(body, "Provider", 4, 62, 116, &lv_font_montserrat_16, themeColor(0));
    autoTuningProviderDropdown = lv_dropdown_create(body);
    lv_obj_set_pos(autoTuningProviderDropdown, 124, 52);
    lv_obj_set_size(autoTuningProviderDropdown, bodyWidth - 132, 40);
    lv_dropdown_set_options(autoTuningProviderDropdown, "Off-board\nOn-board");
    lv_obj_set_style_bg_color(autoTuningProviderDropdown, themeColor(1), 0);
    lv_obj_set_style_text_color(autoTuningProviderDropdown, themeColor(0), 0);
    lv_obj_set_style_border_color(autoTuningProviderDropdown, themeColor(0), 0);
    lv_obj_set_style_radius(autoTuningProviderDropdown, 8, 0);
    lv_obj_add_event_cb(autoTuningProviderDropdown, autoTuningProviderChanged, LV_EVENT_VALUE_CHANGED, this);

    createAutoTuningLabel(body, "Local optimization", 4, 116, bodyWidth - 84, &lv_font_montserrat_16, themeColor(0));
    autoTuningLocalSwitch = lv_switch_create(body);
    lv_obj_set_pos(autoTuningLocalSwitch, bodyWidth - 66, 107);
    lv_obj_set_size(autoTuningLocalSwitch, 58, 32);
    lv_obj_add_event_cb(autoTuningLocalSwitch, autoTuningLocalChanged, LV_EVENT_VALUE_CHANGED, this);

    createAutoTuningLabel(body, "Community upload", 4, 166, bodyWidth - 84, &lv_font_montserrat_16, themeColor(0));
    autoTuningCommunitySwitch = lv_switch_create(body);
    lv_obj_set_pos(autoTuningCommunitySwitch, bodyWidth - 66, 157);
    lv_obj_set_size(autoTuningCommunitySwitch, 58, 32);
    lv_obj_add_event_cb(autoTuningCommunitySwitch, autoTuningCommunityChanged, LV_EVENT_VALUE_CHANGED, this);

    const AutoTuning::RecipeDomain recipeDomain = controller->getSettings().getRLRecipeDomain();
    autoTuningDoseTarget =
        createRecipeDomainSpinbox(body, "Dose target (g)", 204, bodyWidth, controller->getSettings().getTargetGrindVolume(),
                                  recipeDomain.doseMinG, recipeDomain.doseMaxG);
    lv_obj_add_event_cb(autoTuningDoseTarget, autoTuningDoseTargetChanged, LV_EVENT_VALUE_CHANGED, this);

    createAutoTuningLabel(body, "Taste goal", 4, 266, 116, &lv_font_montserrat_16, themeColor(0));
    autoTuningTasteGoalButton = lv_btn_create(body);
    lv_obj_set_pos(autoTuningTasteGoalButton, 124, 254);
    lv_obj_set_size(autoTuningTasteGoalButton, bodyWidth - 132, 40);
    lv_obj_set_style_radius(autoTuningTasteGoalButton, 8, 0);
    lv_obj_set_style_bg_color(autoTuningTasteGoalButton, themeColor(0), 0);
    lv_obj_add_event_cb(autoTuningTasteGoalButton, autoTuningTasteGoalClicked, LV_EVENT_CLICKED, this);
    autoTuningTasteGoalButtonLabel = lv_label_create(autoTuningTasteGoalButton);
    lv_obj_set_width(autoTuningTasteGoalButtonLabel, bodyWidth - 152);
    lv_label_set_long_mode(autoTuningTasteGoalButtonLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(autoTuningTasteGoalButtonLabel, themeColor(1), 0);
    lv_obj_center(autoTuningTasteGoalButtonLabel);

    createAutoTuningLabel(body, "Optimizer", 4, 316, 116, &lv_font_montserrat_16, themeColor(0));
    autoTuningAdvancedButton = lv_btn_create(body);
    lv_obj_set_pos(autoTuningAdvancedButton, 124, 304);
    lv_obj_set_size(autoTuningAdvancedButton, bodyWidth - 132, 40);
    lv_obj_set_style_radius(autoTuningAdvancedButton, 8, 0);
    lv_obj_set_style_bg_color(autoTuningAdvancedButton, themeColor(0), 0);
    lv_obj_add_event_cb(autoTuningAdvancedButton, autoTuningAdvancedClicked, LV_EVENT_CLICKED, this);
    lv_obj_t *advancedLabel = lv_label_create(autoTuningAdvancedButton);
    lv_label_set_text(advancedLabel, "Advanced");
    lv_obj_set_style_text_color(advancedLabel, themeColor(1), 0);
    lv_obj_center(advancedLabel);

    autoTuningStopButton = lv_btn_create(body);
    lv_obj_set_pos(autoTuningStopButton, 4, 360);
    lv_obj_set_size(autoTuningStopButton, bodyWidth - 12, 52);
    lv_obj_set_style_bg_color(autoTuningStopButton, themeColor(0), 0);
    lv_obj_add_event_cb(autoTuningStopButton, [](lv_event_t *event) {
        static_cast<DefaultUI *>(lv_event_get_user_data(event))->confirmStopOptimization();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t *stopLabel = lv_label_create(autoTuningStopButton);
    lv_label_set_text(stopLabel, "Stop & use best shot");
    lv_obj_set_style_text_color(stopLabel, themeColor(1), 0);
    lv_obj_center(stopLabel);
    autoTuningResumeButton = lv_btn_create(body);
    lv_obj_set_pos(autoTuningResumeButton, 4, 424);
    lv_obj_set_size(autoTuningResumeButton, bodyWidth - 12, 48);
    lv_obj_set_style_bg_color(autoTuningResumeButton, themeColor(0), 0);
    lv_obj_add_event_cb(autoTuningResumeButton, [](lv_event_t *event) {
        static_cast<DefaultUI *>(lv_event_get_user_data(event))->resumeOptimization();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t *resumeLabel = lv_label_create(autoTuningResumeButton);
    lv_label_set_text(resumeLabel, "Resume exploration");
    lv_obj_set_style_text_color(resumeLabel, themeColor(1), 0);
    lv_obj_center(resumeLabel);

    autoTuningStatusLabel = createAutoTuningLabel(body, "", 4, 486, bodyWidth - 12, &lv_font_montserrat_16, themeColor(0));
    autoTuningSummaryLabel = createAutoTuningLabel(body, "", 4, 574, bodyWidth - 12, &lv_font_montserrat_14, themeColor(3));

    updateAutoTuningSettingsModal();
}

void DefaultUI::confirmStopOptimization() {
    if (autoTuningStopConfirmation) return;
    autoTuningControlError = "";
    autoTuningStopConfirmation = lv_obj_create(lv_layer_top());
    lv_obj_set_size(autoTuningStopConfirmation, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(autoTuningStopConfirmation, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(autoTuningStopConfirmation, LV_OPA_60, 0);
    lv_obj_clear_flag(autoTuningStopConfirmation, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *card = lv_obj_create(autoTuningStopConfirmation);
    const lv_coord_t width = autoTuningCardWidth();
    const lv_coord_t height = autoTuningCardHeight();
    lv_obj_set_size(card, width, height);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, themeColor(1), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    createAutoTuningLabel(card, "Keep your best recipe?", 8, 8, width - 48, &lv_font_montserrat_20, themeColor(0));
    lv_obj_t *body = lv_obj_create(card);
    lv_obj_set_pos(body, 0, 48);
    lv_obj_set_size(body, width - 32, height - 210);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    String description = String("Bean: ") + controller->getSettings().getRLBeanContextName() +
        "\nProfile: " + controller->getProfileManager()->getSelectedProfile().label +
        "\nGoal: " + AutoTuning::activeTasteGoalSummary(controller->getSettings()) +
        "\n\nStop exploring and restore the best recorded shot for this profile. "
        "An unanswered comparison will be discarded. Move the grinder manually to the restored setting.";
    lv_obj_t *label = createAutoTuningLabel(body, description.c_str(), 0, 0, width - 70, &lv_font_montserrat_16, themeColor(0));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    for (int choice = 0; choice < 2; ++choice) {
        lv_obj_t *button = lv_btn_create(card);
        lv_obj_set_pos(button, 4, height - 152 + choice * 60);
        lv_obj_set_size(button, width - 48, 52);
        lv_obj_set_style_bg_color(button, themeColor(0), 0);
        lv_obj_add_event_cb(button, choice == 0 ? +[](lv_event_t *event) {
            static_cast<DefaultUI *>(lv_event_get_user_data(event))->finishStopConfirmation(true);
        } : +[](lv_event_t *event) {
            static_cast<DefaultUI *>(lv_event_get_user_data(event))->finishStopConfirmation(false);
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *buttonLabel = lv_label_create(button);
        lv_label_set_text(buttonLabel, choice == 0 ? "Stop & restore best" : "Keep optimizing");
        lv_obj_set_style_text_color(buttonLabel, themeColor(1), 0);
        lv_obj_center(buttonLabel);
    }
}

void DefaultUI::finishStopConfirmation(bool confirmed) {
    if (!autoTuningStopConfirmation) return;
    if (confirmed) {
        Event event;
        event.id = "rl:optimization:stop-best";
        pluginManager->trigger(event);
        if (event.getInt("control_persisted") <= 0) autoTuningControlError = event.getString("error");
    }
    lv_obj_del(autoTuningStopConfirmation);
    autoTuningStopConfirmation = nullptr;
    updateAutoTuningSettingsModal();
    markDirty();
}

void DefaultUI::resumeOptimization() {
    Event event;
    event.id = "rl:optimization:resume-exploration";
    pluginManager->trigger(event);
    autoTuningControlError = event.getInt("control_persisted") > 0 ? "" : event.getString("error");
    updateAutoTuningSettingsModal();
    markDirty();
}

void DefaultUI::closeAutoTuningSettings() {
    if (autoTuningStopConfirmation) lv_obj_del(autoTuningStopConfirmation);
    autoTuningStopConfirmation = nullptr;
    autoTuningStopButton = nullptr;
    autoTuningResumeButton = nullptr;
    autoTuningControlError = "";
    closeTasteGoalSettings();
    closeRecipeDomainSettings();
    if (autoTuningModal != nullptr) {
        lv_obj_del(autoTuningModal);
    }
    autoTuningModal = nullptr;
    autoTuningEnabledSwitch = nullptr;
    autoTuningProviderDropdown = nullptr;
    autoTuningLocalSwitch = nullptr;
    autoTuningCommunitySwitch = nullptr;
    autoTuningDoseTarget = nullptr;
    autoTuningTasteGoalButton = nullptr;
    autoTuningTasteGoalButtonLabel = nullptr;
    autoTuningAdvancedButton = nullptr;
    autoTuningStatusLabel = nullptr;
    autoTuningSummaryLabel = nullptr;
}

void DefaultUI::setAutoTuningEnabled(const bool enabled) {
    Settings &settings = controller->getSettings();
    String mode = settings.getRLAutoTuningProviderMode();
    if (enabled && mode == AutoTuning::PROVIDER_DISABLED) {
        mode = AutoTuning::PROVIDER_OFF_BOARD;
    } else if (!enabled) {
        mode = AutoTuning::PROVIDER_DISABLED;
    }
    settings.batchUpdate([&](Settings *settings) {
        settings->setRLAutoTuningProviderMode(mode);
        settings->setRLAutoTuningEnabled(enabled);
        if (!enabled) {
            settings->setRLLocalOptimizationEnabled(false);
        }
    });
    if (pluginManager) {
        pluginManager->trigger("settings:changed");
        pluginManager->trigger("rl:settings:changed");
    }
    updateAutoTuningSettingsModal();
    markDirty();
}

void DefaultUI::setAutoTuningProviderFromIndex(const uint16_t index) {
    const String mode = autoTuningProviderModeFromIndex(index);
    Settings &settings = controller->getSettings();
    settings.batchUpdate([&](Settings *settings) {
        settings->setRLAutoTuningProviderMode(mode);
        settings->setRLAutoTuningEnabled(true);
    });
    if (pluginManager) {
        pluginManager->trigger("settings:changed");
        pluginManager->trigger("rl:settings:changed");
    }
    updateAutoTuningSettingsModal();
    markDirty();
}

void DefaultUI::setAutoTuningLocalOptimization(const bool enabled) {
    Settings &settings = controller->getSettings();
    const bool providerEnabled = AutoTuning::Router(settings.getRLOptimizerConfiguration()).enabled();
    settings.setRLLocalOptimizationEnabled(providerEnabled && enabled);
    if (pluginManager) {
        pluginManager->trigger("rl:settings:changed");
    }
    updateAutoTuningSettingsModal();
    markDirty();
}

void DefaultUI::setAutoTuningCommunityUpload(const bool enabled) {
    Settings &settings = controller->getSettings();
    settings.batchUpdate([&](Settings *settings) {
        settings->setRLCommunityUploadEnabled(enabled);
        settings->setRLCommunityUploadPrompted(true);
    });
    if (pluginManager) {
        pluginManager->trigger("rl:settings:changed");
    }
    updateAutoTuningSettingsModal();
    markDirty();
}

void DefaultUI::setAutoTuningDoseTarget(const float doseG) {
    Settings &settings = controller->getSettings();
    const AutoTuning::RecipeDomain domain = settings.getRLRecipeDomain();
    if (!std::isfinite(doseG) || doseG < domain.doseMinG || doseG > domain.doseMaxG) {
        return;
    }
    controller->setTargetGrindVolume(doseG);
    settings.setRLRecipeChangedAt(static_cast<long>(time(nullptr)));
    settings.save(true);
    if (pluginManager) {
        pluginManager->trigger("settings:changed");
        pluginManager->trigger("rl:settings:changed");
    }
    markDirty();
}

void DefaultUI::showRecipeDomainSettings() {
    if (recipeDomainModal != nullptr || controller == nullptr) {
        return;
    }

    const AutoTuning::RecipeDomain domain = controller->getSettings().getRLRecipeDomain();
    const lv_coord_t cardWidth = autoTuningCardWidth();
    const lv_coord_t cardHeight = autoTuningCardHeight();
    const lv_coord_t bodyWidth = cardWidth - 40;

    recipeDomainModal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(recipeDomainModal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(recipeDomainModal, 0, 0);
    lv_obj_set_style_bg_color(recipeDomainModal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(recipeDomainModal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(recipeDomainModal, 0, 0);
    lv_obj_clear_flag(recipeDomainModal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(recipeDomainModal);
    lv_obj_set_size(card, cardWidth, cardHeight);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, themeColor(1), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, themeColor(0), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = createAutoTuningLabel(card, "Recipe Space", 0, 12, cardWidth, &lv_font_montserrat_24, themeColor(0));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *closeBtn = lv_btn_create(card);
    lv_obj_set_size(closeBtn, 36, 32);
    lv_obj_align(closeBtn, LV_ALIGN_TOP_RIGHT, -28, 10);
    lv_obj_set_style_radius(closeBtn, 8, 0);
    lv_obj_set_style_bg_color(closeBtn, themeColor(0), 0);
    lv_obj_add_event_cb(closeBtn, recipeDomainCloseClicked, LV_EVENT_CLICKED, this);
    lv_obj_t *closeLabel = lv_label_create(closeBtn);
    lv_label_set_text(closeLabel, "X");
    lv_obj_set_style_text_color(closeLabel, themeColor(1), 0);
    lv_obj_center(closeLabel);

    lv_obj_t *body = lv_obj_create(card);
    lv_obj_set_pos(body, 20, 58);
    lv_obj_set_size(body, bodyWidth, cardHeight - 142);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_bottom(body, 8, 0);

    recipeDomainGrindRadius = createRecipeDomainSpinbox(body, "Grind radius", 4, bodyWidth, domain.grindRadiusSteps,
                                                        AutoTuning::RECIPE_DOMAIN_GRIND_RADIUS_MIN_STEPS,
                                                        AutoTuning::RECIPE_DOMAIN_GRIND_RADIUS_MAX_STEPS);
    recipeDomainDoseMin = createRecipeDomainSpinbox(body, "Dose min (g)", 52, bodyWidth, domain.doseMinG,
                                                    AutoTuning::RECIPE_DOMAIN_DOSE_MIN_G, AutoTuning::RECIPE_DOMAIN_DOSE_MAX_G);
    recipeDomainDoseMax = createRecipeDomainSpinbox(body, "Dose max (g)", 100, bodyWidth, domain.doseMaxG,
                                                    AutoTuning::RECIPE_DOMAIN_DOSE_MIN_G, AutoTuning::RECIPE_DOMAIN_DOSE_MAX_G);
    recipeDomainOutputMin =
        createRecipeDomainSpinbox(body, "Output min (g)", 148, bodyWidth, domain.targetOutputMinG,
                                  AutoTuning::RECIPE_DOMAIN_OUTPUT_MIN_G, AutoTuning::RECIPE_DOMAIN_OUTPUT_MAX_G);
    recipeDomainOutputMax =
        createRecipeDomainSpinbox(body, "Output max (g)", 196, bodyWidth, domain.targetOutputMaxG,
                                  AutoTuning::RECIPE_DOMAIN_OUTPUT_MIN_G, AutoTuning::RECIPE_DOMAIN_OUTPUT_MAX_G);

    recipeDomainStatusLabel =
        createAutoTuningLabel(card, "", 20, cardHeight - 78, cardWidth - 190, &lv_font_montserrat_14, themeColor(3));
    lv_obj_t *saveButton = lv_btn_create(card);
    lv_obj_set_size(saveButton, 140, 40);
    lv_obj_align(saveButton, LV_ALIGN_BOTTOM_RIGHT, -20, -14);
    lv_obj_set_style_radius(saveButton, 8, 0);
    lv_obj_set_style_bg_color(saveButton, themeColor(0), 0);
    lv_obj_add_event_cb(saveButton, recipeDomainSaveClicked, LV_EVENT_CLICKED, this);
    lv_obj_t *saveLabel = lv_label_create(saveButton);
    lv_label_set_text(saveLabel, "Save");
    lv_obj_set_style_text_color(saveLabel, themeColor(1), 0);
    lv_obj_center(saveLabel);
}

void DefaultUI::closeRecipeDomainSettings() {
    if (recipeDomainModal != nullptr) {
        lv_obj_del(recipeDomainModal);
    }
    recipeDomainModal = nullptr;
    recipeDomainGrindRadius = nullptr;
    recipeDomainDoseMin = nullptr;
    recipeDomainDoseMax = nullptr;
    recipeDomainOutputMin = nullptr;
    recipeDomainOutputMax = nullptr;
    recipeDomainStatusLabel = nullptr;
}

void DefaultUI::saveRecipeDomainSettings() {
    if (!recipeDomainGrindRadius || !recipeDomainDoseMin || !recipeDomainDoseMax || !recipeDomainOutputMin ||
        !recipeDomainOutputMax || controller == nullptr) {
        return;
    }
    const AutoTuning::RecipeDomain domain{
        lv_spinbox_get_value(recipeDomainGrindRadius) / 10.0f, lv_spinbox_get_value(recipeDomainDoseMin) / 10.0f,
        lv_spinbox_get_value(recipeDomainDoseMax) / 10.0f,     lv_spinbox_get_value(recipeDomainOutputMin) / 10.0f,
        lv_spinbox_get_value(recipeDomainOutputMax) / 10.0f,
    };
    std::string reason;
    const float currentDose = controller->getSettings().getTargetGrindVolume();
    if (!AutoTuning::validateRecipeDomain(domain, reason)) {
        if (recipeDomainStatusLabel) {
            lv_label_set_text(recipeDomainStatusLabel, reason.c_str());
        }
        return;
    }
    if (currentDose < domain.doseMinG || currentDose > domain.doseMaxG) {
        if (recipeDomainStatusLabel) {
            lv_label_set_text(recipeDomainStatusLabel, "Range must include current dose");
        }
        return;
    }
    if (!controller->getSettings().setRLRecipeDomain(domain)) {
        if (recipeDomainStatusLabel) {
            lv_label_set_text(recipeDomainStatusLabel, "Unable to save recipe space");
        }
        return;
    }
    controller->getSettings().save(true);
    if (pluginManager) {
        pluginManager->trigger("settings:changed");
        pluginManager->trigger("rl:settings:changed");
    }
    closeRecipeDomainSettings();
    updateAutoTuningSettingsModal();
    markDirty();
}

void DefaultUI::showTasteGoalSettings() {
    if (tasteGoalModal != nullptr || controller == nullptr) {
        updateTasteGoalSettingsModal();
        return;
    }

    JsonDocument activeGoal;
    AutoTuning::activeTasteGoal(controller->getSettings(), activeGoal);
    tasteGoalCustom = activeGoal["mode"].as<String>() == "custom";
    JsonObjectConst targets = activeGoal["targets"].as<JsonObjectConst>();
    for (size_t index = 0; index < AutoTuning::TASTE_GOAL_ATTRIBUTE_COUNT; ++index) {
        tasteGoalLevels[index] = tasteGoalLevelIndex(targets[AutoTuning::tasteGoalAttributeKey(index)].as<String>());
    }

    const lv_coord_t cardWidth = autoTuningCardWidth();
    const lv_coord_t cardHeight = autoTuningCardHeight();
    const lv_coord_t bodyWidth = cardWidth - 40;
    const lv_coord_t rowWidth = bodyWidth - 8;

    tasteGoalModal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(tasteGoalModal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(tasteGoalModal, 0, 0);
    lv_obj_set_style_bg_color(tasteGoalModal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tasteGoalModal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(tasteGoalModal, 0, 0);
    lv_obj_clear_flag(tasteGoalModal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(tasteGoalModal);
    lv_obj_set_size(card, cardWidth, cardHeight);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, themeColor(1), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, themeColor(0), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = createAutoTuningLabel(card, "Taste Goal", 0, 12, cardWidth, &lv_font_montserrat_24, themeColor(0));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *closeBtn = lv_btn_create(card);
    lv_obj_set_size(closeBtn, 36, 32);
    lv_obj_align(closeBtn, LV_ALIGN_TOP_RIGHT, -28, 10);
    lv_obj_set_style_radius(closeBtn, 8, 0);
    lv_obj_set_style_bg_color(closeBtn, themeColor(0), 0);
    lv_obj_add_event_cb(closeBtn, tasteGoalCloseClicked, LV_EVENT_CLICKED, this);
    lv_obj_t *closeLabel = lv_label_create(closeBtn);
    lv_label_set_text(closeLabel, "X");
    lv_obj_set_style_text_color(closeLabel, themeColor(1), 0);
    lv_obj_center(closeLabel);

    createAutoTuningLabel(card, "Mode", 24, 66, 66, &lv_font_montserrat_16, themeColor(0));
    tasteGoalModeDropdown = lv_dropdown_create(card);
    lv_obj_set_pos(tasteGoalModeDropdown, 94, 56);
    lv_obj_set_size(tasteGoalModeDropdown, cardWidth - 122, 40);
    lv_dropdown_set_options(tasteGoalModeDropdown, "Balanced\nCustom");
    lv_obj_set_style_bg_color(tasteGoalModeDropdown, themeColor(1), 0);
    lv_obj_set_style_text_color(tasteGoalModeDropdown, themeColor(0), 0);
    lv_obj_set_style_border_color(tasteGoalModeDropdown, themeColor(0), 0);
    lv_obj_set_style_radius(tasteGoalModeDropdown, 8, 0);
    lv_obj_add_event_cb(tasteGoalModeDropdown, tasteGoalModeChanged, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *body = lv_obj_create(card);
    lv_obj_set_pos(body, 20, 106);
    lv_obj_set_size(body, bodyWidth, cardHeight - 196);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);

    for (size_t index = 0; index < AutoTuning::TASTE_GOAL_ATTRIBUTE_COUNT; ++index) {
        lv_obj_t *row = lv_obj_create(body);
        lv_obj_set_pos(row, 0, static_cast<lv_coord_t>(index * 46));
        lv_obj_set_size(row, rowWidth, 44);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, themeColor(3), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        createAutoTuningLabel(row, AutoTuning::tasteGoalAttributeLabel(index), 4, 12, rowWidth - 116, &lv_font_montserrat_14,
                              themeColor(0));
        lv_obj_t *levelButton = lv_btn_create(row);
        lv_obj_set_pos(levelButton, rowWidth - 108, 5);
        lv_obj_set_size(levelButton, 104, 34);
        lv_obj_set_style_radius(levelButton, 8, 0);
        lv_obj_set_style_bg_color(levelButton, themeColor(0), 0);
        lv_obj_add_event_cb(levelButton, tasteGoalLevelClicked, LV_EVENT_CLICKED, this);
        lv_obj_t *levelLabel = lv_label_create(levelButton);
        lv_obj_set_style_text_color(levelLabel, themeColor(1), 0);
        lv_obj_center(levelLabel);
        tasteGoalLevelButtons[index] = levelButton;
    }

    tasteGoalStatusLabel =
        createAutoTuningLabel(card, "", 20, cardHeight - 82, cardWidth - 40, &lv_font_montserrat_14, themeColor(3));
    lv_obj_set_style_text_align(tasteGoalStatusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *saveButton = lv_btn_create(card);
    lv_obj_set_size(saveButton, 150, 40);
    lv_obj_align(saveButton, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_radius(saveButton, 8, 0);
    lv_obj_set_style_bg_color(saveButton, themeColor(0), 0);
    lv_obj_add_event_cb(saveButton, tasteGoalSaveClicked, LV_EVENT_CLICKED, this);
    lv_obj_t *saveLabel = lv_label_create(saveButton);
    lv_label_set_text(saveLabel, "Save");
    lv_obj_set_style_text_color(saveLabel, themeColor(1), 0);
    lv_obj_center(saveLabel);

    updateTasteGoalSettingsModal();
}

void DefaultUI::closeTasteGoalSettings() {
    if (tasteGoalModal != nullptr) {
        lv_obj_del(tasteGoalModal);
    }
    tasteGoalModal = nullptr;
    tasteGoalModeDropdown = nullptr;
    tasteGoalStatusLabel = nullptr;
    for (auto &button : tasteGoalLevelButtons) {
        button = nullptr;
    }
}

void DefaultUI::setTasteGoalModeFromIndex(const uint16_t index) {
    tasteGoalCustom = index == 1;
    updateTasteGoalSettingsModal();
}

void DefaultUI::cycleTasteGoalLevel(lv_obj_t *button) {
    if (!tasteGoalCustom || button == nullptr) {
        return;
    }
    for (size_t index = 0; index < AutoTuning::TASTE_GOAL_ATTRIBUTE_COUNT; ++index) {
        if (tasteGoalLevelButtons[index] != button) {
            continue;
        }
        tasteGoalLevels[index] = static_cast<uint8_t>((tasteGoalLevels[index] + 1) % 4);
        updateTasteGoalSettingsModal();
        return;
    }
}

void DefaultUI::saveTasteGoalSettings() {
    if (controller == nullptr) {
        return;
    }
    JsonDocument goal;
    goal["schema_version"] = AutoTuning::TASTE_GOAL_SCHEMA_VERSION;
    goal["mode"] = tasteGoalCustom ? "custom" : "balanced";
    JsonObject targets = goal["targets"].to<JsonObject>();
    if (tasteGoalCustom) {
        for (size_t index = 0; index < AutoTuning::TASTE_GOAL_ATTRIBUTE_COUNT; ++index) {
            if (tasteGoalLevels[index] > 0) {
                targets[AutoTuning::tasteGoalAttributeKey(index)] = tasteGoalLevelValue(tasteGoalLevels[index]);
            }
        }
    }

    Settings &settings = controller->getSettings();
    String error;
    if (!AutoTuning::setTasteGoalForContext(settings, settings.getRLBeanContextId(), settings.getRLGrinderContextId(),
                                            goal.as<JsonVariantConst>(), error)) {
        if (tasteGoalStatusLabel != nullptr) {
            lv_label_set_text(tasteGoalStatusLabel, error.c_str());
        }
        return;
    }
    if (pluginManager != nullptr) {
        pluginManager->trigger("settings:changed");
        pluginManager->trigger("rl:settings:changed");
        pluginManager->trigger("rl:taste-goal:changed");
    }
    closeTasteGoalSettings();
    updateAutoTuningSettingsModal();
    markDirty();
}

void DefaultUI::updateTasteGoalSettingsModal() {
    if (tasteGoalModal == nullptr) {
        return;
    }
    if (tasteGoalModeDropdown != nullptr) {
        lv_dropdown_set_selected(tasteGoalModeDropdown, tasteGoalCustom ? 1 : 0);
    }
    size_t selectedCount = 0;
    for (size_t index = 0; index < AutoTuning::TASTE_GOAL_ATTRIBUTE_COUNT; ++index) {
        lv_obj_t *button = tasteGoalLevelButtons[index];
        if (button == nullptr) {
            continue;
        }
        selectedCount += tasteGoalLevels[index] > 0 ? 1 : 0;
        lv_obj_t *label = lv_obj_get_child(button, 0);
        if (label != nullptr) {
            lv_label_set_text(label, TASTE_GOAL_LEVEL_LABELS[tasteGoalLevels[index]]);
        }
        if (tasteGoalCustom) {
            lv_obj_clear_state(button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(button, LV_STATE_DISABLED);
        }
    }
    if (tasteGoalStatusLabel != nullptr) {
        lv_label_set_text(tasteGoalStatusLabel, tasteGoalCustom && selectedCount == 0 ? "Select at least one target" : "");
    }
}

void DefaultUI::updateAutoTuningSettingsModal() {
    if (autoTuningModal == nullptr) {
        return;
    }
    Settings &settings = controller->getSettings();
    AutoTuning::Router router(settings.getRLOptimizerConfiguration(), controller->getOptimizerTransport());
    const String mode = settings.getRLAutoTuningProviderMode();
    const bool providerEnabled = router.enabled();

    setAutoTuningSwitchState(autoTuningEnabledSwitch, providerEnabled, true);

    if (autoTuningProviderDropdown != nullptr) {
        lv_dropdown_set_selected(autoTuningProviderDropdown, autoTuningProviderIndex(mode));
        if (providerEnabled) {
            lv_obj_clear_state(autoTuningProviderDropdown, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(autoTuningProviderDropdown, LV_STATE_DISABLED);
        }
    }

    if (autoTuningDoseTarget && !lv_obj_has_state(autoTuningDoseTarget, LV_STATE_FOCUSED)) {
        const auto domain = settings.getRLRecipeDomain();
        const float dose = settings.getTargetGrindVolume();
        // A reported actual dose may exceed the search range. Keep it visible
        // without clamping the remembered recipe or widening optimizer limits.
        lv_spinbox_set_range(autoTuningDoseTarget, std::lround(std::min(domain.doseMinG, dose) * 10),
                             std::lround(std::max(domain.doseMaxG, dose) * 10));
        const int32_t value = std::lround(dose * 10);
        if (lv_spinbox_get_value(autoTuningDoseTarget) != value) lv_spinbox_set_value(autoTuningDoseTarget, value);
    }

    setAutoTuningSwitchState(autoTuningLocalSwitch, settings.isRLLocalOptimizationEnabled(), providerEnabled);
    setAutoTuningSwitchState(autoTuningCommunitySwitch, settings.isRLCommunityUploadEnabled(), true);
    if (autoTuningTasteGoalButtonLabel != nullptr) {
        const String summary = AutoTuning::activeTasteGoalSummary(settings);
        lv_label_set_text(autoTuningTasteGoalButtonLabel, summary.c_str());
    }
    if (autoTuningTasteGoalButton != nullptr) {
        const bool contextReady =
            providerEnabled && !settings.getRLBeanContextId().isEmpty() && !settings.getRLGrinderContextId().isEmpty();
        if (contextReady) {
            lv_obj_clear_state(autoTuningTasteGoalButton, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(autoTuningTasteGoalButton, LV_STATE_DISABLED);
        }
    }

    String status = String(autoTuningProviderLabel(mode)) + " - " + router.providerStatus();
    if (providerEnabled && settings.isRLLocalOptimizationEnabled() && settings.getRLBeanContextId().isEmpty()) {
        status += "\nSelect a bean context to optimize";
    }
    lv_label_set_text(autoTuningStatusLabel, status.c_str());

    const String restoreStatus = settings.getRLRestoreBestStatus();
    const bool controlsReady = providerEnabled && mode == AutoTuning::PROVIDER_OFF_BOARD &&
        !controller->isActive() && restoreStatus != "pending";
    for (auto *button : {autoTuningStopButton, autoTuningResumeButton}) {
        if (controlsReady) lv_obj_clear_state(button, LV_STATE_DISABLED);
        else lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    String summary = router.providerSummary();
    if (restoreStatus == "pending") summary = "Request queued. Waiting for EspressoRL. Keep the machine idle.";
    else if (restoreStatus == "restored") {
        summary = "Optimization stopped. Best dose and yield restored.";
        JsonDocument grinders;
        if (!deserializeJson(grinders, settings.getRLGrinderContextsJson())) {
            for (JsonObjectConst grinder : grinders.as<JsonArrayConst>()) {
                if (grinder["id"].as<String>() != settings.getRLGrinderContextId()) continue;
                const bool absolute = grinder["current_absolute_step"].is<float>();
                const float position = absolute ? grinder["current_absolute_step"].as<float>() : grinder["current_relative_step"].as<float>();
                summary += String("\nMove grinder manually to ") + String(position, 1) + (absolute ? "" : " steps from reference");
                break;
            }
        }
        summary += String("\nDose: ") + String(settings.getTargetGrindVolume(), 1) + " g";
    }
    else if (restoreStatus == "failed") summary = "Best recipe not restored. Check connection, profile and recipe limits, then retry.";
    if (!autoTuningControlError.isEmpty()) summary = autoTuningControlError;
    lv_obj_update_layout(autoTuningStatusLabel);
    lv_obj_set_y(autoTuningSummaryLabel, lv_obj_get_y(autoTuningStatusLabel) + lv_obj_get_height(autoTuningStatusLabel) + 16);
    if (mode == AutoTuning::PROVIDER_OFF_BOARD && !settings.isHomeAssistant()) {
        summary += "\nEnable MQTT Broker for off-board mode";
    }
    if (settings.isRLCommunityUploadEnabled()) {
        summary += "\nUpload waits for device registration";
    }
    lv_label_set_text(autoTuningSummaryLabel, summary.c_str());
}

void DefaultUI::setupPanel() {
    ui_init();
    gaugeSetpointsInside = controller->areGaugeSetpointsInside();
    applyGaugeSetpointStyle(gaugeSetpointsInside);
    setupState();
    applyTheme();
    // First run boots straight into the welcome screen; it stays until the setup wizard finishes or is skipped
    welcomeActive = !controller->getSettings().isSetupDone();
    if (welcomeActive) {
        eez_flow_set_screen(SCREEN_ID_WELCOME_SCREEN, LV_SCR_LOAD_ANIM_NONE, 0, 0);
        currentScreen = targetScreen = SCREEN_ID_WELCOME_SCREEN;
    }
    ui_tick();

    lv_obj_t *startScreen = lv_scr_act();
    lv_obj_t *black = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(black, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(black, LV_OPA_COVER, LV_PART_MAIN);
    lv_scr_load(black);
    lv_scr_load_anim(startScreen, LV_SCR_LOAD_ANIM_FADE_ON, STARTUP_FADE_MS, 0, true);

    lv_task_handler();

    delay(100);
    // Set initial brightness based on settings
    const ::Settings &settings = controller->getSettings();
    setBrightness(settings.getMainBrightness());
}

void DefaultUI::setupState() {
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_SET_DOSE_STATE, IntegerValue(0));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_SET_DOSE_AVAILABLE, BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_SCALE_WEIGHT_CURRENT, currentWeight);
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_GRIND_WEIGHT_TARGET, grindWeightTarget);
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_GRIND_TIME_TARGET, grindTimeTarget);

    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_SYSTEM, systemStatus);
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_WARNINGS, warnings);
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_PREVIEW_PROFILE, previewProfileInfo);
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_SELECTED_PROFILE, selectedProfileInfo);
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_BOILER, boiler);
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_UI_FLAGS, uiFlags);
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_BREW_PROCESS_INFO, brewProcess);

    updateState();
    updateSystemStatus();
    updateProfileInfo();
    updateBoiler();
    updateBrewProcess();

    effect_mgr.use_effect([this]() { return currentScreen == SCREEN_ID_INFO_SCREEN; },
                          [this]() { updateAccessQrCode(objects.qrcode); }, &wifiConnected, &apActive);
    effect_mgr.use_effect([this]() { return currentScreen == SCREEN_ID_WELCOME_SCREEN; },
                          [this]() { updateAccessQrCode(objects.qrcode_welcome); }, &wifiConnected, &apActive);
    effect_mgr.use_effect([this]() { return currentScreen == SCREEN_ID_MENU_SCREEN_NEW; },
                          [this]() {
                              int radius = 135;
                              int count = 4;
                              if (!grindAvailable)
                                  count--;
                              if (dualBoiler)
                                  count--;
                              int step = 360 / count;
                              int rotationOffset = count == 4 ? 45 : count == 2 ? 90 : 0;
                              positionMenuIcon(objects.btn_brew_1, step * 0 - rotationOffset, radius);
                              positionMenuIcon(objects.btn_steam_1, step * 1 - rotationOffset, radius);
                              positionMenuIcon(objects.btn_water_1, step * 2 - rotationOffset, radius);
                              positionMenuIcon(objects.btn_grind_1, step * 3 - rotationOffset, radius);
                              // positionMenuIcon(objects.btn_settings_1, step * (3 + iconOffset) - rotationOffset, radius);
                          },
                          &grindAvailable);
}

// AP mode: join-network QR (the captive portal opens the web UI); on Wi-Fi: the device URL.
void DefaultUI::updateAccessQrCode(lv_obj_t *qrcode) {
                              String content = "";
                              if (apActive) {
                                  // WIFI: QR syntax — escape \ ; , : " in the password per the spec.
                                  const String pw = controller->getSettings().getWifiApPassword();
                                  String escaped;
                                  escaped.reserve(pw.length() + 4);
                                  for (size_t i = 0; i < pw.length(); i++) {
                                      const char c = pw.charAt(i);
                                      if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') {
                                          escaped += '\\';
                                      }
                                      escaped += c;
                                  }
                                  if (escaped.isEmpty()) {
                                      content = "WIFI:S:GaggiMate;;;;";
                                  } else {
                                      content = "WIFI:S:GaggiMate;T:WPA;P:" + escaped + ";;";
                                  }
                              } else if (wifiConnected) {
                                  content = "http://" + WiFi.localIP().toString() + "/";
                              }
    if (content == "" || qrcode == nullptr) {
                                  return;
                              }
    lv_qrcode_update(qrcode, content.c_str(), content.length());
}

void DefaultUI::handleScreenChange() {
    // Wait for an animated screen load (the startup fade) to finish. Interrupting it makes LVGL 8
    // finish the load early and, with auto-delete set, delete the screen being loaded instead of
    // the previous one, then load the deleted screen (crash on a mode change right after boot).
    if (lv_disp_get_default()->scr_to_load != nullptr)
        return;
    if (currentScreen != targetScreen) {
        if (currentScreen == SCREEN_ID_BREW_SCREEN)
            brewConfirmVisible = false; // leaving the brew screen dismisses the confirm overlay
        if (targetScreen == SCREEN_ID_STANDBY_SCREEN) {
            standbyEnterTime = ::millis();
        } else if (currentScreen == SCREEN_ID_STANDBY_SCREEN) {
            const ::Settings &settings = controller->getSettings();
            setBrightness(settings.getMainBrightness());
        }
        eez_flow_set_screen(targetScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0);
        // Dial widgets are created lazily by EEZ, so apply the current choice after
        // the new screen's pair of indicators exists.
        applyGaugeSetpointStyle(gaugeSetpointsInside);
        animateGaugeTicks(isShortTickScreen(currentScreen, tickChartMode), isShortTickScreen(targetScreen, tickChartMode));
        rerender = true;
    }
}

// Collect every lv_meter under obj (the dial gauges) so their tick length can be animated together.
void DefaultUI::collectMeters(lv_obj_t *obj) {
    const uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (gaugeCount < 4 && lv_obj_check_type(child, &lv_meter_class)) {
            gaugeMeters[gaugeCount++] = child;
        }
        collectMeters(child);
    }
}

void DefaultUI::setGaugeTickLength(int32_t len) {
    const int16_t insidePivotX = GAUGE_SETPOINT_INSIDE_PIVOT_X - (GAUGE_TICK_LONG - len);
    for (uint8_t i = 0; i < gaugeCount; i++) {
        auto *meter = reinterpret_cast<lv_meter_t *>(gaugeMeters[i]);
        auto *scale = static_cast<lv_meter_scale_t *>(_lv_ll_get_head(&meter->scale_ll));
        if (scale != nullptr) {
            scale->tick_length = static_cast<uint16_t>(len);
        }
        for (auto *indicator = static_cast<lv_meter_indicator_t *>(_lv_ll_get_head(&meter->indicator_ll)); indicator != nullptr;
             indicator = static_cast<lv_meter_indicator_t *>(_lv_ll_get_next(&meter->indicator_ll, indicator))) {
            if (indicator->type == LV_METER_INDICATOR_TYPE_NEEDLE_IMG &&
                indicator->type_data.needle_img.src == &img_indicator_small_inside) {
                indicator->type_data.needle_img.pivot.x = insidePivotX;
            }
        }
        lv_obj_invalidate(gaugeMeters[i]);
    }
}

void DefaultUI::applyGaugeSetpointStyle(bool inside) { applyGaugeSetpointVisibility(lv_scr_act(), inside); }

void DefaultUI::gaugeTickAnimCb(void *var, int32_t v) { static_cast<DefaultUI *>(var)->setGaugeTickLength(v); }

void DefaultUI::animateGaugeTicks(bool fromShort, bool toShort) {
    const int32_t fromLen = fromShort ? GAUGE_TICK_SHORT : GAUGE_TICK_LONG;
    const int32_t toLen = toShort ? GAUGE_TICK_SHORT : GAUGE_TICK_LONG;

    lv_anim_del(this, gaugeTickAnimCb); // cancel any in-flight tick animation
    gaugeCount = 0;
    collectMeters(lv_scr_act());
    if (gaugeCount == 0) {
        return;
    }
    // Start at the previous screen's length so the ticks morph continuously in both directions.
    setGaugeTickLength(fromLen);
    if (fromLen == toLen) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, gaugeTickAnimCb);
    lv_anim_set_values(&a, fromLen, toLen);
    lv_anim_set_time(&a, GAUGE_TICK_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void DefaultUI::positionMenuIcon(lv_obj_t *obj, int angle, int radius) {
    int x = sin(angle * M_PI / 180) * radius;
    int y = -1 * cos(angle * M_PI / 180) * radius;
    lv_obj_set_pos(obj, x, y);
}

void DefaultUI::updateState() {
    const auto &settings = controller->getSettings();
    mode = controller->getMode();
    currentTemp = static_cast<int>(controller->getCurrentTemp());
    targetTemp = static_cast<int>(controller->getTargetTemp());
    pressureAvailable = controller->getSystemInfo().capabilities.pressure ? 1 : 0;
    wifiConnected = WiFi.status() == WL_CONNECTED;
    grindAvailable = settings.isSmartGrindActive() ||
                     (!controller->getSystemInfo().capabilities.dualBoiler && settings.getAltRelayFunction() == ALT_RELAY_GRIND);
    dualBoiler = controller->getSystemInfo().capabilities.dualBoiler ? 1 : 0;

    uiFlags.brew_adjustments(brewScreenState == BrewScreenState::Settings);
    uiFlags.active(dualBoiler && currentScreen == SCREEN_ID_STEAM_SCREEN   ? controller->isSteamHeaterActive()
                   : dualBoiler && currentScreen == SCREEN_ID_WATER_SCREEN ? controller->isWaterValveActive()
                                                                           : controller->isActive());
    uiFlags.grind_active(controller->isGrindActive());
    uiFlags.grind_volumetric(controller->isGrindVolumetricAvailable() && settings.isVolumetricTarget());
    uiFlags.heating_flash(heatingFlash);
    uiFlags.temperature_stable(controller->getWarnings().isTemperatureStable());
    uiFlags.has_prev_profile(currentProfileIdx > 0);
    uiFlags.brew_confirm_visible(brewConfirmVisible);
    uiFlags.profile_details_visible(profileDetailsVisible);
    const bool chartMode = settings.getStatusDisplayMode() == STATUS_DISPLAY_CHART && pressureAvailable && isChartShot();
    uiFlags.chart_mode(chartMode);
    uiFlags.steam_active(dualBoiler && controller->isSteamHeaterActive());
    if (chartMode != tickChartMode) {
        // Re-animate the ring when the mode flips while the status screen is open.
        if (currentScreen == SCREEN_ID_STATUS_SCREEN && currentScreen == targetScreen)
            animateGaugeTicks(!chartMode, chartMode);
        tickChartMode = chartMode;
    }
    {
        std::lock_guard<std::mutex> guard(profilesMutex);
        uiFlags.has_next_profile(currentProfileIdx + 1 < static_cast<int>(favoritedProfileIds.size()));
    }
}

// Pro hardware shows the chart for every brew except utility profiles; prefer the running brew's profile.
bool DefaultUI::isChartShot() {
    if (!initialized)
        return false;
    std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
    Process *process = controller->getProcess();
    if (process == nullptr)
        process = controller->getLastProcess();
    if (process != nullptr && process->getType() == MODE_BREW)
        return !static_cast<BrewProcess *>(process)->profile.utility;
    return !profileManager->getSelectedProfile().utility;
}

void DefaultUI::updateSystemStatus() {
    const auto &settings = controller->getSettings();
    systemStatus.bluetooth(controller->getClientController()->isConnected());
    systemStatus.wifi(!apActive && WiFi.status() == WL_CONNECTED);
    const bool error = controller->getSystemState() != SYSTEM_READY;
    systemStatus.error(error);
    const String errorLabel = error ? getErrorMessage() : "";
    if (stringChanged(systemStatus.error_label(), errorLabel.c_str()))
        systemStatus.error_label(errorLabel.c_str());
    systemStatus.volumetric_available(controller->isVolumetricAvailable());
    const auto dosing = SmartDose.getUIState();
    systemStatus.bluetooth_scales((currentScreen == SCREEN_ID_BREW_SCREEN && dosing.state != 0) ||
                                  controller->isScaleSourceHealthy(controller->getEffectiveScaleSource()));
    systemStatus.controller_version(controller->getSystemInfo().version.c_str());
        systemStatus.display_version(BUILD_GIT_VERSION);
    systemStatus.update_available(updateAvailable);
    systemStatus.in_menu(currentScreen == SCREEN_ID_MENU_SCREEN_NEW);
    systemStatus.pressure_available(pressureAvailable);
    systemStatus.grind_available(grindAvailable);
    systemStatus.mode(mode);
    systemStatus.dual_boiler(dualBoiler);
    const String ip = apActive ? String("4.4.4.1") : WiFi.localIP().toString();
    if (stringChanged(systemStatus.ip(), ip.c_str()))
        systemStatus.ip(ip.c_str());
    const String network = apActive ? String("GaggiMate") : systemStatus.wifi() ? settings.getWifiSsid() : String("Disconnected");
    if (stringChanged(systemStatus.network(), network.c_str()))
        systemStatus.network(network.c_str());
    systemStatus.ap_active(apActive);

    char timeBuf[12] = "";
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5)) {
        strftime(timeBuf, sizeof(timeBuf), settings.isClock24hFormat() ? "%H:%M" : "%I:%M %p", &timeinfo);
        if (!settings.isClock24hFormat() && timeBuf[0] == '0')
            timeBuf[0] = ' ';
    }
    if (stringChanged(systemStatus.time(), timeBuf))
        systemStatus.time(timeBuf);
}

void DefaultUI::updateWarnings() {
    const WarningManager &wm = controller->getWarnings();
    warnings.waterWarn(wm.isWarn(WARNING_WATER));
    warnings.waterError(wm.isError(WARNING_WATER));
    warnings.flushWarn(wm.isWarn(WARNING_FLUSH));
    warnings.flushError(wm.isError(WARNING_FLUSH));
    warnings.switchWarn(wm.isWarn(WARNING_SWITCH));
    warnings.switchError(wm.isError(WARNING_SWITCH));
    warnings.scaleConnectedWarn(wm.isWarn(WARNING_SCALE_CONNECTED));
    warnings.scaleConnectedError(wm.isError(WARNING_SCALE_CONNECTED));
    warnings.scaleBatteryWarn(wm.isWarn(WARNING_SCALE_BATTERY));
    warnings.scaleBatteryError(wm.isError(WARNING_SCALE_BATTERY));
    warnings.temperatureWarn(wm.isWarn(WARNING_TEMPERATURE));
    warnings.temperatureError(wm.isError(WARNING_TEMPERATURE));
    warnings.cleanWarn(wm.isWarn(WARNING_BACKFLUSH) || wm.isWarn(WARNING_DESCALING));
    warnings.cleanError(wm.isError(WARNING_BACKFLUSH) || wm.isError(WARNING_DESCALING));
    const String labels = wm.getLabels();
    if (stringChanged(warnings.labels(), labels.c_str()))
        warnings.labels(labels.c_str());
}

static void populateProfileInfo(ProfileInfoValue &info, const Profile &profile, bool isCurrent, bool hasImage, bool imageLoaded) {
    char timeBuf[12];
    formatDuration(static_cast<unsigned long>(profile.getTotalDuration() * 1000.0f), timeBuf, sizeof(timeBuf));
    if (stringChanged(info.name(), profile.label.c_str()))
        info.name(profile.label.c_str());
    info.temperature(profile.temperature);
    if (stringChanged(info.time(), timeBuf))
        info.time(timeBuf);
    info.phases(static_cast<int>(profile.getPhaseCount()));
    info.steps(static_cast<int>(profile.phases.size()));
    info.is_volumetric(profile.isVolumetric());
    info.is_current(isCurrent);
    info.target_weight(profile.getTotalVolume());
    info.is_pro(profile.type == "pro");
    info.has_image(hasImage);
    info.image_loaded(imageLoaded);
}

void DefaultUI::updateProfileInfo() {
    if (!initialized) {
        return;
    }
    const Profile &selected = profileManager->getSelectedProfile();
    const auto imageLoaded = [this](const String &id) { return imageShown != nullptr && imageShownId == id; };
    bool populated = false;
    bool selectedHasImage = false;
    {
        std::lock_guard<std::mutex> guard(profilesMutex);
        // The profile task always puts the selected profile first.
        selectedHasImage = !favoritedProfileIds.empty() && favoritedProfileIds[0] == selected.id && favoritedHasImage[0];
        if (!favoritedProfiles.empty() && currentProfileIdx >= 0 &&
            currentProfileIdx < static_cast<int>(favoritedProfiles.size())) {
            populateProfileInfo(previewProfileInfo, favoritedProfiles[currentProfileIdx],
                                favoritedProfileIds[currentProfileIdx] == selected.id, favoritedHasImage[currentProfileIdx],
                                imageLoaded(favoritedProfileIds[currentProfileIdx]));
            populated = true;
        }
    }
    populateProfileInfo(selectedProfileInfo, selected, true, selectedHasImage, imageLoaded(selected.id));
    selectedProfileInfo.dirty(profileDirty);
    if (!populated) {
        populateProfileInfo(previewProfileInfo, selected, true, selectedHasImage, imageLoaded(selected.id));
    }
}

void DefaultUI::updateBoiler() {
    const ::Settings &settings = controller->getSettings();
    bool showSteamTemp = dualBoiler && currentScreen == SCREEN_ID_STEAM_SCREEN;
    boiler.current_temperature(showSteamTemp ? controller->getCurrentSteamTemp() : controller->getCurrentTemp());
    boiler.target_temperature(showSteamTemp ? controller->getTargetSteamTemp() : controller->getTargetTemp());
    boiler.current_pressure(pressure);
    boiler.target_pressure(controller->getTargetPressure());
    boiler.max_temperature(160.0f);
    boiler.max_pressure(settings.getPressureScaling());
}

// Mirror the live BrewProcess into brew_process_info; every field must stay valid/typed or the StatusScreen flow aborts.
void DefaultUI::updateBrewProcess() {
    if (!initialized) {
        return;
    }

    const Profile &selected = profileManager->getSelectedProfile();
    char buf[12];

    // Profile-derived defaults so the struct is valid even before a process runs.
    formatDuration(static_cast<unsigned long>(selected.getTotalDuration() * 1000.0f), buf, sizeof(buf));
    brewProcess.profile_temperature(selected.temperature);
    if (stringChanged(brewProcess.profile_time(), buf))
        brewProcess.profile_time(buf);
    brewProcess.profile_phases(static_cast<int>(selected.getPhaseCount()));
    brewProcess.profile_steps(static_cast<int>(selected.phases.size()));
    brewProcess.profile_is_volumetric(selected.isVolumetric());
    brewProcess.profile_is_current(true);
    brewProcess.profile_target_weight(selected.getTotalVolume());
    brewProcess.boiler_target_temperature(controller->getTargetTemp());
    brewProcess.current_flow(controller->getCurrentPumpFlow());

    // Hold the process lock across every deref below — the logic/AsyncTCP/BLE tasks delete
    // the process at any time (GM-147).
    std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
    Process *process = controller->getProcess();
    if (process == nullptr) {
        process = controller->getLastProcess();
    }
    const bool validBrew = process != nullptr && process->getType() == MODE_BREW;
    if (!validBrew) {
        if (stringChanged(brewProcess.phase_type(), ""))
            brewProcess.phase_type("");
        if (stringChanged(brewProcess.phase_name(), ""))
            brewProcess.phase_name("");
        brewProcess.phase_value_current(0.0f);
        brewProcess.phase_value_target(0.0f);
        brewProcess.phase_value_is_weight(false);
        if (stringChanged(brewProcess.elapsed_time(), "0:00"))
            brewProcess.elapsed_time("0:00");
        brewProcess.elapsed_percentage(0.0f);
        brewProcess.is_complete(false);
        return;
    }

    auto *bp = static_cast<BrewProcess *>(process);
    if (bp->profile.phases.empty() || bp->phaseIndex >= bp->profile.phases.size()) {
        // Object is mid-mutation/invalid: keep the last valid values.
        return;
    }

    const Phase phase = bp->currentPhase;
    const bool active = process->isActive();

    // Live profile fields from the running process.
    formatDuration(bp->getTotalDuration(), buf, sizeof(buf));
    brewProcess.profile_temperature(bp->profile.temperature);
    if (stringChanged(brewProcess.profile_time(), buf))
        brewProcess.profile_time(buf);
    brewProcess.profile_phases(static_cast<int>(bp->profile.getPhaseCount()));
    brewProcess.profile_steps(static_cast<int>(bp->profile.phases.size()));
    brewProcess.profile_is_volumetric(bp->target == ProcessTarget::VOLUMETRIC);
    brewProcess.profile_target_weight(bp->getBrewVolume());
    brewProcess.boiler_target_temperature(bp->getTemperature());
    brewProcess.current_volume(bp->currentVolume);

    const char *phaseType = phase.phase == PhaseType::PHASE_TYPE_BREW ? "BREW" : "INFUSION";
    if (stringChanged(brewProcess.phase_type(), phaseType))
        brewProcess.phase_type(phaseType);

    String phaseName = "Finished";
    if (active) {
        phaseName = phase.name;
    } else if (controller->getSettings().isDelayAdjust() && !process->isComplete()) {
        phaseName = "Calibrating...";
    }
    if (stringChanged(brewProcess.phase_name(), phaseName.c_str()))
        brewProcess.phase_name(phaseName.c_str());

    unsigned long now = ::millis();
    if (!active && bp->finished > 0) {
        now = bp->finished;
    }
    const unsigned long elapsedMs = (bp->processStarted > 0 && now >= bp->processStarted) ? now - bp->processStarted : 0;
    formatDuration(elapsedMs, buf, sizeof(buf));
    if (stringChanged(brewProcess.elapsed_time(), buf))
        brewProcess.elapsed_time(buf);

    const bool weightTarget = bp->target == ProcessTarget::VOLUMETRIC && phase.hasVolumetricTarget();
    brewProcess.phase_value_is_weight(weightTarget);
    if (weightTarget) {
        float target = 0.0f;
        Target volumetricTarget = phase.getVolumetricTarget();
        if (volumetricTarget.type == TargetType::TARGET_TYPE_VOLUMETRIC) {
            target = volumetricTarget.value;
        } else if (volumetricTarget.type == TargetType::TARGET_TYPE_RATIO && bp->profile.dose > 0.0f) {
            target = volumetricTarget.value * bp->profile.dose;
        }
        const float current = static_cast<float>(bp->currentVolume);
        brewProcess.phase_value_current(current);
        brewProcess.phase_value_target(target);
        brewProcess.elapsed_percentage(target > 0.0f ? clampPercentage(current / target * 100.0f) : 0.0f);
    } else {
        const unsigned long phaseElapsed =
            (bp->currentPhaseStarted > 0 && now >= bp->currentPhaseStarted) ? now - bp->currentPhaseStarted : 0;
        const float current = phaseElapsed / 1000.0f;
        const float target = bp->getPhaseDuration() / 1000.0f;
        brewProcess.phase_value_current(current);
        brewProcess.phase_value_target(target);
        brewProcess.elapsed_percentage(target > 0.0f ? clampPercentage(current / target * 100.0f) : 0.0f);
    }

    brewProcess.is_complete(process->isComplete());
    brewProcess.shot_max_pressure(shotMaxPressure);
    brewProcess.shot_avg_flow(shotFlowSamples > 0 ? shotFlowSum / shotFlowSamples : 0.0f);
}

void DefaultUI::updateMenuScreen() {
    // menu screen has no dynamic content to refresh
}

void DefaultUI::setupShotChart() {
    shotChart = objects.shot_chart;
    styleTrendChart(shotChart, SHOT_CHART_POINTS);
    for (int i = 0; i < SHOT_SERIES_COUNT; i++) {
        shotSeries[i] = lv_chart_add_series(shotChart, lv_color_hex(0), LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_ext_y_array(shotChart, shotSeries[i], shotPoints[i]);
    }
    applyShotChartTheme();
    lv_obj_add_event_cb(shotChart, shotChartDrawCb, LV_EVENT_DRAW_PART_BEGIN, this);
    lv_obj_add_event_cb(shotChart, shotChartDrawCb, LV_EVENT_DRAW_PART_END, this);
    lv_obj_add_event_cb(shotChart, shotChartBackgroundCb, LV_EVENT_DRAW_MAIN_BEGIN, this);
    if (shotHistory == nullptr) {
        shotHistory = static_cast<ShotSample *>(ps_malloc(SHOT_HISTORY_POINTS * sizeof(ShotSample)));
        resetShotChart(controller->getTargetTemp(), 0.0f);
    } else {
        renderShotChart(); // screen was recreated; redraw from the kept history
    }
}

void DefaultUI::applyShotChartTheme() {
    if (shotChart == nullptr)
        return;
    const int theme = currentThemeMode >= 0 ? currentThemeMode : 0;
    for (int i = 0; i < SHOT_SERIES_COUNT; i++)
        lv_chart_set_series_color(shotChart, shotSeries[i], lv_color_hex(theme_colors[theme][SHOT_THEME_COLORS[i]]));
    lv_chart_refresh(shotChart);
}

void DefaultUI::resetShotChart(float targetTemperature, float targetWeight) {
    shotHistoryStart = 0;
    shotHistoryCount = 0;
    shotPhasePending = false;
    lastShotSample = 0;
    shotFlowRange = SHOT_FLOW_RANGE_DEFAULT;
    shotWeightRange = targetWeight > 0.0f ? growShotRange(10.0f, targetWeight * 1.1f, 10.0f) : SHOT_WEIGHT_RANGE_DEFAULT;
    shotTempMin = targetTemperature - SHOT_TEMP_RANGE;
    shotTempMax = targetTemperature + SHOT_TEMP_RANGE;
    renderShotChart();
}

void DefaultUI::updateShotChart() {
    if (objects.shot_chart == nullptr)
        return;
    if (objects.shot_chart != shotChart)
        setupShotChart();

    std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
    Process *process = controller->getProcess();
    if (process == nullptr || process->getType() != MODE_BREW || !process->isActive()) {
        if (shotChartLive) {
            shotChartLive = false;
            zoomOutShotChart();
        }
        return;
    }
    auto *bp = static_cast<BrewProcess *>(process);
    const bool volumetric = controller->isVolumetricAvailable();
    if (bp->processStarted != shotChartStarted) {
        shotChartStarted = bp->processStarted;
        shotChartLive = true;
        lv_anim_del(this, shotChartZoomAnimCb);
        shotChartZoom = 0.0f;
        shotMaxPressure = 0.0f;
        shotFlowSum = 0.0f;
        shotFlowSamples = 0;
        resetShotChart(bp->getTemperature(), volumetric ? static_cast<float>(bp->getBrewVolume()) : 0.0f);
        shotPhaseIndex = bp->phaseIndex;
    }
    if (bp->phaseIndex != shotPhaseIndex) {
        shotPhaseIndex = bp->phaseIndex;
        shotPhasePending = true;
    }
    const unsigned long now = ::millis();
    if (lastShotSample != 0 && now - lastShotSample < SHOT_CHART_SAMPLE_INTERVAL)
        return;
    // Advance by the interval, not to now, so the coarse UI tick doesn't stretch the 30 s window.
    const bool behind = lastShotSample == 0 || now - lastShotSample >= 2 * SHOT_CHART_SAMPLE_INTERVAL;
    lastShotSample = behind ? now : lastShotSample + SHOT_CHART_SAMPLE_INTERVAL;
    addShotChartSample(volumetric ? static_cast<float>(bp->currentVolume) : -1.0f);
}

void DefaultUI::addShotChartSample(float weight) {
    if (shotHistory == nullptr)
        return;
    const float pressure = controller->getCurrentPressure();
    const float flow = controller->getCurrentPumpFlow();
    const float targetPressure = controller->getTargetPressure();
    const float targetFlow = controller->getTargetFlow();
    shotFlowRange = growShotRange(shotFlowRange, LV_MAX(LV_MAX(pressure, flow), LV_MAX(targetPressure, targetFlow)), 2.0f);
    shotWeightRange = growShotRange(shotWeightRange, weight, 10.0f);
    shotMaxPressure = LV_MAX(shotMaxPressure, pressure);
    shotFlowSum += flow;
    shotFlowSamples++;

    // Oldest sample is overwritten once the 2 min history is full.
    if (shotHistoryCount == SHOT_HISTORY_POINTS) {
        shotHistoryStart = (shotHistoryStart + 1) % SHOT_HISTORY_POINTS;
        shotHistoryCount--;
    }
    ShotSample &sample = shotHistory[(shotHistoryStart + shotHistoryCount) % SHOT_HISTORY_POINTS];
    sample.values[SHOT_PRESSURE] = pressure;
    sample.values[SHOT_TARGET_PRESSURE] = targetPressure > 0.0f ? targetPressure : NAN;
    sample.values[SHOT_FLOW] = flow;
    sample.values[SHOT_TARGET_FLOW] = targetFlow > 0.0f ? targetFlow : NAN;
    sample.values[SHOT_TEMPERATURE] = controller->getCurrentTemp();
    sample.values[SHOT_WEIGHT] = weight >= 0.0f ? weight : NAN;
    sample.phaseStart = shotPhasePending;
    shotPhasePending = false;
    shotHistoryCount++;
    renderShotChart();
}

// Zoom from the live window out to the whole shot; the window's left edge eases back to the shot start.
void DefaultUI::zoomOutShotChart() {
    lv_anim_del(this, shotChartZoomAnimCb);
    if (shotHistoryCount <= SHOT_CHART_POINTS) {
        shotChartZoom = 1.0f; // the whole shot is already visible
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, shotChartZoomAnimCb);
    lv_anim_set_values(&a, 0, SHOT_ZOOM_ANIM_STEPS);
    lv_anim_set_time(&a, SHOT_ZOOM_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

void DefaultUI::shotChartZoomAnimCb(void *var, int32_t value) {
    auto *ui = static_cast<DefaultUI *>(var);
    ui->shotChartZoom = static_cast<float>(value) / SHOT_ZOOM_ANIM_STEPS;
    if (ui->shotChart != nullptr && ui->shotChart == objects.shot_chart)
        ui->renderShotChart();
}

// Samples the history window ending at the latest sample; zoom 0 shows the last 30 s, zoom 1 the whole shot.
void DefaultUI::renderShotChart() {
    const int total = shotHistory != nullptr ? shotHistoryCount : 0;
    const int count = LV_MIN(total, SHOT_CHART_POINTS);
    const float windowStart = (total - count) * (1.0f - shotChartZoom);
    const float step = count > 1 ? (total - 1 - windowStart) / (count - 1) : 0.0f;
    int previous = -1;
    for (int j = 0; j < SHOT_CHART_POINTS; j++) {
        shotPhaseMarks[j] = false;
        if (j >= count) {
            for (auto &series : shotPoints)
                series[j] = LV_CHART_POINT_NONE;
            continue;
        }
        const int pick = j == count - 1 ? total - 1 : lroundf(windowStart + j * step);
        // A point carries a phase mark if any sample it skips over started a phase.
        for (int k = previous < 0 ? pick : previous + 1; k <= pick; k++)
            shotPhaseMarks[j] |= shotHistory[(shotHistoryStart + k) % SHOT_HISTORY_POINTS].phaseStart;
        previous = pick;
        const ShotSample &sample = shotHistory[(shotHistoryStart + pick) % SHOT_HISTORY_POINTS];
        for (int i = 0; i < SHOT_SERIES_COUNT; i++) {
            const float value = sample.values[i];
            if (std::isnan(value))
                shotPoints[i][j] = LV_CHART_POINT_NONE;
            else if (i == SHOT_TEMPERATURE)
                shotPoints[i][j] = scaleShotValue(value, shotTempMin, shotTempMax);
            else
                shotPoints[i][j] = scaleShotValue(value, 0.0f, i == SHOT_WEIGHT ? shotWeightRange : shotFlowRange);
        }
    }
    shotPointCount = count;
    lv_chart_refresh(shotChart);
}

void DefaultUI::shotChartBackgroundCb(lv_event_t *e) {
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(e));
    const int theme = ui->currentThemeMode >= 0 ? ui->currentThemeMode : 0;
    drawTrendGuides(e, theme, ui->shotPhaseMarks, ui->shotPointCount, SHOT_CHART_POINTS);
}

void DefaultUI::shotChartDrawCb(lv_event_t *e) {
    auto *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc == nullptr || dsc->part != LV_PART_ITEMS || dsc->line_dsc == nullptr || dsc->sub_part_ptr == nullptr)
        return;
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(e));
    int index = 0;
    while (index < SHOT_SERIES_COUNT && ui->shotSeries[index] != dsc->sub_part_ptr)
        index++;
    if (index == SHOT_SERIES_COUNT)
        return;

    if (lv_event_get_code(e) == LV_EVENT_DRAW_PART_BEGIN) {
        styleTrendLine(dsc->line_dsc, SHOT_IS_TARGET[index]);
        return;
    }

    if (SHOT_IS_TARGET[index] || dsc->p2 == nullptr || dsc->id != ui->shotPointCount - 2)
        return;
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    dot.bg_color = dsc->line_dsc->color;
    dot.bg_opa = LV_OPA_COVER;
    const lv_area_t area = {
        static_cast<lv_coord_t>(dsc->p2->x - SHOT_DOT_RADIUS), static_cast<lv_coord_t>(dsc->p2->y - SHOT_DOT_RADIUS),
        static_cast<lv_coord_t>(dsc->p2->x + SHOT_DOT_RADIUS), static_cast<lv_coord_t>(dsc->p2->y + SHOT_DOT_RADIUS)};
    lv_draw_rect(dsc->draw_ctx, &dot, &area);
}

// Show the preview profile's image once the profile task has loaded it, and redraw the pro chart when the previewed profile
// changes.
void DefaultUI::updateProfilePreview() {
    if (currentScreen != SCREEN_ID_NEW_PROFILE_SCREEN || objects.profile_preview == nullptr)
        return;
    String id;
    bool hasImage = false;
    bool isPro = false;
    Profile profile;
    {
        std::lock_guard<std::mutex> guard(profilesMutex);
        if (currentProfileIdx < 0 || currentProfileIdx >= static_cast<int>(favoritedProfiles.size()))
            return;
        id = favoritedProfileIds[currentProfileIdx];
        hasImage = favoritedHasImage[currentProfileIdx];
        isPro = favoritedProfiles[currentProfileIdx].type == "pro";
        if (isPro && (id != profileChartId || profilesGeneration != profileChartGeneration)) {
            profile = favoritedProfiles[currentProfileIdx];
            profileChartId = id;
            profileChartGeneration = profilesGeneration;
        }
    }

    uint8_t *pixels = nullptr;
    {
        std::lock_guard<std::mutex> guard(imageMutex);
        const String wanted = hasImage ? id : String();
        if (wanted != imageWantedId) {
            imageWantedId = wanted;
            imageRequest++;
        }
        if (imagePending != nullptr && imagePendingId == imageWantedId) {
            pixels = imagePending;
            imagePending = nullptr;
        }
    }
    if (pixels != nullptr) {
        // Alternate descriptors so LVGL sees a new source; the old pixels are freed once replaced.
        imageDscIndex ^= 1;
        lv_img_dsc_t &dsc = imageDsc[imageDscIndex];
        dsc.header.always_zero = 0;
        dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        dsc.header.w = profile_image::SIZE;
        dsc.header.h = profile_image::SIZE;
        dsc.data_size = profile_image::FILE_BYTES - profile_image::HEADER_BYTES;
        dsc.data = pixels;
        lv_img_set_src(objects.profile_preview, &dsc);
        free(imageShown);
        imageShown = pixels;
        imageShownId = id;
    } else if (imageShown != nullptr && imageShownId != id) {
        lv_img_set_src(objects.profile_preview, nullptr); // don't flash the previous profile's image while loading
        free(imageShown);
        imageShown = nullptr;
        imageShownId = "";
    }
    // EEZ hides the image widget until its pixels are in place; a sourceless image would draw LVGL's "No data".
    previewProfileInfo.image_loaded(imageShown != nullptr && imageShownId == id);

    lv_obj_t *chart = objects.profile_chart;
    if (!isPro || chart == nullptr)
        return;
    if (chart != profileChart) {
        setupProfileChart(chart);
        profileChartGeneration = -1; // re-render into the new chart
    }
    if (!profile.id.isEmpty())
        renderProfileChart(profile);
}

void DefaultUI::setupProfileChart(lv_obj_t *chart) {
    profileChart = chart;
    styleTrendChart(chart, PROFILE_CHART_POINTS);
    const int theme = currentThemeMode >= 0 ? currentThemeMode : 0;
    for (int i = 0; i < 2; i++) {
        profileSeries[i] = lv_chart_add_series(chart, lv_color_hex(PROFILE_COLORS[theme][i]), LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_ext_y_array(chart, profileSeries[i], profilePoints[i]);
    }
    lv_obj_add_event_cb(chart, profileChartDrawCb, LV_EVENT_DRAW_PART_BEGIN, this);
    lv_obj_add_event_cb(chart, profileChartBackgroundCb, LV_EVENT_DRAW_MAIN_BEGIN, this);
}

// Same simulation as the web UI's ExtendedProfileChart: the pump target ramps per transition, the other value is a limit.
void DefaultUI::renderProfileChart(const Profile &profile) {
    const float total = profile.getTotalDuration();
    float pressure[PROFILE_CHART_POINTS] = {};
    float flow[PROFILE_CHART_POINTS] = {};
    bool pressureTarget[PROFILE_CHART_POINTS] = {};
    bool phaseMarks[PROFILE_CHART_POINTS] = {};
    bool phasePending = false;
    int out = 0;
    float maxValue = PROFILE_CHART_MIN_RANGE;

    if (!profile.phases.empty() && total > 0.0f) {
        size_t phaseIndex = 0;
        float time = 0.0f;
        float phaseTime = 0.0f;
        float currentPressure = 0.0f;
        float currentFlow = 0.0f;
        float startPressure = 0.0f;
        float startFlow = 0.0f;
        float effectivePressure = profile.phases[0].pumpAdvanced.pressure;
        float effectiveFlow = profile.phases[0].pumpAdvanced.flow;
        while (phaseIndex < profile.phases.size() && out < PROFILE_CHART_POINTS) {
            const Phase &phase = profile.phases[phaseIndex];
            const bool simple = phase.pumpIsSimple;
            const bool isPressure = !simple && phase.pumpAdvanced.target == PumpTarget::PUMP_TARGET_PRESSURE;
            const float transition = phase.transition.duration > 0.0f ? phase.transition.duration : phase.duration;
            const float alpha = transition > 0.0f ? applyEasing(phaseTime / transition, phase.transition.type) : 1.0f;
            const float limitPressure = simple ? 0.0f : phase.pumpAdvanced.pressure;
            const float limitFlow = simple ? 0.0f : phase.pumpAdvanced.flow;
            currentFlow = !simple && !isPressure ? startFlow + (effectiveFlow - startFlow) * alpha : limitFlow;
            currentPressure = isPressure ? startPressure + (effectivePressure - startPressure) * alpha : limitPressure;
            while (out < PROFILE_CHART_POINTS && time >= total * out / (PROFILE_CHART_POINTS - 1)) {
                pressure[out] = LV_MAX(currentPressure, 0.0f);
                flow[out] = LV_MAX(currentFlow, 0.0f);
                pressureTarget[out] = isPressure;
                phaseMarks[out] = phasePending;
                phasePending = false;
                maxValue = LV_MAX(maxValue, LV_MAX(pressure[out], flow[out]));
                out++;
            }
            time += PROFILE_CHART_STEP;
            phaseTime += PROFILE_CHART_STEP;
            if (phaseTime >= phase.duration) {
                phaseTime = 0.0f;
                if (++phaseIndex < profile.phases.size()) {
                    phasePending = true;
                    startFlow = currentFlow;
                    startPressure = currentPressure;
                    const PumpAdvanced &next = profile.phases[phaseIndex].pumpAdvanced;
                    effectiveFlow = next.flow == -1 ? currentFlow : next.flow;
                    effectivePressure = next.pressure == -1 ? currentPressure : next.pressure;
                }
            }
        }
    }

    const float range = growShotRange(PROFILE_CHART_MIN_RANGE, maxValue * 1.1f, 2.0f);
    for (int i = 0; i < PROFILE_CHART_POINTS; i++) {
        const bool valid = i < out;
        const bool lastValid = out > 0 && i >= out;
        const int src = lastValid ? out - 1 : i;
        profilePoints[0][i] = valid || lastValid ? scaleShotValue(pressure[src], 0.0f, range) : LV_CHART_POINT_NONE;
        profilePoints[1][i] = valid || lastValid ? scaleShotValue(flow[src], 0.0f, range) : LV_CHART_POINT_NONE;
        profilePointIsTarget[0][i] = pressureTarget[src];
        profilePointIsTarget[1][i] = !pressureTarget[src];
        profilePhaseMarks[i] = valid && phaseMarks[i];
    }
    lv_chart_refresh(profileChart);
}

// The controlled value draws solid, the limit dashed (as in the web UI).
void DefaultUI::profileChartDrawCb(lv_event_t *e) {
    auto *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc == nullptr || dsc->part != LV_PART_ITEMS || dsc->line_dsc == nullptr || dsc->sub_part_ptr == nullptr)
        return;
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(e));
    const int series = ui->profileSeries[0] == dsc->sub_part_ptr ? 0 : ui->profileSeries[1] == dsc->sub_part_ptr ? 1 : -1;
    if (series < 0 || dsc->id >= PROFILE_CHART_POINTS)
        return;
    styleTrendLine(dsc->line_dsc, !ui->profilePointIsTarget[series][dsc->id], PROFILE_LINE_WIDTH);
}

void DefaultUI::profileChartBackgroundCb(lv_event_t *e) {
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(e));
    const int theme = ui->currentThemeMode >= 0 ? ui->currentThemeMode : 0;
    drawTrendGuides(e, theme, ui->profilePhaseMarks, PROFILE_CHART_POINTS, PROFILE_CHART_POINTS);
}

String DefaultUI::getErrorMessage() { return controller->getSystemStateMessage(); }

void DefaultUI::applyTheme() {
    const ::Settings &settings = controller->getSettings();
    int newThemeMode = settings.getThemeMode();
#ifndef GAGGIMATE_SIM // Amoled-specific black theme override is device-only
    if (newThemeMode == 0 && panelDriver == AmoledDisplayDriver::getInstance()) {
        newThemeMode = THEME_ID_AMOLED_DARK;
    }
#endif

    if (newThemeMode != currentThemeMode) {
        currentThemeMode = newThemeMode;
        // Via EEZ so its selected index updates too; on-demand screens (new profile) read it when created.
        static const char *const THEME_NAMES[] = {"Dark", "Light", "AmoledDark"};
        eez_flow_set_theme(THEME_NAMES[currentThemeMode]);
        applyShotChartTheme();
        if (profileChart != nullptr) {
            for (int i = 0; i < 2; i++)
                lv_chart_set_series_color(profileChart, profileSeries[i], lv_color_hex(PROFILE_COLORS[currentThemeMode][i]));
            lv_chart_refresh(profileChart);
        }
    }
}

void DefaultUI::loopTask(void *arg) {
    auto *ui = static_cast<DefaultUI *>(arg);
    while (true) {
        ui->loop();
        vTaskDelay(25 / portTICK_PERIOD_MS);
    }
}

void DefaultUI::profileLoopTask(void *arg) {
    auto *ui = static_cast<DefaultUI *>(arg);
    while (true) {
        ui->loopProfiles();
        vTaskDelay(25 / portTICK_PERIOD_MS);
    }
}
