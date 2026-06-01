#include "DefaultUI.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <display/core/Controller.h>
#include <display/core/process/BrewProcess.h>
#include <display/core/process/Process.h>
#include <display/core/zones.h>
#include <display/drivers/AmoledDisplayDriver.h>
#include <display/drivers/LilyGoDriver.h>
#include <display/drivers/WaveshareDriver.h>
#include <display/drivers/common/LV_Helper.h>
#include <display/main.h>
#include <display/ui/default/lvgl/ui_theme_manager.h>
#include <display/ui/default/lvgl/ui_themes.h>
#include <display/ui/utils/effects.h>
#include <algorithm>
#include <ctime>
#include <cstring>
#include <utility>

#include "esp_sntp.h"

static EffectManager effect_mgr;

struct RLContextButtonData {
    DefaultUI *ui;
    String id;
    bool retire;
};

static String rl_default_context_name() {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "Bean %lu", static_cast<unsigned long>(std::time(nullptr)));
    return String(buffer);
}

static String rl_make_context_id(const String &name) {
    String id = name;
    id.toLowerCase();
    id.replace(" ", "_");
    id.replace("/", "_");
    id.replace("\\", "_");
    id.replace(":", "_");
    id.replace("\"", "");
    if (id.isEmpty()) {
        id = "bean";
    }
    return "bean_" + id + "_" + String(static_cast<unsigned long>(std::time(nullptr))) + "_" +
           String(static_cast<unsigned long>(millis()));
}

static JsonArray rl_load_contexts(JsonDocument &doc, const String &rawJson) {
    DeserializationError error = deserializeJson(doc, rawJson);
    if (error || !doc.is<JsonArray>()) {
        doc.clear();
        return doc.to<JsonArray>();
    }
    return doc.as<JsonArray>();
}

static JsonObject rl_find_context(JsonArray contexts, const String &id) {
    for (JsonObject context : contexts) {
        if (context["id"].as<String>() == id) {
            return context;
        }
    }
    return JsonObject();
}

static int rl_current_bag_index(JsonArray contexts, const String &name) {
    int bag = 0;
    for (JsonObject context : contexts) {
        if (context["name"].as<String>() == name) {
            bag = std::max(bag, context["bag_index"] | 0);
        }
    }
    return bag;
}

static void rl_mark_other_contexts_available(JsonArray contexts, const String &activeId) {
    for (JsonObject context : contexts) {
        if (context["id"].as<String>() != activeId && context["status"].as<String>() == "active") {
            context["status"] = "available";
        }
    }
}

static void rl_add_context(JsonArray contexts, const String &id, const String &name, int bagIndex, const char *status) {
    JsonObject context = contexts.add<JsonObject>();
    context["id"] = id;
    context["name"] = name;
    context["bag_index"] = bagIndex;
    context["status"] = status;
    context["created_at"] = static_cast<long>(std::time(nullptr));
}

static void rl_persist_contexts(Settings &settings, JsonDocument &doc) {
    String json;
    serializeJson(doc, json);
    settings.setRLBeanContextsJson(json);
}

static void rl_emit_context_changed(PluginManager *pluginManager) {
    if (pluginManager) {
        pluginManager->trigger("rl:settings:changed");
    }
}

static lv_obj_t *rl_make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *userData) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 148, 34);
    lv_obj_set_style_radius(btn, 7, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xC8860A), LV_STATE_PRESSED);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_ALL, userData);
    }
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

static lv_obj_t *rl_make_label(lv_obj_t *parent, const char *text, const lv_font_t *font = &lv_font_montserrat_14) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(label, lv_color_hex(0xF5F5F5), 0);
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

static lv_obj_t *rl_make_overlay_card() {
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, 480, 480);
    lv_obj_set_align(overlay, LV_ALIGN_CENTER);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_set_size(card, 420, 420);
    lv_obj_set_align(card, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x171717), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x4A4A4A), 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);
    return overlay;
}

static lv_obj_t *rl_overlay_card(lv_obj_t *overlay) {
    return lv_obj_get_child(overlay, 0);
}

static void rl_close_overlay_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto *ui = static_cast<DefaultUI *>(lv_event_get_user_data(e));
    if (ui) {
        ui->markDirty();
    }
    lv_obj_t *overlay = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    while (overlay && lv_obj_get_parent(overlay) != lv_layer_top()) {
        overlay = lv_obj_get_parent(overlay);
    }
    if (overlay) {
        lv_obj_del(overlay);
    }
}

static void rl_show_overlay_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        static_cast<DefaultUI *>(lv_event_get_user_data(e))->showRLAutoTuningOverlay();
    }
}

static void rl_show_contexts_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        static_cast<DefaultUI *>(lv_event_get_user_data(e))->showRLContextPickerOverlay();
    }
}

static void rl_toggle_local_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        static_cast<DefaultUI *>(lv_event_get_user_data(e))->toggleRLLocalOptimization();
    }
}

static void rl_toggle_pause_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        static_cast<DefaultUI *>(lv_event_get_user_data(e))->toggleRLOptimizationPaused();
    }
}

static void rl_new_bean_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        static_cast<DefaultUI *>(lv_event_get_user_data(e))->startRLNewBean();
    }
}

static void rl_new_bag_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        static_cast<DefaultUI *>(lv_event_get_user_data(e))->startRLNewBag();
    }
}

static void rl_reset_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        static_cast<DefaultUI *>(lv_event_get_user_data(e))->resetRLDialIn();
    }
}

static void rl_retire_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        static_cast<DefaultUI *>(lv_event_get_user_data(e))->retireRLContext();
    }
}

static void rl_exclude_last_shot_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        static_cast<DefaultUI *>(lv_event_get_user_data(e))->correctRLLastShotExclude();
    }
}

static void rl_bad_prep_last_shot_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        static_cast<DefaultUI *>(lv_event_get_user_data(e))->correctRLLastShotBadPrep();
    }
}

static void rl_not_followed_grind_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        static_cast<DefaultUI *>(lv_event_get_user_data(e))->correctRLLastShotNotFollowed("grind");
    }
}

static void rl_not_followed_dose_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        static_cast<DefaultUI *>(lv_event_get_user_data(e))->correctRLLastShotNotFollowed("dose");
    }
}

static void rl_not_followed_yield_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        static_cast<DefaultUI *>(lv_event_get_user_data(e))->correctRLLastShotNotFollowed("yield");
    }
}

static void rl_requeue_uploads_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        static_cast<DefaultUI *>(lv_event_get_user_data(e))->requeueRLRejectedUploads();
    }
}

static void rl_context_select_cb(lv_event_t *e) {
    auto *data = static_cast<RLContextButtonData *>(lv_event_get_user_data(e));
    if (!data) {
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (data->retire) {
            data->ui->retireRLContext(data->id);
        } else {
            data->ui->switchRLContext(data->id);
        }
        lv_obj_t *overlay = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
        while (overlay && lv_obj_get_parent(overlay) != lv_layer_top()) {
            overlay = lv_obj_get_parent(overlay);
        }
        if (overlay) {
            lv_obj_del(overlay);
        }
    } else if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        delete data;
    }
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

void DefaultUI::adjustHeatingIndicator(lv_obj_t *dials) {
    lv_obj_t *heatingIcon = ui_comp_get_child(dials, UI_COMP_DIALS_TEMPICON);
    lv_obj_set_style_img_recolor(heatingIcon, lv_color_hex(isTemperatureStable ? 0x00D100 : 0xF62C2C),
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    if (!isTemperatureStable) {
        lv_obj_set_style_opa(heatingIcon, heatingFlash ? LV_OPA_50 : LV_OPA_100, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

void DefaultUI::reloadProfiles() { profileLoaded = 0; }

DefaultUI::DefaultUI(Controller *controller, Driver *driver, PluginManager *pluginManager)
    : controller(controller), panelDriver(driver), pluginManager(pluginManager) {
    setupPanel();
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
    pluginManager->on("controller:targetVolume:change", [=](Event const &event) {
        targetVolume = event.getFloat("value");
        rerender = true;
    });
    pluginManager->on("controller:targetDuration:change", [=](Event const &event) {
        targetDuration = event.getFloat("value");
        rerender = true;
    });
    pluginManager->on("controller:grindDuration:change", [=](Event const &event) {
        grindDuration = event.getInt("value");
        rerender = true;
    });
    pluginManager->on("controller:grindVolume:change", [=](Event const &event) {
        grindVolume = event.getFloat("value");
        rerender = true;
    });
    pluginManager->on("controller:process:end", triggerRender);
    pluginManager->on("controller:process:start", triggerRender);
    pluginManager->on("controller:mode:change", [this](Event const &event) {
        mode = event.getInt("value");
        switch (mode) {
        case MODE_STANDBY:
            changeScreen(&ui_StandbyScreen, &ui_StandbyScreen_screen_init);
            break;
        case MODE_BREW:
            changeScreen(&ui_BrewScreen, &ui_BrewScreen_screen_init);
            break;
        case MODE_GRIND:
            changeScreen(&ui_GrindScreen, &ui_GrindScreen_screen_init);
            break;
        case MODE_STEAM:
            changeScreen(&ui_SimpleProcessScreen, &ui_SimpleProcessScreen_screen_init);
            break;
        case MODE_WATER:
            changeScreen(&ui_SimpleProcessScreen, &ui_SimpleProcessScreen_screen_init);
            break;
        default:
            break;
        };
    });
    pluginManager->on("controller:brew:start",
                      [this](Event const &event) { changeScreen(&ui_StatusScreen, &ui_StatusScreen_screen_init); });
    pluginManager->on("controller:brew:clear", [this](Event const &event) {
        if (lv_scr_act() == ui_StatusScreen) {
            changeScreen(&ui_BrewScreen, &ui_BrewScreen_screen_init);
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
        if (lv_scr_act() == ui_StandbyScreen && !controller->getSystemInfo().protocolMismatch) {
            Settings &settings = controller->getSettings();
            if (settings.getStartupMode() == MODE_BREW) {
                changeScreen(&ui_BrewScreen, &ui_BrewScreen_screen_init);
            } else {
                standbyEnterTime = millis();
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
        updateActive = true;
        rerender = true;
        changeScreen(&ui_StandbyScreen, &ui_StandbyScreen_screen_init);
    });
    pluginManager->on("ota:update:end", [this](Event const &) {
        updateActive = false;
        rerender = true;
        changeScreen(&ui_StandbyScreen, &ui_StandbyScreen_screen_init);
    });
    pluginManager->on("ota:update:status", [this](Event const &event) {
        rerender = true;
        updateAvailable = event.getInt("value");
    });
    pluginManager->on("controller:error", [this](Event const &) {
        rerender = true;
        changeScreen(&ui_StandbyScreen, &ui_StandbyScreen_screen_init);
    });
    pluginManager->on("controller:protocol:mismatch", [this](Event const &) {
        // Incompatible firmware on the other end: control is inhibited (OTA only),
        // so surface it on the standby screen like a runaway error.
        rerender = true;
        changeScreen(&ui_StandbyScreen, &ui_StandbyScreen_screen_init);
    });
    pluginManager->on("controller:autotune:start",
                      [this](Event const &) { changeScreen(&ui_StandbyScreen, &ui_StandbyScreen_screen_init); });
    pluginManager->on("controller:autotune:result",
                      [this](Event const &) { changeScreen(&ui_StandbyScreen, &ui_StandbyScreen_screen_init); });
    pluginManager->on("rl:status:received", [this](Event const &event) {
        rlMode = event.getString("mode");
        rlLocalShotCount = event.getInt("local_shot_count");
        rlRatedShotCount = event.getInt("rated_shot_count");
        rlLastShotId = event.getString("last_shot_id");
        rlUploadQueueRejectedCount = event.getInt("upload_queue_rejected_count");
        rlUploadQueueLastRejectedRecordId = event.getString("upload_queue_last_rejected_record_id");
        rlUploadQueueLastRejectedError = event.getString("upload_queue_last_rejected_error");
        rlCommunityUploadEnabled = event.getInt("community_upload_enabled");
        rlBestKnownRecipe = event.getString("best_known_recipe");
        rerender = true;
    });
    pluginManager->on("rl:settings:changed", [this](Event const &) { rerender = true; });

    pluginManager->on("profiles:profile:select", [this](Event const &event) {
        // Reset the local copy before reload: parseProfile() appends to
        // profile.phases rather than replacing, so feeding it the already-
        // populated member would double the phase count on every profile
        // switch (memory leak + corrupted internal phase count). Mirrors the
        // pattern ProfileManager::selectProfile uses for the same reason.
        selectedProfile = Profile{};
        profileManager->loadSelectedProfile(selectedProfile);
        selectedProfileId = event.getString("id");
        targetDuration = profileManager->getSelectedProfile().getTotalDuration();
        targetVolume = profileManager->getSelectedProfile().getTotalVolume();
        profileVolumetric = profileManager->getSelectedProfile().isVolumetric();
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
        hardwareShotWeight = 0.0;
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
    pluginManager->on("controller:volumetric-measurement:hardware-shot:change", [=](Event const &event) {
        double newWeight = event.getFloat("value");
        if (round(newWeight * 10.0) != round(hardwareShotWeight * 10.0)) {
            hardwareShotWeight = newWeight;
            rerender = true;
        }
    });
#endif
    setupState();
    setupReactive();
    xTaskCreatePinnedToCore(loopTask, "DefaultUI::loop", configMINIMAL_STACK_SIZE * 6, this, 1, &taskHandle, 1);
    xTaskCreatePinnedToCore(profileLoopTask, "DefaultUI::loopProfiles", configMINIMAL_STACK_SIZE * 8, this, 1, &profileTaskHandle,
                            0);
}

void DefaultUI::loop() {
    const unsigned long now = millis();
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
        error = controller->isErrorState();
        protocolMismatch = controller->getSystemInfo().protocolMismatch;
        autotuning = controller->isAutotuning();
        const Settings &settings = controller->getSettings();
        volumetricAvailable = controller->isVolumetricAvailable();
        grindVolumetricAvailable = controller->isGrindVolumetricAvailable();
        bluetoothScales = controller->isBluetoothScaleHealthy();
        hardwareScalePresent = controller->isHardwareScalePresent();
        hardwareShotBaselineActive = controller->isHardwareScaleShotBaselineActive();
        scaleSource = settings.getScaleSource();
        active = controller->isActive();
        brewVolumetricSource = static_cast<int>(active ? controller->getCurrentVolumetricSource()
                                                       : controller->getResolvedBrewSource());
        grindVolumetricTarget = settings.isVolumetricTarget();
        volumetricMode = grindVolumetricAvailable && grindVolumetricTarget;
        brewVolumetric = volumetricAvailable && profileVolumetric;
        grindActive = controller->isGrindActive();
        smartGrindActive = settings.isSmartGrindActive();
        grindAvailable = smartGrindActive || settings.getAltRelayFunction() == ALT_RELAY_GRIND;
        rlAutoTuningEnabled = settings.isHomeAssistant() && settings.isRLRatingEnabled();
        rlOptimizationPaused = settings.isRLOptimizationPaused();
        rlLocalOptimizationEnabled = rlAutoTuningEnabled && settings.isRLLocalOptimizationEnabled() &&
                                     !settings.isRLOptimizationPaused() &&
                                     !settings.getRLBeanContextId().isEmpty();
        applyTheme();
        if (controller->isErrorState()) {
            changeScreen(&ui_StandbyScreen, &ui_StandbyScreen_screen_init);
        }
        updateTempStableFlag();
        handleScreenChange();
        currentScreen = lv_scr_act();
        if (lv_scr_act() == ui_StandbyScreen)
            updateStandbyScreen();
        if (lv_scr_act() == ui_StatusScreen)
            updateStatusScreen();
        updateRLAutoTuningWidgets();
        effect_mgr.evaluate_all();
    }

    lv_task_handler();
}

void DefaultUI::loopProfiles() {
    if (!profileLoaded) {
        const auto favoritedIds = profileManager->getFavoritedProfiles();
        favoritedProfileIds.clear();
        favoritedProfiles.clear();
        favoritedProfileIds.reserve(favoritedIds.size() + 1);
        favoritedProfileIds.emplace_back(controller->getSettings().getSelectedProfile());
        for (const auto &id : favoritedIds) {
            if (std::find(favoritedProfileIds.begin(), favoritedProfileIds.end(), id) == favoritedProfileIds.end())
                favoritedProfileIds.emplace_back(id);
        }
        favoritedProfiles.reserve(favoritedProfileIds.size());
        for (const auto &profileId : favoritedProfileIds) {
            Profile profile{};
            profileManager->loadProfile(profileId, profile);
            favoritedProfiles.emplace_back(std::move(profile));
        }
        profileLoaded = 1;
    }
}

void DefaultUI::changeScreen(lv_obj_t **screen, void (*target_init)()) {
    targetScreen = screen;
    targetScreenInit = target_init;
    rerender = true;

    // Reset some submenus
    brewScreenState = BrewScreenState::Brew;
}

void DefaultUI::changeBrewScreenMode(BrewScreenState state) {
    brewScreenState = state;
    rerender = true;
}

void DefaultUI::onProfileSwitch() {
    currentProfileIdx = 0;
    changeScreen(&ui_ProfileScreen, ui_ProfileScreen_screen_init);
}

void DefaultUI::onNextProfile() {
    if (currentProfileIdx < favoritedProfileIds.size() - 1) {
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
    profileManager->selectProfile(favoritedProfileIds[currentProfileIdx]);
    profileDirty = false;
    changeScreen(&ui_BrewScreen, ui_BrewScreen_screen_init);
}

void DefaultUI::onVolumetricDelete() {
    controller->onVolumetricDelete();
    profileVolumetric = profileManager->getSelectedProfile().isVolumetric();
    profileDirty = true;
}

void DefaultUI::updateRLAutoTuningWidgets() {
    Settings const &settings = controller->getSettings();
    const bool enabled = settings.isHomeAssistant() && settings.isRLRatingEnabled();
    const bool visible = lv_scr_act() == ui_BrewScreen && ui_BrewScreen_contentPanel4 != nullptr && enabled &&
                         brewScreenState == BrewScreenState::Brew;

    if (!visible) {
        if (lv_scr_act() == ui_BrewScreen && rlBrewPanel != nullptr) {
            lv_obj_del(rlBrewPanel);
        }
        rlBrewPanel = nullptr;
        rlBrewBeanLabel = nullptr;
        rlBrewModeLabel = nullptr;
        rlBrewToggleBtn = nullptr;
        rlBrewToggleLabel = nullptr;
        rlBrewContextBtn = nullptr;
        rlBrewContextLabel = nullptr;
        rlBrewManageBtn = nullptr;
        rlBrewManageLabel = nullptr;
        return;
    }

    if (rlBrewPanel == nullptr) {
        rlBrewPanel = lv_obj_create(ui_BrewScreen_contentPanel4);
        lv_obj_set_size(rlBrewPanel, 310, 74);
        lv_obj_set_align(rlBrewPanel, LV_ALIGN_CENTER);
        lv_obj_set_y(rlBrewPanel, 82);
        lv_obj_set_style_radius(rlBrewPanel, 8, 0);
        lv_obj_set_style_bg_color(rlBrewPanel, lv_color_hex(0x111111), 0);
        lv_obj_set_style_bg_opa(rlBrewPanel, LV_OPA_80, 0);
        lv_obj_set_style_border_width(rlBrewPanel, 1, 0);
        lv_obj_set_style_border_color(rlBrewPanel, lv_color_hex(0x4A4A4A), 0);
        lv_obj_set_style_pad_all(rlBrewPanel, 8, 0);
        lv_obj_clear_flag(rlBrewPanel, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *labels = lv_obj_create(rlBrewPanel);
        lv_obj_remove_style_all(labels);
        lv_obj_set_size(labels, 186, 56);
        lv_obj_set_align(labels, LV_ALIGN_LEFT_MID);
        lv_obj_set_flex_flow(labels, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(labels, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(labels, LV_OBJ_FLAG_SCROLLABLE);

        rlBrewBeanLabel = rl_make_label(labels, "Optimizing: none", &lv_font_montserrat_14);
        lv_obj_set_width(rlBrewBeanLabel, 186);
        rlBrewModeLabel = rl_make_label(labels, "Mode: none", &lv_font_montserrat_14);
        lv_obj_set_width(rlBrewModeLabel, 186);
        lv_obj_set_style_text_color(rlBrewModeLabel, lv_color_hex(0xBDBDBD), 0);

        rlBrewContextBtn = rl_make_button(rlBrewPanel, "Beans", rl_show_contexts_cb, this);
        lv_obj_set_size(rlBrewContextBtn, 54, 26);
        lv_obj_set_x(rlBrewContextBtn, 84);
        lv_obj_set_y(rlBrewContextBtn, -15);
        lv_obj_set_align(rlBrewContextBtn, LV_ALIGN_RIGHT_MID);
        rlBrewContextLabel = lv_obj_get_child(rlBrewContextBtn, 0);

        rlBrewManageBtn = rl_make_button(rlBrewPanel, "Tune", rl_show_overlay_cb, this);
        lv_obj_set_size(rlBrewManageBtn, 54, 26);
        lv_obj_set_x(rlBrewManageBtn, 24);
        lv_obj_set_y(rlBrewManageBtn, -15);
        lv_obj_set_align(rlBrewManageBtn, LV_ALIGN_RIGHT_MID);
        rlBrewManageLabel = lv_obj_get_child(rlBrewManageBtn, 0);

        rlBrewToggleBtn = rl_make_button(rlBrewPanel, "Local On", rl_toggle_local_cb, this);
        lv_obj_set_size(rlBrewToggleBtn, 114, 26);
        lv_obj_set_x(rlBrewToggleBtn, 24);
        lv_obj_set_y(rlBrewToggleBtn, 18);
        lv_obj_set_align(rlBrewToggleBtn, LV_ALIGN_RIGHT_MID);
        rlBrewToggleLabel = lv_obj_get_child(rlBrewToggleBtn, 0);
    }

    const String beanName = settings.getRLBeanContextName().isEmpty() ? "No bean selected" : settings.getRLBeanContextName();
    const String modeText = rlMode.isEmpty() ? "none" : rlMode;
    lv_label_set_text_fmt(rlBrewBeanLabel, "Optimizing: %s", beanName.c_str());
    lv_label_set_text_fmt(rlBrewModeLabel, "Mode: %s", modeText.c_str());

    const bool localOn = settings.isRLLocalOptimizationEnabled() && !settings.isRLOptimizationPaused() &&
                         !settings.getRLBeanContextId().isEmpty();
    lv_label_set_text(rlBrewToggleLabel, localOn ? "Local On" : "Local Off");
    lv_obj_set_style_bg_color(rlBrewToggleBtn, localOn ? lv_color_hex(0x2F7D32) : lv_color_hex(0x333333), 0);
    if (settings.getRLBeanContextId().isEmpty()) {
        lv_obj_add_state(rlBrewToggleBtn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(rlBrewToggleBtn, LV_STATE_DISABLED);
    }
}

void DefaultUI::toggleRLLocalOptimization() {
    Settings &settings = controller->getSettings();
    if (!settings.isHomeAssistant() || !settings.isRLRatingEnabled() || settings.getRLBeanContextId().isEmpty()) {
        return;
    }
    const bool next = !(settings.isRLLocalOptimizationEnabled() && !settings.isRLOptimizationPaused());
    settings.setRLLocalOptimizationEnabled(next);
    if (next) {
        settings.setRLOptimizationPaused(false);
    }
    settings.save(true);
    rl_emit_context_changed(pluginManager);
    pluginManager->trigger("settings:changed");
    rerender = true;
}

void DefaultUI::toggleRLOptimizationPaused() {
    Settings &settings = controller->getSettings();
    if (!settings.isHomeAssistant() || !settings.isRLRatingEnabled()) {
        return;
    }
    const bool nextPaused = !settings.isRLOptimizationPaused();
    settings.setRLOptimizationPaused(nextPaused);
    if (!nextPaused) {
        settings.setRLLocalOptimizationEnabled(true);
    }
    settings.save(true);
    rl_emit_context_changed(pluginManager);
    pluginManager->trigger("settings:changed");
    rerender = true;
}

void DefaultUI::startRLNewBean() {
    Settings &settings = controller->getSettings();
    if (!settings.isHomeAssistant() || !settings.isRLRatingEnabled()) {
        return;
    }

    JsonDocument contextsDoc;
    JsonArray contexts = rl_load_contexts(contextsDoc, settings.getRLBeanContextsJson());
    JsonObject current = rl_find_context(contexts, settings.getRLBeanContextId());
    if (!current.isNull()) {
        current["status"] = "retired";
        current["retired_at"] = static_cast<long>(std::time(nullptr));
    }

    const String name = rl_default_context_name();
    const String id = rl_make_context_id(name);
    rl_mark_other_contexts_available(contexts, id);
    rl_add_context(contexts, id, name, 1, "active");
    rl_persist_contexts(settings, contextsDoc);
    settings.setRLBeanContextId(id);
    settings.setRLBeanContextName(name);
    settings.setRLOptimizationPaused(false);
    settings.setRLLocalOptimizationEnabled(true);
    settings.save(true);
    rl_emit_context_changed(pluginManager);
    pluginManager->trigger("settings:changed");
    rerender = true;
}

void DefaultUI::startRLNewBag() {
    Settings &settings = controller->getSettings();
    if (!settings.isHomeAssistant() || !settings.isRLRatingEnabled()) {
        return;
    }

    JsonDocument contextsDoc;
    JsonArray contexts = rl_load_contexts(contextsDoc, settings.getRLBeanContextsJson());
    String name = settings.getRLBeanContextName();
    if (name.isEmpty()) {
        name = rl_default_context_name();
    }

    JsonObject current = rl_find_context(contexts, settings.getRLBeanContextId());
    if (!current.isNull()) {
        current["status"] = "retired";
        current["retired_at"] = static_cast<long>(std::time(nullptr));
    }

    const int bagIndex = rl_current_bag_index(contexts, name) + 1;
    const String id = rl_make_context_id(name);
    rl_mark_other_contexts_available(contexts, id);
    rl_add_context(contexts, id, name, bagIndex, "active");
    rl_persist_contexts(settings, contextsDoc);
    settings.setRLBeanContextId(id);
    settings.setRLBeanContextName(name);
    settings.setRLOptimizationPaused(false);
    settings.setRLLocalOptimizationEnabled(true);
    settings.save(true);
    rl_emit_context_changed(pluginManager);
    pluginManager->trigger("settings:changed");
    rerender = true;
}

void DefaultUI::resetRLDialIn() {
    Settings &settings = controller->getSettings();
    if (!settings.isHomeAssistant() || !settings.isRLRatingEnabled()) {
        return;
    }

    JsonDocument contextsDoc;
    JsonArray contexts = rl_load_contexts(contextsDoc, settings.getRLBeanContextsJson());
    String name = settings.getRLBeanContextName();
    if (name.isEmpty()) {
        name = rl_default_context_name();
    }

    JsonObject current = rl_find_context(contexts, settings.getRLBeanContextId());
    if (!current.isNull()) {
        current["status"] = "retired";
        current["retired_at"] = static_cast<long>(std::time(nullptr));
    }

    const int bagIndex = rl_current_bag_index(contexts, name) + 1;
    const String id = rl_make_context_id(name + "_reset");
    rl_mark_other_contexts_available(contexts, id);
    rl_add_context(contexts, id, name, bagIndex, "active");
    rl_persist_contexts(settings, contextsDoc);
    settings.setRLBeanContextId(id);
    settings.setRLBeanContextName(name);
    settings.setRLOptimizationPaused(false);
    settings.setRLLocalOptimizationEnabled(true);
    settings.save(true);
    rl_emit_context_changed(pluginManager);
    pluginManager->trigger("settings:changed");
    rerender = true;
}

void DefaultUI::retireRLContext() {
    retireRLContext(controller->getSettings().getRLBeanContextId());
}

void DefaultUI::retireRLContext(const String &contextId) {
    Settings &settings = controller->getSettings();
    if (!settings.isHomeAssistant() || !settings.isRLRatingEnabled() || contextId.isEmpty()) {
        return;
    }

    JsonDocument contextsDoc;
    JsonArray contexts = rl_load_contexts(contextsDoc, settings.getRLBeanContextsJson());
    JsonObject context = rl_find_context(contexts, contextId);
    if (!context.isNull()) {
        context["status"] = "retired";
        context["retired_at"] = static_cast<long>(std::time(nullptr));
        rl_persist_contexts(settings, contextsDoc);
    }
    if (settings.getRLBeanContextId() == contextId) {
        settings.setRLBeanContextId("");
        settings.setRLBeanContextName("");
        settings.setRLLocalOptimizationEnabled(false);
    }
    settings.save(true);
    rl_emit_context_changed(pluginManager);
    pluginManager->trigger("settings:changed");
    rerender = true;
}

void DefaultUI::switchRLContext(const String &contextId) {
    Settings &settings = controller->getSettings();
    if (!settings.isHomeAssistant() || !settings.isRLRatingEnabled() || contextId.isEmpty()) {
        return;
    }

    JsonDocument contextsDoc;
    JsonArray contexts = rl_load_contexts(contextsDoc, settings.getRLBeanContextsJson());
    JsonObject context = rl_find_context(contexts, contextId);
    if (context.isNull()) {
        return;
    }

    rl_mark_other_contexts_available(contexts, contextId);
    context["status"] = "active";
    settings.setRLBeanContextId(contextId);
    settings.setRLBeanContextName(context["name"].as<String>());
    settings.setRLOptimizationPaused(false);
    settings.setRLLocalOptimizationEnabled(true);
    rl_persist_contexts(settings, contextsDoc);
    settings.save(true);
    rl_emit_context_changed(pluginManager);
    pluginManager->trigger("settings:changed");
    rerender = true;
}

void DefaultUI::correctRLLastShotExclude() {
    Settings const &settings = controller->getSettings();
    if (!settings.isHomeAssistant() || !settings.isRLRatingEnabled() || rlLastShotId.isEmpty()) {
        return;
    }

    Event event;
    event.id = "rl:shot:correction";
    event.setString("shot_id", rlLastShotId);
    event.setString("source", "gaggimate_lvgl");
    event.setInt("has_exclude_from_local_optimization", 1);
    event.setInt("exclude_from_local_optimization", 1);
    event.setString("correction_tags", "changed_manually");
    pluginManager->trigger(event);
}

void DefaultUI::correctRLLastShotBadPrep() {
    Settings const &settings = controller->getSettings();
    if (!settings.isHomeAssistant() || !settings.isRLRatingEnabled() || rlLastShotId.isEmpty()) {
        return;
    }

    Event event;
    event.id = "rl:shot:correction";
    event.setString("shot_id", rlLastShotId);
    event.setString("source", "gaggimate_lvgl");
    event.setInt("has_exclude_from_local_optimization", 1);
    event.setInt("exclude_from_local_optimization", 1);
    event.setString("correction_tags", "bad_puck_prep,channeling_suspected");
    pluginManager->trigger(event);
}

void DefaultUI::correctRLLastShotNotFollowed(const char *fieldName) {
    Settings const &settings = controller->getSettings();
    if (!settings.isHomeAssistant() || !settings.isRLRatingEnabled() || rlLastShotId.isEmpty()) {
        return;
    }

    Event event;
    event.id = "rl:shot:correction";
    event.setString("shot_id", rlLastShotId);
    event.setString("source", "gaggimate_lvgl");
    String tags = "changed_manually";
    if (strcmp(fieldName, "grind") == 0) {
        event.setInt("has_grind_followed", 1);
        event.setInt("grind_followed", 0);
        tags += ",did_not_follow_grind";
    } else if (strcmp(fieldName, "dose") == 0) {
        event.setInt("has_dose_followed", 1);
        event.setInt("dose_followed", 0);
        tags += ",did_not_follow_dose";
    } else if (strcmp(fieldName, "yield") == 0) {
        event.setInt("has_yield_followed", 1);
        event.setInt("yield_followed", 0);
        tags += ",did_not_follow_yield";
    } else {
        return;
    }
    event.setString("correction_tags", tags);
    pluginManager->trigger(event);
}

void DefaultUI::requeueRLRejectedUploads() {
    Settings const &settings = controller->getSettings();
    if (!settings.isHomeAssistant() || !settings.isRLRatingEnabled() || rlUploadQueueRejectedCount < 1) {
        return;
    }

    Event event;
    event.id = "rl:upload:requeue";
    event.setString("source", "gaggimate_lvgl");
    event.setInt("limit", 50);
    pluginManager->trigger(event);
}

void DefaultUI::showRLAutoTuningOverlay() {
    Settings const &settings = controller->getSettings();
    if (!settings.isHomeAssistant() || !settings.isRLRatingEnabled()) {
        return;
    }

    rlOverlay = rl_make_overlay_card();
    lv_obj_t *card = rl_overlay_card(rlOverlay);

    lv_obj_t *header = lv_obj_create(card);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, 388, 38);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = rl_make_label(header, "Auto Tuning", &lv_font_montserrat_20);
    lv_obj_set_width(title, 240);
    lv_obj_t *close = rl_make_button(header, "Close", rl_close_overlay_cb, this);
    lv_obj_set_size(close, 78, 30);

    const String beanName = settings.getRLBeanContextName().isEmpty() ? "No bean selected" : settings.getRLBeanContextName();
    const String modeText = rlMode.isEmpty() ? "none" : rlMode;
    char line[160];

    snprintf(line, sizeof(line), "Current bean: %s", beanName.c_str());
    lv_obj_set_width(rl_make_label(card, line), 388);
    snprintf(line, sizeof(line), "Context: %s", settings.getRLBeanContextId().isEmpty() ? "none" : settings.getRLBeanContextId().c_str());
    lv_obj_set_width(rl_make_label(card, line), 388);
    snprintf(line, sizeof(line), "Mode: %s", modeText.c_str());
    lv_obj_set_width(rl_make_label(card, line), 388);
    snprintf(line, sizeof(line), "Local optimization: %s",
             settings.isRLOptimizationPaused()
                 ? "paused"
                 : (settings.isRLLocalOptimizationEnabled() && !settings.getRLBeanContextId().isEmpty() ? "on" : "off"));
    lv_obj_set_width(rl_make_label(card, line), 388);
    snprintf(line, sizeof(line), "Community upload: %s", rlCommunityUploadEnabled ? "on" : "off");
    lv_obj_set_width(rl_make_label(card, line), 388);
    snprintf(line, sizeof(line), "Shots: %d local / %d rated", rlLocalShotCount, rlRatedShotCount);
    lv_obj_set_width(rl_make_label(card, line), 388);
    snprintf(line, sizeof(line), "Last shot: %s", rlLastShotId.isEmpty() ? "none" : rlLastShotId.c_str());
    lv_obj_set_width(rl_make_label(card, line), 388);
    snprintf(line, sizeof(line), "Rejected uploads: %d", rlUploadQueueRejectedCount);
    lv_obj_set_width(rl_make_label(card, line), 388);
    if (rlUploadQueueRejectedCount > 0) {
        snprintf(
            line,
            sizeof(line),
            "Latest reject: %s",
            rlUploadQueueLastRejectedRecordId.isEmpty() ? "unknown" : rlUploadQueueLastRejectedRecordId.c_str());
        lv_obj_set_width(rl_make_label(card, line), 388);
        if (!rlUploadQueueLastRejectedError.isEmpty()) {
            snprintf(line, sizeof(line), "Error: %s", rlUploadQueueLastRejectedError.c_str());
            lv_obj_set_width(rl_make_label(card, line), 388);
        }
    }
    snprintf(line, sizeof(line), "Best recipe: %s", rlBestKnownRecipe.isEmpty() ? "none yet" : rlBestKnownRecipe.c_str());
    lv_obj_set_width(rl_make_label(card, line), 388);

    lv_obj_t *row1 = lv_obj_create(card);
    lv_obj_remove_style_all(row1);
    lv_obj_set_size(row1, 388, 38);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row1, 8, 0);
    lv_obj_clear_flag(row1, LV_OBJ_FLAG_SCROLLABLE);
    rl_make_button(row1, settings.isRLLocalOptimizationEnabled() ? "Local Off" : "Local On", rl_toggle_local_cb, this);
    rl_make_button(row1, settings.isRLOptimizationPaused() ? "Resume" : "Pause", rl_toggle_pause_cb, this);

    lv_obj_t *row2 = lv_obj_create(card);
    lv_obj_remove_style_all(row2);
    lv_obj_set_size(row2, 388, 38);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row2, 8, 0);
    lv_obj_clear_flag(row2, LV_OBJ_FLAG_SCROLLABLE);
    rl_make_button(row2, "Bean List", rl_show_contexts_cb, this);
    rl_make_button(row2, "New Bean", rl_new_bean_cb, this);

    lv_obj_t *row3 = lv_obj_create(card);
    lv_obj_remove_style_all(row3);
    lv_obj_set_size(row3, 388, 38);
    lv_obj_set_flex_flow(row3, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row3, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row3, 8, 0);
    lv_obj_clear_flag(row3, LV_OBJ_FLAG_SCROLLABLE);
    rl_make_button(row3, "New Bag", rl_new_bag_cb, this);
    rl_make_button(row3, "Reset Dial-In", rl_reset_cb, this);

    lv_obj_t *row4 = lv_obj_create(card);
    lv_obj_remove_style_all(row4);
    lv_obj_set_size(row4, 388, 38);
    lv_obj_set_flex_flow(row4, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row4, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row4, LV_OBJ_FLAG_SCROLLABLE);
    rl_make_button(row4, "Retire Bean", rl_retire_cb, this);
    lv_obj_t *retryBtn = rl_make_button(row4, "Retry Uploads", rl_requeue_uploads_cb, this);
    if (rlUploadQueueRejectedCount < 1) {
        lv_obj_add_state(retryBtn, LV_STATE_DISABLED);
    }

    lv_obj_t *correctionTitle = rl_make_label(card, "Last Shot Correction", &lv_font_montserrat_14);
    lv_obj_set_width(correctionTitle, 388);

    lv_obj_t *row5 = lv_obj_create(card);
    lv_obj_remove_style_all(row5);
    lv_obj_set_size(row5, 388, 38);
    lv_obj_set_flex_flow(row5, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row5, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row5, 8, 0);
    lv_obj_clear_flag(row5, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *excludeBtn = rl_make_button(row5, "Exclude", rl_exclude_last_shot_cb, this);
    lv_obj_t *badPrepBtn = rl_make_button(row5, "Bad Prep", rl_bad_prep_last_shot_cb, this);
    if (rlLastShotId.isEmpty()) {
        lv_obj_add_state(excludeBtn, LV_STATE_DISABLED);
        lv_obj_add_state(badPrepBtn, LV_STATE_DISABLED);
    }

    lv_obj_t *row6 = lv_obj_create(card);
    lv_obj_remove_style_all(row6);
    lv_obj_set_size(row6, 388, 38);
    lv_obj_set_flex_flow(row6, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row6, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row6, 8, 0);
    lv_obj_clear_flag(row6, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *grindBtn = rl_make_button(row6, "No Grind", rl_not_followed_grind_cb, this);
    lv_obj_t *doseBtn = rl_make_button(row6, "No Dose", rl_not_followed_dose_cb, this);
    lv_obj_t *yieldBtn = rl_make_button(row6, "No Yield", rl_not_followed_yield_cb, this);
    lv_obj_set_size(grindBtn, 118, 34);
    lv_obj_set_size(doseBtn, 118, 34);
    lv_obj_set_size(yieldBtn, 118, 34);
    if (rlLastShotId.isEmpty()) {
        lv_obj_add_state(grindBtn, LV_STATE_DISABLED);
        lv_obj_add_state(doseBtn, LV_STATE_DISABLED);
        lv_obj_add_state(yieldBtn, LV_STATE_DISABLED);
    }
}

void DefaultUI::showRLContextPickerOverlay() {
    Settings const &settings = controller->getSettings();
    if (!settings.isHomeAssistant() || !settings.isRLRatingEnabled()) {
        return;
    }

    rlOverlay = rl_make_overlay_card();
    lv_obj_t *card = rl_overlay_card(rlOverlay);

    lv_obj_t *header = lv_obj_create(card);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, 388, 38);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(rl_make_label(header, "Bean Contexts", &lv_font_montserrat_20), 240);
    lv_obj_t *close = rl_make_button(header, "Close", rl_close_overlay_cb, this);
    lv_obj_set_size(close, 78, 30);

    JsonDocument contextsDoc;
    JsonArray contexts = rl_load_contexts(contextsDoc, settings.getRLBeanContextsJson());
    if (contexts.size() == 0) {
        lv_obj_set_width(rl_make_label(card, "No bean contexts yet."), 388);
    }

    for (JsonObject context : contexts) {
        const String id = context["id"].as<String>();
        const String name = context["name"].as<String>().isEmpty() ? "Unnamed bean" : context["name"].as<String>();
        const int bagIndex = context["bag_index"] | 1;
        const String status = context["status"].as<String>();
        const bool activeContext = id == settings.getRLBeanContextId();

        lv_obj_t *row = lv_obj_create(card);
        lv_obj_set_size(row, 388, 64);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, activeContext ? lv_color_hex(0x2B2B2B) : lv_color_hex(0x202020), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x444444), 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *text = lv_obj_create(row);
        lv_obj_remove_style_all(text);
        lv_obj_set_size(text, 178, 48);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(text, LV_OBJ_FLAG_SCROLLABLE);

        char line[128];
        snprintf(line, sizeof(line), "%s%s", name.c_str(), activeContext ? " (active)" : "");
        lv_obj_set_width(rl_make_label(text, line), 178);
        snprintf(line, sizeof(line), "Bag %d - %s", bagIndex, status.isEmpty() ? "available" : status.c_str());
        lv_obj_t *detail = rl_make_label(text, line);
        lv_obj_set_width(detail, 178);
        lv_obj_set_style_text_color(detail, lv_color_hex(0xBDBDBD), 0);

        auto *switchData = new RLContextButtonData{this, id, false};
        lv_obj_t *switchBtn = rl_make_button(row, activeContext ? "Active" : "Switch", rl_context_select_cb, switchData);
        lv_obj_set_size(switchBtn, 78, 30);
        if (activeContext) {
            lv_obj_add_state(switchBtn, LV_STATE_DISABLED);
        }

        auto *retireData = new RLContextButtonData{this, id, true};
        lv_obj_t *retireBtn = rl_make_button(row, "Retire", rl_context_select_cb, retireData);
        lv_obj_set_size(retireBtn, 78, 30);
        if (status == "retired") {
            lv_obj_add_state(retireBtn, LV_STATE_DISABLED);
        }
    }
}

void DefaultUI::setupPanel() {
    ui_init();
    lv_task_handler();

    delay(100);
    // Set initial brightness based on settings
    const Settings &settings = controller->getSettings();
    setBrightness(settings.getMainBrightness());
}

void DefaultUI::setupState() {
    error = controller->isErrorState();
    protocolMismatch = controller->getSystemInfo().protocolMismatch;
    autotuning = controller->isAutotuning();
    const Settings &settings = controller->getSettings();
    volumetricAvailable = controller->isVolumetricAvailable();
    grindVolumetricAvailable = controller->isGrindVolumetricAvailable();
    bluetoothScales = controller->isBluetoothScaleHealthy();
    hardwareScalePresent = controller->isHardwareScalePresent();
    hardwareShotBaselineActive = controller->isHardwareScaleShotBaselineActive();
    scaleSource = settings.getScaleSource();
    active = controller->isActive();
    brewVolumetricSource = static_cast<int>(active ? controller->getCurrentVolumetricSource()
                                                   : controller->getResolvedBrewSource());
    grindVolumetricTarget = settings.isVolumetricTarget();
    volumetricMode = grindVolumetricAvailable && grindVolumetricTarget;
    grindActive = controller->isGrindActive();
    smartGrindActive = settings.isSmartGrindActive();
    grindAvailable = smartGrindActive || settings.getAltRelayFunction() == ALT_RELAY_GRIND;
    rlAutoTuningEnabled = settings.isHomeAssistant() && settings.isRLRatingEnabled();
    rlOptimizationPaused = settings.isRLOptimizationPaused();
    rlLocalOptimizationEnabled = rlAutoTuningEnabled && settings.isRLLocalOptimizationEnabled() &&
                                 !settings.isRLOptimizationPaused() && !settings.getRLBeanContextId().isEmpty();
    mode = controller->getMode();
    currentTemp = static_cast<int>(controller->getCurrentTemp());
    targetTemp = static_cast<int>(controller->getTargetTemp());
    targetDuration = profileManager->getSelectedProfile().getTotalDuration();
    targetVolume = profileManager->getSelectedProfile().getTotalVolume();
    grindDuration = settings.getTargetGrindDuration();
    grindVolume = settings.getTargetGrindVolume();
    pressureAvailable = controller->getSystemInfo().capabilities.pressure ? 1 : 0;
    pressureScaling = std::ceil(settings.getPressureScaling());
    selectedProfileId = settings.getSelectedProfile();
    // Defense in depth: reset before reload (this site is currently safe in
    // practice because setupState runs once with a fresh field-initialized
    // member, but keeps the pattern consistent with the event handler above).
    selectedProfile = Profile{};
    profileManager->loadSelectedProfile(selectedProfile);
    profileVolumetric = selectedProfile.isVolumetric();
}

void DefaultUI::setupReactive() {
    effect_mgr.use_effect([=] { return currentScreen == ui_MenuScreen; }, [=]() { adjustDials(ui_MenuScreen_dials); },
                          &pressureAvailable);
    effect_mgr.use_effect([=] { return currentScreen == ui_StatusScreen; }, [=]() { adjustDials(ui_StatusScreen_dials); },
                          &pressureAvailable);
    effect_mgr.use_effect([=] { return currentScreen == ui_BrewScreen; }, [=]() { adjustDials(ui_BrewScreen_dials); },
                          &pressureAvailable);
    effect_mgr.use_effect([=] { return currentScreen == ui_GrindScreen; }, [=]() { adjustDials(ui_GrindScreen_dials); },
                          &pressureAvailable);
    effect_mgr.use_effect([=] { return currentScreen == ui_SimpleProcessScreen; },
                          [=]() { adjustDials(ui_SimpleProcessScreen_dials); }, &pressureAvailable);
    effect_mgr.use_effect([=] { return currentScreen == ui_ProfileScreen; }, [=]() { adjustDials(ui_ProfileScreen_dials); },
                          &pressureAvailable);
    effect_mgr.use_effect([=] { return currentScreen == ui_BrewScreen; }, [=]() { adjustHeatingIndicator(ui_BrewScreen_dials); },
                          &isTemperatureStable, &heatingFlash);
    effect_mgr.use_effect([=] { return currentScreen == ui_SimpleProcessScreen; },
                          [=]() { adjustHeatingIndicator(ui_SimpleProcessScreen_dials); }, &isTemperatureStable, &heatingFlash);
    effect_mgr.use_effect([=] { return currentScreen == ui_MenuScreen; }, [=]() { adjustHeatingIndicator(ui_MenuScreen_dials); },
                          &isTemperatureStable, &heatingFlash);
    effect_mgr.use_effect([=] { return currentScreen == ui_ProfileScreen; },
                          [=]() { adjustHeatingIndicator(ui_ProfileScreen_dials); }, &isTemperatureStable, &heatingFlash);
    effect_mgr.use_effect([=] { return currentScreen == ui_GrindScreen; },
                          [=]() { adjustHeatingIndicator(ui_GrindScreen_dials); }, &isTemperatureStable, &heatingFlash);
    effect_mgr.use_effect([=] { return currentScreen == ui_StatusScreen; },
                          [=]() { adjustHeatingIndicator(ui_StatusScreen_dials); }, &isTemperatureStable, &heatingFlash);
    effect_mgr.use_effect([=] { return currentScreen == ui_SimpleProcessScreen; },
                          [=]() { lv_label_set_text(ui_SimpleProcessScreen_mainLabel5, mode == MODE_STEAM ? "Steam" : "Water"); },
                          &mode);
    effect_mgr.use_effect([=] { return currentScreen == ui_MenuScreen; },
                          [=]() {
                              lv_arc_set_value(uic_MenuScreen_dials_tempGauge, currentTemp);
                              lv_label_set_text_fmt(uic_MenuScreen_dials_tempText, "%d°C", currentTemp);
                          },
                          &currentTemp);
    effect_mgr.use_effect([=] { return currentScreen == ui_StatusScreen; },
                          [=]() {
                              lv_arc_set_value(uic_StatusScreen_dials_tempGauge, currentTemp);
                              lv_label_set_text_fmt(uic_StatusScreen_dials_tempText, "%d°C", currentTemp);
                          },
                          &currentTemp);
    effect_mgr.use_effect([=] { return currentScreen == ui_BrewScreen; },
                          [=]() {
                              lv_arc_set_value(uic_BrewScreen_dials_tempGauge, currentTemp);
                              lv_label_set_text_fmt(uic_BrewScreen_dials_tempText, "%d°C", currentTemp);
                          },
                          &currentTemp);
    effect_mgr.use_effect([=] { return currentScreen == ui_GrindScreen; },
                          [=]() {
                              lv_arc_set_value(uic_GrindScreen_dials_tempGauge, currentTemp);
                              lv_label_set_text_fmt(uic_GrindScreen_dials_tempText, "%d°C", currentTemp);
                          },
                          &currentTemp);
    effect_mgr.use_effect([=] { return currentScreen == ui_SimpleProcessScreen; },
                          [=]() {
                              lv_arc_set_value(uic_SimpleProcessScreen_dials_tempGauge, currentTemp);
                              lv_label_set_text_fmt(uic_SimpleProcessScreen_dials_tempText, "%d°C", currentTemp);
                          },
                          &currentTemp);
    effect_mgr.use_effect([=] { return currentScreen == ui_ProfileScreen; },
                          [=]() {
                              lv_arc_set_value(uic_ProfileScreen_dials_tempGauge, currentTemp);
                              lv_label_set_text_fmt(uic_ProfileScreen_dials_tempText, "%d°C", currentTemp);
                          },
                          &currentTemp);
    effect_mgr.use_effect([=] { return currentScreen == ui_MenuScreen; }, [=]() { adjustTempTarget(ui_MenuScreen_dials); },
                          &targetTemp);
    effect_mgr.use_effect([=] { return currentScreen == ui_StatusScreen; },
                          [=]() {
                              lv_label_set_text_fmt(ui_StatusScreen_targetTemp, "%d°C", targetTemp);
                              adjustTempTarget(ui_StatusScreen_dials);
                          },
                          &targetTemp);
    effect_mgr.use_effect([=] { return currentScreen == ui_BrewScreen; },
                          [=]() {
                              lv_label_set_text_fmt(ui_BrewScreen_targetTemp, "%d°C", targetTemp);
                              adjustTempTarget(ui_BrewScreen_dials);
                          },
                          &targetTemp);
    effect_mgr.use_effect([=] { return currentScreen == ui_GrindScreen; }, [=]() { adjustTempTarget(ui_GrindScreen_dials); },
                          &targetTemp);
    effect_mgr.use_effect([=] { return currentScreen == ui_SimpleProcessScreen; },
                          [=]() {
                              lv_label_set_text_fmt(ui_SimpleProcessScreen_targetTemp, "%d°C", targetTemp);
                              adjustTempTarget(ui_SimpleProcessScreen_dials);
                          },
                          &targetTemp);
    effect_mgr.use_effect([=] { return currentScreen == ui_ProfileScreen; }, [=]() { adjustTempTarget(ui_ProfileScreen_dials); },
                          &targetTemp);
    effect_mgr.use_effect([=] { return currentScreen == ui_MenuScreen; },
                          [=]() {
                              lv_arc_set_value(uic_MenuScreen_dials_pressureGauge, pressure * 10.0f);
                              lv_label_set_text_fmt(uic_MenuScreen_dials_pressureText, "%.1f bar", pressure);
                          },
                          &pressure);
    effect_mgr.use_effect([=] { return currentScreen == ui_StatusScreen; },
                          [=]() {
                              lv_arc_set_value(uic_StatusScreen_dials_pressureGauge, pressure * 10.0f);
                              lv_label_set_text_fmt(uic_StatusScreen_dials_pressureText, "%.1f bar", pressure);
                          },
                          &pressure);
    effect_mgr.use_effect([=] { return currentScreen == ui_BrewScreen; },
                          [=]() {
                              lv_arc_set_value(uic_BrewScreen_dials_pressureGauge, pressure * 10.0f);
                              lv_label_set_text_fmt(uic_BrewScreen_dials_pressureText, "%.1f bar", pressure);
                          },
                          &pressure);
    effect_mgr.use_effect([=] { return currentScreen == ui_GrindScreen; },
                          [=]() {
                              lv_arc_set_value(uic_GrindScreen_dials_pressureGauge, pressure * 10.0f);
                              lv_label_set_text_fmt(uic_GrindScreen_dials_pressureText, "%.1f bar", pressure);
                          },
                          &pressure);
    effect_mgr.use_effect([=] { return currentScreen == ui_SimpleProcessScreen; },
                          [=]() {
                              lv_arc_set_value(uic_SimpleProcessScreen_dials_pressureGauge, pressure * 10.0f);
                              lv_label_set_text_fmt(uic_SimpleProcessScreen_dials_pressureText, "%.1f bar", pressure);
                          },
                          &pressure);
    effect_mgr.use_effect([=] { return currentScreen == ui_ProfileScreen; },
                          [=]() {
                              lv_arc_set_value(uic_ProfileScreen_dials_pressureGauge, pressure * 10.0f);
                              lv_label_set_text_fmt(uic_ProfileScreen_dials_pressureText, "%.1f bar", pressure);
                          },
                          &pressure);
    effect_mgr.use_effect([=] { return currentScreen == ui_StandbyScreen; },
                          [=]() {
                              updateAvailable ? lv_obj_clear_flag(ui_StandbyScreen_updateIcon, LV_OBJ_FLAG_HIDDEN)
                                              : lv_obj_add_flag(ui_StandbyScreen_updateIcon, LV_OBJ_FLAG_HIDDEN);
                          },
                          &updateAvailable);
    effect_mgr.use_effect([=] { return currentScreen == ui_StandbyScreen; },
                          [=]() {
                              bool deactivated = true;
                              if (updateActive) {
                                  lv_label_set_text_fmt(ui_StandbyScreen_mainLabel, "Updating...");
                              } else if (protocolMismatch) {
                                  lv_label_set_text_fmt(ui_StandbyScreen_mainLabel, "Protocol error, please update");
                              } else if (error) {
                                  if (controller->getError() == ERROR_CODE_RUNAWAY) {
                                      lv_label_set_text_fmt(ui_StandbyScreen_mainLabel, "Temperature error, please restart");
                                  }
                              } else if (autotuning) {
                                  lv_label_set_text_fmt(ui_StandbyScreen_mainLabel, "Autotuning...");
                              } else if (waitingForController) {
                                  lv_label_set_text_fmt(ui_StandbyScreen_mainLabel, "Waiting for controller...");
                              } else {
                                  deactivated = !initialized;
                              }
                              _ui_flag_modify(ui_StandbyScreen_mainLabel, LV_OBJ_FLAG_HIDDEN, deactivated);
                              _ui_flag_modify(ui_StandbyScreen_touchIcon, LV_OBJ_FLAG_HIDDEN, !deactivated);
                              _ui_flag_modify(ui_StandbyScreen_statusContainer, LV_OBJ_FLAG_HIDDEN, !deactivated);
                          },
                          &updateAvailable, &error, &protocolMismatch, &autotuning, &waitingForController, &initialized);
    effect_mgr.use_effect([=] { return currentScreen == ui_BrewScreen; },
                          [=]() {
                              if (brewVolumetric) {
                                  lv_label_set_text_fmt(ui_BrewScreen_targetDuration, "%.1fg", targetVolume);
                              } else {
                                  const double secondsDouble = targetDuration;
                                  const auto minutes = static_cast<int>(secondsDouble / 60.0);
                                  const auto seconds = static_cast<int>(secondsDouble) % 60;
                                  lv_label_set_text_fmt(ui_BrewScreen_targetDuration, "%2d:%02d", minutes, seconds);
                              }
                          },
                          &targetDuration, &targetVolume, &brewVolumetric);
    effect_mgr.use_effect([=] { return currentScreen == ui_GrindScreen; },
                          [=]() {
                              if (volumetricMode) {
                                  lv_label_set_text_fmt(ui_GrindScreen_targetDuration, "%.1fg", grindVolume);
                              } else {
                                  const double secondsDouble = grindDuration / 1000.0;
                                  const auto minutes = static_cast<int>(secondsDouble / 60.0);
                                  const auto seconds = static_cast<int>(secondsDouble) % 60;
                                  lv_label_set_text_fmt(ui_GrindScreen_targetDuration, "%2d:%02d", minutes, seconds);
                              }
                          },
                          &grindDuration, &grindVolume, &volumetricMode);
    effect_mgr.use_effect([=] { return currentScreen == ui_BrewScreen; },
                          [=]() {
                              lv_img_set_src(ui_BrewScreen_Image4, brewVolumetric ? &ui_img_1424216268 : &ui_img_360122106);
                              _ui_flag_modify(ui_BrewScreen_byTimeButton, LV_OBJ_FLAG_HIDDEN, brewVolumetric);
                          },
                          &brewVolumetric);
    effect_mgr.use_effect(
        [=] { return currentScreen == ui_GrindScreen; },
        [=]() {
            lv_img_set_src(ui_GrindScreen_targetSymbol, volumetricMode ? &ui_img_1424216268 : &ui_img_360122106);
            ui_object_set_themeable_style_property(ui_GrindScreen_weightLabel, LV_PART_MAIN | LV_STATE_DEFAULT,
                                                   LV_STYLE_TEXT_COLOR,
                                                   volumetricMode ? _ui_theme_color_Dark : _ui_theme_color_NiceWhite);
            ui_object_set_themeable_style_property(ui_GrindScreen_volumetricButton, LV_PART_MAIN | LV_STATE_DEFAULT,
                                                   LV_STYLE_IMG_RECOLOR,
                                                   volumetricMode ? _ui_theme_color_Dark : _ui_theme_color_NiceWhite);
            ui_object_set_themeable_style_property(ui_GrindScreen_modeSwitch, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR,
                                                   volumetricMode ? _ui_theme_color_NiceWhite : _ui_theme_color_Dark);
        },
        &volumetricMode);
    effect_mgr.use_effect([=] { return currentScreen == ui_GrindScreen; },
                          [=]() {
                              _ui_flag_modify(ui_GrindScreen_modeSwitch, LV_OBJ_FLAG_HIDDEN,
                                              grindVolumetricAvailable || grindVolumetricTarget);
                              const lv_img_dsc_t *icon =
                                  grindVolumetricTarget ? &ui_img_1424216268 : &ui_img_2044104741;
                              lv_img_set_src(ui_GrindScreen_volumetricButton, icon);
                          },
                          &grindVolumetricAvailable, &grindVolumetricTarget);
    effect_mgr.use_effect([=] { return currentScreen == ui_SimpleProcessScreen; },
                          [=]() {
                              if (mode == MODE_STEAM) {
                                  _ui_flag_modify(ui_SimpleProcessScreen_goButton, LV_OBJ_FLAG_HIDDEN, active);
                                  lv_imgbtn_set_src(ui_SimpleProcessScreen_goButton, LV_IMGBTN_STATE_RELEASED, nullptr,
                                                    &ui_img_691326438, nullptr);
                              } else {
                                  lv_imgbtn_set_src(ui_SimpleProcessScreen_goButton, LV_IMGBTN_STATE_RELEASED, nullptr,
                                                    active ? &ui_img_1456692430 : &ui_img_445946954, nullptr);
                              }
                          },
                          &active, &mode);
    effect_mgr.use_effect([=] { return currentScreen == ui_GrindScreen; },
                          [=]() {
                              lv_imgbtn_set_src(ui_GrindScreen_startButton, LV_IMGBTN_STATE_RELEASED, nullptr,
                                                grindActive ? &ui_img_1456692430 : &ui_img_445946954, nullptr);
                          },
                          &grindActive);
    effect_mgr.use_effect([=] { return currentScreen == ui_BrewScreen; },
                          [=] { lv_label_set_text(ui_BrewScreen_profileName, selectedProfile.label.c_str()); },
                          &selectedProfileId);

    effect_mgr.use_effect(
        [=] { return currentScreen == ui_ProfileScreen; },
        [=] {
            if (profileLoaded) {
                _ui_flag_modify(ui_ProfileScreen_profileDetails, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_REMOVE);
                _ui_flag_modify(ui_ProfileScreen_loadingSpinner, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_ADD);
                lv_label_set_text(ui_ProfileScreen_profileName, favoritedProfiles[currentProfileIdx].label.c_str());
                lv_label_set_text(ui_ProfileScreen_mainLabel, currentProfileIdx == 0 ? "Current profile" : "Select profile");

                const auto minutes = static_cast<int>(favoritedProfiles[currentProfileIdx].getTotalDuration() / 60.0 - 0.5);
                const auto seconds = static_cast<int>(favoritedProfiles[currentProfileIdx].getTotalDuration()) % 60;
                lv_label_set_text_fmt(ui_ProfileScreen_targetDuration2, "%2d:%02d", minutes, seconds);
                lv_label_set_text_fmt(ui_ProfileScreen_targetTemp2, "%d°C",
                                      static_cast<int>(favoritedProfiles[currentProfileIdx].temperature));
                unsigned int phaseCount = favoritedProfiles[currentProfileIdx].getPhaseCount();
                unsigned int stepCount = favoritedProfiles[currentProfileIdx].phases.size();
                lv_label_set_text_fmt(ui_ProfileScreen_stepsLabel, "%d step%s", stepCount, stepCount > 1 ? "s" : "");
                lv_label_set_text_fmt(ui_ProfileScreen_phasesLabel, "%d phase%s", phaseCount, phaseCount > 1 ? "s" : "");
            } else {
                _ui_flag_modify(ui_ProfileScreen_profileDetails, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_ADD);
                _ui_flag_modify(ui_ProfileScreen_loadingSpinner, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_REMOVE);
            }

            ui_object_set_themeable_style_property(ui_ProfileScreen_previousProfileBtn, LV_PART_MAIN | LV_STATE_DEFAULT,
                                                   LV_STYLE_IMG_RECOLOR,
                                                   currentProfileIdx > 0 ? _ui_theme_color_NiceWhite : _ui_theme_color_SemiDark);
            ui_object_set_themeable_style_property(ui_ProfileScreen_previousProfileBtn, LV_PART_MAIN | LV_STATE_DEFAULT,
                                                   LV_STYLE_IMG_RECOLOR_OPA,
                                                   currentProfileIdx > 0 ? _ui_theme_alpha_NiceWhite : _ui_theme_alpha_SemiDark);
            ui_object_set_themeable_style_property(
                ui_ProfileScreen_nextProfileBtn, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_IMG_RECOLOR,
                currentProfileIdx < favoritedProfiles.size() - 1 ? _ui_theme_color_NiceWhite : _ui_theme_color_SemiDark);
            ui_object_set_themeable_style_property(
                ui_ProfileScreen_nextProfileBtn, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_IMG_RECOLOR_OPA,
                currentProfileIdx < favoritedProfiles.size() - 1 ? _ui_theme_alpha_NiceWhite : _ui_theme_alpha_SemiDark);
        },
        &currentProfileIdx, &profileLoaded);

    // Show/hide grind button based on SmartGrind setting or Alt Relay function
    effect_mgr.use_effect([=] { return currentScreen == ui_MenuScreen; },
                          [=]() {
                              grindAvailable ? lv_obj_clear_flag(ui_MenuScreen_grindBtn, LV_OBJ_FLAG_HIDDEN)
                                             : lv_obj_add_flag(ui_MenuScreen_grindBtn, LV_OBJ_FLAG_HIDDEN);
                          },
                          &grindAvailable);
    effect_mgr.use_effect([=] { return currentScreen == ui_BrewScreen; },
                          [=]() {
                              const auto source = static_cast<VolumetricMeasurementSource>(brewVolumetricSource);
                              if (volumetricAvailable && source == VolumetricMeasurementSource::HARDWARE_SCALE) {
                                  lv_label_set_text_fmt(ui_BrewScreen_weightLabel, "%.1fg",
                                                        hardwareShotBaselineActive ? hardwareShotWeight : hardwareWeight);
                              } else if (volumetricAvailable && source == VolumetricMeasurementSource::BLUETOOTH) {
                                  lv_label_set_text_fmt(ui_BrewScreen_weightLabel, "%.1fg", bluetoothWeight);
                              } else if (volumetricAvailable && source == VolumetricMeasurementSource::FLOW_ESTIMATION) {
                                  lv_label_set_text_fmt(ui_BrewScreen_weightLabel, "%.1fg", estimatedWeight);
                              } else {
                                  lv_label_set_text(ui_BrewScreen_weightLabel, "-");
                              }
                          },
                          &bluetoothWeight, &hardwareWeight, &hardwareShotWeight, &estimatedWeight, &volumetricAvailable,
                          &brewVolumetricSource, &hardwareShotBaselineActive);
    effect_mgr.use_effect([=] { return currentScreen == ui_GrindScreen; },
                          [=]() {
                              if (grindVolumetricAvailable && bluetoothScales) {
                                  lv_label_set_text_fmt(ui_GrindScreen_weightLabel, "%.1fg", bluetoothWeight);
                              } else {
                                  lv_label_set_text(ui_GrindScreen_weightLabel, "-");
                              }
                          },
                          &bluetoothWeight, &grindVolumetricAvailable, &bluetoothScales);
    effect_mgr.use_effect(
        [=] { return currentScreen == ui_BrewScreen; },
        [=]() {
            _ui_flag_modify(ui_BrewScreen_adjustments, LV_OBJ_FLAG_HIDDEN, brewScreenState == BrewScreenState::Settings);
            _ui_flag_modify(ui_BrewScreen_acceptButton, LV_OBJ_FLAG_HIDDEN, brewScreenState == BrewScreenState::Settings);
            _ui_flag_modify(ui_BrewScreen_saveButton, LV_OBJ_FLAG_HIDDEN, brewScreenState == BrewScreenState::Settings);
            _ui_flag_modify(ui_BrewScreen_saveAsNewButton, LV_OBJ_FLAG_HIDDEN, brewScreenState == BrewScreenState::Settings);
            _ui_flag_modify(ui_BrewScreen_startButton, LV_OBJ_FLAG_HIDDEN, brewScreenState == BrewScreenState::Brew);
            _ui_flag_modify(ui_BrewScreen_profileInfo, LV_OBJ_FLAG_HIDDEN, brewScreenState == BrewScreenState::Brew);
            _ui_flag_modify(ui_BrewScreen_modeSwitch, LV_OBJ_FLAG_HIDDEN,
                            brewScreenState == BrewScreenState::Brew && (volumetricAvailable || scaleSource > 0));
            {
                const lv_img_dsc_t *icon = (scaleSource == 1)   ? &ui_img_1424216268
                                           : (scaleSource == 2) ? &ui_img_flowmeter_png
                                           : (scaleSource == 3) ? &ui_img_2074354459
                                           : (scaleSource == 4) ? &ui_img_2044104741
                                                                : (bluetoothScales ? &ui_img_1424216268 : &ui_img_flowmeter_png);
                lv_img_set_src(ui_BrewScreen_volumetricButton, icon);
            }
        },
        &brewScreenState, &volumetricAvailable, &bluetoothScales, &scaleSource);
    effect_mgr.use_effect(
        [=] { return currentScreen == ui_BrewScreen; },
        [=]() {
            ui_object_set_themeable_style_property(ui_BrewScreen_saveButton, LV_PART_MAIN | LV_STATE_DEFAULT,
                                                   LV_STYLE_IMG_RECOLOR,
                                                   profileDirty ? _ui_theme_color_NiceWhite : _ui_theme_color_SemiDark);
            ui_object_set_themeable_style_property(ui_BrewScreen_saveButton, LV_PART_MAIN | LV_STATE_DEFAULT,
                                                   LV_STYLE_IMG_RECOLOR_OPA,
                                                   profileDirty ? _ui_theme_alpha_NiceWhite : _ui_theme_alpha_SemiDark);
            ui_object_set_themeable_style_property(ui_BrewScreen_saveAsNewButton, LV_PART_MAIN | LV_STATE_DEFAULT,
                                                   LV_STYLE_IMG_RECOLOR,
                                                   profileDirty ? _ui_theme_color_NiceWhite : _ui_theme_color_SemiDark);
            ui_object_set_themeable_style_property(ui_BrewScreen_saveAsNewButton, LV_PART_MAIN | LV_STATE_DEFAULT,
                                                   LV_STYLE_IMG_RECOLOR_OPA,
                                                   profileDirty ? _ui_theme_alpha_NiceWhite : _ui_theme_alpha_SemiDark);
        },
        &brewScreenState, &profileDirty);
}

void DefaultUI::handleScreenChange() {
    lv_obj_t *current = lv_scr_act();

    if (current != *targetScreen) {
        if (current == ui_BrewScreen) {
            rlBrewPanel = nullptr;
            rlBrewBeanLabel = nullptr;
            rlBrewModeLabel = nullptr;
            rlBrewToggleBtn = nullptr;
            rlBrewToggleLabel = nullptr;
            rlBrewContextBtn = nullptr;
            rlBrewContextLabel = nullptr;
            rlBrewManageBtn = nullptr;
            rlBrewManageLabel = nullptr;
        }
        if (*targetScreen == ui_StandbyScreen) {
            standbyEnterTime = millis();
        } else if (current == ui_StandbyScreen) {
            const Settings &settings = controller->getSettings();
            setBrightness(settings.getMainBrightness());
        }

        _ui_screen_change(targetScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, targetScreenInit);
        lv_obj_del(current);
        rerender = true;
    }
}

void DefaultUI::updateStandbyScreen() {
    if (standbyEnterTime > 0) {
        const Settings &settings = controller->getSettings();
        const unsigned long now = millis();
        if (now - standbyEnterTime >= settings.getStandbyBrightnessTimeout()) {
            setBrightness(settings.getStandbyBrightness());
        }
    }

    if (!apActive && WiFi.status() == WL_CONNECTED && !updateActive && !error && !protocolMismatch && !autotuning &&
        !waitingForController && initialized) {
        time_t now;
        struct tm timeinfo;

        localtime_r(&now, &timeinfo);
        // allocate enough space for both 12h/24h time formats
        if (getLocalTime(&timeinfo, 500)) {
            char time[9];
            Settings &settings = controller->getSettings();
            const char *format = settings.isClock24hFormat() ? "%H:%M" : "%I:%M %p";
            strftime(time, sizeof(time), format, &timeinfo);
            lv_label_set_text(ui_StandbyScreen_time, time);
            lv_obj_clear_flag(ui_StandbyScreen_time, LV_OBJ_FLAG_HIDDEN);

            christmasMode = (timeinfo.tm_mon == 11 && timeinfo.tm_mday < 27) || (timeinfo.tm_mon == 0 && timeinfo.tm_mday < 6);
        }
    } else {
        lv_obj_add_flag(ui_StandbyScreen_time, LV_OBJ_FLAG_HIDDEN);
    }
    controller->getClientController()->isConnected() ? lv_obj_clear_flag(ui_StandbyScreen_bluetoothIcon, LV_OBJ_FLAG_HIDDEN)
                                                     : lv_obj_add_flag(ui_StandbyScreen_bluetoothIcon, LV_OBJ_FLAG_HIDDEN);
    !apActive &&WiFi.status() == WL_CONNECTED ? lv_obj_clear_flag(ui_StandbyScreen_wifiIcon, LV_OBJ_FLAG_HIDDEN)
                                              : lv_obj_add_flag(ui_StandbyScreen_wifiIcon, LV_OBJ_FLAG_HIDDEN);
}

void DefaultUI::updateStatusScreen() const {
    // Copy process pointers to avoid race conditions with controller thread
    Process *process = controller->getProcess();
    Process *lastProcess = controller->getLastProcess();

    if (process == nullptr) {
        process = lastProcess;
    }
    if (process == nullptr || process->getType() != MODE_BREW) {
        return;
    }

    // Additional safety: Validate that the process pointer is still valid
    // by checking if it matches either current or last process
    if (process != controller->getProcess() && process != controller->getLastProcess()) {
        ESP_LOGW("DefaultUI", "Process pointer became invalid during access, skipping update");
        return;
    }

    auto *brewProcess = static_cast<BrewProcess *>(process);
    if (brewProcess == nullptr) {
        ESP_LOGE("DefaultUI", "brewProcess is null after cast");
        return;
    }

    // Validate the brewProcess object before accessing its members
    // Check if the object is in a reasonable state by validating key fields
    if (brewProcess->profile.phases.empty() || brewProcess->phaseIndex >= brewProcess->profile.phases.size()) {
        ESP_LOGE("DefaultUI", "brewProcess phaseIndex out of bounds: %u >= %zu", brewProcess->phaseIndex,
                 brewProcess->profile.phases.size());
        return;
    }

    // Final safety check before accessing brewProcess members
    if (!brewProcess) {
        ESP_LOGE("DefaultUI", "brewProcess became null after validation");
        return;
    }

    const auto phase = brewProcess->currentPhase;

    unsigned long now = millis();
    if (!process->isActive()) {
        // Add bounds check for finished timestamp
        if (brewProcess && brewProcess->finished > 0) {
            now = brewProcess->finished;
        }
    }

    lv_label_set_text(ui_StatusScreen_stepLabel, phase.phase == PhaseType::PHASE_TYPE_BREW ? "BREW" : "INFUSION");
    String phaseText = "Finished";
    if (process->isActive()) {
        phaseText = phase.name;
    } else if (controller->getSettings().isDelayAdjust() && !process->isComplete()) {
        phaseText = "Calibrating...";
    }
    lv_label_set_text(ui_StatusScreen_phaseLabel, phaseText.c_str());

    // Add bounds check for processStarted timestamp
    if (brewProcess && brewProcess->processStarted > 0 && now >= brewProcess->processStarted) {
        const unsigned long processDuration = now - brewProcess->processStarted;
        const double processSecondsDouble = processDuration / 1000.0;
        const auto processMinutes = static_cast<int>(processSecondsDouble / 60.0);
        const auto processSeconds = static_cast<int>(processSecondsDouble) % 60;
        lv_label_set_text_fmt(ui_StatusScreen_currentDuration, "%2d:%02d", processMinutes, processSeconds);
    } else {
        lv_label_set_text_fmt(ui_StatusScreen_currentDuration, "00:00");
    }

    if (brewProcess && brewProcess->target == ProcessTarget::VOLUMETRIC && phase.hasVolumetricTarget()) {
        Target target = phase.getVolumetricTarget();
        lv_bar_set_value(ui_StatusScreen_brewBar, brewProcess->currentVolume * 10.0, LV_ANIM_OFF);
        lv_bar_set_range(ui_StatusScreen_brewBar, 0, target.value * 10.0 + 1.0);
        lv_label_set_text_fmt(ui_StatusScreen_brewLabel, "%.1f / %.1fg", brewProcess->currentVolume, target.value);
    } else if (brewProcess) {
        // Add bounds check for currentPhaseStarted timestamp
        if (brewProcess->currentPhaseStarted > 0 && now >= brewProcess->currentPhaseStarted) {
            const unsigned long progress = now - brewProcess->currentPhaseStarted;
            lv_bar_set_value(ui_StatusScreen_brewBar, progress, LV_ANIM_OFF);
            lv_bar_set_range(ui_StatusScreen_brewBar, 0, std::max(static_cast<int>(brewProcess->getPhaseDuration()), 1));
            lv_label_set_text_fmt(ui_StatusScreen_brewLabel, "%d / %ds", progress / 1000, brewProcess->getPhaseDuration() / 1000);
        } else {
            lv_bar_set_value(ui_StatusScreen_brewBar, 0, LV_ANIM_OFF);
            lv_bar_set_range(ui_StatusScreen_brewBar, 0, 1);
            lv_label_set_text(ui_StatusScreen_brewLabel, "0s");
        }
    }

    if (brewProcess && brewProcess->target == ProcessTarget::TIME) {
        const unsigned long targetDuration = brewProcess->getTotalDuration();
        const double targetSecondsDouble = targetDuration / 1000.0;
        const auto targetMinutes = static_cast<int>(targetSecondsDouble / 60.0);
        const auto targetSeconds = static_cast<int>(targetSecondsDouble) % 60;
        lv_label_set_text_fmt(ui_StatusScreen_targetDuration, "%2d:%02d", targetMinutes, targetSeconds);
    } else if (brewProcess) {
        lv_label_set_text_fmt(ui_StatusScreen_targetDuration, "%.1fg", brewProcess->getBrewVolume());
    }
    if (brewProcess) {
        lv_img_set_src(ui_StatusScreen_Image8,
                       brewProcess->target == ProcessTarget::TIME ? &ui_img_360122106 : &ui_img_1424216268);
    }

    if (brewProcess && brewProcess->isAdvancedPump()) {
        float pressure = brewProcess->getPumpPressure();
        const double percentage = 1.0 - static_cast<double>(pressure) / static_cast<double>(pressureScaling);
        adjustTarget(uic_StatusScreen_dials_pressureTarget, percentage, -62.0, 124.0);
    } else {
        const double percentage = 1.0 - 0.5;
        adjustTarget(uic_StatusScreen_dials_pressureTarget, percentage, -62.0, 124.0);
    }

    // Brew finished adjustments
    if (process->isActive()) {
        lv_obj_add_flag(ui_StatusScreen_brewVolume, LV_OBJ_FLAG_HIDDEN);
    } else {
        // Re-validate brewProcess pointer before accessing members
        if (brewProcess && brewProcess->target == ProcessTarget::VOLUMETRIC) {
            lv_obj_clear_flag(ui_StatusScreen_brewVolume, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(ui_StatusScreen_barContainer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_StatusScreen_labelContainer, LV_OBJ_FLAG_HIDDEN);
        if (brewProcess) {
            lv_label_set_text_fmt(ui_StatusScreen_brewVolume, "%.1lfg", brewProcess->currentVolume);
        }
        lv_imgbtn_set_src(ui_StatusScreen_pauseButton, LV_IMGBTN_STATE_RELEASED, nullptr, &ui_img_631115820, nullptr);
    }
}

void DefaultUI::adjustDials(lv_obj_t *dials) {
    lv_obj_t *tempGauge = ui_comp_get_child(dials, UI_COMP_DIALS_TEMPGAUGE);
    lv_obj_t *tempText = ui_comp_get_child(dials, UI_COMP_DIALS_TEMPTEXT);
    lv_obj_t *pressureTarget = ui_comp_get_child(dials, UI_COMP_DIALS_PRESSURETARGET);
    lv_obj_t *pressureGauge = ui_comp_get_child(dials, UI_COMP_DIALS_PRESSUREGAUGE);
    lv_obj_t *pressureText = ui_comp_get_child(dials, UI_COMP_DIALS_PRESSURETEXT);
    lv_obj_t *pressureSymbol = ui_comp_get_child(dials, UI_COMP_DIALS_IMAGE6);
    _ui_flag_modify(pressureTarget, LV_OBJ_FLAG_HIDDEN, pressureAvailable);
    _ui_flag_modify(pressureGauge, LV_OBJ_FLAG_HIDDEN, pressureAvailable);
    _ui_flag_modify(pressureText, LV_OBJ_FLAG_HIDDEN, pressureAvailable);
    _ui_flag_modify(pressureSymbol, LV_OBJ_FLAG_HIDDEN, pressureAvailable);
    lv_obj_set_x(tempText, pressureAvailable ? -50 : 0);
    lv_obj_set_y(tempText, pressureAvailable ? -205 : -180);
    lv_arc_set_bg_angles(tempGauge, 118, pressureAvailable ? 242 : 62);
    lv_arc_set_range(pressureGauge, 0, pressureScaling * 10);
}

inline void DefaultUI::adjustTempTarget(lv_obj_t *dials) {
    double gaugeAngle = pressureAvailable ? 124.0 : 304;
    double gaugeStart = pressureAvailable ? 118.0 : -62;
    double percentage = static_cast<double>(targetTemp) / 160.0;
    lv_obj_t *tempTarget = ui_comp_get_child(dials, UI_COMP_DIALS_TEMPTARGET);
    adjustTarget(tempTarget, percentage, gaugeStart, gaugeAngle);
}

void DefaultUI::applyTheme() {
    const Settings &settings = controller->getSettings();
    int newThemeMode = settings.getThemeMode();

    if (newThemeMode != currentThemeMode) {
        currentThemeMode = newThemeMode;
        ui_theme_set(currentThemeMode);

        if (AmoledDisplayDriver::getInstance() == panelDriver && currentThemeMode == UI_THEME_DEFAULT) {
            enable_amoled_black_theme_override(lv_disp_get_default());
        }
    }
}

void DefaultUI::adjustTarget(lv_obj_t *obj, double percentage, double start, double range) const {
    double angle = start + range - range * percentage;

    lv_img_set_angle(obj, angle * -10);
    int x = static_cast<int>(std::cos(angle * M_PI / 180.0f) * 235.0);
    int y = static_cast<int>(std::sin(angle * M_PI / 180.0f) * -235.0);
    lv_obj_set_pos(obj, x, y);
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
