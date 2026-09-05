# Phase 2 command-to-output path

Historical Phase 2 notes. Phase 3 replaces the transitional link gate and TX
behavior described below; see PHASE3.md for current firmware behavior.

Exactly the existing COMMAND_RX, ACTUATE, WATCHDOG and STATUS tasks remain in
Core/Src/main.c with their generated priorities and stack sizes. The first two
call the application module from USER CODE sections. WATCHDOG and STATUS still
contain their generated delay loops. No HAL/middleware sources were changed.

## Reception and command grammar

USART1 is 115200 8N1, PA9 TX / PA10 RX. HAL receives one byte per interrupt into
a 256-byte ring (255 usable slots). Callbacks only buffer, rearm, record stream
loss and signal tasks; parsing is entirely in COMMAND_RX. Task-side ring access
briefly saves/restores PRIMASK. ISR code never takes the application mutex.

Commands are uppercase PING, THROTTLE, STEER and BRAKE. Numeric commands require
one ASCII space, an optional + or - sign and decimal digits. Extra whitespace,
tokens, fractions and out-of-range values are rejected; leading zeros and -0
are accepted. Values are never clamped. CR, LF and CRLF terminate lines; empty
lines are ignored. A line has at most 63 characters, excluding its terminator.

Overlong or binary lines are discarded through the next terminator. Ring
overflow or a UART error invalidates the buffered stream: COMMAND_RX flushes
it and discards through the next terminator before accepting another command.
The first line after detected stream loss may therefore also be discarded.
Bytes lost before validation cannot be reconstructed; this is reported as a
best-effort RX rejected diagnostic. An already validated BRAKE is never dropped.

Only a complete valid command updates has_valid_command and
last_valid_command_timestamp_ms under the mutex. timestamp_ms uses HAL_GetTick
at task-side validation, not the physical wire-arrival time of buffered bytes.
Invalid input changes neither liveness state nor the FIFO.

## Brake ordering and synchronization

The one command FIFO contains 16 Command_t items, always with message priority
zero. COMMAND_RX is the only producer and ACTUATE the only consumer.

For BRAKE > 0, COMMAND_RX increments pending_positive_brakes under the
priority-inheritance mutex, releases it and sets ACTUATE's BRAKE_PENDING flag.
It then blocks for a BRAKE_APPLIED acknowledgement. ACTUATE disarms and writes
zero before acknowledging. This handshake lets ACTUATE run despite RX's higher
priority, even when more input is buffered. Only then does RX enqueue the brake,
blocking for space if needed. ACTUATE never waits for enqueue to acknowledge.
There is one outstanding handshake at a time. Flags are wakeups, not counters.

Only ACTUATE decrements the pending counter, exactly once per positive brake
dequeued. BRAKE 0 never decrements it. A full queue cannot delay the assertion
or safe-zero acknowledgement, and the mutex is never held while awaiting space.
Multiple outstanding brakes retain separate counts.

ACTUATE is the only runtime PWM writer and the only writer of throttle_armed,
brake_active, actuator demands and the PWM snapshot. COMMAND_RX owns valid
command presence/timestamp. WATCHDOG retains ownership of link_healthy.
Every task access to AppState_t uses the state mutex; startup initialization
occurs before scheduling. Diagnostic formatting and TX occur after unlocking.

The final safety check and CCR write share one short mutex transaction so RX
cannot assert a brake between that check and a nonzero write. A
throttle_blocked_at_rx bit in Command_t preserves braking context while queued:
a THROTTLE validated while active/pending braking cannot arm after a delayed
dequeue. BRAKE 0 disarms. For rearming, send a new THROTTLE after ACTUATE has
processed release. A pasted BRAKE 0 / THROTTLE burst may conservatively leave
throttle disarmed if the THROTTLE was validated before release was processed.

## PWM, logs and blocking

ACTUATE starts TIM3 CH1 at zero and disables CCR preload for prompt safety-zero
writes. TIM3 PSC=71 and ARR=999 remain persistent in the .ioc and initialization.
CCR = throttle * (ARR + 1) / 100, yielding 0..1000: 0%, 50%, 100% give 0, 500,
1000 respectively. Immediate compare updates can alter the current PWM pulse;
zero does not wait for the next 1 kHz period.

ACTUATE alone initiates UART TX using HAL_UART_Transmit_IT and a persistent
64-byte buffer. TX completion only wakes ACTUATE. Diagnostics such as
DEQ T 50 PWM 500 are best effort and skipped when TX is busy. PING replies are
counted and prioritized as OK followed by CRLF; each dequeued PING earns one
reply. Formatting uses a small integer formatter, not printf. Safety checks
precede diagnostics; no task waits for TX.

Blocking operations:
- State mutex acquisition; only fixed, bounded state/register work while held.
- COMMAND_RX waits for RX flags, brake acknowledgement and FIFO space, unlocked.
- COMMAND_RX delays one RTOS tick after each 64 consumed bytes for bounded
  high-priority work, unlocked; positive brake handshake completes first.
- ACTUATE waits on flags only after an empty nonblocking FIFO read, unlocked.
- Existing WATCHDOG/STATUS placeholder delays are unchanged.

RX ring access and HAL TX setup use brief interrupt masking without any
blocking RTOS operation. COMMAND_READY notifications follow successful enqueue,
closing the empty-queue/check-to-sleep race.

## Phase 3 handoff and limits

Boot has zero physical output, disarmed throttle and has_valid_command=false.
main initializes the timestamp to HAL_GetTick before scheduler start as a
reference for the future initial 500 ms grace period. No LINK LOST event is
generated now.

For Phase 2 only, LinkAllowsOutput uses has_valid_command so valid THROTTLE can
be tested. link_healthy remains untouched by RX and ACTUATE. WATCHDOG must later
own link transitions and replace that transitional gate with link_healthy,
using wrap-safe elapsed arithmetic and implementing recovery/fresh-throttle
rules together. There is currently NO timeout fail-safe, LED behavior or STATUS.

Debug build: RAM 13,864 / 20,480 bytes; Flash 31,312 / 65,536 bytes.
Native tests pass; see tests/README.md. No board flashing, oscilloscope timing,
electrical output verification, stack high-water measurement or real scheduler
stress test was performed. Static compiler stack reports show small application
frames (largest 40 bytes), but these are not total runtime stack measurements.
