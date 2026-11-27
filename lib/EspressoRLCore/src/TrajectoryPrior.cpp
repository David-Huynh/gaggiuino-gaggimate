#include "TrajectoryPrior.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace EspressoRL {

namespace {

template <typename T> bool readPod(const std::uint8_t *data, std::size_t size, std::size_t &pos, T &out) {
    if (pos + sizeof(T) > size) {
        return false;
    }
    std::memcpy(&out, data + pos, sizeof(T));
    pos += sizeof(T);
    return true;
}

float bandMean(const std::vector<float> &vals, const std::vector<float> &prog, float lo, float hi, float fallback) {
    double sum = 0.0;
    std::size_t cnt = 0;
    for (std::size_t i = 0; i < vals.size(); ++i) {
        const bool inBand = (hi >= 1.0f) ? (prog[i] >= lo && prog[i] <= hi) : (prog[i] >= lo && prog[i] < hi);
        if (inBand) {
            sum += static_cast<double>(vals[i]);
            ++cnt;
        }
    }
    return (cnt > 0) ? static_cast<float>(sum / static_cast<double>(cnt)) : fallback;
}

} // namespace

std::uint64_t hashTasteGoalFingerprint(const char *fingerprint, std::size_t len, bool isBalanced) {
    if (isBalanced || fingerprint == nullptr || len == 0) {
        return 0ULL;
    }
    std::uint64_t h = 14695981039346656037ULL;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint8_t>(fingerprint[i]);
        h *= 1099511628211ULL;
    }
    return (h == 0ULL) ? 1ULL : h;
}

std::uint32_t etp1Crc32(const std::uint8_t *data, std::size_t len) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return ~crc;
}

bool decodeEtp1(const std::uint8_t *data, std::size_t size, TrajectoryTastePrior &outPrior,
                const std::uint8_t *trustedPublicKey, bool requireSignature) {
    outPrior = TrajectoryTastePrior{};
    if (data == nullptr || size < ETP1_HEADER_SIZE || size > ETP1_MAX_BYTES) {
        return false;
    }

    std::size_t pos = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint32_t payloadLen = 0;
    std::uint32_t expectedCrc = 0;
    if (!readPod(data, size, pos, magic) || !readPod(data, size, pos, version) ||
        !readPod(data, size, pos, flags) || !readPod(data, size, pos, payloadLen) ||
        !readPod(data, size, pos, expectedCrc)) {
        return false;
    }
    if (magic != ETP1_MAGIC || version != ETP1_VERSION || (flags & ~ETP1_FLAG_SIGNED) != 0u) {
        return false;
    }

    const bool isSigned = (flags & ETP1_FLAG_SIGNED) != 0u;
    if (requireSignature && (!isSigned || trustedPublicKey == nullptr)) {
        return false;
    }
    const std::size_t sigBytes = isSigned ? ETP1_SIGNATURE_SIZE : 0u;
    if (static_cast<std::size_t>(payloadLen) > size - ETP1_HEADER_SIZE) {
        return false;
    }
    const std::size_t bodyEnd = ETP1_HEADER_SIZE + static_cast<std::size_t>(payloadLen);
    if (bodyEnd + sigBytes != size) {
        return false;
    }
    if (etp1Crc32(data + ETP1_HEADER_SIZE, payloadLen) != expectedCrc) {
        return false;
    }

    bool sigVerified = false;
    if (isSigned && trustedPublicKey != nullptr) {
        if (!ed25519Verify(trustedPublicKey, data, bodyEnd, data + bodyEnd)) {
            return false;
        }
        sigVerified = true;
    }

    std::int64_t createdAt = 0;
    std::uint8_t approvedByte = 0;
    float decayComp = 4.0f;
    std::uint8_t bucketCount = 0;
    if (!readPod(data, bodyEnd, pos, createdAt) || !readPod(data, bodyEnd, pos, approvedByte) ||
        !readPod(data, bodyEnd, pos, decayComp) || !readPod(data, bodyEnd, pos, bucketCount)) {
        return false;
    }
    if ((approvedByte != 0u && approvedByte != 1u) || !std::isfinite(decayComp) || decayComp < 0.5f ||
        decayComp > 50.0f || bucketCount > ETP1_MAX_GOAL_BUCKETS) {
        return false;
    }

    TrajectoryTastePrior tmp;
    tmp.createdAt = createdAt;
    tmp.approved = (approvedByte == 1u);
    tmp.signatureVerified = sigVerified;
    tmp.decayComparisons = decayComp;
    tmp.bucketCount = bucketCount;

    for (std::size_t i = 0; i < TRAJECTORY_FEATURE_DIM; ++i) {
        if (!readPod(data, bodyEnd, pos, tmp.featureMean[i]) || !std::isfinite(tmp.featureMean[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < TRAJECTORY_FEATURE_DIM; ++i) {
        if (!readPod(data, bodyEnd, pos, tmp.featureScale[i]) || !std::isfinite(tmp.featureScale[i]) ||
            tmp.featureScale[i] < 1e-4f) {
            return false;
        }
    }

    for (std::size_t b = 0; b < bucketCount; ++b) {
        GoalPriorBucket &bk = tmp.buckets[b];
        if (!readPod(data, bodyEnd, pos, bk.goalHash) || !readPod(data, bodyEnd, pos, bk.priorScale) ||
            !readPod(data, bodyEnd, pos, bk.confidence) || !readPod(data, bodyEnd, pos, bk.comparisonCount) ||
            !readPod(data, bodyEnd, pos, bk.installCount)) {
            return false;
        }
        if (!std::isfinite(bk.priorScale) || bk.priorScale < 0.0f || bk.priorScale > ETP1_MAX_PRIOR_SCALE + 1e-5f ||
            !std::isfinite(bk.confidence) || bk.confidence < 0.0f || bk.confidence > 1.0f + 1e-5f) {
            return false;
        }
        for (std::size_t i = 0; i < TRAJECTORY_FEATURE_DIM; ++i) {
            if (!readPod(data, bodyEnd, pos, bk.linearWeights[i]) || !std::isfinite(bk.linearWeights[i]) ||
                std::fabs(bk.linearWeights[i]) > ETP1_MAX_ABS_LINEAR_WEIGHT + 1e-5f) {
                return false;
            }
        }
        for (std::size_t i = 0; i < TRAJECTORY_FEATURE_DIM; ++i) {
            if (!readPod(data, bodyEnd, pos, bk.quadWeights[i]) || !std::isfinite(bk.quadWeights[i]) ||
                std::fabs(bk.quadWeights[i]) > ETP1_MAX_ABS_QUAD_WEIGHT + 1e-5f) {
                return false;
            }
        }
    }

    if (pos != bodyEnd) {
        return false;
    }

    tmp.valid = true;
    outPrior = tmp;
    return true;
}

float TrajectoryTastePrior::evaluate(const TrajectoryFeatureVec &features, std::uint64_t goalHash,
                                     std::size_t localComparisonCount) const {
    if (!valid || !approved || bucketCount == 0) {
        return 0.0f;
    }
    const GoalPriorBucket *matched = nullptr;
    const GoalPriorBucket *fallback = nullptr;
    for (std::size_t i = 0; i < bucketCount; ++i) {
        if (buckets[i].goalHash == goalHash) {
            matched = &buckets[i];
            break;
        }
        if (buckets[i].goalHash == 0ULL) {
            fallback = &buckets[i];
        }
    }
    const GoalPriorBucket *bk = (matched != nullptr) ? matched : fallback;
    if (bk == nullptr || bk->priorScale <= 0.0f) {
        return 0.0f;
    }

    const double fallbackFactor = (goalHash != 0ULL && bk->goalHash == 0ULL) ? 0.5 : 1.0;
    const double baseScale = std::clamp(static_cast<double>(bk->priorScale) * fallbackFactor, 0.0,
                                        static_cast<double>(ETP1_MAX_PRIOR_SCALE));
    const double decay = std::max(0.5, static_cast<double>(decayComparisons));
    const double weight = baseScale / (1.0 + static_cast<double>(localComparisonCount) / decay);
    if (weight <= 0.0) {
        return 0.0f;
    }

    double rawScore = 0.0;
    for (std::size_t i = 0; i < TRAJECTORY_FEATURE_DIM; ++i) {
        if (!std::isfinite(features[i])) {
            return 0.0f;
        }
        const double z = std::clamp(
            (static_cast<double>(features[i]) - static_cast<double>(featureMean[i])) /
                static_cast<double>(featureScale[i]),
            -static_cast<double>(ETP1_STANDARDIZED_CLIP), static_cast<double>(ETP1_STANDARDIZED_CLIP));
        rawScore += static_cast<double>(bk->linearWeights[i]) * z +
                    static_cast<double>(bk->quadWeights[i]) * (z * z);
    }
    return static_cast<float>(weight * std::tanh(rawScore));
}

float TrajectoryTastePrior::primaryConfidence() const {
    if (!valid || !approved || bucketCount == 0) {
        return 0.0f;
    }
    return buckets[0].confidence;
}

bool extractTrajectoryFeatures(const TrajectorySamplePoint *samples, std::size_t count, float doseG,
                               float beverageOutG, float shotTimeS, TrajectoryFeatureVec &outFeatures) {
    outFeatures.fill(0.0f);
    if (samples == nullptr || count < 2 || !std::isfinite(doseG) || doseG <= 0.1f || doseG > 100.0f) {
        return false;
    }

    std::vector<float> tV, pV, qV, wV, tempV;
    std::vector<uint8_t> valveV;
    tV.reserve(count);
    pV.reserve(count);
    qV.reserve(count);
    wV.reserve(count);
    tempV.reserve(count);
    valveV.reserve(count);

    float maxW = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        const auto &s = samples[i];
        if ((s.systemInfo & ETP1_EXTENDED_RECORDING_BIT) != 0u) {
            continue;
        }
        if (!std::isfinite(s.elapsedS) || !std::isfinite(s.pressureBar) || !std::isfinite(s.beverageFlowGS) ||
            !std::isfinite(s.weightG)) {
            continue;
        }
        if (s.weightG + 1.0f < maxW) {
            continue;
        }
        if (s.weightG > maxW) {
            maxW = s.weightG;
        }
        tV.push_back(std::clamp(s.elapsedS, 0.0f, 300.0f));
        pV.push_back(std::clamp(s.pressureBar, 0.0f, 15.0f));
        qV.push_back(std::clamp(s.beverageFlowGS, 0.0f, 12.0f));
        wV.push_back(std::clamp(s.weightG, 0.0f, 300.0f));
        const float tc = (std::isfinite(s.temperatureC) && s.temperatureC >= 60.0f && s.temperatureC <= 110.0f)
                             ? s.temperatureC
                             : 93.0f;
        tempV.push_back(tc);
        valveV.push_back(s.valveOpen ? 1u : 0u);
    }

    const std::size_t nv = tV.size();
    if (nv < 2) {
        return false;
    }

    float maxFilteredW = 0.0f;
    for (float w : wV) {
        maxFilteredW = std::max(maxFilteredW, w);
    }
    const float finalWeight =
        std::clamp((std::isfinite(beverageOutG) && beverageOutG > 0.0f) ? beverageOutG : maxFilteredW, 0.5f, 300.0f);
    const float brewRatio = std::clamp(finalWeight / doseG, 0.2f, 20.0f);
    const float totalTime =
        std::clamp((std::isfinite(shotTimeS) && shotTimeS > 0.0f) ? shotTimeS : (tV.back() - tV.front()), 1.0f, 180.0f);

    float ttfd = std::min(totalTime, 10.0f);
    for (std::size_t i = 0; i < nv; ++i) {
        if (wV[i] >= 0.5f) {
            ttfd = tV[i] - tV[0];
            break;
        }
    }
    ttfd = std::clamp(ttfd, 0.0f, totalTime);

    auto collectActive = [&](int level, std::vector<float> &qA, std::vector<float> &pA, std::vector<float> &wA,
                             std::vector<float> &tempA) {
        qA.clear();
        pA.clear();
        wA.clear();
        tempA.clear();
        for (std::size_t i = 0; i < nv; ++i) {
            bool ok = true;
            if (level == 0) {
                ok = (valveV[i] != 0u) && (pV[i] >= 1.0f) && (wV[i] >= 0.5f || qV[i] >= 0.15f);
            } else if (level == 1) {
                ok = (valveV[i] != 0u) && (pV[i] >= 0.5f);
            }
            if (ok) {
                qA.push_back(qV[i]);
                pA.push_back(pV[i]);
                wA.push_back(wV[i]);
                tempA.push_back(tempV[i]);
            }
        }
    };

    std::vector<float> qAct, pAct, wAct, tempAct;
    collectActive(0, qAct, pAct, wAct, tempAct);
    if (qAct.size() < 2) {
        collectActive(1, qAct, pAct, wAct, tempAct);
    }
    if (qAct.size() < 2) {
        collectActive(2, qAct, pAct, wAct, tempAct);
    }

    const std::size_t na = qAct.size();
    double sumQ = 0.0, sumP = 0.0, sumTemp = 0.0;
    float peakP = 0.0f;
    std::vector<float> prog(na, 0.0f);
    const float invFinalW = 1.0f / std::max(finalWeight, 1.0f);
    for (std::size_t i = 0; i < na; ++i) {
        sumQ += static_cast<double>(qAct[i]);
        sumP += static_cast<double>(pAct[i]);
        sumTemp += static_cast<double>(tempAct[i]);
        peakP = std::max(peakP, pAct[i]);
        prog[i] = std::clamp(wAct[i] * invFinalW, 0.0f, 1.0f);
    }
    const float meanFlow = static_cast<float>(sumQ / static_cast<double>(na));
    const float meanPressure = static_cast<float>(sumP / static_cast<double>(na));
    const float meanTemp = static_cast<float>(sumTemp / static_cast<double>(na));

    const float earlyFlow = bandMean(qAct, prog, 0.0f, 1.0f / 3.0f, meanFlow);
    const float midFlow = bandMean(qAct, prog, 1.0f / 3.0f, 2.0f / 3.0f, meanFlow);
    const float lateFlow = bandMean(qAct, prog, 2.0f / 3.0f, 1.0f, meanFlow);
    const float flowSlope = lateFlow - earlyFlow;

    const float tailPressure = bandMean(pAct, prog, 0.75f, 1.0f, pAct.back());
    const float pressureDecline = std::clamp(peakP - tailPressure, -5.0f, 15.0f);

    float flowRoughness = 0.0f;
    if (na >= 2) {
        double sumSq = 0.0;
        for (std::size_t i = 1; i < na; ++i) {
            const double d = static_cast<double>(qAct[i]) - static_cast<double>(qAct[i - 1]);
            sumSq += d * d;
        }
        flowRoughness = static_cast<float>(std::sqrt(sumSq / static_cast<double>(na - 1)));
    }

    outFeatures = {
        brewRatio,       doseG,     totalTime,       ttfd,          meanFlow,      earlyFlow,
        midFlow,         lateFlow,  flowSlope,       peakP,         meanPressure,  pressureDecline,
        flowRoughness,   meanTemp,
    };
    return true;
}

bool predictCandidateTrajectoryFeatures(const CombinedHydraulicPredictor &predictor, float hydraulicGrindSteps,
                                        float doseG, float targetYieldG, const std::array<float, 3> &splineDelta,
                                        float basePressureBar, float tailPressureBar, float temperatureC,
                                        TrajectoryFeatureVec &outFeatures) {
    outFeatures.fill(0.0f);
    if (!std::isfinite(hydraulicGrindSteps) || !std::isfinite(doseG) || doseG <= 0.1f || !std::isfinite(targetYieldG) ||
        targetYieldG < 5.0f) {
        return false;
    }

    const HydraulicPhysicsConfig &phys = predictor.physics();
    const float dtS = 0.5f; // Coarse 2 Hz forward rollout keeps 65-candidate scoring < 3 ms on ESP32-S3.
    const float pStart = std::clamp(std::isfinite(basePressureBar) ? basePressureBar : 8.5f, 2.0f, 11.0f);
    const float pTail = std::clamp(std::isfinite(tailPressureBar) ? tailPressureBar : 5.5f, 1.5f, pStart);
    const float tempC = (std::isfinite(temperatureC) && temperatureC >= 60.0f && temperatureC <= 110.0f)
                            ? temperatureC
                            : 93.0f;
    const SplineTrajectoryBasis splineBasis{3, 1.0f};
    const float mPhys = physicsPriorLogConductance(hydraulicGrindSteps, doseG, phys);

    std::vector<TrajectorySamplePoint> sim;
    sim.reserve(120);

    float pressure = 1.5f;
    float waterMl = 0.0f;
    float weightG = 0.0f;
    float elapsedS = 0.0f;
    constexpr float kPreinfusionS = 4.0f;

    for (int step = 0; step < 120; ++step) {
        elapsedS = static_cast<float>(step) * dtS;
        const bool inBrew = (elapsedS >= kPreinfusionS);
        const float progress = std::clamp(weightG / std::max(targetYieldG, 1.0f), 0.0f, 1.0f);
        float targetP = inBrew ? (pStart + (pTail - pStart) * progress) : 1.8f;

        const FeatureVec4 z = buildHydraulicFeature(waterMl, pressure, hydraulicGrindSteps, doseG);
        float gpMean = 0.0f, gpVar = 0.0f;
        predictor.residualGP().predict(z, gpMean, gpVar);
        const float ell = std::clamp(mPhys + gpMean, phys.logConductanceMin, phys.logConductanceMax);
        const float nominalQ = inBrew ? std::exp(ell) * std::sqrt(std::max(targetP, 0.2f)) : 0.0f;
        const float warpedQ =
            inBrew ? splineBasis.warpTarget(nominalQ, progress, splineDelta.data(), splineDelta.size(), 0.2f, 6.0f)
                   : 0.0f;

        // Invert orifice relation q = exp(ell)*sqrt(P) inside +/-2.5 bar corridor to estimate realized pressure & flow.
        if (inBrew && std::fabs(warpedQ - nominalQ) > 1e-3f) {
            const float reqP = (warpedQ * warpedQ) / std::max(std::exp(2.0f * ell), 1e-4f);
            targetP = std::clamp(reqP, std::max(1.0f, targetP - 2.5f), std::min(11.5f, targetP + 2.5f));
        }

        pressure = pressure + (targetP - pressure) * std::clamp(dtS / std::max(phys.pressureTrackTauS, 0.1f), 0.1f, 1.0f);
        const float qBev = inBrew ? std::clamp(std::exp(ell) * std::sqrt(std::max(pressure, 0.1f)), 0.1f, 6.0f) : 0.0f;
        const float qPump = qBev / std::max(predictor.bevToPumpRatio(), 0.5f) + (inBrew ? 0.0f : 1.2f);
        waterMl += qPump * dtS;
        weightG += qBev * dtS;

        TrajectorySamplePoint pt;
        pt.elapsedS = elapsedS;
        pt.pressureBar = pressure;
        pt.beverageFlowGS = qBev;
        pt.weightG = weightG;
        pt.temperatureC = tempC;
        pt.valveOpen = true;
        sim.push_back(pt);

        if (weightG >= targetYieldG && step >= 6) {
            break;
        }
    }

    return extractTrajectoryFeatures(sim.data(), sim.size(), doseG, weightG, elapsedS, outFeatures);
}

} // namespace EspressoRL
