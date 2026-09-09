#pragma once
#include <display/core/Plugin.h>
#include <display/plugins/community/CommunityUploadQueue.h>

// Migration adapter only. New records use the completed-shot MQTT path.
class CommunityUploadPlugin : public Plugin {
public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;
private:
    void relayPending();
    static void worker(void *argument);
    Controller *controller = nullptr;
    CommunityUploadQueue backlog;
};
