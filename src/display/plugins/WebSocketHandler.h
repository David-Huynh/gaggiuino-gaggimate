#pragma once

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <functional>
#include <map>
#include <vector>

// Plugin hooks on the WebUI's existing socket, which owns request framing and dispatch.
class WebSocketHandler {
  public:
    using ClientRequestHandler = std::function<void(uint32_t clientId, JsonDocument &request)>;
    using StatusProvider = std::function<void(JsonDocument &status)>;

    explicit WebSocketHandler(AsyncWebSocket &socket) : socket(socket) {}

    void registerRequestHandler(const String &type, ClientRequestHandler handler) { handlers[type] = std::move(handler); }
    void registerStatusProvider(StatusProvider provider) { statusProviders.push_back(std::move(provider)); }
    void sendJson(uint32_t clientId, JsonDocument &doc);
    void broadcastJson(JsonDocument &doc);

    void dispatch(const String &type, uint32_t clientId, JsonDocument &request) {
        auto handler = handlers.find(type);
        if (handler != handlers.end()) handler->second(clientId, request);
    }

    void addStatus(JsonDocument &doc) {
        for (auto &provider : statusProviders) provider(doc);
    }

  private:
    AsyncWebSocket &socket;
    std::map<String, ClientRequestHandler> handlers;
    std::vector<StatusProvider> statusProviders;
};
