#ifndef ONBOARDSTATECODEC_H
#define ONBOARDSTATECODEC_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace OnBoardStateCodec {

constexpr std::uint32_t MAGIC = 0x314C5245u; // "ERL1"
constexpr std::uint16_t FORMAT_VERSION_V1 = 1u;
constexpr std::uint16_t FORMAT_VERSION = 2u;
constexpr std::size_t HEADER_SIZE = 16;
constexpr std::size_t MAX_ENCODED_BYTES = 64u * 1024u;
constexpr std::size_t MAX_HISTORY = 480;
constexpr std::size_t MAX_POINTS = 40;
constexpr std::size_t MAX_COMPARISONS = 48;
constexpr std::size_t MAX_STRING_LEN = 256;
constexpr std::size_t TASTE_TARGET_COUNT = 14;
constexpr std::size_t PHI0_DIM = 9;
constexpr std::size_t MAX_RAW_DIM = 6;

struct PersistedPoint {
    std::string shotId;
    double grindSteps = 0.0;
    double doseG = 18.0;
    double targetYieldG = 36.0;
    double beverageOutG = 0.0;
    std::array<float, 3> splineDelta{0.0f, 0.0f, 0.0f};
    std::uint8_t normDim = 3;
    std::array<double, MAX_RAW_DIM> normX{0.5, 0.5, 0.5, 0.5, 0.5, 0.5};
    std::array<double, PHI0_DIM> phi0{};
};

struct PersistedComparison {
    std::uint16_t newIndex = 0;
    std::uint16_t anchorIndex = 0;
    std::uint8_t label = 2; // 0=NEW_BETTER, 1=ANCHOR_BETTER, 2=TIE
};

struct PersistedState {
    std::string activeContextKey;
    std::string optimizationRunId;
    std::string activeBeanContextId;
    std::string activeGrinderContextId;
    std::string activeProfileId;
    std::string activeProfileLabel;
    std::string activeRawProfileHash;

    std::uint8_t controlMode = 0; // 0=recipe_only, 1=spline_only, 2=joint_6d
    std::uint8_t tasteGoalMode = 0; // 0=Balanced, 1=Custom
    std::array<std::uint8_t, TASTE_TARGET_COUNT> tasteGoalTargets{};

    float bevToPumpRatio = 0.80f;
    bool hasLastShotRecipe = false;
    float lastShotHydraulicGrindSteps = 0.0f;
    float lastShotDoseG = 18.0f;
    std::array<float, 3> activeSplineDelta{0.0f, 0.0f, 0.0f};
    bool hasPendingSplineDelta = false;
    std::array<float, 3> pendingSplineDelta{0.0f, 0.0f, 0.0f};
    std::string pendingSplineRecId;

    std::uint8_t trustDim = 3;
    std::array<double, MAX_RAW_DIM> trustCenter{0.5, 0.5, 0.5, 0.5, 0.5, 0.5};
    double trustLength = 0.20;
    std::int32_t trustSuccesses = 0;
    std::int32_t trustFailures = 0;
    bool trustConverged = false;
    std::uint8_t trustLastAction = 0;

    bool hasBestPoint = false;
    std::uint16_t bestPointIndex = 0;
    std::int32_t localShotCount = 0;
    std::string lastShotId;
    std::int64_t lastShotAt = 0;
    float lastShotTimeS = 0.0f;
    float lastShotBeverageOutG = 0.0f;
    float lastShotTargetYieldG = 0.0f;
    std::string lastRecommendationId;
    std::int64_t lastRecommendationAt = 0;

    std::vector<std::array<float, 4>> histZ;
    std::vector<float> histY;
    std::vector<PersistedPoint> points;
    std::vector<PersistedComparison> comparisons;
};

inline std::uint32_t crc32(const std::uint8_t *data, std::size_t len) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return ~crc;
}

namespace detail {

template <typename T> inline void appendPod(std::vector<std::uint8_t> &buf, T value) {
    const std::size_t offset = buf.size();
    buf.resize(offset + sizeof(T));
    std::memcpy(buf.data() + offset, &value, sizeof(T));
}

inline bool appendString(std::vector<std::uint8_t> &buf, const std::string &s) {
    if (s.size() > MAX_STRING_LEN) {
        return false;
    }
    appendPod<std::uint16_t>(buf, static_cast<std::uint16_t>(s.size()));
    if (!s.empty()) {
        const std::size_t offset = buf.size();
        buf.resize(offset + s.size());
        std::memcpy(buf.data() + offset, s.data(), s.size());
    }
    return true;
}

struct Reader {
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;

    std::size_t remaining() const { return (pos <= size) ? (size - pos) : 0; }

    template <typename T> bool readPod(T &out) {
        if (remaining() < sizeof(T)) {
            return false;
        }
        std::memcpy(&out, data + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }

    bool readString(std::string &out) {
        std::uint16_t len = 0;
        if (!readPod(len) || len > MAX_STRING_LEN || remaining() < len) {
            return false;
        }
        out.assign(reinterpret_cast<const char *>(data + pos), len);
        pos += len;
        return true;
    }
};

inline bool validateState(const PersistedState &st) {
    if (st.histZ.size() != st.histY.size() || st.histZ.size() > MAX_HISTORY || st.points.size() > MAX_POINTS ||
        st.comparisons.size() > MAX_COMPARISONS) {
        return false;
    }
    if (st.controlMode > 2u || st.tasteGoalMode > 1u) {
        return false;
    }
    if (st.trustDim != 3u && st.trustDim != 6u) {
        return false;
    }
    for (std::uint8_t t : st.tasteGoalTargets) {
        if (t > 3u) {
            return false;
        }
    }
    if (!std::isfinite(st.bevToPumpRatio) || st.bevToPumpRatio < 0.50f || st.bevToPumpRatio > 1.20f) {
        return false;
    }
    if (!std::isfinite(st.lastShotHydraulicGrindSteps) || !std::isfinite(st.lastShotDoseG) ||
        st.lastShotDoseG < 0.0f || st.lastShotDoseG > 100.0f) {
        return false;
    }
    for (float d : st.activeSplineDelta) {
        if (!std::isfinite(d) || d < -1.5f || d > 1.5f) {
            return false;
        }
    }
    for (float d : st.pendingSplineDelta) {
        if (!std::isfinite(d) || d < -1.5f || d > 1.5f) {
            return false;
        }
    }
    if (st.pendingSplineRecId.size() > MAX_STRING_LEN) {
        return false;
    }
    for (std::size_t i = 0; i < st.trustDim; ++i) {
        const double c = st.trustCenter[i];
        if (!std::isfinite(c) || c < 0.0 || c > 1.0) {
            return false;
        }
    }
    if (!std::isfinite(st.trustLength) || st.trustLength < 0.01 || st.trustLength > 1.0 || st.trustSuccesses < 0 ||
        st.trustFailures < 0 || st.trustLastAction > 5u) {
        return false;
    }
    if (st.localShotCount < 0 || !std::isfinite(st.lastShotTimeS) || st.lastShotTimeS < 0.0f ||
        !std::isfinite(st.lastShotBeverageOutG) || st.lastShotBeverageOutG < 0.0f ||
        !std::isfinite(st.lastShotTargetYieldG) || st.lastShotTargetYieldG < 0.0f) {
        return false;
    }
    if (st.hasBestPoint && st.bestPointIndex >= st.points.size()) {
        return false;
    }
    for (std::size_t i = 0; i < st.histZ.size(); ++i) {
        for (float v : st.histZ[i]) {
            if (!std::isfinite(v)) {
                return false;
            }
        }
        if (!std::isfinite(st.histY[i]) || std::fabs(st.histY[i]) > 5.0f) {
            return false;
        }
    }
    for (const auto &pt : st.points) {
        if (pt.shotId.empty() || pt.shotId.size() > MAX_STRING_LEN || !std::isfinite(pt.grindSteps) ||
            !std::isfinite(pt.doseG) || pt.doseG <= 0.0 || !std::isfinite(pt.targetYieldG) || pt.targetYieldG <= 0.0 ||
            !std::isfinite(pt.beverageOutG) || pt.beverageOutG < 0.0 || (pt.normDim != 3u && pt.normDim != 6u)) {
            return false;
        }
        for (float d : pt.splineDelta) {
            if (!std::isfinite(d) || d < -1.5f || d > 1.5f) {
                return false;
            }
        }
        for (std::size_t k = 0; k < pt.normDim; ++k) {
            const double x = pt.normX[k];
            if (!std::isfinite(x) || x < 0.0 || x > 1.0) {
                return false;
            }
        }
        for (double p : pt.phi0) {
            if (!std::isfinite(p)) {
                return false;
            }
        }
    }
    for (const auto &cmp : st.comparisons) {
        if (cmp.newIndex >= st.points.size() || cmp.anchorIndex >= st.points.size() ||
            cmp.newIndex == cmp.anchorIndex || cmp.label > 2u) {
            return false;
        }
    }
    return true;
}

} // namespace detail

inline bool encode(const PersistedState &st, std::vector<std::uint8_t> &outBytes) {
    outBytes.clear();
    if (!detail::validateState(st)) {
        return false;
    }
    outBytes.reserve(HEADER_SIZE + 512 + st.histZ.size() * 20 + st.points.size() * 144 + st.comparisons.size() * 6);
    // Reserve header slots; fill after payload is written.
    outBytes.resize(HEADER_SIZE, 0);

    if (!detail::appendString(outBytes, st.activeContextKey) || !detail::appendString(outBytes, st.optimizationRunId) ||
        !detail::appendString(outBytes, st.activeBeanContextId) ||
        !detail::appendString(outBytes, st.activeGrinderContextId) ||
        !detail::appendString(outBytes, st.activeProfileId) || !detail::appendString(outBytes, st.activeProfileLabel) ||
        !detail::appendString(outBytes, st.activeRawProfileHash)) {
        outBytes.clear();
        return false;
    }

    detail::appendPod<std::uint8_t>(outBytes, st.controlMode);
    detail::appendPod<std::uint8_t>(outBytes, st.tasteGoalMode);
    for (std::uint8_t t : st.tasteGoalTargets) {
        detail::appendPod<std::uint8_t>(outBytes, t);
    }

    detail::appendPod<float>(outBytes, st.bevToPumpRatio);
    detail::appendPod<std::uint8_t>(outBytes, st.hasLastShotRecipe ? 1u : 0u);
    detail::appendPod<float>(outBytes, st.lastShotHydraulicGrindSteps);
    detail::appendPod<float>(outBytes, st.lastShotDoseG);
    for (float d : st.activeSplineDelta) {
        detail::appendPod<float>(outBytes, d);
    }
    detail::appendPod<std::uint8_t>(outBytes, st.hasPendingSplineDelta ? 1u : 0u);
    for (float d : st.pendingSplineDelta) {
        detail::appendPod<float>(outBytes, d);
    }
    if (!detail::appendString(outBytes, st.pendingSplineRecId)) {
        outBytes.clear();
        return false;
    }

    detail::appendPod<std::uint8_t>(outBytes, st.trustDim);
    for (std::size_t i = 0; i < st.trustDim; ++i) {
        detail::appendPod<double>(outBytes, st.trustCenter[i]);
    }
    detail::appendPod<double>(outBytes, st.trustLength);
    detail::appendPod<std::int32_t>(outBytes, st.trustSuccesses);
    detail::appendPod<std::int32_t>(outBytes, st.trustFailures);
    detail::appendPod<std::uint8_t>(outBytes, st.trustConverged ? 1u : 0u);
    detail::appendPod<std::uint8_t>(outBytes, st.trustLastAction);

    detail::appendPod<std::uint8_t>(outBytes, st.hasBestPoint ? 1u : 0u);
    detail::appendPod<std::uint16_t>(outBytes, st.bestPointIndex);
    detail::appendPod<std::int32_t>(outBytes, st.localShotCount);
    if (!detail::appendString(outBytes, st.lastShotId)) {
        outBytes.clear();
        return false;
    }
    detail::appendPod<std::int64_t>(outBytes, st.lastShotAt);
    detail::appendPod<float>(outBytes, st.lastShotTimeS);
    detail::appendPod<float>(outBytes, st.lastShotBeverageOutG);
    detail::appendPod<float>(outBytes, st.lastShotTargetYieldG);
    if (!detail::appendString(outBytes, st.lastRecommendationId)) {
        outBytes.clear();
        return false;
    }
    detail::appendPod<std::int64_t>(outBytes, st.lastRecommendationAt);

    detail::appendPod<std::uint16_t>(outBytes, static_cast<std::uint16_t>(st.histZ.size()));
    for (std::size_t i = 0; i < st.histZ.size(); ++i) {
        for (float v : st.histZ[i]) {
            detail::appendPod<float>(outBytes, v);
        }
        detail::appendPod<float>(outBytes, st.histY[i]);
    }

    detail::appendPod<std::uint16_t>(outBytes, static_cast<std::uint16_t>(st.points.size()));
    for (const auto &pt : st.points) {
        if (!detail::appendString(outBytes, pt.shotId)) {
            outBytes.clear();
            return false;
        }
        detail::appendPod<double>(outBytes, pt.grindSteps);
        detail::appendPod<double>(outBytes, pt.doseG);
        detail::appendPod<double>(outBytes, pt.targetYieldG);
        detail::appendPod<double>(outBytes, pt.beverageOutG);
        for (float d : pt.splineDelta) {
            detail::appendPod<float>(outBytes, d);
        }
        detail::appendPod<std::uint8_t>(outBytes, pt.normDim);
        for (std::size_t k = 0; k < pt.normDim; ++k) {
            detail::appendPod<double>(outBytes, pt.normX[k]);
        }
        for (double p : pt.phi0) {
            detail::appendPod<double>(outBytes, p);
        }
    }

    detail::appendPod<std::uint16_t>(outBytes, static_cast<std::uint16_t>(st.comparisons.size()));
    for (const auto &cmp : st.comparisons) {
        detail::appendPod<std::uint16_t>(outBytes, cmp.newIndex);
        detail::appendPod<std::uint16_t>(outBytes, cmp.anchorIndex);
        detail::appendPod<std::uint8_t>(outBytes, cmp.label);
    }

    if (outBytes.size() > MAX_ENCODED_BYTES) {
        outBytes.clear();
        return false;
    }

    const std::uint32_t payloadLen = static_cast<std::uint32_t>(outBytes.size() - HEADER_SIZE);
    const std::uint32_t checksum = crc32(outBytes.data() + HEADER_SIZE, payloadLen);
    const std::uint16_t reserved = 0u;
    std::memcpy(outBytes.data() + 0, &MAGIC, sizeof(MAGIC));
    std::memcpy(outBytes.data() + 4, &FORMAT_VERSION, sizeof(FORMAT_VERSION));
    std::memcpy(outBytes.data() + 6, &reserved, sizeof(reserved));
    std::memcpy(outBytes.data() + 8, &payloadLen, sizeof(payloadLen));
    std::memcpy(outBytes.data() + 12, &checksum, sizeof(checksum));
    return true;
}

inline bool decode(const std::uint8_t *data, std::size_t size, PersistedState &outState) {
    outState = PersistedState{};
    if (data == nullptr || size < HEADER_SIZE || size > MAX_ENCODED_BYTES) {
        return false;
    }

    detail::Reader r{data, size, 0};
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint32_t payloadLen = 0;
    std::uint32_t expectedCrc = 0;
    if (!r.readPod(magic) || !r.readPod(version) || !r.readPod(reserved) || !r.readPod(payloadLen) ||
        !r.readPod(expectedCrc)) {
        return false;
    }
    if (magic != MAGIC || (version != FORMAT_VERSION_V1 && version != FORMAT_VERSION) ||
        payloadLen != (size - HEADER_SIZE)) {
        return false;
    }
    if (crc32(data + HEADER_SIZE, payloadLen) != expectedCrc) {
        return false;
    }

    const bool isV2 = (version == FORMAT_VERSION);
    PersistedState st;
    if (!r.readString(st.activeContextKey) || !r.readString(st.optimizationRunId) ||
        !r.readString(st.activeBeanContextId) || !r.readString(st.activeGrinderContextId) ||
        !r.readString(st.activeProfileId) || !r.readString(st.activeProfileLabel) ||
        !r.readString(st.activeRawProfileHash)) {
        return false;
    }
    if (isV2) {
        if (!r.readPod(st.controlMode)) {
            return false;
        }
    } else {
        st.controlMode = 0u;
    }
    if (!r.readPod(st.tasteGoalMode)) {
        return false;
    }
    for (std::uint8_t &t : st.tasteGoalTargets) {
        if (!r.readPod(t)) {
            return false;
        }
    }

    std::uint8_t hasLastRecipeByte = 0;
    if (!r.readPod(st.bevToPumpRatio) || !r.readPod(hasLastRecipeByte) || !r.readPod(st.lastShotHydraulicGrindSteps) ||
        !r.readPod(st.lastShotDoseG)) {
        return false;
    }
    st.hasLastShotRecipe = (hasLastRecipeByte != 0u);
    for (float &d : st.activeSplineDelta) {
        if (!r.readPod(d)) {
            return false;
        }
    }
    if (isV2) {
        std::uint8_t hasPendingByte = 0;
        if (!r.readPod(hasPendingByte)) {
            return false;
        }
        st.hasPendingSplineDelta = (hasPendingByte != 0u);
        for (float &d : st.pendingSplineDelta) {
            if (!r.readPod(d)) {
                return false;
            }
        }
        if (!r.readString(st.pendingSplineRecId) || !r.readPod(st.trustDim) ||
            (st.trustDim != 3u && st.trustDim != 6u)) {
            return false;
        }
    } else {
        st.hasPendingSplineDelta = false;
        st.pendingSplineDelta = {0.0f, 0.0f, 0.0f};
        st.pendingSplineRecId.clear();
        st.trustDim = 3u;
    }

    st.trustCenter.fill(0.5);
    for (std::size_t i = 0; i < st.trustDim; ++i) {
        if (!r.readPod(st.trustCenter[i])) {
            return false;
        }
    }
    std::uint8_t convByte = 0;
    if (!r.readPod(st.trustLength) || !r.readPod(st.trustSuccesses) || !r.readPod(st.trustFailures) ||
        !r.readPod(convByte) || !r.readPod(st.trustLastAction)) {
        return false;
    }
    st.trustConverged = (convByte != 0u);

    std::uint8_t hasBestByte = 0;
    if (!r.readPod(hasBestByte) || !r.readPod(st.bestPointIndex) || !r.readPod(st.localShotCount) ||
        !r.readString(st.lastShotId) || !r.readPod(st.lastShotAt) || !r.readPod(st.lastShotTimeS) ||
        !r.readPod(st.lastShotBeverageOutG) || !r.readPod(st.lastShotTargetYieldG) ||
        !r.readString(st.lastRecommendationId) || !r.readPod(st.lastRecommendationAt)) {
        return false;
    }
    st.hasBestPoint = (hasBestByte != 0u);

    std::uint16_t histCount = 0;
    if (!r.readPod(histCount) || histCount > MAX_HISTORY) {
        return false;
    }
    st.histZ.resize(histCount);
    st.histY.resize(histCount);
    for (std::size_t i = 0; i < histCount; ++i) {
        for (float &v : st.histZ[i]) {
            if (!r.readPod(v)) {
                return false;
            }
        }
        if (!r.readPod(st.histY[i])) {
            return false;
        }
    }

    std::uint16_t pointCount = 0;
    if (!r.readPod(pointCount) || pointCount > MAX_POINTS) {
        return false;
    }
    st.points.resize(pointCount);
    for (std::size_t i = 0; i < pointCount; ++i) {
        auto &pt = st.points[i];
        if (!r.readString(pt.shotId) || !r.readPod(pt.grindSteps) || !r.readPod(pt.doseG) ||
            !r.readPod(pt.targetYieldG) || !r.readPod(pt.beverageOutG)) {
            return false;
        }
        if (isV2) {
            for (float &d : pt.splineDelta) {
                if (!r.readPod(d)) {
                    return false;
                }
            }
            if (!r.readPod(pt.normDim) || (pt.normDim != 3u && pt.normDim != 6u)) {
                return false;
            }
        } else {
            pt.splineDelta = {0.0f, 0.0f, 0.0f};
            pt.normDim = 3u;
        }
        pt.normX.fill(0.5);
        for (std::size_t k = 0; k < pt.normDim; ++k) {
            if (!r.readPod(pt.normX[k])) {
                return false;
            }
        }
        for (double &p : pt.phi0) {
            if (!r.readPod(p)) {
                return false;
            }
        }
    }

    std::uint16_t compCount = 0;
    if (!r.readPod(compCount) || compCount > MAX_COMPARISONS) {
        return false;
    }
    st.comparisons.resize(compCount);
    for (std::size_t i = 0; i < compCount; ++i) {
        auto &cmp = st.comparisons[i];
        if (!r.readPod(cmp.newIndex) || !r.readPod(cmp.anchorIndex) || !r.readPod(cmp.label)) {
            return false;
        }
    }

    if (r.remaining() != 0 || !detail::validateState(st)) {
        return false;
    }
    outState = std::move(st);
    return true;
}

} // namespace OnBoardStateCodec

#endif // ONBOARDSTATECODEC_H
