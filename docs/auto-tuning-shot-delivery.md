# Auto-Tuning Shot Delivery

Completed auto-tuning shots use a safety-first delivery sequence:

1. The brew process reaches its terminal condition.
2. The display sends the stopped process state to the controller, including
   pump off and valve closed.
3. Shot capture records the control-cutoff observation.
4. A bounded immutable replay snapshot is written to LittleFS.
5. If grind by weight did not measure the dose, the user confirms whether the
   configured dose was followed.
6. The shot is delivered to the local optimizer transport and independently
   queued for optional community upload.
7. The comparison prompt is released only after EspressoRL acknowledges that
   the shot was accepted or was already processed.

Network and filesystem work therefore cannot delay the command that ends the
brew. Controller acknowledgement and retry remain asynchronous; shot delivery
does not wait on network services.

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

JSON is restricted to adapter boundaries: MQTT and Supabase transport, WebUI,
and LittleFS persistence. Capture builds one typed sample vector and passes a
non-owning sample view to the record-store port. Persistence owns the JSON
encoding and reconstructs an owned typed record before replaying it through a
transport port.

The community adapter is divided into orchestration, payload validation, atomic
queue storage, and HTTP transport. Its worker consumes an adapter-owned settings
snapshot; credential changes are applied on the controller loop. The local
store similarly separates immutable replays from compact summaries and context
snapshots, with all stores sharing the same recoverable atomic-file utility.

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

If no receipt arrives, the device treats it as transient unavailability and
retries. Delivery state, attempt count, next retry time, and terminal result are
persisted in the replay envelope, so reboot recovery uses the same shot ID and
payload. Automatic local retries do not enqueue another community upload.
Version-1 replay snapshots migrate on first retry; previously dispatched shots
remain prompt-complete so recovery cannot reopen stale comparison feedback.

The Auto Tuning page remains unchanged while delivery is healthy. Pending,
retrying, or rejected local shots replace the compact runtime summary and are
shown inside its existing details disclosure.

## Dose Confirmation

Grind-by-weight measurements set `dose_observed`. For optimizer-bound shots,
the WebUI and LVGL UI ask whether the configured dose was followed when no
measurement exists. A positive answer sets `dose_target_confirmed` without
pretending that the dose was measured. A negative answer keeps dose masked for
optimization and upload. Upload-only capture records the configured dose as a
target without claiming it was measured or confirmed and does not prompt.

## Local Replay

LittleFS targets at most four terminal raw replay snapshots under `/rll/p` and
256 KB total. Active, unacknowledged deliveries are never pruned silently; they
may temporarily exceed that retention target until delivery becomes terminal or
the user explicitly deletes local history. Shot history exposes
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
raw replay snapshot. It does not request remote deletion.
