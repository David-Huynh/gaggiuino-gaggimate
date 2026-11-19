#include "WebSocketHandler.h"
#include <display/util/PsramWsBuffer.h>

namespace {
AsyncWebSocketSharedBuffer encode(JsonDocument &doc) {
    const size_t size = measureJson(doc);
    auto buffer = makePsramWsBuffer(size);
    serializeJson(doc, buffer->data(), size);
    return buffer;
}
}

void WebSocketHandler::sendJson(uint32_t clientId, JsonDocument &doc) {
    socket.text(clientId, encode(doc));
}

void WebSocketHandler::broadcastJson(JsonDocument &doc) {
    if (!socket.getClients().empty()) socket.textAll(encode(doc));
}
