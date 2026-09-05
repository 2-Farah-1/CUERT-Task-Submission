# STM32F103 USB-to-UART Bridge Report

Evidence: implementation statements below come from the repository. Hardware results are identified as earlier session observations or user-reported tests; no hardware tests were run while preparing this report.

## 1. Problem

The separate CUERT STM32F103C8T6 controller requires USART1 on PA9/PA10 at 115200 8N1. According to the project context, no FTDI, CP2102, or CH340 USB-to-TTL converter was available, and the available ST-LINK/V2 provided SWD programming/debugging without UART TX/RX or a Virtual COM Port.

## 2. Design Decision

A second STM32F103C8T6 Blue Pill supplies the USB CDC-to-USART1 conversion. This preserves the assignment's intended USART1 interface, avoids adding USB complexity to the safety-critical RTOS controller, and isolates test infrastructure from submitted CUERT logic. These are the supplied design motivations; the separate controller firmware was not inspected for this report.

## 3. Hardware / Peripheral Configuration

The `.ioc`, `SystemClock_Config()`, HAL configuration, and USART MSP initialization establish:

| Item | Current configuration |
|---|---|
| MCU | STM32F103C8T6; linker allocation: 64 KB flash, 20 KB RAM |
| Clock | 8 MHz HSE, predivider 1, PLL x9; SYSCLK/AHB 72 MHz |
| Peripheral clocks | APB1 36 MHz, APB2 72 MHz; USB PLL / 1.5 = 48 MHz |
| USB | Full-speed device, CDC Virtual COM Port; PA11 D-, PA12 D+ |
| USART1 | PA9 TX, PA10 RX; 115200 baud, 8 data bits, no parity, 1 stop bit; TX/RX enabled, no hardware flow control |

These are configured frequencies, not independent clock measurements. The bridge uses a bare-metal main loop and interrupts; its build includes no FreeRTOS.

## 4. Software Architecture

```text
Laptop COM port --USB CDC OUT--> bridge queue --USART1 PA9 TX--> CUERT PA10 RX
Laptop COM port <--USB CDC IN--- bridge queue <--USART1 PA10 RX-- CUERT PA9 TX
```

`main()` initializes GPIO, USART1, USB, and `Bridge_Init()`, then repeatedly calls `Bridge_Poll()` (`Core/Src/main.c`). The ST USB CDC class invokes the interface callbacks in `USB_DEVICE/App/usbd_cdc_if.c`.

- **USB to UART:** `CDC_Receive_FS()` calls `Bridge_UsbReceive()` to copy a received packet into the 256-byte ring (255 usable). Reception pauses until `Bridge_Poll()` finds room for a full 64-byte packet and rearms USB OUT. The loop copies up to 64 bytes into persistent UART TX storage and starts `HAL_UART_Transmit_IT()` when USART1 is ready.
- **UART to USB:** `Bridge_Init()` arms one-byte interrupt reception. `HAL_UART_RxCpltCallback()` queues each byte in the 1024-byte ring (1023 usable) and rearms reception when no UART error is pending. The loop submits contiguous chunks of up to 64 bytes to `CDC_Transmit_FS()`.
- **Completion and errors:** USB `BUSY`/`FAIL` leaves bytes queued for retry. Submitted storage remains reserved until CDC `TxState` clears after data and any terminating zero-length packet complete. `HAL_UART_ErrorCallback()` counts errors, aborts reception, clears the UART error flags, and rearms RX; the loop also retries RX when ready.

`Core/Src/usb_uart_bridge.c` implements these queues and short interrupt-masked polling operations. USB and USART IRQ handlers dispatch to the HAL. CDC control/receive callbacks do not wait. Normal mode forwards bytes without CUERT command parsing, added acknowledgments, or newline conversion.

## 5. USB Enumeration Issue and Fix

**Observed during debugging:** Windows displayed USB Serial Device (COM3), but opening it sometimes failed with "A device attached to the system is not functioning." The MCU was running, with no recorded HardFault. At one observation, Windows retained COM3 while the MCU USB state was default/unconfigured, address zero, with endpoints not open. A forced USB disconnect/reconnect restored operation. This supports a host/device USB-session mismatch; it does not establish that every possible open failure has the same cause.

**Current implementation:** Before USB stack initialization, `MX_USB_DEVICE_Init()` enables GPIOA, asserts/releases the USB peripheral reset, drives PA12/D+ low as a push-pull output, calls `HAL_Delay(20)`, and restores PA12 to input. It then initializes USB, registers CDC and its interface, and starts the device. This startup-only disconnect is intended to make the host enumerate afresh despite the Blue Pill's fixed D+ pull-up; the delay is outside USB callbacks.

`CDC_Control_FS()` already accepts SET_LINE_CODING without changing USART1. GET_LINE_CODING returns `00 C2 01 00 00 00 08` for a seven-byte request (115200 8N1). DTR/RTS and BREAK requests succeed without physical UART control-line or break effects.

## 6. Validation Strategy

The temporary `USB_BRIDGE_SELF_TEST = 1` branch used newline-terminated commands:

| Command | GPIO action | USB response |
|---|---|---|
| LEFT | PA0 on, PA1/PA2 off | `OK LEFT\r\n` |
| MIDDLE | PA1 on, PA0/PA2 off | `OK MIDDLE\r\n` |
| RIGHT | PA2 on, PA0/PA1 off | `OK RIGHT\r\n` |
| OFF | PA0/PA1/PA2 off | `OK OFF\r\n` |

Earlier session tests received the correct responses; the user confirmed LED operation. This verified `Laptop -> USB CDC -> firmware -> GPIO`, plus USB replies, without exercising USART1 forwarding. After enumeration recovery, repeated opens, DTR/RTS changes, host baud changes, BREAK, and self-test commands succeeded.

The current header sets `USB_BRIDGE_SELF_TEST = 0`; self-test source remains present but is excluded from the normal build. The earlier Debug build passed (35,736 bytes flash; 7,936 bytes RAM).

## 7. UART Loopback Result

**User-reported hardware result:** After disabling self-test, rebuilding, and flashing, PA9 was jumpered to PA10 on the same bridge board. Arbitrary transmitted data returned unchanged:

```text
Laptop -> USB CDC -> bridge -> USART1 TX PA9 -> jumper -> USART1 RX PA10
       <- USB CDC <- bridge <-----------------------------------------+
```

This verifies both USB directions and the actual USART1 transmit/receive path for the tested data before connecting CUERT. The report records the supplied result; payload sizes, byte captures, and test duration were not supplied.

## 8. Final Connection to CUERT Controller

Remove the same-board loopback jumper before connecting:

```text
Bridge PA9 TX  --> CUERT PA10 RX
Bridge PA10 RX <-- CUERT PA9 TX
Bridge GND    --- CUERT GND
```

Do not join the boards' 3.3 V or 5 V rails when they are independently powered. Both USARTs must use 115200 8N1.

## 9. Limitations

- USART1 settings are fixed; host baud/format requests do not reconfigure it.
- UART RX has no flow control: a full ring drops incoming bytes and increments `bridge_uart_rx_dropped`. Error recovery does not reconstruct lost bytes.
- USB reconfiguration discards queued USB-to-UART data. Unconfirmed UART-to-USB bytes are retained for retry; exactly-once delivery across disconnects is not guaranteed.
- Long-duration throughput, exhaustive binary patterns, repeated reset recovery with the startup fix, and communication with the separate CUERT controller are not established by the supplied tests.

## 10. Conclusion

The repository implements a transparent USB CDC-to-USART1 bridge, with self-test disabled and the startup disconnect fix retained. Earlier observations establish USB/GPIO operation and enumeration recovery; the user's subsequent loopback result establishes the complete USB/UART route for the tested data. Current code agrees with that history. The reported later flashing and loopback extend the earlier session, which ended with an unflashed build. CUERT integration remains the next validation step.
