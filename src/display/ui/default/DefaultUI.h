#ifndef DEFAULTUI_H
#define DEFAULTUI_H

#include <display/core/PluginManager.h>
#include <display/core/ProfileManager.h>
#include <display/core/constants.h>
#include <display/drivers/Driver.h>
#include <display/models/profile.h>

#include "./lvgl/ui.h"

class Controller;

constexpr int RERENDER_INTERVAL_IDLE = 2500;
constexpr int RERENDER_INTERVAL_ACTIVE = 100;

constexpr int TEMP_HISTORY_INTERVAL = 250;
constexpr int TEMP_HISTORY_LENGTH = 20 * 1000 / TEMP_HISTORY_INTERVAL;

int16_t calculate_angle(int set_temp, int range, int offset);

enum class BrewScreenState { Brew, Settings };

class DefaultUI {
  public:
    DefaultUI(Controller *controller, Driver *driver, PluginManager *pluginManager);

    // Default work methods
    void init();
    void loop();
    void loopProfiles();

    // Interface methods
    void changeScreen(lv_obj_t **screen, void (*target_init)(void));

    void changeBrewScreenMode(BrewScreenState state);
    void onProfileSwitch();
    void onNextProfile();
    void onPreviousProfile();
    void onProfileSelect();
    void showRLAutoTuningOverlay();
    void showRLContextPickerOverlay();
    void toggleRLLocalOptimization();
    void toggleRLOptimizationPaused();
    void startRLNewBean();
    void startRLNewBag();
    void resetRLDialIn();
    void retireRLContext();
    void retireRLContext(const String &contextId);
    void switchRLContext(const String &contextId);
    void setBrightness(int brightness) {
        if (panelDriver) {
            panelDriver->setBrightness(brightness);
        }
    };

    void onVolumetricDelete();

    void markDirty() { rerender = true; }
    void markProfileDirty() { profileDirty = true; }
    void markProfileClean() { profileDirty = false; }

    void applyTheme();

    bool isTaskHealthy() const {
        return is_task_healthy(eTaskGetState(taskHandle)) && is_task_healthy(eTaskGetState(profileTaskHandle));
    }

  private:
    void setupPanel();
    void setupState();
    void setupReactive();

    void handleScreenChange();

    void updateStandbyScreen();
    void updateStatusScreen() const;
    void updateRLAutoTuningWidgets();

    void adjustDials(lv_obj_t *dials);
    void adjustTempTarget(lv_obj_t *dials);
    void adjustTarget(lv_obj_t *obj, double percentage, double start, double range) const;

    int tempHistory[TEMP_HISTORY_LENGTH] = {0};
    int tempHistoryIndex = 0;
    int prevTargetTemp = 0;
    bool isTempHistoryInitialized = false;
    int isTemperatureStable = false;
    unsigned long lastTempLog = 0;

    void updateTempHistory();
    void updateTempStableFlag();
    void adjustHeatingIndicator(lv_obj_t *contentPanel);
    void reloadProfiles();

    Driver *panelDriver = nullptr;
    Controller *controller;
    PluginManager *pluginManager;
    ProfileManager *profileManager;

    // Screen state
    String selectedProfileId = "";
    Profile selectedProfile{};
    int updateAvailable = false;
    int updateActive = false;
    int apActive = false;
    int error = false;
    int protocolMismatch = false;
    int autotuning = false;
    int waitingForController = false;
    int volumetricAvailable = false;
    int grindVolumetricAvailable = false;
    int bluetoothScales = false;
    int hardwareScalePresent = false;
    int hardwareShotBaselineActive = false;
    int scaleSource = 0;
    int brewVolumetricSource = 0;
    int grindVolumetricTarget = false;
    int volumetricMode = false;
    int brewVolumetric = false;
    int profileVolumetric = false;
    int grindActive = false;
    int active = false;
    int smartGrindActive = false;
    int grindAvailable = false;
    int initialized = false;
    int rlAutoTuningEnabled = false;
    int rlLocalOptimizationEnabled = false;
    int rlOptimizationPaused = false;
    String rlMode = "";
    int rlLocalShotCount = 0;
    int rlRatedShotCount = 0;
    int rlCommunityUploadEnabled = false;
    String rlBestKnownRecipe = "";
    lv_obj_t *rlBrewPanel = nullptr;
    lv_obj_t *rlBrewBeanLabel = nullptr;
    lv_obj_t *rlBrewModeLabel = nullptr;
    lv_obj_t *rlBrewToggleBtn = nullptr;
    lv_obj_t *rlBrewToggleLabel = nullptr;
    lv_obj_t *rlBrewContextBtn = nullptr;
    lv_obj_t *rlBrewContextLabel = nullptr;
    lv_obj_t *rlBrewManageBtn = nullptr;
    lv_obj_t *rlBrewManageLabel = nullptr;
    lv_obj_t *rlOverlay = nullptr;

    // Seasonal flags
    int christmasMode = false;

    bool rerender = false;
    unsigned long lastRender = 0;

    int mode = MODE_STANDBY;
    int currentTemp = 0;
    int targetTemp = 0;
    float targetDuration = 0;
    float targetVolume = 0;
    int grindDuration = 0;
    float grindVolume = 0.0f;
    int pressureAvailable = 0;
    float pressure = 0.0f;
    int pressureScaling = DEFAULT_PRESSURE_SCALING;
    int heatingFlash = 0;
    double bluetoothWeight = 0.0;
    double hardwareWeight = 0.0;
    double hardwareShotWeight = 0.0;
    double estimatedWeight = 0.0;
    BrewScreenState brewScreenState = BrewScreenState::Brew;

    int profileDirty = 0;
    int currentProfileIdx;
    int profileLoaded = 0;
    std::vector<String> favoritedProfileIds;
    std::vector<Profile> favoritedProfiles;
    int currentThemeMode = -1; // Force applyTheme on first loop

    // Screen change
    lv_obj_t **targetScreen = &ui_StandbyScreen;
    lv_obj_t *currentScreen = ui_StandbyScreen;
    void (*targetScreenInit)(void) = &ui_StandbyScreen_screen_init;

    // Standby brightness control
    unsigned long standbyEnterTime = 0;

    xTaskHandle taskHandle;
    static void loopTask(void *arg);
    xTaskHandle profileTaskHandle;
    static void profileLoopTask(void *arg);
};

#endif // DEFAULTUI_H
