#ifndef COMMUNITYUPLOADPLUGIN_H
#define COMMUNITYUPLOADPLUGIN_H

#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <display/core/AutoTuningPorts.h>
#include <display/core/Event.h>
#include <display/core/Plugin.h>
#include <display/plugins/community/CommunityHttpTransport.h>
#include <display/plugins/community/CommunityUploadQueue.h>

class CommunityUploadPlugin : public Plugin, public AutoTuning::CommunityUploadPort {
  public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;
    bool enqueueShot(AutoTuning::ShotRecord const &shot) override;
    bool enqueueRecommendation(AutoTuning::Recommendation const &recommendation) override;
    bool enqueuePreference(AutoTuning::PreferenceFeedback const &preference) override;
    bool applyCorrection(AutoTuning::ShotCorrection const &correction) override;

  private:
    struct UploadConfiguration {
        bool requested = false;
        String baseUrl;
        String installId;
        String tokenId;
        String secret;

        bool configured() const {
            return !baseUrl.isEmpty() && !installId.isEmpty() && !tokenId.isEmpty() && secret.length() >= 32;
        }
    };

    void publishStatus();
    void handlePreference(Event const &event);
    void handleShotCorrection(Event const &event);
    bool uploadRequested() const;
    bool uploadConfigured() const;
    String uploadBaseUrl() const;
    UploadConfiguration configurationSnapshot() const;
    void refreshConfiguration();
    void resetWorkerAttempts();
    String registrationUrl() const;
    String machineTopicId() const;
    String machineId() const;
    void requestStatusPublish();
    void setLastError(const String &error);
    String getLastError() const;
    void incrementRejected();
    int rejectedCount() const;
    void maybeRegister();
    void maybeUpload();
    bool registerDevice();
    bool uploadOne();
    bool enqueueValidatedPayload(const String &recordType, const String &recordId, const String &payloadJson,
                                 uint32_t delaySeconds, bool replaceRecord);
    bool buildShotPayload(AutoTuning::ShotRecord const &shot, String &shotId, String &payloadJson);
    bool buildRecommendationPayload(AutoTuning::Recommendation const &recommendation, String &recommendationId,
                                    String &payloadJson);
    bool buildComparisonPayload(AutoTuning::PreferenceFeedback const &preference, String &comparisonId, String &payloadJson);
    void discardMismatchedQueueItems();
    [[noreturn]] static void workerTask(void *arg);

    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;
    TaskHandle_t workerTaskHandle = nullptr;
    CommunityHttpTransport httpTransport;
    CommunityUploadQueue uploadQueue;
    mutable SemaphoreHandle_t stateMutex = nullptr;
    UploadConfiguration uploadConfiguration;
    unsigned long lastStatusMs = 0;
    unsigned long lastRegisterAttemptMs = 0;
    unsigned long lastUploadAttemptMs = 0;
    bool statusPublishRequested = false;
    bool registrationReadyPending = false;
    bool credentialUpdatePending = false;
    bool credentialClearPending = false;
    String pendingInstallId;
    String pendingTokenId;
    String pendingSecret;
    int rejectedSinceBoot = 0;
    String lastError = "";
};

#endif // COMMUNITYUPLOADPLUGIN_H
