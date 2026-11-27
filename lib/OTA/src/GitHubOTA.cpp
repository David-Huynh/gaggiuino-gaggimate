#include "GitHubOTA.h"
#include "EspHttpTransport.h"
#include "ResumableDownloader.h"
#include "ca_bundle.h"
#include "common.h"
#include "logging.h"
#include "semver_extensions.h"
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <sdkconfig.h>

// Flash erases park core 0 and slow every other task, so IDLE0 and the AsyncTCP service task (both watched by
// the 5 s task watchdog) can miss their deadline mid-update (seen twice in field coredumps). Widen the timeout for
// the update instead of unsubscribing anyone: a real hang still reboots, just later.
struct OtaWatchdogRelax {
    static constexpr uint32_t UPDATE_TIMEOUT_S = 60;
    OtaWatchdogRelax() { esp_task_wdt_init(UPDATE_TIMEOUT_S, true); }
    ~OtaWatchdogRelax() { esp_task_wdt_init(CONFIG_ESP_TASK_WDT_TIMEOUT_S, true); }
};

GitHubOTA::GitHubOTA(const String &display_version, const String &controller_version, const String &release_url,
                     const phase_callback_t &phase_callback, const progress_callback_t &progress_callback,
                     const String &firmware_name, const String &controller_firmware_name)
    : _release_url(release_url), _firmware_name(firmware_name), _controller_firmware_name(controller_firmware_name),
      _phase_callback(phase_callback), _progress_callback(progress_callback) {
    parseFirmwareVersion(display_version.c_str(), _version);
    parseFirmwareVersion(controller_version.c_str(), _controller_version);
    _wifi_client.setCACertBundle(gm_ca_bundle);
}

void GitHubOTA::init() {
    _controller_ota.init([this](int progress) { _progress_callback(PHASE_CONTROLLER_FW, progress); });
}

void GitHubOTA::checkForUpdates() {
    String releaseUrl;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        releaseUrl = _release_url;
    }

    String downloadUrl;
    String version;
    const char *error = "release_unavailable";
    if (releaseUrl.endsWith("/releases/latest")) {
        String releasePath = releaseUrl.substring(0, releaseUrl.length() - 6);
        const String tagPrefix = releasePath + "tag/";
        int status = 0;
        const String location = get_redirect_location(_wifi_client, releaseUrl, status);
        if (location.startsWith(tagPrefix)) {
            version = location.substring(tagPrefix.length());
            downloadUrl = releasePath + "download/" + version + "/";
        } else if (status == HTTP_CODE_NOT_FOUND) {
            error = "no_stable_release";
        }
    } else {
        downloadUrl = releaseUrl;
        downloadUrl.replace("/releases/tag/", "/releases/download/");
        downloadUrl += "/";
        version = get_updated_version_via_txt_file(_wifi_client, downloadUrl);
    }

    std::lock_guard<std::mutex> lock(_stateMutex);
    if (releaseUrl != _release_url)
        return;
    _latest_url = "";
    _latest_version_string = "";
    if (version.isEmpty() || !downloadUrl.startsWith("https://")) {
        _checkError = error;
    } else if (!parseFirmwareVersion(version.c_str(), _latest_version)) {
        _checkError = "invalid_release_version";
    } else {
        _latest_url = downloadUrl;
        _latest_version_string = version;
        _checkError = "";
        ESP_LOGI("OTA", "Checked release %s at %s", version.c_str(), downloadUrl.c_str());
    }
}

String GitHubOTA::getCurrentVersion() const {
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _latest_version_string;
}

const char *GitHubOTA::getCheckError() const {
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _checkError;
}

void GitHubOTA::setCheckError(const char *error) {
    std::lock_guard<std::mutex> lock(_stateMutex);
    _checkError = (error != nullptr) ? error : "";
}

bool GitHubOTA::isUpdateAvailable(bool controller) const {
    std::lock_guard<std::mutex> lock(_stateMutex);
    return !_latest_url.isEmpty() && semver_gt(_latest_version, controller ? _controller_version : _version);
}

void GitHubOTA::setPhase(uint8_t phase) { _phase_callback(phase); }

void GitHubOTA::fail(const char *reason) {
    _updateError = reason;
    setPhase(PHASE_FAILED);
}

void GitHubOTA::update(bool controller, bool display, OtaSender *controllerLink) {
    String downloadUrl;
    bool updateController = false;
    bool updateDisplay = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        downloadUrl = _latest_url;
        updateController = controller && semver_gt(_latest_version, _controller_version);
        updateDisplay = display && semver_gt(_latest_version, _version);
    }
    _updateError = "";
    if (downloadUrl.isEmpty() || (!updateController && !updateDisplay)) {
        fail("no_update");
        return;
    }

    OtaWatchdogRelax watchdogRelax;
    if (updateController) {
        setPhase(PHASE_CONTROLLER_FW);
        if (!_controller_ota.update(controllerLink, downloadUrl + _controller_firmware_name)) {
            fail("controller_update_failed");
            return;
        }
    }
    if (updateDisplay) {
        setPhase(PHASE_DISPLAY_FW);
        if (!flashDisplayFirmware(downloadUrl + _firmware_name)) {
            fail(_updateError);
            return;
        }
    }
    setPhase(PHASE_FINISHED);
    delay(1000);
    ESP.restart();
}

void GitHubOTA::setReleaseUrl(const String &release_url) {
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (release_url == _release_url)
        return;
    _release_url = release_url;
    _latest_url = "";
    _latest_version_string = "";
    _checkError = "";
}

bool GitHubOTA::flashDisplayFirmware(const String &url) {
    const char *TAG = "flashDisplayFirmware";
    ESP_LOGI(TAG, "Download URL: %s", url.c_str());

    const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
    if (partition == nullptr) {
        ESP_LOGE(TAG, "No OTA partition available");
        _updateError = "no_ota_partition";
        return false;
    }
    esp_ota_handle_t handle = 0;
    auto abort = [&]() {
        if (handle != 0) {
            esp_ota_abort(handle);
            handle = 0;
        }
    };

    DownloadSink sink;
    // Erasing the whole image range once up front (cooperatively, in yielding slices) beats one sector erase per
    // 4 KB write: far fewer core-0 stalls during the download. Unknown size falls back to erase-as-you-go.
    sink.prepare = [&](size_t total) {
        abort();
        if (total > partition->size) {
            _updateError = "image_too_large";
            return false;
        }
        esp_err_t err = esp_ota_begin(partition, total > 0 ? total : OTA_WITH_SEQUENTIAL_WRITES, &handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin(%u) failed: %s", static_cast<unsigned>(total), esp_err_to_name(err));
            handle = 0;
            _updateError = "flash_failed";
        }
        return err == ESP_OK;
    };
    sink.write = [&](const uint8_t *data, size_t len) {
        if (handle == 0) {
            return false;
        }
        esp_err_t err = esp_ota_write(handle, data, len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            _updateError = "flash_failed";
        }
        return err == ESP_OK;
    };
    sink.restart = [&]() {
        abort(); // the next full response prepares a fresh handle
        return true;
    };
    EspHttpTransport transport;
    EspDownloadEnv env;
    const auto percent = [](size_t received, size_t total) {
        return total ? static_cast<int>((static_cast<uint64_t>(received) * 100) / total) : 0;
    };
    ResumableDownloader downloader(
        transport, env, url.c_str(), sink,
        [&](size_t received, size_t total) { _progress_callback(PHASE_DISPLAY_FW, percent(received, total)); },
        DownloadPolicy(),
        [&](size_t received, size_t total) { _progress_callback(PHASE_RETRYING, percent(received, total)); });
    if (!downloader.run() || downloader.received() == 0 || handle == 0) {
        abort();
        if (!_updateError[0])
            _updateError = downloader.lastStatus() == HTTP_CODE_NOT_FOUND ? "image_missing" : "download_failed";
        return false;
    }

    // esp_ota_end verifies the complete image (header, checksum, appended SHA-256) before it can be booted.
    esp_err_t err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Image verification failed: %s", esp_err_to_name(err));
        _updateError = "image_invalid";
        return false;
    }
    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        _updateError = "flash_failed";
        return false;
    }
    ESP_LOGI(TAG, "Wrote %u bytes to %s", static_cast<unsigned>(downloader.received()), partition->label);
    return true;
}

void GitHubOTA::setControllerVersion(const String &controller_version) {
    std::lock_guard<std::mutex> lock(_stateMutex);
    parseFirmwareVersion(controller_version.c_str(), _controller_version);
}
