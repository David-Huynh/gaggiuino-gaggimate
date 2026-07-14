#include "DefaultUI.h"
#include <display/plugins/autotuning/AutoTuningTasteGoalJson.h>

#include <WiFi.h>
#include <display/core/AutoTuning.h>
#include <display/core/Controller.h>
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
#include <display/ui/utils/effects.h>
#include <utility>

#include "esp_sntp.h"

#include <display/ui/default/eez/ui.h>

static EffectManager effect_mgr;

static constexpr uint32_t STARTUP_FADE_MS = 1000; // standby fade-in duration on power-up

static constexpr int32_t GAUGE_TICK_LONG = 25;      // meter tick length on most screens
static constexpr int32_t GAUGE_TICK_SHORT = 10;     // shortened tick length on profile / new-menu screens
static constexpr uint32_t GAUGE_TICK_ANIM_MS = 300; // tick length transition duration
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

// Profile and the new menu screen show shortened meter ticks.
static bool isShortTickScreen(ScreensEnum s) {
    return s == SCREEN_ID_PROFILE_SCREEN || s == SCREEN_ID_MENU_SCREEN_NEW || s == SCREEN_ID_INFO_SCREEN;
}

// Format a millisecond duration as "m:ss" for the brew/profile time labels.
static void formatDuration(unsigned long ms, char *buf, size_t len) {
    const double seconds = ms / 1000.0;
    const int minutes = static_cast<int>(seconds / 60.0);
    const int secs = static_cast<int>(seconds) % 60;
    snprintf(buf, len, "%d:%02d", minutes, secs);
}

static float clampPercentage(float pct) { return pct < 0.0f ? 0.0f : (pct > 100.0f ? 100.0f : pct); }

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

void DefaultUI::updateTempHistory() {
    if (currentTemp > 0) {
        if (tempHistoryIndex >= TEMP_HISTORY_LENGTH) {
            tempHistoryIndex = 0;
            isTempHistoryInitialized = true;
        }
        tempHistory[tempHistoryIndex] = currentTemp;
        tempHistoryIndex += 1;
    }

    if (tempHistoryIndex % 4 == 0) {
        heatingFlash = !heatingFlash;
        rerender = true;
    }
}

void DefaultUI::updateTempStableFlag() {
    if (isTempHistoryInitialized) {
        float totalError = 0.0f;
        float maxError = 0.0f;
        for (uint16_t i = 0; i < TEMP_HISTORY_LENGTH; i++) {
            float error = abs(tempHistory[i] - targetTemp);
            totalError += error;
            maxError = error > maxError ? error : maxError;
        }

        const float avgError = totalError / TEMP_HISTORY_LENGTH;
        const float errorMargin = max(2.0f, static_cast<float>(targetTemp) * 0.02f);

        isTemperatureStable = avgError < errorMargin && maxError <= errorMargin;
    }

    // instantly reset stability if setpoint has changed
    if (prevTargetTemp != targetTemp) {
        isTemperatureStable = false;
    }

    prevTargetTemp = targetTemp;
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
    pluginManager->on("controller:targetVolume:change", [=](Event const &event) { rerender = true; });
    pluginManager->on("controller:targetDuration:change", [=](Event const &event) { rerender = true; });
    pluginManager->on("controller:grindDuration:change", [=](Event const &event) { rerender = true; });
    pluginManager->on("controller:grindVolume:change", [=](Event const &event) { rerender = true; });
    pluginManager->on("controller:process:end", triggerRender);
    pluginManager->on("controller:process:start", triggerRender);
    pluginManager->on("controller:mode:change", [this](Event const &event) {
        mode = event.getInt("value");
        switch (mode) {
        case MODE_STANDBY:
            changeScreen(SCREEN_ID_STANDBY_SCREEN);
            break;
        case MODE_BREW:
            changeScreen(SCREEN_ID_BREW_SCREEN);
            break;
        case MODE_GRIND:
            changeScreen(SCREEN_ID_GRIND_SCREEN);
            break;
        case MODE_STEAM:
            changeScreen(SCREEN_ID_STEAM_SCREEN);
            break;
        case MODE_WATER:
            changeScreen(SCREEN_ID_WATER_SCREEN);
            break;
        default:
            break;
        };
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
    pluginManager->on("controller:bluetooth:connect", [this](Event const &) {
        waitingForController = false;
        rerender = true;
        initialized = true;
        // Stay on the standby screen when the controller is incompatible so the
        // mismatch message remains visible instead of jumping into brew.
        if (eez_flow_get_current_screen() == SCREEN_ID_STANDBY_SCREEN && !controller->getSystemInfo().protocolMismatch) {
            ::Settings &settings = controller->getSettings();
            if (settings.getStartupMode() == MODE_BREW) {
                changeScreen(SCREEN_ID_BREW_SCREEN);
            } else {
                standbyEnterTime = ::millis();
            }
        }
        pressureAvailable = controller->getSystemInfo().capabilities.pressure;
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
    pluginManager->on("controller:volumetric-measurement:bluetooth:change", [=](Event const &event) {
        double newWeight = event.getFloat("value");
        if (round(newWeight * 10.0) != round(bluetoothWeight * 10.0)) {
            bluetoothWeight = newWeight;
            rerender = true;
        }
    });
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
    xTaskCreatePinnedToCore(profileLoopTask, "DefaultUI::loopProfiles", configMINIMAL_STACK_SIZE * 8, this, 1, &profileTaskHandle,
                            0);
}

void DefaultUI::loop() {
    const unsigned long now = ::millis();
    const unsigned long diff = now - lastRender;

    if (now - lastTempLog > TEMP_HISTORY_INTERVAL) {
        updateTempHistory();
        lastTempLog = now;
    }

    if ((controller->isActive() && diff > RERENDER_INTERVAL_ACTIVE) || diff > RERENDER_INTERVAL_IDLE) {
        rerender = true;
    }

    if (rerender) {
        rerender = false;
        lastRender = now;
        applyTheme();
        if (controller->isErrorState()) {
            changeScreen(SCREEN_ID_STANDBY_SCREEN);
        }
        updateTempStableFlag();

        updateState();
        // Fill the EEZ data models before handleScreenChange() creates/ticks a screen (undefined fields abort the flow).
        updateSystemStatus();
        updateProfileInfo();
        updateBoiler();
        updateBrewProcess();
        const auto weightSource = controller->getCurrentVolumetricSource();
        double displayWeight = 0.0;
        if (weightSource == VolumetricMeasurementSource::BLUETOOTH) {
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
        profiles.reserve(ids.size());
        for (const auto &profileId : ids) {
            Profile profile{};
            profileManager->loadProfile(profileId, profile);
            profiles.emplace_back(std::move(profile));
        }
        {
            std::lock_guard<std::mutex> guard(profilesMutex);
            favoritedProfileIds = std::move(ids);
            favoritedProfiles = std::move(profiles);
        }
        profileLoaded = 1;
    }
}

void DefaultUI::changeScreen(ScreensEnum screen) {
    targetScreen = screen;
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
    changeScreen(SCREEN_ID_PROFILE_SCREEN);
}

void DefaultUI::onNextProfile() {
    std::lock_guard<std::mutex> guard(profilesMutex);
    if (currentProfileIdx + 1 < static_cast<int>(favoritedProfileIds.size())) {
        currentProfileIdx++;
    }
    rerender = true;
}

void DefaultUI::onPreviousProfile() {
    if (currentProfileIdx > 0) {
        currentProfileIdx--;
    }
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
    if (!id.isEmpty()) {
        profileManager->selectProfile(id);
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

    autoTuningStatusLabel = createAutoTuningLabel(body, "", 4, 370, bodyWidth - 12, &lv_font_montserrat_16, themeColor(0));
    autoTuningSummaryLabel = createAutoTuningLabel(body, "", 4, 438, bodyWidth - 12, &lv_font_montserrat_14, themeColor(3));

    updateAutoTuningSettingsModal();
}

void DefaultUI::closeAutoTuningSettings() {
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

    String summary = router.providerSummary();
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
    setupState();
    applyTheme();
    ui_tick();

    // Polished power-up: ui_init() makes standby active instantly, so stage a black screen and
    // fade standby in over it (lv_scr_load_anim no-ops when the target is already the active screen).
    lv_obj_t *standby = lv_scr_act();
    lv_obj_t *black = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(black, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(black, LV_OPA_COVER, LV_PART_MAIN);
    lv_scr_load(black);
    lv_scr_load_anim(standby, LV_SCR_LOAD_ANIM_FADE_ON, STARTUP_FADE_MS, 0, true);

    lv_task_handler();

    delay(100);
    // Set initial brightness based on settings
    const ::Settings &settings = controller->getSettings();
    setBrightness(settings.getMainBrightness());
}

void DefaultUI::setupState() {
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_SCALE_WEIGHT_CURRENT, currentWeight);
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_GRIND_WEIGHT_TARGET, grindWeightTarget);
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_GRIND_TIME_TARGET, grindTimeTarget);

    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_SYSTEM, systemStatus);
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
                          [=]() {
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
                              if (content == "") {
                                  return;
                              }
                              const char *data = content.c_str();
                              lv_qrcode_update(objects.qrcode, data, strlen(data));
                          },
                          &wifiConnected, &apActive);
    effect_mgr.use_effect([this]() { return currentScreen == SCREEN_ID_MENU_SCREEN_NEW; },
                          [this]() {
                              int radius = 135;
                              int count = grindAvailable ? 4 : 3;
                              int step = 360 / (grindAvailable ? 4 : 3);
                              int iconOffset = grindAvailable ? 1 : 0;
                              int rotationOffset = count == 4 ? 45 : 0;
                              positionMenuIcon(objects.btn_brew_1, step * 0 - rotationOffset, radius);
                              positionMenuIcon(objects.btn_steam_1, step * 1 - rotationOffset, radius);
                              positionMenuIcon(objects.btn_water_1, step * 2 - rotationOffset, radius);
                              positionMenuIcon(objects.btn_grind_1, step * 3 - rotationOffset, radius);
                              // positionMenuIcon(objects.btn_settings_1, step * (3 + iconOffset) - rotationOffset, radius);
                          },
                          &grindAvailable);
}

void DefaultUI::handleScreenChange() {
    if (currentScreen != targetScreen) {
        if (targetScreen == SCREEN_ID_STANDBY_SCREEN) {
            standbyEnterTime = ::millis();
        } else if (currentScreen == SCREEN_ID_STANDBY_SCREEN) {
            const ::Settings &settings = controller->getSettings();
            setBrightness(settings.getMainBrightness());
        }
        eez_flow_set_screen(targetScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0);
        animateGaugeTicks(currentScreen, targetScreen);
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
    for (uint8_t i = 0; i < gaugeCount; i++) {
        auto *meter = reinterpret_cast<lv_meter_t *>(gaugeMeters[i]);
        auto *scale = static_cast<lv_meter_scale_t *>(_lv_ll_get_head(&meter->scale_ll));
        if (scale != nullptr) {
            scale->tick_length = static_cast<uint16_t>(len);
        }
        lv_obj_invalidate(gaugeMeters[i]);
    }
}

void DefaultUI::gaugeTickAnimCb(void *var, int32_t v) { static_cast<DefaultUI *>(var)->setGaugeTickLength(v); }

void DefaultUI::animateGaugeTicks(ScreensEnum from, ScreensEnum to) {
    const int32_t fromLen = isShortTickScreen(from) ? GAUGE_TICK_SHORT : GAUGE_TICK_LONG;
    const int32_t toLen = isShortTickScreen(to) ? GAUGE_TICK_SHORT : GAUGE_TICK_LONG;

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
    grindAvailable = settings.isSmartGrindActive() || settings.getAltRelayFunction() == ALT_RELAY_GRIND;

    uiFlags.brew_adjustments(brewScreenState == BrewScreenState::Settings);
    uiFlags.active(controller->isActive());
    uiFlags.grind_active(controller->isGrindActive());
    uiFlags.grind_volumetric(controller->isVolumetricAvailable() && settings.isVolumetricTarget());
    uiFlags.heating_flash(heatingFlash);
    uiFlags.temperature_stable(isTemperatureStable);
    uiFlags.has_prev_profile(currentProfileIdx > 0);
    {
        std::lock_guard<std::mutex> guard(profilesMutex);
        uiFlags.has_next_profile(currentProfileIdx + 1 < static_cast<int>(favoritedProfileIds.size()));
    }
}

void DefaultUI::updateSystemStatus() {
    const auto &settings = controller->getSettings();
    systemStatus.bluetooth(controller->getClientController()->isConnected());
    systemStatus.wifi(!apActive && WiFi.status() == WL_CONNECTED);
    bool error = !initialized || waitingForController || controller->isErrorState() || controller->isUpdating() ||
                 controller->isAutotuning() || controller->getSystemInfo().protocolMismatch || !controller->isReady();
    systemStatus.error(error);
    const String errorLabel = error ? getErrorMessage() : "";
    if (stringChanged(systemStatus.error_label(), errorLabel.c_str()))
        systemStatus.error_label(errorLabel.c_str());
    systemStatus.volumetric_available(controller->isVolumetricAvailable());
    systemStatus.bluetooth_scales(controller->isBluetoothScaleHealthy());
    const String controllerVersion = controller->getSystemInfo().version;
    if (stringChanged(systemStatus.controller_version(), controllerVersion.c_str()))
        systemStatus.controller_version(controllerVersion.c_str());
    if (stringChanged(systemStatus.display_version(), BUILD_GIT_VERSION))
        systemStatus.display_version(BUILD_GIT_VERSION);
    systemStatus.update_available(updateAvailable);
    systemStatus.in_menu(currentScreen == SCREEN_ID_MENU_SCREEN_NEW);
    systemStatus.pressure_available(pressureAvailable);
    systemStatus.grind_available(grindAvailable);
    systemStatus.mode(mode);
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

static void populateProfileInfo(ProfileInfoValue &info, const Profile &profile, bool isCurrent) {
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
}

void DefaultUI::updateProfileInfo() {
    if (!initialized) {
        return;
    }
    populateProfileInfo(selectedProfileInfo, profileManager->getSelectedProfile(), true);
    selectedProfileInfo.dirty(profileDirty);

    // Preview backs the ProfileScreen carousel (index 0 = selected); hold the lock while
    // reading the vector — the profile task rebuilds it concurrently (GM-147).
    bool populated = false;
    {
        std::lock_guard<std::mutex> guard(profilesMutex);
        if (!favoritedProfiles.empty() && currentProfileIdx >= 0 &&
            currentProfileIdx < static_cast<int>(favoritedProfiles.size())) {
            populateProfileInfo(previewProfileInfo, favoritedProfiles[currentProfileIdx], currentProfileIdx == 0);
            populated = true;
        }
    }
    if (!populated) {
        populateProfileInfo(previewProfileInfo, profileManager->getSelectedProfile(), true);
    }
}

void DefaultUI::updateBoiler() {
    const ::Settings &settings = controller->getSettings();
    boiler.current_temperature(controller->getCurrentTemp());
    boiler.target_temperature(controller->getTargetTemp());
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
        const float target = phase.getVolumetricTarget().value;
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
}

void DefaultUI::updateMenuScreen() {}

String DefaultUI::getErrorMessage() {
    if (controller->isUpdating()) {
        return "Updating...";
    }
    if (controller->isAutotuning()) {
        return "Autotuning...";
    }
    if (controller->getSystemInfo().protocolMismatch) {
        return controller->getSystemInfo().protocolVersion > gm_proto::PROTOCOL_VERSION ? "Version mismatch, update display"
                                                                                        : "Version mismatch, update controller";
    }
    if (controller->isErrorState()) {
        switch (controller->getError()) {
        case ERROR_CODE_RUNAWAY:
            return "Temperature error, restart...";
        default:
            return "Unknown error";
        }
    }
    if (waitingForController) {
        return "Waiting for controller...";
    }
    return initialized ? "" : "Starting...";
}

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
        change_color_theme(currentThemeMode);
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
