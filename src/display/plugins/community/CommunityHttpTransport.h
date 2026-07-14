#ifndef COMMUNITYHTTPTRANSPORT_H
#define COMMUNITYHTTPTRANSPORT_H

#include <Arduino.h>
#include <cstdint>

class CommunityHttpTransport {
  public:
    struct Result {
        bool started = false;
        int statusCode = 0;
        String response;
        String error;
    };

    struct UploadRequest {
        String url;
        String installId;
        String tokenId;
        String uploadId;
        String uploadSecret;
        String payloadJson;
        std::int64_t timestamp = 0;
    };

    static String sha256Hex(const String &value);
    Result registerDevice(const String &url) const;
    Result upload(UploadRequest const &request) const;
};

#endif // COMMUNITYHTTPTRANSPORT_H
