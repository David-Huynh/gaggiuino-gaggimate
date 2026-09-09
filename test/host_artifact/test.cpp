#include <display/core/RecipeConfirmation.h>
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

int main(int argc, char **argv) {
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
    assert(store.remove("confirmed-recipe"));

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
