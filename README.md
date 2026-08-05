# Power interval relay

Touchscreen relay-cycle controller for the GUITION JC1060P470C_I_W
(ESP32-P4 revision v1.0, 1024x600 JD9165 LCD, GT911 touch).

## Wiring used for the control-side test

| Relay module | JC1060P470 / supply |
|---|---|
| D+ | External regulated 5 V |
| D- | GND |
| IN2 | FPC3 pin 9 / GPIO3 |
| CH2 trigger jumper | H (active-high) |

The relay module and board must share GND. GPIO3 only drives the optocoupled
module input; never connect a bare relay coil to GPIO3. Keep an external 10 kΩ
pull-down from GPIO3 to GND so channel 2 remains OFF while the ESP32-P4 is
reset or unpowered.

Do not connect a DUT or mains wiring while developing or validating the UI.
The relay cube's printed contact rating does not establish a safe rating for a
particular load, especially one with high startup inrush.

## Touch UI

- `ON TIME`: 1-5 s, 1 s per tap or held repeat
- `OFF TIME`: 1-60 s, 1 s per tap or held repeat
- `CYCLES`: 1-1,000,000, accelerated while held
- `HOLD TO START`: a left-to-right gauge fills during the 1.5-second hold;
  releasing early cancels and resets the gauge
- `PAUSE`: immediately forces OFF and discards the partial cycle
- `RESUME`: waits one complete OFF interval before restarting that cycle
- `STOP`: operates on touch-down and immediately forces OFF

The controller ignores a touch already present during boot until a clean
release is observed. It always boots OFF and never resumes a run after reset.

## Serial commands

The CH340 UART Type-C port is 115200 baud.

```text
config 1000 1000 3  # ON ms, OFF ms, completed cycles
start
pause
resume
stop
off                 # alias for stop
pulse 1000          # manual test; maximum 5 seconds
status
help
```

Serial and touchscreen settings are synchronized. Serial OFF time may be set
up to 24 hours; the touchscreen currently caps it at 60 seconds.

## Safety behavior

- GPIO3 is preloaded LOW before it becomes an output.
- Every run begins with one full OFF interval.
- A cycle is counted only after a full ON interval and its following OFF dwell.
- Stop, pause, reset, invalid internal state, or serial-buffer overflow force
  the relay OFF.
- ON time has a hard 5-second limit until electrical feedback is added.
- Relay deadlines are checked before and after serial and LVGL processing.
- The Arduino loop watchdog is enabled.
- `status` reports the largest measured software ON-time overrun.

This version verifies commanded GPIO timing only. It does not yet prove that
the relay contacts changed state or that the DUT actually powered up. Add
isolated electrical feedback and persistent checkpoints before using it for a
qualification run.

## Build and upload

The project-local `libraries/lvgl` directory is the manufacturer's LVGL 8.4.0
release. The driver is configured for synchronous partial-buffer updates to
avoid recycling a buffer during an asynchronous DMA2D copy.

```powershell
arduino-cli compile --build-path .\build --libraries .\libraries `
  --fqbn esp32:esp32:esp32p4 `
  --board-options ChipVariant=prev3,JTAGAdapter=default,PSRAM=enabled,USBMode=default,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,PartitionScheme=default,FlashMode=qio,FlashFreq=80,FlashSize=16M,UploadSpeed=921600,DebugLevel=none,EraseFlash=none `
  .

arduino-cli upload -p COM13 --fqbn esp32:esp32:esp32p4 `
  --board-options ChipVariant=prev3,PSRAM=enabled,FlashMode=qio,FlashFreq=80,FlashSize=16M,UploadSpeed=921600 `
  --input-dir .\build
```
