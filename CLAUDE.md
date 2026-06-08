# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project: Smart Connected Clock System (智能联网时钟系统)

University capstone project — S800 ARM dev board + PC digital twin. The MCU runs a local clock with alarm and key-based settings. The PC host mirrors the MCU state 1:1 via UART protocol at 115200 8N1.

## Build & Run

### MCU (S800 board — TM4C123 ARM Cortex-M4)

Build: Keil MDK IDE (`mcu/S800.uvprojx`). The only source file you should edit is `mcu/main.c` (all self-written code must be centralized here per assignment rules). Other files in `mcu/src/`, `mcu/driverlib/` are provided by the course and should not be modified.

Output: `mcu/Objects/S800.axf` (flashable binary)

The MCU runs independently after flashing — no PC required for local clock, alarm, and key editing.

### PC Host (Python + PyQt5)

```bash
cd pc_host
.venv\Scripts\activate
python virual_twin_panel.py
```

Dependencies: PyQt5, pyserial (+ ntplib, requests, astral, matplotlib for extensions)

## Architecture

```
┌─────────────────────────┐    UART 115200 8N1     ┌─────────────────────────┐
│        MCU (main.c)     │ ◄──────────────────► │   PC Host (PyQt5)       │
│                         │                        │                         │
│  S800_GPIO_Init()       │  *SET:* / *GET: /     │  SerialThread           │
│  S800_I2C0_Init()       │  *RST / *PING          │  VirtualTwinPanel       │
│  S800_UART_Init()       │  ──────────────────►  │                         │
│                         │  *EVT:* / *PONG        │  SimpleSevenSegment     │
│  Boot_Sequence()        │  ◄──────────────────  │  SimpleLED              │
│                         │                        │                         │
│  Main loop (superloop): │                        │                         │
│    10ms: key_scan()     │                        │                         │
│    10ms: edit_tick()    │                        │                         │
│    100ms: led_update()  │                        │                         │
│    100ms: scroll tick   │                        │                         │
│    100ms: alarm_check() │                        │                         │
│    1s: update_time()    │                        │                         │
│    1s: send EVT:DISP/LED│                        │                         │
│    idle: process_uart()  │                        │                         │
└─────────────────────────┘                        └─────────────────────────┘
```

### MCU Hardware map (main.c lines 98-101)

- **Beeper**: PN1 (GPIO_PORTN_BASE, PIN_1) — active buzzer with built-in oscillator, driven HIGH to sound
- **LEDs**: PCA9557 I2C expander at 0x18 — active low, 8 bits (heartbeat/alarm/edit/TX/RX/NTP/reserved/reserved)
- **7-segment**: TCA6424 I2C expander at 0x22 — Port1=segment data, Port2=digit select (active high)
- **Keys**: TCA6424 Port0 (K0-K7, I2C read) + PJ0/PJ1 (USER1/USER2, GPIO with pull-up) — all active low
- **USER1 indicator**: PN0 mirrors PJ0 state in SysTick ISR

### Key mapping (mapped in key_action(), ~line 820)

K0=ADD, K1=FUNC, K2=SHIFT, K3=SPEED, K4=SAVE, K5=FORMAT, K6=DISP, K7=EXT, GPIO-PJ0=USER1, GPIO-PJ1=USER2. Long-press on FUNC (>1s) = save-and-exit. Keys are read in key_scan() every 10ms with 20ms debounce.

### Protocol (implemented in process_uart_command() ~line 1480)

12 PC→MCU commands: *RST, *SET:DATE, *SET:TIME, *SET:DISPLAY, *SET:FORMAT, *SET:MSG, *SET:BEEP, *SET:LED, *SET:KEY, *SET:MODE, *GET, *PING

7 MCU→PC events: *EVT:DISP + *EVT:LED (1Hz heartbeat, core of digital twin), *EVT:KEY, *EVT:ALARM/ALARM_OFF, *EVT:EDIT, *EVT:MODE, *PONG

Abbreviation support via `match_abbrev()`: uppercase chars in pattern are mandatory, lowercase optional. E.g., MINute matches MIN/MINU/MINUT/MINUTE. Case-insensitive. Space-before-colon tolerant (both `*SET:TIME` and `*SET : TIME` work).

### Critical pitfalls

- **I2C bus sharing**: SysTick ISR writes to TCA6424 for display scanning at 1kHz. All foreground I2C accesses (key read, LED write) must wrap with `SysTickIntDisable()` / `SysTickIntEnable()` or risk bus corruption.
- **UART line ending**: Uses `\r\n`. The UART ISR skips `\r`, fires on `\n`. The timeout path (0x40) flushes partial buffers.
- **Beeper is ACTIVE type**: Drive PN1 HIGH=on, LOW=off. Do NOT PWM-toggle it — the 500Hz SysTick toggle approach was tried and reverted in commit 966678f.
- **Edit cancel behavior**: Values are backed up on entry and restored on cancel (via backup_* globals). The edit display needs per-100ms refresh for visible field blinking — do not rely on the 1Hz display update alone.
- **Night mode**: Only HH.MM shown on SEG, only heartbeat bit on LED, beeper suppressed. Triggered by `*SET:MODE NIGHT`.

## Remaining Work

- PC Host P2 (control panel with parameter combos), P4 polish (night mode 4-digit, edit field highlight mirror)
- Extensions E1-E4 (NTP sync, weather, auto day/night, data visualization)
- Custom feature (§4.3, 8 points, required for A/A+ grade)
- Alarm enable/disable toggle (currently always enabled)
