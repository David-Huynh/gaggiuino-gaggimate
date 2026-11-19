#include "RecipeStateHarness.h"
#include <display/core/RecipeConfirmation.h>
#include "ProjectionHarness.h"
#include <display/plugins/autotuning/local/RecipePrompt.h>
#include <display/plugins/autotuning/LifecycleReceipt.h>
#include <Arduino.h>
#include <LittleFS.h>
#include <cassert>
#include <limits>
#include <fstream>
#include <iterator>
#include <type_traits>
#include <display/plugins/autotuning/local/CompletedShotArtifactStore.h>
#include <display/plugins/autotuning/AutoTuningJsonCodec.h>
#include <display/util/AtomicFile.h>
#include <display/core/RecommendationContext.h>

extern "C" unsigned long millis() { return 1000; }
extern "C" void delay(uint32_t) {}
extern "C" void yield() {}

static AutoTuning::CompletedShotArtifact fixture() {
    AutoTuning::CompletedShotArtifact artifact;
    artifact.record.shotId = "recovery-fixture";
    artifact.record.machineId = "fixture-machine";
    artifact.record.timestamp = 1720000000;
    artifact.record.beverageOutG = 36;
    artifact.record.shotTimeS = 30;
    artifact.record.weightSource = "hardware_scale";
    artifact.record.flowSource = "hardware_scale";
    artifact.record.excludeFromLocalOptimization = true;
    artifact.record.history.reserved = true;
    artifact.record.history.id = 42;
    artifact.record.history.finalMeasuredWeightG = 36;
    artifact.record.history.phaseTransitionCount = 1;
    artifact.record.history.phaseTransitions[0].phaseName = "extraction";
    artifact.record.history.phaseTransitions[0].exitReason = 7;
    artifact.record.grindFollowed = false;
    artifact.completion.shotId = artifact.record.shotId;
    artifact.committedAt = artifact.record.timestamp + 30;
    for (unsigned i = 0; i < 240; ++i) {
        AutoTuning::ShotSample sample;
        sample.elapsedMs = i * 250;
        sample.pressure = 9;
        sample.temperature = 93;
        sample.weight = sample.measuredWeight = i * 0.15f;
        sample.valveOpen = true;
        artifact.samples.push_back(sample);
    }
    artifact.bindSamples();
    return artifact;
}

void testRememberedRecipe() {
    RecipeStateController controller;
    MQTTPlugin mqtt{&controller};
    auto &r = mqtt.latestRecommendation;
    r.recommendationId = "saved-rec"; r.sourceShotId = "shot";
    r.machineId = "machine"; r.installId = "install";
    r.beanContextId = "bean"; r.grinderContextId = "grinder"; r.profileId = "profile";
    r.optimizationRunId = "run"; r.comparisonAnchorShotId = "shot";
    r.comparisonMode = AutoTuning::ComparisonMode::BestIncumbent;
    r.mode = AutoTuning::RecommendationMode::CpboBestIncumbent;
    r.status = AutoTuning::RecommendationStatus::Pending;
    r.nextDoseG = 20; r.targetYieldG = 40; r.targetRatio = 2;
    r.projectedRelativeStepFromReference = 2; r.createdAt = r.updatedAt = 1720000000;
    mqtt.persistRecommendationView();
    const auto saves = controller.settings.saves;
    assert(!controller.settings.savedRecommendation.isEmpty());
    r.shownCount = 20; r.updatedAt += 100;
    mqtt.persistRecommendationView();
    assert(controller.settings.saves == saves); // Retained duplicates do not wear flash.
    AutoTuning::Recommendation rebooted;
    String error;
    assert(AutoTuningJsonCodec::deserializeRecommendation(controller.settings.savedRecommendation, rebooted, error));
    assert(rebooted.nextDoseG == 20 && rebooted.recommendationId == "saved-rec");

    AutoTuning::ConfirmedRecipe actual;
    actual.shotId = "shot"; actual.machineId = "machine"; actual.profileId = "profile";
    actual.timestamp = 1720000030;
    actual.recipe.beanContextId = "bean"; actual.recipe.grinder.contextId = "grinder";
    actual.recipe.grinder.absoluteReferenceStep = 10;
    actual.recipe.grinder.currentAbsoluteStep = 14;
    actual.recipe.grinder.relativeStepsFromReference = 4;
    actual.recipe.grinder.observed = true; actual.recipe.doseTargetG = 24;
    actual.recipe.targetYieldG = 36;
    Event event; event.setPayload(actual);
    mqtt.rememberConfirmedRecipe(event);
    assert(controller.settings.dose == 24 && controller.settings.persistedDose == 24);
    assert(mqtt.publishedDose == 24 && mqtt.plugins.changes == 1);
    JsonDocument contexts;
    assert(!deserializeJson(contexts, controller.settings.contexts));
    assert(contexts[0]["current_absolute_step"].as<float>() == 14);
    assert(contexts[0]["current_relative_step"].as<float>() == 4);
    controller.settings.dose = 22; controller.settings.changedAt = actual.timestamp + 1;
    mqtt.rememberConfirmedRecipe(event);
    assert(controller.settings.dose == 22); // An older prompt cannot undo a newer Apply.
    controller.settings.changedAt = 0;
    controller.profiles.selected.id = "other";
    mqtt.rememberConfirmedRecipe(event);
    assert(controller.settings.dose == 22);
    controller.profiles.selected.id = "profile";
    controller.active = true;
    mqtt.rememberConfirmedRecipe(event);
    assert(controller.settings.dose == 22);
    controller.active = false;
    auto recalibrated = actual;
    recalibrated.recipe.grinder.micronsPerStep = 20;
    Event calibrationChange; calibrationChange.setPayload(recalibrated);
    mqtt.rememberConfirmedRecipe(calibrationChange);
    assert(controller.settings.dose == 22);
    controller.settings.failSave = true;
    Event retry; retry.setPayload(actual);
    mqtt.publishedDose = 0;
    mqtt.rememberConfirmedRecipe(retry);
    assert(retry.getInt("recipe_state_retry") == 1 && mqtt.publishedDose == 0);
    controller.settings.failSave = false;
    Event recovered; recovered.setPayload(actual);
    mqtt.rememberConfirmedRecipe(recovered);
    assert(recovered.getInt("recipe_state_retry") == 0 && mqtt.publishedDose == 24);
    puts("PASS recipe state: saved recommendation roundtrip, deduplicated writes, 24 g actual inputs before publish, stale/context/active guards");
}

int main(int argc, char **argv) {
    if (argc == 1) testRememberedRecipe();
    auto recipeArtifact = fixture();
    recipeArtifact.bindSamples();
    auto recipe = recipeArtifact.record;
    recipe.recipe.grinder.currentAbsoluteStep = 12.5f;
    recipe.recipe.grinder.absoluteReferenceStep = 10.0f;
    recipe.recipe.grinder.relativeStepsFromReference = 2.5f;
    recipe.recipe.grinder.micronsPerStep = 10.0f;
    recipe.recipe.grinder.observed = false;
    recipe.recipe.doseTargetG = 18.0f;
    recipe.recipe.targetYieldG = 36.0f;
    recipe.recommendation.recommendationId = "recommendation";
    recipe.recommendation.projectedRelativeStepFromReference = 2.5f;
    recipe.recommendation.nextDoseG = 18.0f;
    auto confirmed = recipe;
    assert(AutoTuning::confirmRecipe(confirmed, {AutoTuning::RecipeAnswer::Confirm}));
    assert(confirmed.recipe.grinder.observed && confirmed.doseTargetConfirmed && !confirmed.doseObserved);
    assert(confirmed.grindFollowed == true && confirmed.doseFollowed == true);
    auto edited = recipe;
    assert(AutoTuning::confirmRecipe(edited, {AutoTuning::RecipeAnswer::Change, 14.0f, 19.0f}));
    assert(edited.recipe.grinder.relativeStepsFromReference == 4.0f);
    assert(edited.recipe.grinder.relativeMicronsFromReference == 40.0f);
    assert(edited.recipe.grinder.currentAbsoluteStep == 14.0f);
    assert(edited.recipe.doseTargetG == 19.0f && edited.doseTargetConfirmed);
    assert(edited.grindFollowed == false && edited.doseFollowed == false);
    auto actual = recipe;
    actual.recipe.beanContextId = "bean";
    actual.recipe.grinder.contextId = "grinder";
    actual.profile.id = "profile";
    assert(AutoTuning::confirmRecipe(actual, {AutoTuning::RecipeAnswer::Change, 14.0f, 24.0f}));
    auto remembered = AutoTuning::confirmedRecipe(actual);
    auto matches = [&](std::string const &profile, AutoTuning::Timestamp changedAt) {
        return AutoTuning::canRememberRecipe(remembered, "fixture-machine", "bean", "grinder", profile,
                                             AutoTuning::TasteGoal::balanced(), changedAt);
    };
    assert(matches("profile", 0)); // 24 g is actual evidence, not an optimizer bound.
    assert(!matches("other-profile", 0));
    assert(!matches("profile", remembered.timestamp + 1)); // Do not overwrite a newer Apply.
    remembered.recipe.doseTargetG = std::numeric_limits<float>::quiet_NaN();
    assert(!matches("profile", 0));
    remembered.recipe.doseTargetG = 24;
    remembered.recipe.grinder.observed = false;
    assert(!matches("profile", 0));
    recipe.beverageOutG = 47.3f; // Measurement is not a new target.
    auto measuredActual = recipe;
    measuredActual.doseObserved = true;
    measuredActual.measuredDoseG = 24;
    assert(AutoTuning::confirmRecipe(measuredActual, {AutoTuning::RecipeAnswer::Confirm}));
    assert(measuredActual.recipe.doseTargetG == 24 && measuredActual.doseObserved);
    assert(measuredActual.doseFollowed == false);
    assert(measuredActual.recipe.targetYieldG == 36.0f && measuredActual.beverageOutG == 47.3f);
    JsonDocument savedRecipe;
    assert(AutoTuningJsonCodec::writeShotRecord(edited, savedRecipe));
    AutoTuningJsonCodec::DecodedShotRecord decodedRecipe;
    String recipeError;
    assert(AutoTuningJsonCodec::parseShotRecord(savedRecipe.as<JsonObjectConst>(), decodedRecipe, recipeError));
    assert(decodedRecipe.record.recipe.grinder.currentAbsoluteStep == 14.0f);
    assert(decodedRecipe.record.doseTargetConfirmed);
    auto relative = recipe;
    relative.recipe.grinder.currentAbsoluteStep.reset();
    relative.recipe.grinder.absoluteReferenceStep.reset();
    relative.recipe.grinder.stepDirection = "higher_is_coarser";
    assert(AutoTuning::confirmRecipe(relative, {AutoTuning::RecipeAnswer::Change, -1.5f, 17.0f}));
    assert(relative.recipe.grinder.relativeStepsFromReference == -1.5f);
    assert(relative.recipe.grinder.relativeMicronsFromReference == 15.0f);
    auto unknown = confirmed;
    assert(AutoTuning::confirmRecipe(unknown, {AutoTuning::RecipeAnswer::Unknown}));
    assert(!unknown.recipe.grinder.observed && !unknown.doseTargetConfirmed && !unknown.grindFollowed);
    auto invalid = recipe;
    assert(!AutoTuning::confirmRecipe(invalid, {AutoTuning::RecipeAnswer::Change, 14.0f, -1.0f}));
    assert(invalid.recipe.grinder.currentAbsoluteStep == 12.5f);
    assert(!AutoTuning::confirmRecipe(invalid, {AutoTuning::RecipeAnswer::Change, std::numeric_limits<float>::quiet_NaN(), 18.0f}));
    auto measured = recipe;
    measured.doseObserved = true;
    measured.measuredDoseG = 18.4f;
    assert(AutoTuning::confirmRecipe(measured, {AutoTuning::RecipeAnswer::Confirm}));
    assert(measured.doseObserved && measured.measuredDoseG == 18.4f && !measured.doseTargetConfirmed);
    assert(AutoTuning::confirmRecipe(measured, {AutoTuning::RecipeAnswer::Change, 13.0f, 18.4f}));
    assert(measured.doseObserved); // Editing grind alone does not erase measured dose provenance.


    JsonDocument receipt;
    const std::string digest(64, 'a');
    receipt["event_type"] = "lifecycle_ack";
    receipt["schema_version"] = 1;
    receipt["machine_id"] = "machine";
    receipt["delivery_id"] = digest;
    receipt["outcome"] = "accepted";
    const auto ack = [&]() { return AutoTuning::acceptedLifecycleReceipt(receipt.as<JsonObjectConst>(), "machine", digest); };
    assert(ack() == true);
    receipt["outcome"] = "permanent_rejection";
    assert(ack() == false);
    receipt["outcome"] = "broker_accepted";
    assert(!ack().has_value());
    receipt["outcome"] = "accepted";
    receipt["machine_id"] = "another-machine";
    assert(!ack().has_value());
    receipt["machine_id"] = "machine";
    receipt["delivery_id"] = std::string(64, 'b');
    assert(!ack().has_value());
    receipt["delivery_id"] = digest;
    receipt["schema_version"] = "1";
    assert(!ack().has_value());
    receipt["schema_version"] = 1;
    receipt["unexpected"] = true;
    assert(!ack().has_value());

    AutoTuning::Recommendation recommendation;
    recommendation.machineId = "machine";
    recommendation.beanContextId = "bean";
    recommendation.grinderContextId = "grinder";
    recommendation.profileId = "A";
    const auto goal = AutoTuning::TasteGoal::balanced();
    auto otherGoal = goal;
    otherGoal.mode = AutoTuning::TasteGoalMode::Custom;
    otherGoal.targets[static_cast<size_t>(AutoTuning::TasteAttribute::Sweet)] = AutoTuning::TasteLevel::High;
    assert(!AutoTuning::recommendationContextMatches(recommendation, "machine", "bean", "grinder", "A", otherGoal));

    assert(AutoTuning::recommendationContextMatches(recommendation, "machine", "bean", "grinder", "A", goal));
    assert(!AutoTuning::recommendationContextMatches(recommendation, "machine", "bean", "grinder", "B", goal));
    assert(!AutoTuning::recommendationContextMatches(recommendation, "machine", "bean", "grinder", "", goal));
    assert(!AutoTuning::recommendationContextMatches(recommendation, "other", "bean", "grinder", "A", goal));
    assert(!AutoTuning::recommendationContextMatches(recommendation, "machine", "other", "grinder", "A", goal));
    assert(!AutoTuning::recommendationContextMatches(recommendation, "machine", "bean", "other", "A", goal));
    recommendation.recommendationId = "restore-1";
    recommendation.mode = AutoTuning::RecommendationMode::RestoreBest;
    recommendation.preferenceFeedbackRequired = false;
    assert(AutoTuning::restoreBestRequestMatches(recommendation, "restore-1"));
    assert(!AutoTuning::restoreBestRequestMatches(recommendation, ""));
    assert(!AutoTuning::restoreBestRequestMatches(recommendation, "stale-request"));
    recommendation.preferenceFeedbackRequired = true;
    assert(!AutoTuning::restoreBestRequestMatches(recommendation, "restore-1"));
    recommendation.preferenceFeedbackRequired = false;
    recommendation.mode = AutoTuning::RecommendationMode::CpboBestIncumbent;
    assert(!AutoTuning::restoreBestRequestMatches(recommendation, "restore-1"));
    assert(AutoTuning::recommendationModeFromKey("restore_best") == AutoTuning::RecommendationMode::RestoreBest);
    recommendation.profileId.clear();
    assert(!AutoTuning::recommendationContextMatches(recommendation, "machine", "bean", "grinder", "A", goal));
    static_assert(!std::is_default_constructible<AutoTuning::PreferenceFeedback>::value,
                  "Feedback must require an explicit choice, never default to a tie");
    if (argc == 2 && (std::string(argv[1]) == "--export-confirmed-recipe" ||
                      std::string(argv[1]) == "--export-unknown-recipe")) {
        auto exported = std::string(argv[1]) == "--export-confirmed-recipe" ? edited : unknown;
        exported.machineId = "gaggimate:AA_BB";
        exported.localOptimizationEnabled = true;
        exported.excludeFromLocalOptimization = false;
        JsonDocument document;
        assert(AutoTuningJsonCodec::writeShotRecord(exported, document));
        std::string encoded;
        serializeJson(document, encoded);
        puts(encoded.c_str());
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--export-shot") {
        auto artifact = fixture();
        artifact.bindSamples();
        artifact.record.machineId = "gaggimate:AA_BB";
        JsonDocument document;
        assert(AutoTuningJsonCodec::writeShotRecord(artifact.record, document));
        AutoTuning::ShotDeliveryAttempt attempt{artifact.record.shotId, 7, false};
        assert(AutoTuningJsonCodec::writeShotDelivery(attempt, document["delivery"].to<JsonObject>()));
        std::string encoded;
        serializeJson(document, encoded);
        puts(encoded.c_str());
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--verify-receipt") {
        std::ifstream file(argv[2]);
        std::string encoded((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        JsonDocument document;
        assert(!deserializeJson(document, encoded));
        AutoTuning::ShotDeliveryAcknowledgement ack;
        String error;
        assert(AutoTuningJsonCodec::parseShotAcknowledgement(document.as<JsonVariantConst>(), ack, error));
        assert(ack.recordRevision == 7 && ack.shotId == "recovery-fixture");
        assert(ack.preferenceRequest && ack.preferenceRequest->anchor);
        assert(ack.preferenceRequest->anchor->doseG == 18);
        assert(ack.preferenceRequest->anchor->profileLabel.find("espresso") != std::string::npos);
        // Keep the receipt envelope strict and reject changes to its orientation.
        document["record_revision"] = true;
        assert(!AutoTuningJsonCodec::parseShotAcknowledgement(document.as<JsonVariantConst>(), ack, error));
        document["record_revision"] = 7;
        document["preference_request"]["new_shot_id"] = "different-shot";
        assert(!AutoTuningJsonCodec::parseShotAcknowledgement(document.as<JsonVariantConst>(), ack, error));
        document["preference_request"]["new_shot_id"] = "recovery-fixture";
        document["preference_request"]["anchor"]["dose_g"] = -1;
        assert(!AutoTuningJsonCodec::parseShotAcknowledgement(document.as<JsonVariantConst>(), ack, error));
        document["preference_request"]["anchor"]["dose_g"] = 18;
        document["attempt_id"] = "obsolete";
        assert(!AutoTuningJsonCodec::parseShotAcknowledgement(document.as<JsonVariantConst>(), ack, error));
        puts("PASS Python receipt parsed by firmware; invalid envelopes rejected");
        return 0;
    }
    CompletedShotArtifactStore store;
    assert(store.begin());
    // Reproduce boot recovery with a persisted shot spanning multiple 4 KB
    // write quanta, including rebuilding a partial projection after a restart.
    auto historyArtifact = fixture();
    historyArtifact.record.shotId = historyArtifact.completion.shotId = "history-recovery";
    historyArtifact.samples[1].elapsedMs = 380; // actual late capture, not a fixed interval
    historyArtifact.samples[1].waterPumped = 1.25f;
    historyArtifact.bindSamples();
    historyArtifact.samples[0].waterPumped = -1.0f;
    assert(!store.write(historyArtifact));
    historyArtifact.samples[0].waterPumped.reset();
    assert(store.write(historyArtifact));
    AutoTuning::CompletedShotArtifact restoredHistory;
    assert(store.load("history-recovery", restoredHistory));
    ShotHistoryPlugin history;
    assert(history.ensureProjection(restoredHistory));
    auto historyFile = LittleFS.open("/h/000042.slog", FILE_READ);
    assert(historyFile.size() == sizeof(ShotLogHeader) + 240 * sizeof(ShotLogSample));
    const auto expectedHistory = historyFile.node->bytes;
    ShotLogHeader historyHeader{};
    assert(historyFile.read(reinterpret_cast<uint8_t *>(&historyHeader), sizeof(historyHeader)) == sizeof(historyHeader));
    assert(historyHeader.sampleCount == 240 && historyHeader.durationMs == 59750);
    assert(historyHeader.phaseTransitionCount == 1 && historyHeader.phaseTransitions[0].transitionReason == 7);
    for (unsigned i = 0; i < 240; ++i) {
        ShotLogSample sample{};
        assert(historyFile.read(reinterpret_cast<uint8_t *>(&sample), sizeof(sample)) == sizeof(sample));
        assert(sample.t == (i == 1 ? 380 : i * 250) && sample.ct == 930 && sample.cp == 90);
        assert(sample.wp == (i == 1 ? 13 : SHOT_LOG_WATER_PUMPED_UNKNOWN));
    }
    assert(history.index.size() == 1 && history.index.at(42).avgTemp == 930);
    assert(history.savedNotes.indexOf("history-recovery") >= 0);
    assert(history.ensureProjection(restoredHistory));
    assert(history.index.size() == 1 && historyFile.node->bytes == expectedHistory);
    historyFile.node->bytes[offsetof(ShotLogHeader, version)] = 7;
    const auto oldHistory = historyFile.node->bytes;
    assert(history.ensureProjection(restoredHistory));
    assert(historyFile.node->bytes == oldHistory);
    historyFile.node->bytes.resize(100); // interrupted/partial old history file
    assert(history.ensureProjection(restoredHistory));
    assert(LittleFS.open("/h/000042.slog").node->bytes == expectedHistory);
    assert(!LittleFS.exists("/h/000042.slog.tmp"));
    restoredHistory.record.history.phaseTransitionCount = restoredHistory.record.history.phaseTransitions.size() + 1;
    assert(!history.ensureProjection(restoredHistory));
    assert(LittleFS.open("/h/000042.slog").node->bytes == expectedHistory);
    assert(LittleFS.remove("/h/000042.slog"));
    history.machine.clearedBefore = 43;
    restoredHistory.record.history.phaseTransitionCount = 1;
    assert(history.ensureProjection(restoredHistory));
    assert(!LittleFS.exists("/h/000042.slog"));
    assert(store.remove("history-recovery"));
    puts("PASS history projection: persisted-shot recovery, old logs retained, invalid metadata, bulk deletion respected");
    recipeArtifact.record = edited;
    recipeArtifact.record.shotId = "confirmed-recipe";
    recipeArtifact.completion.shotId = "confirmed-recipe";
    recipeArtifact.disposition.doseConfirmationRequired = false;
    recipeArtifact.bindSamples();
    assert(store.write(recipeArtifact));
    AutoTuning::CompletedShotArtifact recoveredRecipe;
    assert(store.load("confirmed-recipe", recoveredRecipe));
    assert(!recoveredRecipe.disposition.doseConfirmationRequired);
    assert(recoveredRecipe.record.recipe.grinder.currentAbsoluteStep == 14.0f);
    assert(recoveredRecipe.record.doseTargetConfirmed);
    // The worker's copied prompt survives destruction of the loaded artifact;
    // the UI must not need to reload it or retain sample/context references.
    auto recipePrompt = RecipePrompt::fromShot(recoveredRecipe.record, 18.0f);
    assert(store.remove("confirmed-recipe"));
    recoveredRecipe = {};
    Event recipeEvent;
    recipePrompt.writeTo(recipeEvent);
    assert(recipeEvent.getFloat("grind_setting") == 14.0f);
    assert(recipeEvent.getFloat("dose_target_g") == 19.0f);
    assert(recipeEvent.getInt("has_grind_setting") == 1);
    assert(recipeEvent.getInt("grind_is_absolute") == 1);
    assert(recipeEvent.getInt("dose_measured") == 0);

    auto promptShot = recipe;
    promptShot.doseObserved = true;
    promptShot.measuredDoseG = 18.7f;
    promptShot.recipe.grinder.absoluteReferenceStep.reset();
    Event measuredPrompt;
    RecipePrompt::fromShot(promptShot, 18.0f).writeTo(measuredPrompt);
    assert(measuredPrompt.getFloat("dose_target_g") == 18.7f);
    assert(measuredPrompt.getInt("dose_measured") == 1);
    assert(measuredPrompt.getFloat("grind_setting") == 2.5f);
    assert(measuredPrompt.getInt("grind_is_absolute") == 0);

    promptShot.doseObserved = false;
    promptShot.recipe.doseTargetG.reset();
    promptShot.recipe.grinder.relativeStepsFromReference.reset();
    Event missingPrompt;
    RecipePrompt::fromShot(promptShot, 17.5f).writeTo(missingPrompt);
    assert(missingPrompt.getFloat("dose_target_g") == 17.5f);
    assert(missingPrompt.getInt("has_grind_setting") == 0);
    assert(missingPrompt.getInt("dose_measured") == 0);

    auto artifact = fixture();
    artifact.bindSamples();
    AutoTuning::PreferenceRequest request;
    request.installId = "install";
    request.optimizationRunId = "run";
    request.newShotId = artifact.record.shotId;
    request.anchorShotId = "anchor";
    request.comparisonMode = AutoTuning::ComparisonMode::BestIncumbent;
    AutoTuning::PreferenceAnchorSummary anchor;
    anchor.timestamp = 1720000000;
    anchor.doseG = 18;
    anchor.targetYieldG = 36;
    anchor.profileLabel = "Morning espresso";
    request.anchor = anchor;
    artifact.completion.preferenceRequest = request;
    assert(store.write(artifact));
    auto path = store.pathFor(artifact.record.shotId.c_str());
    assert(LittleFS.open(path).size() > 4096); // exercises chunked IO and validation
    const auto hash = artifact.payloadHash;
    assert(store.write(artifact)); // idempotent write still validates the prior file
    assert(artifact.payloadHash == hash);
    for (unsigned cycle = 0; cycle < 5; ++cycle) {
        // Simulate power loss after writing a complete .tmp but before rename.
        assert(LittleFS.rename(path, AtomicFile::temporaryPath(path)));
        CompletedShotArtifactStore restarted;
        assert(restarted.begin());
        assert(LittleFS.exists(path) && !LittleFS.exists(AtomicFile::temporaryPath(path)));
        AutoTuning::CompletedShotArtifact loaded;
        loaded.payloadHash = "stale";
        assert(restarted.load(artifact.record.shotId.c_str(), loaded));
        assert(loaded.samples.size() == 240 && loaded.record.samples.data() == loaded.samples.data());
        assert(loaded.record.excludeFromLocalOptimization && loaded.record.grindFollowed == false);
        assert(loaded.record.history.id == 42 && loaded.record.history.phaseTransitions[0].phaseName == "extraction");
        assert(loaded.record.history.phaseTransitions[0].exitReason == 7);
        assert(loaded.payloadHash == hash);
        assert(loaded.completion.preferenceRequest && loaded.completion.preferenceRequest->anchor);
        assert(loaded.completion.preferenceRequest->anchor->profileLabel == "Morning espresso");
    }
    // Invalid pending write must not replace the valid prior artifact.
    assert(LittleFS.rename(path, AtomicFile::backupPath(path)));
    auto corrupt = LittleFS.open(AtomicFile::temporaryPath(path), FILE_WRITE);
    corrupt.write(0xc1); // invalid MessagePack token
    corrupt.close();
    assert(store.begin());
    AutoTuning::CompletedShotArtifact recovered;
    assert(store.load(artifact.record.shotId.c_str(), recovered));
    assert(recovered.payloadHash == hash);
    // Reject a conflicting same revision and malformed sample data.
    artifact.record.beverageOutG = 40;
    assert(!store.write(artifact));
    artifact.revision++;
    assert(store.write(artifact));
    JsonDocument shot;
    assert(AutoTuningJsonCodec::writeShotRecord(artifact.record, shot));
    shot["pressure"][0] = "invalid";
    AutoTuningJsonCodec::DecodedShotRecord decoded;
    String error;
    assert(!AutoTuningJsonCodec::parseShotRecord(shot.as<JsonVariantConst>(), decoded, error));
    assert(!error.isEmpty());
    puts("PASS artifacts: 240 samples, repeated pending-write recovery, invalid temp/backup recovery, revision and input validation");
}
