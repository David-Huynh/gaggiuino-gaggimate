#ifndef DEFAULTUI_H
#define DEFAULTUI_H

#include <atomic>
#include <display/core/AutoTuningModels.h>
#include <display/core/PluginManager.h>
#include <display/core/ProfileManager.h>
#include <display/core/constants.h>
#include <display/drivers/Driver.h>
#include <display/models/profile.h>
#include <display/ui/default/eez/screens.h>
#include <display/ui/default/eez/structs.h>
#include <mutex>

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
    void changeScreen(ScreensEnum screen);

    void changeBrewScreenMode(BrewScreenState state);
    void onProfileSwitch();
    void onNextProfile();
    void onPreviousProfile();
    void onProfileSelect();
    void setBrightness(int brightness) {
        if (panelDriver) {
            panelDriver->setBrightness(brightness);
        }
    };

    void onVolumetricDelete();
    void showAutoTuningSettings();
    void closeAutoTuningSettings();
    void setAutoTuningEnabled(bool enabled);
    void setAutoTuningProviderFromIndex(uint16_t index);
    void setAutoTuningLocalOptimization(bool enabled);
    void setAutoTuningCommunityUpload(bool enabled);
    void setAutoTuningDoseTarget(float doseG);
    void showRecipeDomainSettings();
    void closeRecipeDomainSettings();
    void saveRecipeDomainSettings();
    void showTasteGoalSettings();
    void closeTasteGoalSettings();
    void setTasteGoalModeFromIndex(uint16_t index);
    void cycleTasteGoalLevel(lv_obj_t *button);
    void saveTasteGoalSettings();

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

    void handleScreenChange();

    // Animate the dial meters' tick length on screen change (short on profile/new-menu, long elsewhere).
    void animateGaugeTicks(ScreensEnum from, ScreensEnum to);
    void collectMeters(lv_obj_t *obj);
    void setGaugeTickLength(int32_t len);
    static void gaugeTickAnimCb(void *var, int32_t v);
    lv_obj_t *gaugeMeters[4] = {nullptr};
    uint8_t gaugeCount = 0;
    void positionMenuIcon(lv_obj_t *obj, int angle, int radius);

    void updateState();
    void updateSystemStatus();
    void updateProfileInfo();
    void updateBoiler();
    void updateBrewProcess();
    void updateMenuScreen();
    void updateAutoTuningSettingsModal();
    void updateTasteGoalSettingsModal();
    String getErrorMessage();

    void adjustDials(lv_obj_t *dials);
    void adjustTarget(lv_obj_t *obj, double percentage, double start, double range) const;

    int tempHistory[TEMP_HISTORY_LENGTH] = {0};
    int tempHistoryIndex = 0;
    int prevTargetTemp = 0;
    bool isTempHistoryInitialized = false;
    int isTemperatureStable = false;
    unsigned long lastTempLog = 0;

    void updateTempHistory();
    void updateTempStableFlag();
    void reloadProfiles();

    Driver *panelDriver = nullptr;
    Controller *controller;
    PluginManager *pluginManager;
    ProfileManager *profileManager;

    // Screen state
    int updateAvailable = false;
    int apActive = false;
    int wifiConnected = false;
    int waitingForController = false;
    int volumetricAvailable = false;
    int grindVolumetricAvailable = false;
    int bluetoothScales = false;
    int grindVolumetricTarget = false;
    int volumetricMode = false;
    int brewVolumetric = false;
    int profileVolumetric = false;
    int grindActive = false;
    int active = false;
    int smartGrindActive = false;
    int grindAvailable = false;
    int initialized = false;

    // Seasonal flags
    int christmasMode = false;

    bool rerender = false;
    unsigned long lastRender = 0;

    int mode = MODE_STANDBY;
    bool pressureAvailable = false;
    int heatingFlash = 0;
    float pressure = 0.0f;
    float currentTemp = 0.0f;
    float targetTemp = 0.0f;
    double bluetoothWeight = 0.0;
    double estimatedWeight = 0.0;
    BrewScreenState brewScreenState = BrewScreenState::Brew;

    // EEZ Structs
    SystemStatusValue systemStatus;
    ProfileInfoValue selectedProfileInfo;
    ProfileInfoValue previewProfileInfo;
    BoilerValue boiler;
    UIFlagsValue uiFlags;
    BrewProcessValue brewProcess;
    Value currentWeight = FloatValue(0.0);
    Value steamReady = BooleanValue(false);
    Value grindWeightTarget = FloatValue(18.0);
    Value grindTimeTarget = StringValue("0:15");

    lv_obj_t *autoTuningModal = nullptr;
    lv_obj_t *autoTuningEnabledSwitch = nullptr;
    lv_obj_t *autoTuningProviderDropdown = nullptr;
    lv_obj_t *autoTuningLocalSwitch = nullptr;
    lv_obj_t *autoTuningCommunitySwitch = nullptr;
    lv_obj_t *autoTuningDoseTarget = nullptr;
    lv_obj_t *autoTuningTasteGoalButton = nullptr;
    lv_obj_t *autoTuningTasteGoalButtonLabel = nullptr;
    lv_obj_t *autoTuningAdvancedButton = nullptr;
    lv_obj_t *autoTuningStatusLabel = nullptr;
    lv_obj_t *autoTuningSummaryLabel = nullptr;
    lv_obj_t *recipeDomainModal = nullptr;
    lv_obj_t *recipeDomainGrindRadius = nullptr;
    lv_obj_t *recipeDomainDoseMin = nullptr;
    lv_obj_t *recipeDomainDoseMax = nullptr;
    lv_obj_t *recipeDomainOutputMin = nullptr;
    lv_obj_t *recipeDomainOutputMax = nullptr;
    lv_obj_t *recipeDomainStatusLabel = nullptr;
    lv_obj_t *tasteGoalModal = nullptr;
    lv_obj_t *tasteGoalModeDropdown = nullptr;
    lv_obj_t *tasteGoalStatusLabel = nullptr;
    lv_obj_t *tasteGoalLevelButtons[AutoTuning::TASTE_GOAL_ATTRIBUTE_COUNT] = {};
    uint8_t tasteGoalLevels[AutoTuning::TASTE_GOAL_ATTRIBUTE_COUNT] = {};
    bool tasteGoalCustom = false;

    int profileDirty = 0;
    int currentProfileIdx = 0;
    std::atomic<int> profileLoaded{0}; // cleared from event callbacks on arbitrary tasks
    // The profile task (core 0) rebuilds these while the UI task reads them (GM-147).
    std::mutex profilesMutex;
    std::vector<String> favoritedProfileIds;
    std::vector<Profile> favoritedProfiles;
    int currentThemeMode = -1; // Force applyTheme on first loop

    // Screen change
    ScreensEnum targetScreen = ScreensEnum::SCREEN_ID_STANDBY_SCREEN;
    ScreensEnum currentScreen = ScreensEnum::SCREEN_ID_STANDBY_SCREEN;

    // Standby brightness control
    unsigned long standbyEnterTime = 0;

    xTaskHandle taskHandle;
    static void loopTask(void *arg);
    xTaskHandle profileTaskHandle;
    static void profileLoopTask(void *arg);
};

#endif // DEFAULTUI_H
