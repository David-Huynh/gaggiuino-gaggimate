#ifndef PSRAMWSBUFFER_H
#define PSRAMWSBUFFER_H

#include <ESPAsyncWebServer.h>
#include <esp_heap_caps.h>

// Host simulator has no sdkconfig; value matches the arduino-esp32 default.
#ifndef CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
#define CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 4096
#endif

inline AsyncWebSocketSharedBuffer makePsramWsBuffer(size_t size) {
#if defined(GAGGIMATE_N16R8)
    // The N16R8 sets its PSRAM preference once during display setup.
    return std::make_shared<std::vector<uint8_t>>(size);
#else
    // AsyncWebSocket's vector has no allocator hook. Prefer PSRAM while
    // constructing it on displays that use the SDK's default heap preference.
    heap_caps_malloc_extmem_enable(64);
    auto buffer = std::make_shared<std::vector<uint8_t>>(size);
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    return buffer;
#endif
}

#endif // PSRAMWSBUFFER_H
