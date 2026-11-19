#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <cassert>
#include <ctime>
#include <display/core/constants.h>
#include <display/plugins/WifiStaWatchdogPlugin.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <functional>
#include <map>
#include <string>

WiFiClass WiFi;
static unsigned long clockMs = 0;
static unsigned long ipAtMs = 0;
static int reassociations = 0;
static constexpr char LOG_TAG[] = "test";
static constexpr char WIFI_AP_SSID[] = "Setup";
static constexpr char NTP_SERVER[] = "pool.ntp.org";
static const IPAddress WIFI_AP_IP(4, 4, 4, 1);
static const IPAddress WIFI_SUBNET_MASK(255, 255, 255, 0);

extern "C" unsigned long millis() { return clockMs; }
extern "C" void delay(uint32_t duration) {
    clockMs += duration;
    if (ipAtMs != 0 && clockMs >= ipAtMs) {
        ipAtMs = 0;
        WiFi.simulateStationGotIp();
    }
}
static String generateShortID(int) { return "test-hotspot"; }
static const char *resolve_timezone(const String &) { return "UTC0"; }
static void configTzTime(const char *, const char *) {}
#if defined(_WIN32)
static int setenv(const char *name, const char *value, int) { return _putenv_s(name, value); }
#endif

struct Event {
    int ap = 0;
};

struct PluginManager {
    std::map<std::string, std::function<void(Event const &)>> handlers;
    std::vector<int> connections;
    int disconnections = 0;
    void on(const char *name, std::function<void(Event const &)> handler) { handlers[name] = std::move(handler); }
    void trigger(const char *name, const char * = nullptr, int ap = 0) {
        const std::string id(name);
        if (id == "controller:wifi:connect") connections.push_back(ap);
        if (id == "controller:wifi:disconnect") ++disconnections;
        const auto handler = handlers.find(id);
        if (handler != handlers.end()) handler->second(Event{ap});
    }
};

struct Settings {
    String ssid = "Home";
    String apPassword;
    String getWifiSsid() const { return ssid; }
    String getWifiPassword() const { return "test-password"; }
    String getMdnsName() const { return "test"; }
    String getTimezone() const { return "UTC"; }
    const String &getWifiApPassword() const { return apPassword; }
    void setWifiApPassword(const String &password) { apPassword = password; }
};

class Controller {
  public:
    Settings settings;
    PluginManager manager;
    PluginManager *pluginManager = &manager;
    bool isApConnection = false;
    bool wifiTimeConfigured = false;
    std::atomic<bool> wifiConnectedPending{false};
    std::atomic<bool> wifiDisconnectedPending{false};

    Settings &getSettings() { return settings; }
    void setupWifi();
    void handleWifiEvents();
};

void WifiStaWatchdogPlugin::forceReassoc() { ++reassociations; }

static void resetWifi() {
    WiFi = WiFiClass{};
    clockMs = 0;
    ipAtMs = 0;
    reassociations = 0;
    WiFi.deferStationIp();
}

int main() {
    resetWifi();
    Controller normal;
    ipAtMs = 13000;
    normal.setupWifi();
    normal.handleWifiEvents();
    assert(clockMs == 13000 && WiFi.getMode() == WIFI_MODE_STA);
    assert(normal.manager.connections == std::vector<int>{0});
    assert(normal.wifiTimeConfigured);

    resetWifi();
    Controller delayed;
    WifiStaWatchdogPlugin watchdog;
    watchdog.setup(&delayed, &delayed.manager);
    delayed.manager.trigger("controller:startup");
    delayed.setupWifi();
    assert(clockMs == WIFI_STARTUP_WAIT_MS);
    assert(WiFi.getMode() == WIFI_MODE_APSTA && WiFi.status() == WL_IDLE_STATUS);
    assert(delayed.manager.connections == std::vector<int>{1});

    clockMs = 19999;
    watchdog.loop();
    assert(reassociations == 0);
    clockMs = 20000;
    watchdog.loop();
    assert(reassociations == 1);
    clockMs = 49999;
    watchdog.loop();
    assert(reassociations == 1);

    delayed.wifiDisconnectedPending = true;
    delayed.handleWifiEvents();
    assert(delayed.manager.disconnections == 0);
    WiFi.simulateStationGotIp();
    delayed.handleWifiEvents();
    assert(WiFi.getMode() == WIFI_MODE_STA && !delayed.isApConnection && delayed.wifiTimeConfigured);
    assert((delayed.manager.connections == std::vector<int>{1, 0}));
    assert(delayed.manager.disconnections == 0);

    resetWifi();
    Controller unconfigured;
    unconfigured.settings.ssid = "";
    WifiStaWatchdogPlugin unconfiguredWatchdog;
    unconfiguredWatchdog.setup(&unconfigured, &unconfigured.manager);
    unconfigured.setupWifi();
    clockMs = 60000;
    unconfiguredWatchdog.loop();
    assert(unconfigured.manager.connections == std::vector<int>{1});
    assert(reassociations == 0 && WiFi.getMode() == WIFI_MODE_APSTA);
    puts("PASS Wi-Fi startup: slow DHCP, concurrent setup hotspot, retry pacing, LAN recovery and unconfigured setup");
}
