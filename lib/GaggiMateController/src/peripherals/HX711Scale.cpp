#ifdef ARDUINO_ARCH_STM32
#include <STM32FreeRTOS.h>
#define xTaskDelayUntil vTaskDelayUntil
#else
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#include "HX711Scale.h"
#include "logging.h"
#include <cfloat>
#include <cmath>

HX711Scale::HX711Scale(uint8_t dout1_pin, uint8_t dout2_pin, uint8_t sck_pin, const weight_callback_t &callback,
                       float calibration1, float calibration2)
    : _dout1_pin(dout1_pin), _dout2_pin(dout2_pin), _sck_pin(sck_pin), _calibration1(calibration1), _calibration2(calibration2),
      _callback(callback), _taskHandle(nullptr), _sgIndex(0), _sgCount(0), _sgActive(10), _lastMAD(0.1f) {}

void HX711Scale::setup() {
#ifdef ARDUINO_ARCH_STM32
    _loadCells = new HX711_2(TIM3);
    _loadCells->begin(_dout1_pin, _dout2_pin, _sck_pin, 128U, OUTPUT_OPEN_DRAIN);
#else
    _loadCells = new HX711_2();
    _loadCells->begin(_dout1_pin, _dout2_pin, _sck_pin, 128U, OUTPUT);
#endif

    _loadCells->set_scale(_calibration1, _calibration2);
    _loadCells->power_up();

    if (_loadCells->wait_ready_timeout(SCALE_INIT_TIMEOUT_MS, SCALE_READY_DELAY_MS)) {
        _loadCells->tare(SCALE_TARE_READINGS);
        _present = true;
        ESP_LOGI(LOG_TAG, "HX711 scale detected on DOUT1=%d, DOUT2=%d, SCK=%d", _dout1_pin, _dout2_pin, _sck_pin);
        xTaskCreate(loopTask, "HX711Scale::loop", configMINIMAL_STACK_SIZE * 4, this, 1, &_taskHandle);
    } else {
        _loadCells->power_down();
        _present = false;
        ESP_LOGW(LOG_TAG, "HX711 scale not detected, disabling");
    }
}

void HX711Scale::loop() {
    if (!_present || !_loadCells)
        return;

    if (_loadCells->wait_ready_timeout(150, SCALE_READY_DELAY_MS)) {
        float values[2];
        _loadCells->get_units(values, 2); // avg 2

        float raw = values[0] + values[1];
        float smoothed = 0.0f;
        float slopeGPerSec = 0.0f;

        if (sgFilter(raw, smoothed, slopeGPerSec)) {
            _weight = smoothed;

            ESP_LOGV(LOG_TAG, "SG: raw=%.2f smoothed=%.2f slope=%.2fg/s MAD=%.3f window=%d", raw, smoothed, slopeGPerSec,
                     _lastMAD, _sgActive);

            _callback(_weight);
        }
    }
}

void HX711Scale::tare() {
    if (!_present || !_loadCells)
        return;

    if (_loadCells->wait_ready_timeout(150, SCALE_READY_DELAY_MS)) {
        _loadCells->tare(SCALE_TARE_READINGS);
        _weight = 0.0f;
        long offsets[2] = {0, 0};
        _loadCells->get_offset(offsets);
        _offset1 = offsets[0];
        _offset2 = offsets[1];
        ESP_LOGI(LOG_TAG, "Scale tared, offsets: %ld, %ld", _offset1, _offset2);
        if (_tareResultCallback) {
            _tareResultCallback(_offset1, _offset2);
        }
    }
}

void HX711Scale::setCalibration(float calibration1, float calibration2) {
    _calibration1 = calibration1;
    _calibration2 = calibration2;
    if (_loadCells) {
        _loadCells->set_scale(_calibration1, _calibration2);
    }
}

void HX711Scale::getCalibration(float &calibration1, float &calibration2) const {
    calibration1 = _calibration1;
    calibration2 = _calibration2;
}

void HX711Scale::setOffset(long offset1, long offset2) {
    _offset1 = offset1;
    _offset2 = offset2;
    if (_loadCells) {
        _loadCells->set_offset(offset1, offset2);
        ESP_LOGI(LOG_TAG, "Scale offsets set: %ld, %ld", offset1, offset2);
    }
}

void HX711Scale::getOffsets(long &offset1, long &offset2) const {
    if (_loadCells) {
        long offsets[2] = {0, 0};
        _loadCells->get_offset(offsets);
        offset1 = offsets[0];
        offset2 = offsets[1];
    } else {
        offset1 = _offset1;
        offset2 = _offset2;
    }
}

float HX711Scale::calibrateChannel(uint8_t channel, float refWeight) {
    if (!_present || !_loadCells || refWeight == 0.0f)
        return 0.0f;
    if (!_loadCells->wait_ready_timeout(500, SCALE_READY_DELAY_MS))
        return 0.0f;

    long values[2] = {0, 0};
    _loadCells->get_value(values, 5);

    float newFactor = 0.0f;
    if (channel == 1 && values[0] != 0) {
        newFactor = static_cast<float>(values[0]) / refWeight;
        _calibration1 = newFactor;
        _loadCells->set_scale(_calibration1, _calibration2);
        ESP_LOGI(LOG_TAG, "Ch1 calibrated: factor=%.4f (raw=%ld, ref=%.1f)", newFactor, values[0], refWeight);
    } else if (channel == 2 && values[1] != 0) {
        newFactor = static_cast<float>(values[1]) / refWeight;
        _calibration2 = newFactor;
        _loadCells->set_scale(_calibration1, _calibration2);
        ESP_LOGI(LOG_TAG, "Ch2 calibrated: factor=%.4f (raw=%ld, ref=%.1f)", newFactor, values[1], refWeight);
    }
    return newFactor;
}

void HX711Scale::computeFit(float &a, float &b, float &c, int n) {
    // Closed-form power sums for x = 0..n-1
    float s1 = n * (n - 1) / 2.0f;
    float s2 = n * (n - 1) * (2 * n - 1) / 6.0f;
    float s3 = s1 * s1; // Σi³ = (n(n-1)/2)²
    float s4 = (float)n * (n - 1) * (2 * n - 1) * (3.0f * n * n - 3.0f * n - 1.0f) / 30.0f;

    float sy = 0.0f, sxy = 0.0f, sx2y = 0.0f;
    for (int i = 0; i < n; i++) {
        int bufIdx = (_sgIndex - n + i + SG_WINDOW_MAX) % SG_WINDOW_MAX;
        float y = _sgBuffer[bufIdx];
        float fi = (float)i;
        sy += y;
        sxy += fi * y;
        sx2y += fi * fi * y;
    }

    // Gaussian elimination on the 3x3 normal equations [M | rhs]
    float M[3][4] = {{(float)n, s1, s2, sy}, {s1, s2, s3, sxy}, {s2, s3, s4, sx2y}};
    for (int col = 0; col < 3; col++) {
        float pivot = M[col][col];
        if (fabsf(pivot) < 1e-10f) {
            a = b = c = 0.0f;
            return;
        }
        for (int row = col + 1; row < 3; row++) {
            float f = M[row][col] / pivot;
            for (int k = col; k < 4; k++)
                M[row][k] -= f * M[col][k];
        }
    }
    c = M[2][3] / M[2][2];
    b = (M[1][3] - M[1][2] * c) / M[1][1];
    a = (M[0][3] - M[0][2] * c - M[0][1] * b) / M[0][0];
}

float HX711Scale::computeGCV(int h) {
    if (_sgCount < h || h <= 3) // degree-2 needs at least 4 points
        return FLT_MAX;

    float a, b, c;
    computeFit(a, b, c, h);

    float rss = 0.0f;
    for (int i = 0; i < h; i++) {
        int bufIdx = (_sgIndex - h + i + SG_WINDOW_MAX) % SG_WINDOW_MAX;
        float yi = _sgBuffer[bufIdx];
        float fi = (float)i;
        float yHat = a + b * fi + c * fi * fi;
        float r = yi - yHat;
        rss += r * r;
    }

    // Quadratic fit: trace(H) = 3, so dof = (1 - 3/h)
    float dof = 1.0f - 3.0f / h;
    float gcv = (rss / h) / (dof * dof);

    ESP_LOGV(LOG_TAG, "GCV h=%d rss=%.4f gcv=%.4f", h, rss, gcv);
    return gcv;
}

int HX711Scale::selectWindow() {
    static constexpr int candidates[] = {5, 7, 10, 15, 20};
    static constexpr int numCandidates = 5;

    float bestGCV = FLT_MAX;
    int bestH = _sgActive;

    for (int i = 0; i < numCandidates; i++) {
        int h = candidates[i];
        float gcv = computeGCV(h);
        if (gcv < bestGCV) {
            bestGCV = gcv;
            bestH = h;
        }
    }

    if (bestH != _sgActive) {
        ESP_LOGI(LOG_TAG, "GCV window: %d → %d (GCV=%.4f)", _sgActive, bestH, bestGCV);
    }
    return bestH;
}

bool HX711Scale::sgFilter(float rawValue, float &smoothed, float &slopeGPerSec) {
    int n = _sgCount;

    // --- 1. Outlier check before inserting (MAD-based) ---
    if (n >= 4) {
        int h = min(n, _sgActive);
        float a, b, c;
        computeFit(a, b, c, h);

        // Compute absolute residuals of current window
        float absRes[SG_WINDOW_MAX];
        for (int i = 0; i < h; i++) {
            int bufIdx = (_sgIndex - h + i + SG_WINDOW_MAX) % SG_WINDOW_MAX;
            float fi = (float)i;
            float yHat = a + b * fi + c * fi * fi;
            absRes[i] = fabsf(_sgBuffer[bufIdx] - yHat);
        }
        // Insertion sort to find median (h <= SG_WINDOW_MAX = 20)
        for (int i = 1; i < h; i++) {
            float key = absRes[i];
            int j = i - 1;
            while (j >= 0 && absRes[j] > key) {
                absRes[j + 1] = absRes[j];
                j--;
            }
            absRes[j + 1] = key;
        }
        float median = (h % 2 == 0) ? (absRes[h / 2 - 1] + absRes[h / 2]) * 0.5f : absRes[h / 2];
        float mad = median * 1.4826f; // consistent estimator of sigma
        _lastMAD = mad;

        // Skip rejection when scale is still — MAD ≈ 0 would block all readings including
        // legitimate sudden weight changes (e.g. placing a cup)
        if (mad >= MAD_NOISE_FLOOR) {
            float fh = (float)h;
            float yHatNew = a + b * fh + c * fh * fh; // predicted at next index
            float residual = rawValue - yHatNew;

            if (fabsf(residual) > K_SIGMA * mad) {
                ESP_LOGD(LOG_TAG, "Outlier rejected: res=%.2f threshold=%.2f", fabsf(residual), K_SIGMA * mad);
                return false;
            }
        }
    }

    // --- 2. Insert into circular buffer ---
    _sgBuffer[_sgIndex] = rawValue;
    _sgIndex = (_sgIndex + 1) % SG_WINDOW_MAX;
    if (_sgCount < SG_WINDOW_MAX)
        _sgCount++;
    n = _sgCount;

    if (n < 4)
        return false;

    // --- 3. GCV adaptive window selection ---
    _sgActive = selectWindow();

    // --- 4. Final quadratic fit with selected window ---
    int h = min(n, _sgActive);
    float a, b, c;
    computeFit(a, b, c, h);

    float fLast = (float)(h - 1);
    smoothed = a + b * fLast + c * fLast * fLast;
    slopeGPerSec = (b + 2.0f * c * fLast) * (1000.0f / SCALE_READ_INTERVAL_MS);

    return true;
}

[[noreturn]] void HX711Scale::loopTask(void *arg) {
    TickType_t lastWake = xTaskGetTickCount();
    auto *scale = static_cast<HX711Scale *>(arg);
    while (true) {
        scale->loop();
        xTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SCALE_READ_INTERVAL_MS));
    }
}