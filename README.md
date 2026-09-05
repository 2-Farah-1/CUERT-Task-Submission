# CUERT Embedded & Control Pre-Interview Task – RTOS Command Controller

## 1. Overview

This repository contains my submission for the Cairo University Eco-Racing Team (CUERT) Embedded & Control pre-interview task for the 2026–2027 season.

The main project is a command-driven FreeRTOS controller running on a real STM32F103C8T6 Blue Pill.

The controller receives plain-text commands over UART:

- `PING`
- `THROTTLE <0-100>`
- `STEER <-100..100>`
- `BRAKE <0-100>`

These UART commands represent the commands that would normally arrive over CAN from a vehicle's main controller.

The controller processes the commands using four RTOS tasks:

1. `COMMAND_RX`
2. `ACTUATE`
3. `WATCHDOG / FAIL-SAFE`
4. `STATUS`

A PWM-driven LED on PA6 represents the actuator output. The firmware also includes safety behavior for brake override, stale throttle prevention, and communication timeout detection.

The main design goal was:

> Make safety actions immediate without destroying the chronological meaning of the received command stream.

---

## 2. Repository Structure

```text
CUERT-Embedded-RTOS-Task/
├── CUERT_RtosController/
│   └── Main FreeRTOS command controller
│
├── BluePill_USB_UART_Bridge/
│   └── USB CDC <-> USART1 test/communication bridge
│
├── README.md
└── ...
```

### `CUERT_RtosController`

This is the actual CUERT task implementation.

It contains the FreeRTOS tasks, command queue, UART command parser, watchdog, safety logic, actuator state, PWM output, and status telemetry.

### `BluePill_USB_UART_Bridge`

This is supporting test infrastructure, not part of the main RTOS controller.

I did not have an FTDI, CP2102, CH340, or another USB-to-TTL UART adapter. I therefore used a second STM32F103C8T6 Blue Pill to build a USB CDC-to-UART bridge.

The controller itself still communicates through USART1 normally.

---

## 3. Hardware Used

### Main controller

- STM32F103C8T6 Blue Pill
- ST-LINK/V2-compatible programmer/debugger
- External LED used as the actuator/PWM output
- Series resistor for the actuator LED
  - **TODO / confirm resistor value used in final hardware**
- USB/UART connection through the second Blue Pill bridge

### USB-to-UART bridge

- Second STM32F103C8T6 Blue Pill
- Micro-USB cable to PC
- USB Device FS configured as CDC / Virtual COM Port
- USART1 used to communicate with the CUERT controller

### Main controller pins

| Function | Pin | Configuration |
|---|---|---|
| USART1 TX | PA9 | UART TX |
| USART1 RX | PA10 | UART RX |
| Actuator PWM | PA6 | TIM3 Channel 1 |
| SWDIO | PA13 | ST-LINK debugging/programming |
| SWCLK | PA14 | ST-LINK debugging/programming |
| Onboard LED | PC13 | **TODO / confirm use in final fail-safe commit** |

The final hardware setup also requires `BOOT0 = 0` so the MCU boots from application Flash.

This became important during debugging because the controller was initially booting from the wrong memory region even though the firmware had been flashed correctly.

### PWM configuration

The actuator LED uses:

```text
TIM3 Channel 1
Pin: PA6
Timer clock: 72 MHz
Prescaler: 71
ARR: 999
PWM frequency: 1 kHz
```

This gives approximately:

```text
0% throttle   -> CCR = 0
40% throttle  -> CCR ≈ 400
55% throttle  -> CCR ≈ 550
100% throttle -> full-scale PWM
```

---

## 4. Development Environment

The projects were generated/configured using the STM32Cube toolchain and use:

- STM32CubeMX-generated configuration
- STM32 HAL
- FreeRTOS
- CMSIS-RTOS v2 interface
- CMake / GCC ARM embedded toolchain
- STM32CubeProgrammer / ST-LINK for flashing and hardware debugging
- USART1 at `115200 8N1`

The USB-UART bridge additionally uses:

- STM32 USB Device FS
- USB CDC / Virtual COM Port

No FreeRTOS or command-specific control logic is used in the USB-UART bridge.

> **TODO / confirm:** Add exact STM32CubeMX / compiler versions if desired. I have intentionally not guessed version numbers.

---

## 5. System Architecture

The overall test setup is:

```text
PC Serial Terminal
       |
       | USB
       v
+-------------------------+
| BluePill USB-UART Bridge|
| USB CDC <-> USART1      |
+-------------------------+
       |
       | 115200 8N1
       v
+------------------------------------------------+
|              CUERT RTOS Controller             |
|                                                |
| UART RX                                        |
|    |                                           |
|    v                                           |
| COMMAND_RX  ---------> Command FIFO            |
|    |                     |                     |
|    | BRAKE fast safety   v                     |
|    +-----------------> ACTUATE ---> TIM3 PWM   |
|                              |          |       |
|                              |          v       |
|                              |        PA6 LED   |
|                                                |
| WATCHDOG ---------> Link state / safety ------>|
|                                                |
| STATUS ------------> UART telemetry            |
+------------------------------------------------+
```

The watchdog and status tasks run independently of normal command processing.

### Command representation

The design uses a `Command_t`-style queue item containing at least the following concepts:

```c
type
value
timestamp_ms
```

For example:

- `T` = throttle
- `S` = steer
- `B` = brake
- `P` = ping

The timestamp is used for command timing and communication-liveness tracking.

> **TODO / confirm from final source:** Exact final `Command_t` typedef and whether it contains any additional throttle-eligibility / safety-generation field.

---

## 6. RTOS Tasks

## COMMAND_RX

`COMMAND_RX` is the highest-priority application task.

Its responsibilities are:

- receive UART data
- frame complete command lines
- parse command text
- validate command syntax and ranges
- timestamp valid commands
- enqueue commands into the command FIFO
- recognize urgent positive brake commands
- update communication-liveness information

The UART interrupt path is intentionally kept small. Parsing, string processing, and actuator decisions are performed in the task rather than inside the ISR.

### Invalid input

Commands are validated rather than clamped.

Examples such as:

```text
THROTTLE abc
THROTTLE 900
```

must not turn into valid actuator commands.

Invalid commands do not actuate the output and are not intended to keep a broken communication link artificially alive.

### BRAKE reliability

A normal FIFO is kept so commands retain their arrival order.

However, waiting for a positive `BRAKE` command to eventually reach the front of a busy queue would not be safe enough.

The design therefore separates:

```text
command chronology
```

from:

```text
immediate safety authority
```

When a valid:

```text
BRAKE > 0
```

is received, it does two things:

```text
BRAKE received
     |
     +----> immediate safety indication to ACTUATE
     |
     +----> normal ordered FIFO entry
```

The FIFO preserves the history of the command stream, while the safety side channel immediately makes non-zero propulsion illegal.

The design uses an outstanding positive-brake count rather than only a boolean. This avoids losing the safety indication if multiple positive BRAKE commands arrive before ACTUATE has consumed all of them.

If the normal command queue is full, the positive-brake safety inhibit is asserted before waiting for queue capacity. The BRAKE command therefore cannot be silently ignored just because the FIFO is temporarily full.

---

## ACTUATE

`ACTUATE` is the next-highest task after `COMMAND_RX`.

It:

- consumes the command FIFO
- updates throttle state
- updates steering state
- updates brake state
- performs actuator safety arbitration
- controls the PA6 PWM output
- logs dequeued command state

An important ownership rule is:

> **ACTUATE is the only task that writes the PA6 PWM hardware.**

Even though `COMMAND_RX` recognizes an urgent BRAKE first, it does not directly write TIM3. This prevents multiple tasks from racing over the same hardware peripheral.

### Throttle

When the link is healthy, no brake inhibit exists, and throttle is armed, the throttle value is mapped to the PWM duty cycle on PA6.

For example, a real hardware log showed:

```text
DEQ T:100 rx_ms=476773 T=100 S=0 B=0 OUT=100 PWM=1000 armed=1 pending=0 LINK=HEALTHY
```

and:

```text
DEQ T:20 rx_ms=487867 T=20 S=0 B=0 OUT=20 PWM=200 armed=1 pending=0 LINK=HEALTHY
```

### BRAKE override

A positive brake immediately forbids non-zero propulsion.

A hardware log from testing showed:

```text
DEQ B:100 rx_ms=539470 T=100 S=0 B=100 OUT=0 PWM=0 armed=0 pending=0 LINK=HEALTHY
```

The previous throttle demand was still recorded as `T=100`, but the effective output was forced to zero.

### Brake release and stale throttle

`BRAKE 0` releases the brake state, but it does not automatically restore an old throttle value.

Example:

```text
THROTTLE 80
BRAKE 100
BRAKE 0
```

does not automatically return the actuator to 80%.

A fresh throttle command is required.

Throttle commands received while braking may still be received, queued, and logged, but they must not later become a fresh propulsion request simply because the brake was released.

This prevents stale intent from reappearing after a safety event.

> **TODO / confirm from final source:** Exact implementation mechanism used to mark throttle commands as eligible/ineligible while a safety inhibit is active.

---

## WATCHDOG / FAIL-SAFE

The watchdog tracks the timestamp of the last valid received command.

The communication timeout is:

```text
> 500 ms
```

If the controller has received no valid command for more than 500 ms, the link is considered lost.

The intended transition is:

```text
HEALTHY
   |
   | >500 ms without valid command
   v
LINK LOST
```

The controller then logs:

```text
LINK LOST, failing safe
```

and enters a defined safe-idle state rather than simply stopping communication.

The watchdog does not directly write the PWM peripheral.

Instead:

```text
WATCHDOG decides link state
          |
          v
ACTUATE applies hardware-safe behavior
```

This keeps actuator ownership in one task.

### Recovery

A new valid command restores communication automatically.

However, restoring the link does not automatically restore stale throttle.

For example:

```text
LINK LOST
PING
```

may restore communication health, but propulsion remains disarmed.

A fresh throttle request is required before non-zero output is allowed again.

### Safe-idle LED indication

The CUERT requirement is that the actuator output changes to a visually distinct safe-idle blink pattern during link loss.

The current design uses PA6 as the required actuator indication.

There was also a final hardware change planned to mirror the fail-safe indication on the Blue Pill's PC13 onboard LED.

**TODO / confirm from final firmware before publishing:**

- exact final fail-safe ON/OFF timing
- whether PA6 and PC13 both blink in the final commit
- whether the final timing is the latest intended fast pattern
- confirm BRAKE keeps PA6 continuously at zero even if the link is also lost

Do not replace this TODO with a guessed value.

---

## STATUS

`STATUS` is the lowest-priority task.

It prints background telemetry approximately once per second without being requested.

The current output format includes fields such as:

```text
up_ms
last command
last command timestamp
T
S
B
OUT
PWM
armed
pending
LINK
```

Example hardware output:

```text
STATUS up_ms=477003 last=T:100 at_ms=476773 T=100 S=0 B=0 OUT=100 PWM=1000 armed=1 pending=0 LINK=HEALTHY
```

The one-second scheduling was confirmed from hardware output. Consecutive lines appeared at:

```text
278003
279003
280003
281003
...
```

which is a 1000 ms interval.

STATUS is deliberately lowest priority because losing or delaying a debug print is less important than receiving a BRAKE or updating an actuator.

---

## 7. Task Priorities

| Task | Relative Priority | Reason |
|---|---|---|
| `COMMAND_RX` | Highest | Incoming commands must be recognized quickly, especially BRAKE. |
| `ACTUATE` | Above WATCHDOG | Once a control/safety command is recognized, applying its output effect is the next most urgent action. |
| `WATCHDOG` | Below ACTUATE | Must always run often enough to detect the 500 ms timeout, but does not need to pre-empt command reception or actuator changes continuously. |
| `STATUS` | Lowest | Telemetry is useful for debugging but is not part of the critical safety path. |

High priority does not mean busy-waiting.

The high-priority tasks block when there is no work so lower-priority watchdog and telemetry activity still gets CPU time.

---

## 8. Why I Kept a FIFO Instead of a Priority Queue

The first idea was to give BRAKE queue priority.

That makes the emergency command appear faster, but it can also change the meaning of the command history.

Consider:

```text
BRAKE 100
STEER -40
THROTTLE 30
BRAKE 0
STEER 20
THROTTLE 60
BRAKE 100
```

If all BRAKE commands are pulled to the front, the resulting sequence could become:

```text
BRAKE 100
BRAKE 0
BRAKE 100
STEER -40
THROTTLE 30
STEER 20
THROTTLE 60
```

That is no longer the sequence sent by the controller.

Giving only positive BRAKE commands priority creates the opposite issue: a `BRAKE 0` release can be delayed behind later positive brakes.

I therefore kept:

```text
one FIFO -> chronological history
```

and added:

```text
positive BRAKE side channel -> immediate safety authority
```

The FIFO answers:

> What happened, and in what order?

The side channel answers:

> Is non-zero output allowed right now?

Those are different questions and I did not want the queue implementation to mix them.

---

## 9. Supported Serial Commands

Commands are case-sensitive.

| Command | Valid Range | Description |
|---|---:|---|
| `PING` | — | Checks that UART/COMMAND_RX are alive and refreshes valid communication |
| `THROTTLE <value>` | `0..100` | Requests throttle/output percentage |
| `STEER <value>` | `-100..100` | Updates steering state |
| `BRAKE <value>` | `0..100` | `>0` activates brake safety override; `0` releases brake |

Examples:

```text
PING
THROTTLE 40
STEER -60
BRAKE 100
BRAKE 0
THROTTLE 55
```

Malformed or out-of-range commands are rejected rather than converted into a different valid command.

---

## 10. Building and Flashing

## Main RTOS Controller

Project:

```text
CUERT_RtosController/
```

The repository contains a CubeMX-generated STM32 project and CMake configuration.

### Build

Use the CMake configuration included with the project.

```text
TODO / confirm exact CMake preset/build command from final CMakePresets.json
```

The final Debug ELF used during hardware testing was located at:

```text
build/Debug/CUERT_RtosController.elf
```

### Flash with ST-LINK / STM32CubeProgrammer

A command successfully used during hardware testing was:

```powershell
STM32_Programmer_CLI `
  -c port=SWD mode=NORMAL `
  -w "build\Debug\CUERT_RtosController.elf" `
  -v `
  -rst `
  -run
```

The exact executable location for `STM32_Programmer_CLI` depends on the local STM32CubeProgrammer installation.

Before resetting the board:

```text
BOOT0 = 0
```

This is required for normal application execution from Flash.

---

## USB-UART Bridge

Project:

```text
BluePill_USB_UART_Bridge/
```

The bridge uses:

```text
USB Device FS -> CDC
USART1        -> PA9 / PA10
UART baud     -> 115200 8N1
SYSCLK        -> 72 MHz
USB clock     -> 48 MHz
```

Build and flash it as a separate STM32 project using ST-LINK.

```text
TODO / confirm exact final bridge build command and ELF filename from source.
```

After flashing, the bridge is powered from its micro-USB connection to the PC.

---

## 11. Hardware Wiring

### Bridge to controller

```text
USB-UART Bridge             CUERT Controller
------------------------------------------------
PA9  TX   ----------------> PA10 RX
PA10 RX   <---------------- PA9  TX
GND       ----------------- GND
```

TX connects to RX and RX connects to TX.

During the tested setup:

- Bridge board was powered from its own USB connection.
- Controller board was powered/programmed through ST-LINK.
- The boards shared GND.
- No extra PA9-to-PA10 loopback jumper remained on the bridge after bridge testing.

### Controller ST-LINK connection

```text
ST-LINK              CUERT Blue Pill
--------------------------------------
SWDIO   ------------ PA13
SWCLK   ------------ PA14
GND     ------------ GND
3.3 V   ------------ 3.3 V
```

Avoid accidentally powering the same board from conflicting supplies.

---

## 12. Running with a Serial Monitor

1. Flash both boards.
2. Set `BOOT0 = 0` on the CUERT controller.
3. Connect the bridge to the controller using crossed TX/RX and common GND.
4. Connect the bridge USB cable to the PC.
5. Open the virtual COM port.
6. Configure:
   - `115200 baud`
   - `8 data bits`
   - `no parity`
   - `1 stop bit`
   - line ending: newline
7. Send one command per line.

During my Windows testing, the bridge appeared as:

```text
USB Serial Device (COM3)
```

The COM number may be different on another PC.

### Example session

Input:

```text
PING
```

Actual hardware output included:

```text
DEQ P:0 rx_ms=347037 T=0 S=0 B=0 OUT=0 PWM=0 armed=0 pending=0 LINK=HEALTHY
OK
```

Input:

```text
THROTTLE 100
```

Actual output included:

```text
DEQ T:100 rx_ms=476773 T=100 S=0 B=0 OUT=100 PWM=1000 armed=1 pending=0 LINK=HEALTHY
```

Input:

```text
BRAKE 100
```

Actual output included:

```text
DEQ B:100 rx_ms=539470 T=100 S=0 B=100 OUT=0 PWM=0 armed=0 pending=0 LINK=HEALTHY
```

### Optional PowerShell serial setup

```powershell
if ($port -and $port.IsOpen) {
    $port.Close()
}

$port = New-Object System.IO.Ports.SerialPort "COM3",115200,"None",8,"One"
$port.NewLine = "`r`n"
$port.ReadTimeout = 1000
$port.WriteTimeout = 1000
$port.Open()

$port.WriteLine("PING")
Start-Sleep -Milliseconds 250
$port.ReadExisting()
```

Close it when finished:

```powershell
$port.Close()
```

---

## 13. CUERT Test Procedure

This follows the test sequence provided in the CUERT task.

| # | Test | Expected behavior | Current result |
|---:|---|---|---|
| 1 | `PING` | Board acknowledges | **PASS** — hardware returned `DEQ P:0` and `OK` |
| 2 | `THROTTLE 40` | Output approximately 40% | **TODO / run exact final test** |
| 3 | `STEER -60` | Steering value appears in state/STATUS | **TODO / run exact final test** |
| 4 | `THROTTLE 90` then immediately `BRAKE 100` | Output immediately becomes zero | **TODO / run exact final test** |
| 5 | `BRAKE 0` then `THROTTLE 55` | Output resumes at approximately 55% | **TODO / run exact final test** |
| 6 | No commands for at least 600 ms | Link loss detected, fail-safe activates, distinct safe-idle blink appears, link-loss message logged | **TODO / re-run after final safe-blink change** |
| 7 | `THROTTLE 20` | Fail-safe clears and normal operation recovers | `THROTTLE 20 -> PWM=200` confirmed; **TODO / confirm specifically as post-timeout recovery on final build** |
| 8 | `THROTTLE abc` | Must not crash or hang | **TODO / run exact final test** |
| 9 | Paste `BRAKE 100` and `THROTTLE 100` as one burst | BRAKE still wins | **TODO / run exact final test** |

Throughout the test, `STATUS` should continue printing approximately once per second.

That one-second STATUS interval has already been confirmed on hardware.

### Additional hardware checks already performed

`THROTTLE 100`:

```text
OUT=100 PWM=1000
```

`THROTTLE 20`:

```text
OUT=20 PWM=200
```

`BRAKE 100` after a previous throttle command:

```text
OUT=0 PWM=0 armed=0
```

These tests confirmed the basic PWM mapping and positive BRAKE output override.

---

## 14. Safety Design

### Why BRAKE is treated specially

A delayed steering update is incorrect, but a delayed BRAKE can allow a non-zero actuator command to remain active when the controller has already requested braking.

For that reason, positive BRAKE reception immediately creates a propulsion inhibit instead of relying only on eventual FIFO processing.

At the same time, the BRAKE command remains in the FIFO so the ordered command history is not destroyed.

### Why stale throttle is not restored after BRAKE

A previous throttle demand does not necessarily remain safe after a brake event.

The controller therefore disarms throttle during braking.

`BRAKE 0` releases the brake state but does not mean:

> Resume whatever throttle happened to exist before.

A new throttle request is required.

The same principle applies to throttle commands received while braking: they must not become valid post-brake propulsion merely because queue processing was delayed.

### Why the watchdog exists

A real actuation node cannot assume that silence means everything is fine.

The sender may have:

- crashed
- lost communication
- stopped transmitting
- suffered a bus failure

The node therefore detects missing valid communication itself.

After more than 500 ms without a valid command, the system actively enters a known fail-safe state.

It does not simply stop printing data.

### Recovery

Communication recovery is automatic when a new valid command arrives.

However, communication recovery and propulsion authorization are separate concepts.

A `PING` can prove that the link is alive again without restoring a stale throttle value.

---

## 15. Required Answers

### 1. Why did you assign the task priorities the way you did?

`COMMAND_RX` is highest because incoming control messages need to be recognized quickly, especially positive BRAKE commands. `ACTUATE` comes next because once a safety or control request has been recognized, changing the effective actuator state is the next most urgent action.

`WATCHDOG` must always get CPU time and meet the 500 ms communication timeout, but it does not need to pre-empt normal reception and actuation continuously. `STATUS` is lowest because telemetry is useful for debugging but should never delay command reception or a safety action.

The higher-priority tasks block while idle rather than busy-spinning, so assigning them a high priority does not starve the lower tasks.

### 2. Why does a stale or missing BRAKE matter more than a stale STEER? How does your watchdog design guarantee the system actually fails safe, rather than just going quiet?

Both steering and braking are safety-relevant, but a missed BRAKE can directly allow non-zero propulsion to continue when the sender has already requested a stop. For that reason, positive BRAKE commands receive an immediate safety path in addition to their ordered FIFO entry.

The watchdog separately protects against loss of the whole command source. It checks the age of the last valid command and explicitly changes the controller into a `LINK LOST` state after more than 500 ms. ACTUATE then applies defined fail-safe hardware behavior and disarms stale throttle. Because the controller changes its own actuator state rather than only printing an error or waiting for the sender, it actually fails safe instead of merely going silent.

### 3. What would you add or fix first if you had one more day?

I would add independent task-liveness supervision, especially for `ACTUATE`, and expand the hardware stress tests around the safety path.

The current watchdog is focused on communication freshness. It can detect that commands stopped arriving, but a production controller should also be able to detect a task that has deadlocked or stopped servicing commands even while the communication side still appears alive.

I would also measure actual BRAKE reaction latency on hardware with a logic analyser/oscilloscope rather than only verifying the state through UART and LEDs.

---

## 16. Testing and Debugging

A large part of this task was tested on the physical boards rather than only by building the firmware.

### USB-to-UART bridge

Because I did not have a USB-to-TTL adapter, I built a second STM32F103C8T6 into a USB CDC-to-UART bridge.

It was tested in stages.

#### USB self-test

A temporary bridge test accepted:

```text
LEFT
MIDDLE
RIGHT
OFF
```

and controlled test LEDs.

This verified:

```text
PC
 -> USB CDC
 -> bridge STM32 application
```

#### Full UART loopback

After disabling the temporary self-test, the bridge's:

```text
PA9 TX -> PA10 RX
```

were connected together.

Sending arbitrary text such as:

```text
HELLO
```

returned the same bytes to the PC.

This verified the full path:

```text
PC
 -> USB CDC
 -> USART1 TX
 -> USART1 RX
 -> USB CDC
 -> PC
```

The loopback jumper was removed before connecting the bridge to the CUERT controller.

### USB enumeration issue

Windows initially showed:

```text
USB Serial Device (COM3)
```

but opening the port intermittently failed with:

```text
A device attached to the system is not functioning.
```

Debugging showed that Windows could keep the COM entry while the MCU's USB peripheral had returned to an unconfigured state.

The bridge startup was changed to briefly force a USB D+ disconnect before starting the USB stack. This caused Windows to see a real disconnect/reconnect and enumerate the device cleanly.

After this change, repeated COM-port operations worked.

### CUERT controller boot problem

The largest hardware debugging issue was initially mistaken for UART corruption.

The firmware source and UART configuration appeared correct, but serial data was unreadable.

Runtime inspection with ST-LINK showed:

```text
Reset PC:      0x20000108
Clock source:  HSI
HSE/PLL:       OFF
USART1 BRR:    0
USART1 CR1:    0
GPIO PA9/PA10: floating inputs
```

The MCU was booting into SRAM rather than executing the application stored in Flash.

The application itself had been correctly programmed.

The physical fix was:

```text
BOOT0 -> 0
```

After correcting BOOT0, runtime checks showed:

```text
PC in Flash
SystemCoreClock = 72000000
SYSCLK = 72 MHz
APB2 = 72 MHz
USART1 BRR = 0x0271
PA9 = USART1 TX
PA10 = USART1 RX
```

A temporary pre-RTOS diagnostic:

```text
UART_DIRECT_OK
```

was then received exactly at 115200 8N1.

The temporary diagnostic was removed and normal firmware was reflashed.

`PING` then passed on the final normal UART path.

### UART hardware validation

The final serial capture used during UART validation contained readable ASCII with no invalid bytes.

`PING` passed before and after removing the temporary UART diagnostic.

### PWM configuration issue

The original generated TIM3 period was not the intended PWM period.

It was corrected to:

```text
PSC = 71
ARR = 999
```

giving a 1 kHz PWM output.

### STATUS validation

STATUS telemetry was observed at approximately one-second intervals on the real controller.

---

## 17. Known Limitations / Future Improvements

### UART currently represents CAN commands

The task intentionally uses serial commands as a stand-in for vehicle CAN traffic.

The current controller therefore implements UART command reception, not a real CAN peripheral/protocol interface.

A future version could replace the UART input layer with real CAN reception while keeping most of the RTOS safety/actuation architecture.

### LED represents the actuator

The current output is a PWM LED rather than a real throttle actuator or motor driver.

This keeps the hardware simple while allowing the output and safety behavior to be demonstrated visibly.

### Watchdog monitors communication freshness

The current watchdog is designed around stale/missing commands.

A more production-oriented system should also supervise internal task health, for example by detecting whether ACTUATE itself has stopped running.

### Final fail-safe LED configuration

**TODO / confirm from final source:** Update this README with the exact final PA6/PC13 fail-safe blink implementation and measured/observed timing after the latest hardware change.

---

## 18. Demo Video

[Demo Video][(INSERT_LINK_HERE)](https://drive.google.com/file/d/1A2obi2nCf16-361q58kdhx-8VmcAl4-U/view?usp=drivesdk)
-please note that in the video i had pc13 as the failsafe debug light instead of the yellow LED for debugging purposes and in my actual submission i have it as both the LED and PC13.)


---

## 19. Author / Submission

**Cairo University Eco-Racing Team**  
**Embedded & Control Pre-Interview Task**  
**Season 2026–2027**
