#include "CommunityHttpTransport.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

extern const uint8_t x509_crt_imported_bundle_bin_start[] asm("_binary_x509_crt_bundle_start");

namespace {

static bool beginRequest(HTTPClient &http, WiFiClientSecure &client, const String &url) {
    client.setCACertBundle(x509_crt_imported_bundle_bin_start);
    http.setTimeout(15000);
    http.setReuse(false);
    return http.begin(client, url);
}

static String hexBytes(const uint8_t *bytes, size_t length) {
    static constexpr char HEX_CHARS[] = "0123456789abcdef";
    String output;
    output.reserve(length * 2);
    for (size_t index = 0; index < length; ++index) {
        output += HEX_CHARS[(bytes[index] >> 4) & 0x0F];
        output += HEX_CHARS[bytes[index] & 0x0F];
    }
    return output;
}

static String hmacSha256Hex(const String &secret, const String &value) {
    uint8_t digest[32]{};
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) {
        return "";
    }
    const int result = mbedtls_md_hmac(info, reinterpret_cast<const unsigned char *>(secret.c_str()), secret.length(),
                                       reinterpret_cast<const unsigned char *>(value.c_str()), value.length(), digest);
    return result == 0 ? hexBytes(digest, sizeof(digest)) : String();
}

} // namespace

String CommunityHttpTransport::sha256Hex(const String &value) {
    uint8_t digest[32]{};
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    mbedtls_sha256_starts_ret(&context, 0);
    mbedtls_sha256_update_ret(&context, reinterpret_cast<const unsigned char *>(value.c_str()), value.length());
    mbedtls_sha256_finish_ret(&context, digest);
    mbedtls_sha256_free(&context);
    return hexBytes(digest, sizeof(digest));
}

CommunityHttpTransport::Result CommunityHttpTransport::registerDevice(const String &url) const {
    Result result;
    WiFiClientSecure client;
    HTTPClient http;
    if (!beginRequest(http, client, url)) {
        result.error = "registration connection failed";
        return result;
    }

    result.started = true;
    http.addHeader("Content-Type", "application/json");
    result.statusCode = http.POST("{\"action\":\"register\"}");
    result.response = http.getString();
    http.end();
    return result;
}

CommunityHttpTransport::Result CommunityHttpTransport::upload(UploadRequest const &request) const {
    Result result;
    const String timestamp = String(static_cast<long long>(request.timestamp));
    const String payloadHash = sha256Hex(request.payloadJson);
    const String signature = hmacSha256Hex(request.uploadSecret, timestamp + "." + request.payloadJson);
    if (payloadHash.length() != 64 || signature.length() != 64) {
        result.error = "upload signature failed";
        return result;
    }

    WiFiClientSecure client;
    HTTPClient http;
    if (!beginRequest(http, client, request.url)) {
        result.error = "upload connection failed";
        return result;
    }

    result.started = true;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-EspressoRL-Install-ID", request.installId);
    http.addHeader("X-EspressoRL-Token-ID", request.tokenId);
    http.addHeader("X-EspressoRL-Upload-ID", request.uploadId);
    http.addHeader("X-EspressoRL-Timestamp", timestamp);
    http.addHeader("X-EspressoRL-Signature", signature);
    http.addHeader("X-EspressoRL-Payload-Hash", payloadHash);
    result.statusCode = http.POST(request.payloadJson);
    result.response = http.getString();
    http.end();
    return result;
}
