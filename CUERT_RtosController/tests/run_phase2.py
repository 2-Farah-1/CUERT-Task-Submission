"""Run the MSVC x64 test DLL; no Windows SDK/runtime dependency."""
import ctypes
import os
from pathlib import Path

dll = ctypes.CDLL(str(Path(__file__).resolve().parents[1] / "build/tests/phase2_tests.dll"))
callback_type = ctypes.CFUNCTYPE(None, ctypes.c_char_p)

@callback_type
def fail(message):
    print("FAIL: " + message.decode(), flush=True)
    os._exit(1)

dll.RunTests.argtypes = [callback_type]
dll.RunTests.restype = None
dll.RunTests(fail)
print("PASS: parser/framing, brake ordering, watchdog startup/500-ms/wrap/recovery, blink, retained TX/STATUS")
