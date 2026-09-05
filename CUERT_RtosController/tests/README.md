# Phase 2 and Phase 3 tests

phase2_tests.c includes the actual application source with HAL/CMSIS test
doubles. It checks malformed/range/overflow input, CR/LF, timestamp updates and
wrap values, RX stream loss and HAL overrun recovery, repeated pending brakes,
release/fresh-throttle behavior, full-FIFO brake ordering and nonblocking TX.

Phase 3 extends the same executable with startup grace, strict 500/501 ms
boundaries, tick wrap, delayed WATCHDOG recovery, PING/STEER safe recovery,
pre-loss queued throttle rejection, blink timing, STATUS formatting and TX-ring
fullness/retention. The historical phase2 filenames are retained.
The final safe-idle tests check synchronized PA6/PC13 100 ms ON / 100 ms OFF,
recovery during an ON interval, stale-throttle rejection, and an active brake
keeping PA6 zero while PC13 continues indicating loss.

These are deterministic state/order checks, not real FreeRTOS scheduling or
hardware latency tests. The doubles assert that FIFO/flag waits and UART TX
occur without the shared mutex, and that PWM writes hold the state mutex and
satisfy the safety gate.

On a host with a normal C toolchain:
    cc -std=c11 -Wall -Wextra -Itests/stubs -ICore/Inc tests/phase2_tests.c -o phase2_tests
    ./phase2_tests

The local Windows machine has MSVC but lacks Windows SDK/UCRT headers. The
test-only native directory provides minimal C runtime functions so the tests
can run as an x64 DLL without installing dependencies. These files are not
included in firmware builds.

From the project root in a Visual Studio x64 developer command prompt:

    mkdir build\tests
    cl /nologo /std:c11 /W4 /GS- /Oi- /Zl /LD /Itests/native /Itests/stubs /ICore/Inc tests/phase2_tests.c tests/native/runtime.c /Fobuild/tests/ /Febuild/tests/phase2_tests.dll /link /NOENTRY /NODEFAULTLIB
    python tests/run_phase2.py

The Python runner exits immediately with failure if any C assertion fails.
