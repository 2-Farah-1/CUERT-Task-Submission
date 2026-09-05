# CUERT RTOS controller

STM32F103C8T6 Blue Pill firmware using the existing COMMAND_RX, ACTUATE,
WATCHDOG and STATUS tasks. USART1 uses PA9 TX / PA10 RX at 115200 8N1.
Keep BOOT0 at 0 to boot the flashed application.

## Final output LED and safe-idle behavior

**PA6 / TIM3 CH1 is the primary actuator/output LED required by the assignment.**
During healthy operation it displays throttle PWM: 0..100 maps to CCR1=0..1000
with PSC=71 and ARR=999 (1 kHz PWM). A fresh eligible THROTTLE command arms
output; a PING only maintains or restores communication.

When more than 500 ms elapses without a valid command, LINK becomes LOST and
the firmware logs exactly once per loss transition:

```text
LINK LOST, failing safe
```

PA6 then displays a distinctive rapid safe-idle blink: **100 ms ON / 100 ms
OFF**, repeating at 5 Hz. Full ON uses CCR1=ARR+1=1000; OFF uses CCR1=0.
The blink does not use the saved throttle demand. Logical `OUT=0` and `armed=0`
remain in force while `PWM` reports the physical blink compare.

**PC13 is a supplemental onboard status LED; it does not replace PA6's required
safe-idle behavior.** It mirrors the same phase to make LINK LOST easier to
identify during hardware demonstrations. PC13 is active-low: LOW means ON,
HIGH means OFF. It is push-pull, 2 MHz, no pull, initialized HIGH before its
output driver is enabled. No external watchdog LED is needed.

| State | PA6 | PC13 |
| --- | --- | --- |
| Startup grace period | Zero PWM | HIGH / OFF |
| Healthy, eligible throttle armed | Throttle PWM | HIGH / OFF |
| Healthy, braking or disarmed | Zero PWM | HIGH / OFF |
| Lost, no active/pending brake | 100 ms full ON / 100 ms OFF | 100 ms LOW / 100 ms HIGH, synchronized |
| Lost, active/pending positive brake | Continuous zero PWM | Continues rapid loss blink |

**BRAKE has higher priority than the blink.** Both an active brake and a positive
brake awaiting FIFO processing inhibit PA6. The existing fast path applies zero
before acknowledging the brake request. BRAKE 0 disarms and cannot restore
stale throttle; a fresh eligible THROTTLE is required.

Recovery drives PC13 HIGH/OFF and returns PA6 to the existing actuator safety
logic. PING/STEER recovery does not rearm stale throttle. ACTUATE remains the
sole runtime PWM owner and updates both LEDs from one elapsed-time phase.
The first loss phase is 100 ms OFF, followed by 100 ms ON. The existing 10 ms
thread-flag wait during loss remains interruptible by brake and recovery events;
there is no blink delay loop or extra task. Transitions have approximately
10 ms scheduling granularity. WATCHDOG still tests strict `elapsed > 500`
on valid-command wakes and its existing 20 ms periodic check.

## Build and hardware demonstration

Build with the STM32 toolchain using `cmake --build --preset Debug`, then flash
`build/Debug/CUERT_RtosController.elf` with verification enabled.

1. Send `THROTTLE 40` and valid `PING` commands every 200 ms: PA6 shows 40%
   PWM and PC13 stays OFF.
2. Stop commands: after the watchdog detects more than 500 ms of silence,
   both LEDs blink together and the exact loss line appears.
3. Send `PING`: PC13 turns OFF, `OK` is returned, and PA6 remains disarmed.
   Send a fresh `THROTTLE 40` to resume PWM.
4. Send `BRAKE 100`: PA6 becomes zero immediately. Stop commands again:
   PC13 continues indicating loss while PA6 stays zero throughout.
5. Send `BRAKE 0`: output stays disarmed. A fresh `THROTTLE` can rearm it.

STATUS continues once per second. A human pause exceeding 500 ms legitimately
causes loss; keep sending PING while inspecting normal throttle output.

See [PHASE3.md](PHASE3.md) for state/queue/logging details and
[tests/README.md](tests/README.md) for host verification.

## Hardware verification

The final Debug build was flashed and verified on CUERT. COM3 tests passed for
40% throttle with keepalive, exact loss logging, synchronized loss indication,
PING recovery without stale rearming, fresh throttle, and BRAKE 100 overriding
PA6 while PC13 continues blinking after loss. BRAKE 0 left output disarmed.
The final PING/STATUS capture contained 466 bytes with no invalid ASCII bytes.

A burst of 180 live ST-LINK samples observed half-periods of 92..113 ms
(98 ms median), consistent with the nominal 100 ms and the existing scheduler
granularity. Read intervals were 4..22 ms; this is sampled register timing,
not an oscilloscope measurement. All 171 samples that did not cross a blink
edge agreed between PA6 input level, CCR1 and PC13 output level. Separate
functional tests confirmed PA6 stayed at CCR1=0 throughout sampled brake-held
loss while PC13 exhibited both phases. No sub-millisecond brake latency claim
is made. Test captures are saved locally under `build/uart-diagnostic/rapid-*`.

Build usage: RAM 16,072 / 20,480 bytes; Flash 33,748 / 65,536 bytes. Compilation
and host tests passed without compiler warnings or errors. COM3 was released
after testing.
