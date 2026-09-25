# CPM firmware

Firmware for the **Charge Point Module** of the Pine modular EV charging hub — one per outlet: contactor switching, safety supervision, metering, CANopen node towards the CCU, and master of the RS485 link to the outlet's SIU. Prototype on a **NUCLEO-F302R8**.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the design. The SIU firmware lives in [yaronchenA/siu](https://github.com/yaronchenA/siu).

## Status

| Area | State |
|---|---|
| Board bring-up (blink LD2 on PB13, bare metal) | Done |
| Architecture | v0.1 — this repo's plan |
| Everything else | Planned (ARCHITECTURE.md §5) |

## Toolchain

- `arm-none-eabi-gcc` — Arm GNU Toolchain (`brew install --cask gcc-arm-embedded`, or unpack the official release and add `bin/` to `PATH`)
- `openocd` — flashing over the Nucleo's on-board ST-LINK/V2-1

## Build & flash

```sh
make          # builds build/cpm.elf and build/cpm.bin
make flash    # programs the board via OpenOCD (selects the Nucleo's ST-LINK, so the SIU board can stay plugged in)
```
