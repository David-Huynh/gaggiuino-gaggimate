#include "common.h"
#include "logging.h"

namespace {
constexpr uint16_t OTA_HTTP_TIMEOUT_MS = 5000;

void configureRequestTimeouts(HTTPClient &client) {
    client.setConnectTimeout(OTA_HTTP_TIMEOUT_MS);
    client.setTimeout(OTA_HTTP_TIMEOUT_MS);
}
} // namespace

String get_redirect_location(WiFiClientSecure &client, const String &url, int &status) {
    HTTPClient https;
    https.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    configureRequestTimeouts(https);
    if (!https.begin(client, url)) {
        status = 0;
        return "";
    }
    status = https.GET();
    const String location = status == HTTP_CODE_FOUND ? https.getLocation() : String();
    https.end();
    return location;
}

String get_updated_version_via_txt_file(WiFiClientSecure &client, const String &release_url) {
    HTTPClient https;
    https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    configureRequestTimeouts(https);
    if (!https.begin(client, release_url + "version.txt"))
        return "";

    const int status = https.GET();
    String version;
    if (status == HTTP_CODE_OK && https.getSize() > 0 && https.getSize() <= 128) {
        version = https.getString();
        version.trim();
    } else {
        ESP_LOGW("OTA", "Could not read release version (HTTP %d)", status);
    }
    https.end();
    return version;
}
