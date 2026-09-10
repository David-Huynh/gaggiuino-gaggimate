# Auto-Tuning Shot Delivery

Completed auto-tuning shots use this commit and delivery sequence:

1. The brew process reaches its terminal condition.
2. The display sends the stopped process state to the controller, including
   pump off and valve closed.
3. Shot capture records the control-cutoff observation.
4. Capture transfers the completed typed shot to a bounded RAM queue.
5. The storage worker atomically commits one canonical completed-shot artifact
   under `/rll/c`.
6. Replay state, compact summaries, shot history, and community work are
   idempotent projections of that committed artifact.
7. The user confirms the captured grind and dose, supplies the values actually
   used, or answers that they are unsure. A measured dose does not establish
   that the manual grinder was moved.
8. The shot is delivered to the local optimizer transport and independently
   queued for optional community upload.
9. The comparison prompt is released only after EspressoRL acknowledges that
   the shot was accepted or was already processed.

Network and filesystem work therefore cannot delay the command that ends the
brew. Controller acknowledgement and retry remain asynchronous; shot delivery
does not wait on network services.

Capture transfers each completed typed shot into a bounded in-memory queue.
One low-priority replay worker owns shot encoding, replay persistence, delivery
and community scans, acknowledgement updates, and local-store status scans.
Recipe confirmation reads and writes also run on this worker. It returns copied
recipe values and confirmation result events that the controller loop drains
before invoking UI callbacks. Restoring or displaying the recipe prompt must
never load a full shot artifact on `loopTask`: the artifact and decoder frames
can exhaust its 8 KB stack even when the heap has plenty of free memory.
Migration-only decoded records use scoped heap storage instead of reserving
large stack frames on every canonical load. A pending or retrying replay cannot
hold up a later brew command. The queue accepts at most four unwritten shots;
an exhausted queue is reported as a capture failure instead of falling back to
synchronous filesystem work. Queue acceptance is not reported as durable
commit; only the validated atomic artifact rename establishes durability.

Recipe confirmation has three distinct milestones: the answer is saved on the
machine, EspressoRL acknowledges the shot, and a comparison or recommendation
becomes available. The browser displays these separately; saving the answer
does not imply the optimizer received it or generated a recommendation.

Display builds verify the embedded web bundle against hashes of its sources
and generated files. Changed or missing assets trigger a web build and packing
before firmware compilation. Install Node and run `npm ci` in `web/` once;
`python scripts/build_webui.py` also builds and embeds explicitly on Windows.
A failed web build stops firmware compilation instead of silently shipping an
old UI that sends obsolete confirmation messages.

Run `python scripts/check_recipe_stack.py --environment display` (or the
headless environment) after building to check the Xtensa prompt/loader frame
budgets. These checks do not replace a hardware test of pending-shot recovery,
recipe confirmation, and delivery through to a comparison or recommendation.

## Flash Coordination

Every runtime LittleFS read, write, scan, rename, verification, and prune passes
through `StorageCoordinator`. Brew, steam, hot-water, flush, and grind starts
take a high-priority process lease. A process waiter immediately closes the
flash gate, and no new flash lease can start until that process ends. The
controller sends the stopped control frame before releasing the process lease,
then dispatches post-process observers.

Flash work is split into at most 4 KB data quanta. Directory scans and pruning
release and reacquire their lease at bounded checkpoints. A process request can
therefore wait only for the current bounded filesystem operation, not an entire
history rebuild or queue scan. No LittleFS read is allowed while a process
lease exists, so process control, live telemetry, active profile state, and
capture buffers must already be resident in RAM.

The coordinator is the arbiter; callers do not rely on a racy
`if (!process_active)` check. Storage helpers assert that internal operations
hold a flash lease. Code must not hold a flash lease while entering a store
whose mutex is acquired before its own flash lease.

## Commit Recovery

The canonical artifact contains the typed `ShotRecord`, `ShotCompletion`,
capture disposition, history metadata, all fixed-cadence samples, a monotonic
artifact revision, and its SHA-256 payload identity. It is MessagePack encoded,
verified after writing, and committed by recoverable temporary/backup renames.
An exact retry is idempotent. A different payload cannot replace the same
revision, and a lower revision cannot replace a newer artifact.

Replay JSON stores delivery and prompt state plus the canonical artifact
revision and hash; it does not duplicate a version-3 shot payload. Corrections,
dose confirmation, timestamp repair, or an acknowledgement that adds a
comparison request create a new canonical revision before updating projections.
Reset and pruning remove projections first and the canonical artifact last.

At boot, temporary and backup artifacts are recovered first. Every committed
artifact is then reconciled into any missing replay, summary, and shot-history
projection. Each operation can be repeated safely. A reset between projection
steps can therefore leave extra recoverable canonical data, but never a newly
committed replay whose canonical shot was not durable.

The auto-tuning router depends on an optimizer transport port. It does not read
Home Assistant or MQTT settings directly. The MQTT adapter reports whether its
transport is configured and connected, while capture requires a configured,
implemented provider. A temporary broker outage therefore keeps replay capture
active; selecting the unimplemented on-board provider does not create
undeliverable snapshots.

## Architecture Boundary

`ShotRecord`, `ShotSample`, `Recommendation`, `TasteGoal`, `DeliveryState`, and
their lifecycle enums are framework-independent domain models. Core routing
depends only on typed optimizer, record-store, and community-upload ports. It
does not include ArduinoJson, MQTT topics, Supabase requests, firmware settings,
or UI payloads.

Serialization is restricted to adapter boundaries. MQTT, Supabase, WebUI, and
mutable replay state use JSON. Canonical completed-shot persistence uses
MessagePack. Capture builds one typed sample vector and passes a non-owning
sample view to the record-store port; the queue takes an owned PSRAM copy before
the capture buffer can be reused.

The community adapter is divided into orchestration, payload validation, atomic
queue storage, and HTTP transport. Its worker consumes an adapter-owned settings
snapshot; credential changes are applied on the controller loop. The local
store similarly separates canonical shot artifacts, mutable delivery replay
state, compact summaries, and context snapshots, with all stores sharing the
same recoverable atomic-file utility.

### Gaggimate Source Layout

Gaggimate keeps registered plugin entry points in `src/display/plugins`, like
the rest of the firmware. Reusable auto-tuning adapter codecs and local-store
helpers live under `src/display/plugins/autotuning`; community-upload helpers
remain under `src/display/plugins/community` because community upload can run
without optimization. Pure typed models, routing, and ports remain in
`src/display/core`, while the protocol-independent atomic-file helper belongs
in `src/display/util`.

Community upload consent is independent from optimizer participation. With
community upload enabled and Auto Tuning disabled, the device captures and
queues anonymized shot records without delivering them to an optimizer,
attaching a recommendation, or opening dose and preference prompts. The local
replay is retained until the signed community queue accepts the record, so
registration and wall-clock synchronization may complete after the shot.

## Recipe Search Space

Advanced optimizer settings expose the CPBO grind radius, dose range, and
target-output range. Defaults are a 10-step grind radius, 6-30 g dose, and
5-250 g target output. These values define normalization and candidate search;
they are not machine-safety claims. Brew ratio is derived from target output
divided by dose and is not configured as a separate bound.

Advanced values remain inside a broad data-integrity envelope: grind radius
0.1-1,000 steps, dose 0.1-100 g, and target output 0.1-1,000 g. These outer
limits reject malformed settings and uploads; they do not replace the active
recipe domain or CPBO trust region. Upload ratios are checked against output
divided by dose rather than restricted to a separate ratio range.

The Active Session and touchscreen settings expose the current intended dose.
Manual-grinder users can therefore set dose without grind-by-weight hardware.
Accepting a recommendation always updates this intended target; the apply
acknowledgement still marks dose as manual unless hardware can apply it, and a
later confirmation or measurement determines whether the shot followed it.

The retained optimizer-settings event carries the configured physical domain
to EspressoRL. Recommendations are accepted only when their finite dose and
target output remain inside that user-authorized domain and their reported
ratio matches the derived value. Their grind delta must also remain inside the
configured grind radius. CPBO may use a tighter trust region within this
authorized domain; firmware validation is the final adapter-side apply gate.

## Off-Board Delivery

Shot profiles are published at QoS 1. EspressoRL returns a non-retained receipt
on `gaggimate/{topic_id}/rl/shot/ack` with one of four outcomes:

- `accepted` and `already_processed` finish local delivery.
- `transient_failure` retries the immutable payload with exponential backoff.
- `permanent_rejection` stops retries and leaves physical shot history intact.

`shot_id` and the canonical payload hash remain stable across retries.
`attempt_id` changes for every transport submission. A local publish acceptance
only moves the replay to `awaiting_ack`; it is not application delivery.
Duplicate shots and duplicate acknowledgements are idempotent. An
acknowledgement for an older attempt may complete the shot only when its shot
ID, payload hash, artifact revision, and encoding version still match the
active canonical payload. Oversized payloads are permanent delivery errors
rather than retries of unchanged bytes.

An accepted or already-processed receipt may also contain a validated
`preference_request` with the run ID, candidate shot ID, anchor shot ID,
comparison mode, snapshotted taste goal, and optional recommendation ID. The
worker stores this request in the replay before releasing `rl:shot:complete`.
This lets EspressoRL request a comparison for a replayed historical shot even
when that shot did not capture a recommendation before brewing. Comparison
identity is stored separately from recommendation follow-through, so replay
cannot falsely attribute the shot to a recommendation.

If no receipt arrives, the device treats it as transient unavailability and
retries. Delivery state, attempt count, next retry time, and terminal result are
persisted in the replay envelope, so reboot recovery uses the same shot ID and
payload. Automatic local retries do not enqueue another community upload.
Version-1 replay snapshots migrate on first retry; previously dispatched shots
remain prompt-complete so recovery cannot reopen stale comparison feedback.

The Auto Tuning page remains unchanged while delivery is healthy. Pending,
retrying, or rejected local shots replace the compact runtime summary and are
shown inside its existing details disclosure.

Prompt state is durable and revisioned. Its states are `processing`,
`delivery_retrying`, `awaiting_ack`, `awaiting_comparison`,
`comparison_available`, `recommendation_available`, `delivery_error`,
`resolved`, and `dismissed`. WebUI and LVGL claims include the shot ID and
prompt revision. Stale claims, responses, and releases are ignored. Dismissal
is presentation state; resolution records that no further answer is required.
Reconnect reconstructs the prompt from the latest persisted revision instead
of replaying transient UI events.

## Hardware-Scale Output

The hardware scale measures the drip tray. Its usable beverage observation ends
at control cutoff so OPV discharge cannot be mistaken for espresso output.

- `beverage_out_g` is the measured cutoff value.
- `predicted_final_beverage_out_g` is the separately predicted final value.
- `predictive_stop_delay_ms`, `predictive_stop_rate_g_per_s`, and
  `predictive_stop_lead_g` describe the prediction.
- A difference between measured cutoff, predicted final output, and target
  output is not itself a rejection reason.

## Dose Confirmation

Grind-by-weight measurements set `dose_observed`. For optimizer-bound shots,
the WebUI and LVGL UI ask whether the configured dose was followed when no
measurement exists. A positive answer sets `dose_target_confirmed` without
pretending that the dose was measured. A negative answer keeps dose masked for
optimization and upload. Upload-only capture records the configured dose as a
target without claiming it was measured or confirmed and does not prompt.

## Local Replay

LittleFS targets at most four terminal replay envelopes under `/rll/p` and
256 KB total. Canonical completed-shot artifacts live under `/rll/c` for the
same retained shots. Active, unacknowledged deliveries are never pruned
silently; they may temporarily exceed that retention target until delivery
becomes terminal or the user explicitly deletes local history. Shot history exposes
`req:history:rl:reprocess` only while a snapshot is available. Reprocessing
preserves the original shot ID and payload; the EspressoRL ingest path treats
exact repeats idempotently and rejects conflicting reuse of an existing shot
ID.

Replay files are written through validated temporary files and atomic renames.
Temporary and backup files are recovered at startup. Delivery waits until the
wall clock is valid; pre-sync captures receive a valid timestamp before their
first dispatch. Shot IDs use device randomness rather than wall-clock or uptime
values, so a reboot before time synchronization cannot reuse an ID.

Recommendation decisions, apply acknowledgements, preferences, corrections,
and reset events use a separate LittleFS MQTT outbox. Broker reconnects run on a
worker task, and the controller loop only performs non-blocking MQTT work.
Queued lifecycle events retain ordering and are retried after reconnect. The
outbox is bounded to 64 records and 128 KB; when full, a new user decision is
reported as unpersisted and its prompt remains available instead of pretending
the lifecycle event was accepted.

When community upload is enabled, a replay also remains protected until the
community adapter confirms that its endpoint-bound queue accepted the payload.
Shots captured while device registration is pending are replayed after
credentials arrive. Changing the upload endpoint deliberately clears records
bound to the old install identity and surfaces that action in upload status.
Community queue updates use temporary and backup files and recover interrupted
renames before endpoint identity checks run at startup.

Deleting local shot history also removes its compact auto-tuning summary and
replay state, then its canonical local artifact. It does not request remote
deletion.

## Local CPBO Convergence

Best-incumbent mode uses a local trust region. `new_better` moves its center to
the new incumbent, resets failures, and increments successes. `anchor_better`
and `tie` reset successes and increment failures. Three consecutive
improvements expand the region once; two consecutive non-improvements halve it
once. Counters reset after either resize.

A contraction that reaches the configured minimum radius marks the local run
converged. Local mode does not silently restart over the full recipe domain and
does not emit another recommendation. Resume Exploration restores the initial
radius around the current incumbent, clipped to the configured search space,
while retaining shots, comparisons, incumbent history, checkpoints, and the
transition audit. Global-previous mode remains available and searches the full
configured domain.
