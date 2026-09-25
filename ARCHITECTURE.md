# CPM Firmware Architecture

Firmware for the **Charge Point Module (CPM)** — one per outlet, in the rack. It switches the outlet's power (4-pole contactor), meters energy, supervises safety, talks **up** to the **CCU** over the site-wide CAN bus (CANopen) and **down** to its **SIU** over RS485 (the CPM↔SIU protocol, where the CPM is master).

Design references (in the `pine/design` document set): `hardware_design.md` §3 (CPM requirements), `cpm_detailed_design.md` (hardware, hardware trip circuit), `cpm_siu_protocol.md` (the SIU link), `bench_wiring.md` §4 (prototype pins), `manufacturing_procedures.md` §5/§7 (programming, production test). The SIU firmware (`yaronchenA/siu`) is the other end of the RS485 link and the model for this design.

Status: **architecture v0.1** — the repo currently holds only the M0 board bring-up (blink). This document is the plan.

## 1. What the CPM is responsible for — and what it isn't

| The CPM **decides / enforces** | The CCU decides (the CPM executes) | Hardware does, independent of any firmware |
|---|---|---|
| When the contactors may close and must open (safety gates, §6) | Authorization (RFID, app, kiosk) | Leakage trip (RDC-DD module → trip circuit) |
| The CP duty cycle actually offered: min(CCU allocation, cable rating from PP, outlet rating) | Current allocation per outlet (load management) | E-stop loop → trip circuit |
| Unlock gate: power verified off before the SIU may unlock | When to lock / unlock, when to stop a session (G4/G6/G7 in user_manual.md) | CAN-silence watchdog → trip circuit |
| Welded-contact detection (mirror contact + SIU `AC_SENSE`) | The outlet's OCPP status, and so the LED status the SIU shows | Contactors open on trip, whatever the MCU does |
| Reaction to CCU loss and SIU loss (§6.3) | Firmware updates (source of images) | |

**Principle carried over from cpm_detailed_design.md §0:** anything that must *actively do something* to be safe has a hardware path that doesn't depend on this firmware running. The firmware is the everyday controller and the first line of graceful behaviour, never the only safety layer.

## 2. Hardware targets

| | Prototype (now) | Production (planned) |
|---|---|---|
| Board | NUCLEO-F302R8 | own PCB |
| MCU | STM32F302R8 — Cortex-M4F 72 MHz, **64 KB flash, 16 KB RAM**, bxCAN | Cortex-M0+ class with more memory, e.g. STM32G0B1 (512 KB / 144 KB, 2× FDCAN) — prototype_plan.md §2.3 |
| Rule | Don't depend on the FPU or on 72 MHz (production MCU is M0+) | |

**Memory is the prototype's real limit.** Rough flash budget: CANopenNode stack + object dictionary ≈ 20–25 KB, SIU protocol master ≈ 6 KB, application ≈ 10–15 KB, ST startup/drivers ≈ 3 KB → **≈ 40–50 KB of 64 KB**. That fits one application, but *not* the SIU-style bootloader + two 26 KB slots. **Decided: CPM firmware update is out of scope for the prototype.** The prototype CPM is a single application, programmed over SWD — no bootloader. On the F302R8 the SIU's scheme (bootloader + two slots) would leave ~26 KB per slot, too small for a ~40–50 KB application; the alternatives (a bootloader that receives images itself, or external SPI flash for staging) aren't worth building for a chip we won't use in production.
- **Production MCU:** STM32G0B1 class (512 KB) — same bootloader scheme as the SIU (bootloader + app slot + staging slot, first-boot install), so one bootloader design serves both modules.
- **Relaying SIU updates** (§8) stays in the prototype's scope: it needs no flash on the CPM.

RAM budget (16 KB): CANopenNode ≈ 3–4 KB, SIU master buffers ≈ 1.5 KB (RX/TX 256 B + response parse + SIU image relay chunk), outlet state ≈ 0.5 KB, log ≈ 0.5 KB, stacks ≈ 2 KB → ≈ 8–9 KB, leaving headroom.

## 3. Execution model

Bare metal, no RTOS — same reasoning as the SIU (small MCU, few activities, easier to prove timing). Three contexts:

| Context | Runs | Rule |
|---|---|---|
| **Interrupts** | CAN RX (into CANopenNode's buffers), UART RX/TX (SIU link), SysTick | Move data, set flags |
| **1 ms control tick** (timer interrupt, low priority) | CANopen real-time part (`CO_process_RPDO/TPDO/SYNC`), SIU poll scheduler (send poll every 20 ms, response timeout, retries), safety supervision (§6), contactor state machine | Bounded work per tick; deterministic timing |
| **Main loop** | CANopen non-real-time part (`CO_process`: NMT, SDO, heartbeat consumer), SIU response decoding, outlet state machine, metering reads, event/log forwarding, firmware relay, flash | Non-blocking `xxx_poll()` |

The SIU link here is the **master** side: the CPM sends, then waits ≤ 10 ms for the answer. There's no hard turnaround requirement on the CPM, so response decoding can live in the main loop; the *timing* (when to poll, when to give up and retry) lives in the 1 ms tick.

## 4. Layers

```
┌──────────────────────────────────────────────────────────────────────┐
│ app/   outlet_ctrl · siu_master · canopen_app (OD glue) · safety_sup │  pure logic —
│        fault_mgr · metering · fw_relay · cpm_log                     │  host-tested
├──────────────────────────────────────────────────────────────────────┤
│ common/protocol   COBS · CRC16 · TLV codec · type defs  (shared with SIU)
│ third_party/CANopenNode   CANopen stack (Apache-2.0)                  │
├──────────────────────────────────────────────────────────────────────┤
│ drivers/  can · rs485 (master) · contactors · trip_inputs · straps   │  hardware
│           metering_ic (SPI, later) · flash · led                     │
├──────────────────────────────────────────────────────────────────────┤
│ board/    board_nucleo_f302r8.c   (later: board_cpm_rev1.c)          │  pins, clocks
├──────────────────────────────────────────────────────────────────────┤
│ third_party/st   CMSIS + LL for STM32F3 (later G0)                   │
└──────────────────────────────────────────────────────────────────────┘
```

Same rules as the SIU: `app/` has no register access and compiles on the PC; ST LL, not HAL; one board file per board.

## 5. Modules

| Module | Responsibility |
|---|---|
| `app/siu_master` | Master side of the CPM↔SIU protocol: HELLO → SESSION_START handshake (identity + rating + pairing check, cpm_siu_protocol.md §4.3), 20 ms polls with `CP_SET` every poll and `LED_SET` ≥ 1/s, SEQ + retries (2 × 10 ms), `REQ_ID` actions (`LOCK_CMD`, `AUTH_FEEDBACK`, `BUZZER`, `CONFIG_SET`), event receipt + `EVENT_ACK` (dedup by `EVT_SEQ`), `LOG_TEXT` forwarding, link-loss detection (100 ms) → outlet fault. Leading `0x00` before every frame (§2.2). |
| `app/outlet_ctrl` | The outlet's session state machine (§6.1) — combines CCU commands, SIU status (CP/PP/lock/E-stop/AC sense) and local inputs into: contactor command, CP setpoint, lock requests, LED status for the SIU, status to the CCU. |
| `app/safety_sup` | The firmware's safety checks (§6.2): trip read-back, mirror contact vs. command, welded contact, CCU heartbeat loss → graceful ramp-down, SIU link loss, over-temperature from the SIU. Owns the "commanded trip" output into the hardware trip circuit. |
| `app/canopen_app` | Object dictionary glue (§7): maps outlet status / metering / events to TPDOs, CCU commands from RPDOs to `outlet_ctrl`, identity and diagnostics to SDO objects; EMCY on faults. Node ID from the rack/slot straps. |
| `app/fault_mgr` | One list of active/latched faults (CPM + relayed SIU faults) → CANopen EMCY + status bits; clearing rules. |
| `app/metering` | Prototype: none (no metering IC on the bench). Later: reads the metering IC (ADE9000 class) over SPI every second — per-phase V, I, P, energy counter — for TPDOs and MeterValues. |
| `app/fw_relay` | Relays SIU firmware images from the CCU (CAN SDO block download) to the SIU (`FW_BEGIN`/`FW_CHUNK`/…), pacing chunks at the poll rate and reporting progress. |
| `app/cpm_log` | Debug log (same formatter as the SIU's `siu_log`) → ST-LINK virtual COM port on the prototype; SIU `LOG_TEXT` lines are forwarded to it with an `SIU:` prefix. |
| `drivers/can` | bxCAN (F302) / FDCAN (G0) glue for CANopenNode's driver interface (`CO_driver`). |
| `drivers/rs485` | USART1 master with hardware DE (same design as the SIU's driver). |
| `drivers/contactors` | 4 contactor outputs (L1, L2, L3, N) + mirror-contact input. Outputs are forced **off** at reset before anything else. |
| `drivers/trip_inputs` | Hardware-trip read-back, leakage-trip status, E-stop loop sense, commanded-trip output. |
| `drivers/straps` | Rack/slot ID jumpers → CANopen node ID. |

## 6. Outlet control and safety

### 6.1 Session state machine (`outlet_ctrl`)

```
            ┌──────────── fault / trip / SIU lost / CCU lost ────────────┐
            ▼                                                            │
 UNAVAILABLE ──SIU linked, approved──► AVAILABLE ──plug in──► PREPARING ──CCU authorizes──► READY
 (no SIU, not approved,                    ▲                                            │
  disabled by CCU)                         │                          CP = C/D and allocation ≥ 6 A
                                           │                                            ▼
                               FINISHING ◄─┴─ session ends ◄── SUSPENDED_EV / SUSPENDED_EVSE ◄──► CHARGING
                               (unlock when power verified off)
 FAULTED / STOPPED (E-stop): contactors open, CP state F; leaves only when the cause is gone and the CCU clears it
```

States map 1:1 onto OCPP connector status (and so onto the SIU's LED states, siu_detailed_design.md §6.1). The CCU owns the transitions that are business decisions (authorize, stop, reserve, disable); the CPM owns the ones that are physics (plug, CP state, faults).

### 6.2 Contactor rules

The contactors may **close** only when **all** hold:
- a session is authorized by the CCU and the CCU heartbeat is alive;
- the SIU link is active, the SIU is paired, approved (not in factory mode / unprovisioned / rating mismatch) and reports the lock **sensed locked**;
- CP state is **C or D** (vehicle requests power) and the CPM is offering PWM;
- no active fault: hardware trip clear, leakage OK, E-stop loop closed, SIU over-temperature clear, mirror contact consistent;
- the offered current ≥ 6 A.

They must **open** when any of these stops holding. Two ways:
- **Graceful** (loss of authorization, allocation to 0, CCU heartbeat lost, vehicle back to state B): CP → `CONST_12V` (stop offering), wait for the vehicle to drop to state B or up to **3 s** (IEC 61851-1 allows the vehicle this long to reduce current), then open the contactors.
- **Immediate** (safety faults: trip, E-stop, SIU over-temperature, SIU link lost, CP state E/F, welded-contact suspicion): open now; CP → `STATE_F`. The hardware trip circuit already acts independently for trip / E-stop / CAN silence; the firmware follows it, it doesn't race it.

**CP duty offered** = min(CCU allocation, cable rating from PP, outlet rating), converted per IEC 61851-1 (6–51 A: duty % = A / 0.6), sent as `CP_SET` in every poll — fixes gap G11 (cap at the cable's rating).

**Unlock gate:** `LOCK_CMD(unlock, power_off_confirmed = 1)` is sent only when the contactors are commanded open **and** the mirror contact confirms open **and** the SIU's `AC_SENSE` shows no voltage. The SIU checks `AC_SENSE` again on its own side.

**Welded contact:** mirror contact says closed while commanded open, **or** SIU `AC_SENSE` sees voltage while commanded open → fault `WELDED_CONTACT`, outlet out of service, lock stays locked, EMCY to the CCU. Voltage absent while closed → `OPEN_POLE` fault.

### 6.3 Loss of the CCU or the SIU

| Event | Detection | CPM reaction |
|---|---|---|
| CCU silent | CANopen heartbeat consumer timeout (**1.5 s**, 3 × the CCU's 500 ms heartbeat) | Graceful stop (§6.2); lock stays; SIU LED → Faulted; keep polling the SIU; resume only after the CCU is back and sends new commands |
| CCU silent **and** CPM firmware stuck | Hardware CAN-silence watchdog (**≥ 3 s**, longer than the above so the graceful path runs first) | Hardware trip |
| SIU silent | No valid response for **100 ms** (5 polls) | Immediate open; report `SIU_LOST`; restart the handshake every 100 ms |
| SIU rebooted | `BOOT_ID` changed in `HELLO_INFO` | Session interrupted → immediate open; report to the CCU |
| SIU replaced | Different UID / serial | Outlet `UNAVAILABLE` until the CCU approves it (cpm_siu_protocol.md §4.3) |

### 6.4 Start-up

1. Contactor outputs **off** and commanded-trip **asserted**, before clocks or anything else.
2. Read straps → node ID. Initialise CAN, CANopen (pre-operational), SIU link.
3. SIU handshake; wait for the CCU's NMT *start*.
4. Only then release the commanded-trip line. The outlet starts in `UNAVAILABLE` until the CCU enables it.

## 7. CANopen interface to the CCU

**Specified in `pine/design/cpm_ccu_can_interface.md`** — that document is the contract; the summary below is for orientation only. The CAN bus is shared by up to 32 CPMs (8 per rack, 4 racks) and the CCU: **250 kbps**, CANopen (CiA 301), **CCU = node 1, CPM = 2 + rack × 8 + slot** (nodes 2–33) from the backplane straps.

| Object | Content | Access |
|---|---|---|
| `0x1000`, `0x1008–0x100A`, `0x1018` | Device type, name, versions, identity (vendor, product, serial) | SDO |
| `0x1017` | Producer heartbeat **500 ms** | |
| `0x1016` | Consumer heartbeat: CCU, **1500 ms** | |
| `0x1F50` | Program download (CiA 302) — CPM's own firmware (production MCU) | SDO block |
| `0x2000` | Outlet status record: state, CP state, PP rating, lock state, contactor state, offered current, faults | TPDO1 + SDO |
| `0x2010` | Metering: per-phase V, I, P; energy counter (Wh) | TPDO2 + SDO |
| `0x2020` | Event record: plug in/out, RFID tag (UID up to 10 bytes → notification in TPDO3, full UID read by SDO), E-stop, lock failed | TPDO3 + SDO |
| `0x2100` | CCU commands: allocated current (0.1 A), session command (authorize / stop / reserve / disable / clear fault), lock request, LED status, auth feedback | RPDO1 + SDO |
| `0x2200` | Paired SIU identity: UID, serial, hardware info, firmware version, bootloader version | SDO |
| `0x2210` | SIU firmware relay: image download + status | SDO block |
| `0x2300` | Diagnostics: SIU link counters, CAN error counters, fault history, uptime | SDO |
| EMCY | Fault raised / cleared (fault code + outlet state) | EMCY |

**PDO timing:** TPDO1 on change (inhibit 50 ms) + every 1 s; TPDO2 every 1 s; TPDO3 on event. **Bus load** for 32 CPMs ≈ 32 × (2 heartbeats + ~3 PDOs) per second ≈ 160 frames/s ≈ 21 kbit/s — **under 10 %** of 250 kbps, with room for SDO transfers (firmware relay) one outlet at a time.

## 8. Firmware updates

- **SIU updates (relay):** the CCU downloads the SIU image into the CPM (`0x2210`, SDO block transfer, 1 KB at a time into RAM, never the whole image — it doesn't fit), and the CPM forwards it chunk by chunk (`FW_CHUNK`, 200 B per poll) with flow control back to the CCU. Uses the SIU protocol exactly as `tools/fw_update.py` does today. No flash needed on the CPM for this.
- **CPM's own updates:** **not in the prototype** (§2). Production MCU only — same scheme as the SIU (bootloader + app slot + staging slot, first-boot install at the factory, manufacturing_procedures.md §5), image via CiA 302 program download (`0x1F50`).
- **Signing:** open item for both modules (manufacturing_procedures.md §12).

## 9. Testing

| Level | How |
|---|---|
| Host unit tests (`make test`) | `outlet_ctrl` state machine and contactor rules, `siu_master` (against the SIU's own `link_session` compiled in — both ends of the protocol in one test), safety supervision, CANopen OD mapping |
| HIL, CPM + real SIU | The SIU bench board on RS485 (or directly UART-to-UART on the bench); PC drives the CPM over CAN with `python-can` (candleLight adapter) as a CCU stand-in |
| HIL, CPM + simulated SIU | Python SIU simulator on the USB-serial adapter — for faults that are hard to make with the real SIU (garbage responses, reboots, wrong rating) |
| Safety | Hardware trip with the CPM MCU held in reset (bench_wiring.md §4.4), heartbeat-loss timing, welded-contact simulation |

## 10. Directory layout (target)

```
cpm/
├── ARCHITECTURE.md       this file
├── README.md
├── Makefile
├── ld/                   linker script(s)
├── board/                board.h + board_nucleo_f302r8.c
├── drivers/              can, rs485, contactors, trip_inputs, straps, ...
├── app/                  pure logic, host-testable
├── common/               shared with the SIU (see §11)
├── src/main.c            init + main loop + 1 ms tick
├── tests/                host tests; hil/ (pytest)
├── tools/                CCU stand-in scripts, SIU simulator
└── third_party/          st/ (CMSIS + LL F3), CANopenNode/
```

## 11. Open decisions

1. **Sharing `common/protocol` with the SIU repo.** Options: (a) a small third repo (`pine-common`) included in both as a **git submodule** — one source of truth, recommended; (b) copies kept in sync by a script + a test that fails when they differ; (c) move both firmwares into one repo. Needed before `siu_master` is written.
2. ~~**CPM↔CCU CANopen interface spec**~~ — written: `pine/design/cpm_ccu_can_interface.md` (v0.1).
3. **CANopenNode version and footprint** on 64 KB — measure early; if it doesn't fit with the application, move the prototype to the NUCLEO-G0B1RE sooner.
4. **Graceful-stop timing** — 3 s maximum wait for the vehicle to drop to state B (IEC 61851-1); confirm against the edition used for certification.
5. **Metering IC** choice and whether billing-grade (MID) accuracy is required (hardware_design.md §3.4) — defines `app/metering`.
