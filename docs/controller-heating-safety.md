# Controller startup and heating

The reported startup overshoot has not been reproduced on hardware. Code review
found independent failure paths: sequence numbers could be mistaken for old
commands after either board restarted, fallback PID gains could heat before the
saved gains arrived, and the thermocouple filter started at zero even on a hot
boiler. Fixing these paths does not establish which caused the reported event or
rule out a sensor, power, or relay fault.

## Paired firmware and sessions

Protocol version 8 requires updating both the ESP32 display and its controller
(STM32 for UART machines, controller ESP32 for Bluetooth machines). Older peers
cannot authorize outputs; read-only legacy INFO remains available for the
protocol mismatch screen. EspressoRL/container changes are not required for the
heating protocol.

Each connection negotiates two session identifiers before exchanging commands.
A peer restart invalidates the old command queue, sequence numbers and ACKs,
including when the UART wire remains connected. Session identifiers identify
boots; they are not authentication. The frame header adds ten bytes. A small
HELLO is retried every 250 ms during negotiation, with no steady-state heartbeat
added by the handshake.

After a session change the controller turns the heater, pump and valves off.
The heater requires a finite, nonnegative PID configuration with at least one
positive PID gain, followed by a fresh target after installation. Only the heater
task installs gains or resets PID state. The display waits for matching INFO,
sends changes immediately and refreshes desired controls once per second.
Normal heating loses authorization after three seconds without a target refresh;
unrelated pings cannot keep a stale heating target alive. PID autotuning uses its
existing bounded test workflow and is cancelled by session loss or invalid
sensor readings.

The first valid thermocouple sample initializes the filter directly. Failed or
stale readings inhibit heating. Normal heating stops at the target using both
the filtered reading and the latest raw reading; zero PWM explicitly drives the
heater pin low. The raw autotune limit and existing absolute temperature limit
remain enforced. Ordinary at-target cutoff holds the learned PID state; a target
change, invalid configuration or lost authorization clears it. These are
software output checks, not a physical power cutoff.

## Verification

`scripts/test_heater_safety.py` builds the real endpoint, heater/PID and
thermocouple sources with fake RTOS, link and sensor implementations. It covers
lost handshakes, separate board restarts, duplicate commands, stale ACKs and
steam targets, missing session IDs, bounded frames, configuration/target gating,
target expiry, warm readings, invalid inputs, PWM shutoff/rollover and autotune
cancellation. Its integration case connects the actual endpoint to the actual
heater across partial board restarts.

`scripts/test_brew_start.py` checks initial negotiation, control refresh,
protocol mismatch, correlated tare handling and stalled HX711 recovery.
The safety host tests run in CI after building the UART controller.

Physical validation remains necessary: cold starts, warm power cycles,
independent board resets, brew/steam transitions and wired scale tare recovery.
Compare requested target, controller readback, temperature and heater output.
