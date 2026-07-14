#ifndef COMMUNITYPAYLOADVALIDATOR_H
#define COMMUNITYPAYLOADVALIDATOR_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstddef>

namespace CommunityPayloadValidator {

inline constexpr size_t MAX_PAYLOAD_BYTES = 256 * 1024;

bool validateRecord(JsonObjectConst payload, const String &recordType, const String &recordId, String &reason);
bool serializeValidated(JsonObjectConst payload, const String &recordType, const String &recordId, String &payloadJson,
                        String &reason);

} // namespace CommunityPayloadValidator

#endif // COMMUNITYPAYLOADVALIDATOR_H
