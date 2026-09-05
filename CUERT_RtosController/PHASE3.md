# Phase 3: watchdog, fail-safe LED, STATUS and retained telemetry

## Requirements and physical output

The supplied CUERT_Firmware_Task.pdf is the reference for externally visible
behavior (task requirements on pages 3-4; official test on page 4).
The four existing generated tasks, priorities, FIFO and positive-brake counter
are preserved. No additional task, command queue, middleware changes, host
dependencies or SDK installation were introduced.

PA6 / TIM3 CH1 remains the primary actuator/output LED required by the assignment.
This implementation assumes an active-high LED connected to PA6 with appropriate
wiring/current limiting. The active-low PC13 onboard LED supplements PA6 during
LINK LOST; it does not replace or remap the required PA6 output.
USART1 remains PA9/PA10 at 115200 8N1.
TIM3 remains PSC=71, ARR=999. This is an LED demonstration: during fail-safe
the PWM pin carries the diagnostic blink, while logical output is zero.

## Watchdog timing and startup

Only WATCHDOG writes link_healthy, link_lost and loss_epoch. The two booleans
represent STARTUP_WAIT (both false), HEALTHY or LOST.

Before the scheduler starts, main initializes last_valid_command_timestamp_ms
to HAL_GetTick. has_valid_command remains false. WATCHDOG evaluates the state,
then blocks on APP_WATCHDOG_FLAG_VALID with a 20-tick timeout (20 ms at the
configured 1 kHz RTOS tick). COMMAND_RX signals that flag after every complete
valid command, without holding the state mutex. Invalid input cannot signal
recovery or update liveness.

Elapsed time is uint32_t subtraction of HAL ticks. Loss requires elapsed > 500,
never >= 500. Thus boot has a full initial grace period; silence is normally
detected on the first watchdog evaluation after 500 ms, within approximately
20 additional milliseconds plus scheduling latency. A valid-command event
wakes WATCHDOG without waiting for that periodic interval.

On entering LOST, WATCHDOG updates link state and increments loss_epoch under
the short state mutex, then immediately notifies ACTUATE after unlocking.
ACTUATE applies safe state before it schedules the exact line:

    LINK LOST, failing safe

The line is generated once per loss epoch. Epoch counting preserves transitions
even if notification flags coalesce, TX is full, or loss and recovery occur
before ACTUATE runs. WATCHDOG does not touch PWM or wait for logging.

## Physical fail-safe and recovery ordering

ACTUATE remains the sole runtime writer of TIM3 and all actuator-owned fields.
It checks link/brake conditions and writes CCR in the same state-mutex
transaction. It clears throttle_armed on every previously unhandled loss epoch.
Saved throttle remains available for diagnostics, but cannot restore output.

During LOST both LEDs are dark for 100 ms and bright for 100 ms, repeated at 5 Hz.
One shared phase synchronizes PA6 CCR1=0/1000 with PC13 HIGH/LOW respectively.
The first interval is dark. ACTUATE uses a 10-tick flag-wait timeout during loss,
so transitions have approximately 10 ms scheduling granularity. It blocks
between work; it does not spin. Logical OUT remains 0 and armed remains 0 even
during the bright diagnostic interval. An active or pending positive brake
forces PA6 continuously to zero; PC13 continues indicating LINK LOST without
restarting the shared phase. Recovery sets PC13 HIGH/OFF on the existing wake
and returns PA6 to the existing throttle-arming logic.

ACTUATE does not consume ordinary FIFO items during STARTUP_WAIT or LOST. It
still performs the positive-brake safety handshake and services TX. It then
blocks, allowing the lower-priority WATCHDOG to process the valid-command
event, set HEALTHY and notify it. A fresh recovery THROTTLE therefore stays in
the FIFO until link recovery is acknowledged.

Each command captures loss_epoch at validation. A pre-loss queued THROTTLE
cannot rearm after recovery because its epoch differs. The existing
throttle_blocked_at_rx marker independently rejects throttle validated during
active/pending braking. PING and STEER recovery leave output zero/disarmed;
a fresh eligible THROTTLE can rearm. Positive BRAKE recovery retains the brake
inhibit.

RX now waits for ACTUATE's release acknowledgement after enqueueing BRAKE 0.
ACTUATE acknowledges only after ordered dequeue, disarming and applying zero.
The next buffered line is then validated after actual release, making the
official BRAKE 0 / THROTTLE 55 sequence work even as a burst. THROTTLE before
release remains ineligible. Positive brakes retain their original
assert/wake/safe-zero-ack/enqueue ordering and pending counter.

## STATUS and UART retention

STATUS uses osDelayUntil with 1000-tick deadlines. It copies AppState_t under
the shared mutex and releases it before formatting or sending. If unusual TX
backlog delays a report, the complete report is retained; missed scheduling
slots are skipped to avoid catch-up bursts.

Example status (values illustrative):

    STATUS up_ms=1000 last=S:-60 at_ms=980 T=40 S=-60 B=0 OUT=40 PWM=400 armed=1 pending=0 LINK=HEALTHY

Each dequeue includes command type/value/reception timestamp and current
throttle, steer, brake, logical output, physical PWM compare, arming, pending
brakes and link state. For example:

    DEQ T:20 rx_ms=2000 T=20 S=-60 B=0 OUT=20 PWM=200 armed=1 pending=0 LINK=HEALTHY

Each dequeued PING also produces OK on its own line.

A 2048-byte TX ring replaces skipped required diagnostics. A separate
priority-inheritance TX mutex serializes task producers and publication.
ACTUATE owns tail advancement and starts HAL_UART_Transmit_IT transfers of up
to 128 contiguous bytes. The UART completion ISR only signals ACTUATE. Bytes
remain occupied and immutable until completion is processed.

No required record is discarded for lack of TX capacity:
- ACTUATE retains one complete dequeue record and pauses further dequeue until
  it fits; safety and blink work still run before every retry.
- Loss transitions remain counted until each exact line has been appended.
- PING acknowledgement shares its retained dequeue record.
- STATUS retains its snapshot/line and blocks for TX-space notification.

No capacity wait holds either mutex. UART starts after both mutexes have been
released; only the brief HAL setup saves/restores PRIMASK to serialize against
UART callbacks. The ring copy uses the TX mutex, not interrupt masking.
No new task is needed to drain TX.

This bounds normal bursts without claiming unlimited input throughput:
sustained overload can backpressure the command FIFO and eventually overflow
the finite RX byte ring before validation. Such damaged input is discarded and
resynchronized as in Phase 2. Required records for commands already accepted
are retained. Optional invalid-input chatter is omitted. Under exceptional TX
backlog, STATUS and dequeue telemetry can arrive late.

## Verification

Debug compile/link passed without compiler warnings:
- RAM: 16,072 / 20,480 bytes (78.48%)
- Flash: 33,748 / 65,536 bytes (51.50%)

The existing native executable was extended, without new dependencies. Passing
checks cover parser/framing/ranges, corrupt/overflow RX recovery, positive-brake
counting/full FIFO, brake release bursts, throttle freshness, startup, strict
500/501 ms boundaries, unsigned wrap, recovery with delayed WATCHDOG,
PING/STEER safe recovery, stale pre-loss FIFO commands, synchronized 100/100 ms
blink phases, active-brake override with independent PC13 indication, transition
log retention, STATUS formatting and TX-full safety. See tests/README.md.

These tests use deterministic HAL/CMSIS doubles; they are not proof of physical
interrupt latency or a real FreeRTOS scheduler stress test. Compiler stack
frames are bounded (ACTUATE 128 bytes, STATUS 256 bytes for their own frames),
but runtime stack high-water marks have not been measured.

Current hardware demonstration instructions are in README.md. Register sampling
and UART capture verify output states; they do not measure sub-millisecond
brake latency or replace an oscilloscope waveform measurement. Runtime stack
high-water marks remain unmeasured.
The final firmware was flashed and verified. Throttle keepalive, rapid mirrored
loss blinking, brake-held PA6 zero with continuing PC13 blink, recovery, PING
and STATUS passed on the board. The measured sample-based half-period range
was 92..113 ms; see README.md for sampling limits and capture locations.
Run the PDF page-4 sequence: PING; THROTTLE 40; STEER -60; THROTTLE 90 then BRAKE
100; BRAKE 0 then THROTTLE 55; silence for at least 600 ms; THROTTLE 20;
THROTTLE abc; paste BRAKE 100 / THROTTLE 100. A human pause exceeding 500 ms
between individual commands legitimately triggers loss; send valid PINGs if
you need to hold a normal throttle indication for longer inspection.
