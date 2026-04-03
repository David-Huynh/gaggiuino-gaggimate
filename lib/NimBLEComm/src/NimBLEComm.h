#ifndef NIMBLECOMM_H
#define NIMBLECOMM_H

#include "CommonTypes.h"
#include <Arduino.h>
#include <NimBLEDevice.h>

// UUIDs for BLE services and characteristics
#define SERVICE_UUID "e75bc5b6-ff6e-4337-9d31-0c128f2e6e68"
#define ALT_CONTROL_CHAR_UUID "cca5a577-ec67-4499-8ccb-654f1312db1d"
#define PING_CHAR_UUID "9731755e-29ce-41a8-91d9-7a244f49859b"
#define ERROR_CHAR_UUID "d6676ec7-820c-41de-820d-95620749003b"
#define AUTOTUNE_CHAR_UUID "d54df381-69b6-4531-b1cc-dde7766bbaf4"
#define AUTOTUNE_RESULT_UUID "7f61607a-2817-4354-9b94-d49c057fc879"
#define PID_CONTROL_CHAR_UUID "d448c469-3e1d-4105-b5b8-75bf7d492fad"
#define PUMP_MODEL_COEFFS_CHAR_UUID "e448c469-3e1d-4105-b5b8-75bf7d492fae"
#define BREW_BTN_UUID "a29eb137-b33e-45a4-b1fc-15eb04e8ab39"
#define STEAM_BTN_UUID "53750675-4839-421e-971e-cc6823507d8e"
#define INFO_UUID "f8d7203b-e00c-48e2-83ba-37ff49cdba74"

#define PRESSURE_SCALE_UUID "3aa65ab6-2dda-4c95-9cf3-58b2a0480623"
#define SENSOR_DATA_UUID "62b69e72-ac19-4d4b-bd53-2edd65330c93"
#define OUTPUT_CONTROL_UUID "77fbb08f-c29c-4f2e-8e1d-ed0a9afa5e1a"
#define VOLUMETRIC_MEASUREMENT_UUID "b0080557-3865-4a9c-be37-492d77ee5951"
#define VOLUMETRIC_TARE_UUID "a8bd52e0-77c3-412c-847c-4e802c3982f9"
#define TOF_MEASUREMENT_UUID "7282c525-21a0-416a-880d-21fe98602533"
#define WEIGHT_MEASUREMENT_UUID "9e7c9e60-1c7a-4c3e-b2f1-8a3d5e7f1234"
#define SCALE_TARE_UUID "9e7c9e61-1c7a-4c3e-b2f1-8a3d5e7f1234"
#define SCALE_CALIBRATION_UUID "9e7c9e62-1c7a-4c3e-b2f1-8a3d5e7f1234"
#define SCALE_OFFSETS_UUID "9e7c9e63-1c7a-4c3e-b2f1-8a3d5e7f1234"
#define SCALE_CAL_START_UUID "9e7c9e64-1c7a-4c3e-b2f1-8a3d5e7f1234"
#define SCALE_CAL_RESULT_UUID "9e7c9e65-1c7a-4c3e-b2f1-8a3d5e7f1234"
#define LED_CONTROL_UUID "37804a2b-49ab-4500-8582-db4279fc8573"

String get_token(const String &from, uint8_t index, char separator, String default_value = "");

#endif // NIMBLECOMM_H
