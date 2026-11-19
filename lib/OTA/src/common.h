#ifndef GITHUBOTA_COMMON_H
#define GITHUBOTA_COMMON_H

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

String get_redirect_location(WiFiClientSecure &client, const String &url, int &status);
String get_updated_version_via_txt_file(WiFiClientSecure &client, const String &release_url);

#endif
