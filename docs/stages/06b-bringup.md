# Stage 6b: Rev A bring-up

## 1. Goal
Bring the Rev A boards up step by step, port the firmware to the new board (`micnote_puck`), turn on firmware updates, and tune power and battery behavior. Every Rev A problem gets recorded with a proposed Rev B fix.

**Exit criterion (from PLAN.md):** every bring-up step passes, and the puck works on battery for a full day.

## 2. Depends on / unblocks
- **Depends on:** 6a (assembled boards), 1 (firmware), 2+ (host software to sync with).
- **Unblocks:** 6c (Rev B + enclosure), and 5b (production firmware images for the puck).

## 3. Deliverables
```
firmware/boards/micnote/micnote_puck/
  board.yml, Kconfig.micnote_puck, Kconfig.defconfig
  micnote_puck_nrf5340_cpuapp.dts, micnote_puck_nrf5340_cpuapp_defconfig
  micnote_puck_nrf5340_cpunet.dts (+ defconfig)      # network core for BLE controller
  micnote_puck-pinctrl.dtsi, board.cmake (nrfjprog/jlink runners)
firmware/app/boards/micnote_puck_nrf5340_cpuapp.overlay (app-specific bits)
firmware/app/src/sensors/{qi.c}                       # PG/CHG handling, charger classification
firmware/tests/hil/                                   # hardware-in-the-loop scripts driven by micnote-cli
docs/hardware/bringup-rev-a.md                        # filled checklist per board serial
docs/hardware/rev-a-issues.md                         # issue → root cause → Rev B change
docs/measurements/stage6b.md                          # power, RF, thermal, audio
```

## 4. Design detail

### 4.1 Equipment
- Bench supply with current limit (0–6 V, ≥ 1 A).
- PPK2.
- Multimeter.
- Oscilloscope (≥ 50 MHz) for PDM/QSPI/I²C.
- J-Link (the DK's on-board one is fine) + TC2030-NL cable.
- Thermal camera (or a thermocouple).
- MagSafe/Qi2 charger + Pixel (Battery Share) + iPhone.
- Hot-air station, microscope.
- A reference DK for comparison.

### 4.2 Bring-up checklist (per board, recorded in `bringup-rev-a.md`)
Each step has pass/fail limits. **Stop at the first failure** and investigate before powering more of the board.

| # | Step | Method | Pass criteria |
|---|---|---|---|
| 0 | Visual | Microscope: polarity, rotations, bridges, LGA/QFN fillets | No defects |
| 1 | Shorts | Ohms from each rail TP to GND, unpowered | VBUS > 1 kΩ, VSYS > 1 kΩ, 1V8 > 100 Ω (*use the DK as a reference*) |
| 2 | VBUS power, no battery | Bench 5.0 V at 100 mA limit into the USB-C test point | Idle current < 30 mA; nPM1300 alive (VSYS ≈ 4.5–5 V) |
| 3 | Rails | DMM at test points | BUCK1 1.80 V ± 3%; MIC_VDD = 0 V (LOADSW off by default); BUCK2 off |
| 4 | SWD | `nrfjprog --ids` / `JLinkExe` connect | Both cores detected; APPROTECT not set |
| 5 | Blinky | Flash the `micnote_puck` blinky (nPM1300 LED driver via I²C) | RGB cycles |
| 6 | I²C scan | Zephyr shell `i2c scan` | nPM1300, TMAG5273, LIS2DW12 and DRV2605L all respond at their expected addresses |
| 7 | Battery + charging | Connect the cell (pre-charged to about 3.8 V); USB attached | Charge current per config (±10%); NTC reads room temperature ±3 °C; stops at 4.2 V |
| 8 | Ship mode | `npm1300 ship` command; button wake | Ship current < 1 µA (PPK2); wakes on button |
| 9 | QSPI flash | Shell: JEDEC ID read, erase/write/read of 1 MB, LittleFS mount | Correct ID; 0 errors; ≥ 1 MB/s read |
| 10 | Mics | Enable LOADSW1, PDM capture of 10 s to flash, pull it over USB | Both channels present; noise floor ≤ the DK breakout + 3 dB; 1 kHz tone recorded with correct L/R mapping |
| 11 | USB | Plug into a host | CDC ACM + SMP enumerate; `micnote-cli status --usb` works |
| 12 | BLE | Advertise; `micnote-cli scan` | Seen; RSSI at 1 m in open air ≥ −60 dBm (*compare with the DK*) |
| 13 | RF while attached | Snapped onto an iPhone and a Pixel, the phone 1 m from the host | Link holds; L2CAP throughput ≥ 80% of the open-air value |
| 14 | Sensors | Snap/unsnap 20× on each phone | Snap detection ≥ 19/20; no false snaps over 30 min in a pocket, on a desk, near a laptop |
| 15 | Haptic | DRV2605L auto-calibration, play the effect table | Calibration passes; patterns distinguishable |
| 16 | Qi | Place on a MagSafe charger, a Qi2 pad, and Pixel Battery Share | PG asserted; charging starts; puck surface temperature ≤ 41 °C after 30 min (*limit to confirm against the battery spec*); the battery temperature stays inside the nPM1300 JEITA window |
| 17 | Power profile | PPK2 through the rail jumpers, then whole-device from the battery | See §4.5 targets |
| 18 | Full flow | Record 3 memos detached → snap on → sync on the phone → actions | Works end to end |
| 19 | Firmware update | Signed image via SMP over BLE and over USB; then a bad signature | Good image boots; bad image rejected, the old one keeps running |

### 4.3 Board port (Zephyr hardware model v2)
- **`board.yml`:** board `micnote_puck`, SoC `nrf5340`; the cpuapp and cpunet variants mirror `nrf5340dk`'s structure.
- **DTS outline (cpuapp):**
  ```dts
  / {
    model = "Micnote Puck Rev A";
    compatible = "micnote,puck";
    chosen { zephyr,console = &uart0; zephyr,shell-uart = &uart0; nordic,pm-ext-flash = &w25q512; };
    aliases { led-pwm = &npm1300_leds; button0 = &button0; mic = &pdm0; };
    buttons { compatible = "gpio-keys"; button0: button_0 { gpios = <&gpio0 23 (GPIO_PULL_UP | GPIO_ACTIVE_LOW)>; }; };
  };
  &i2c1 { status = "okay"; pinctrl-0 = <&i2c1_default>; clock-frequency = <I2C_BITRATE_FAST>;
    npm1300_pmic: pmic@6b { compatible = "nordic,npm1300"; reg = <0x6b>;
      regulators { compatible = "nordic,npm1300-regulator";
        buck1: BUCK1 { regulator-init-microvolt = <1800000>; regulator-boot-on; regulator-always-on; };
        mic_sw: LDO1 { regulator-initial-mode = <NPM1300_LDSW_MODE_LDSW>; };  /* load switch for mics */ };
      charger { compatible = "nordic,npm1300-charger"; term-microvolt = <4200000>; current-microamp = <250000>;
        thermistor-ohms = <10000>; thermistor-beta = <3380>; charging-enable; };
      npm1300_leds: leds { compatible = "nordic,npm1300-led"; nordic,led0-mode = "host"; /* … */ }; };
    tmag5273: hall@35 { compatible = "ti,tmag5273"; reg = <0x35>; int-gpios = <&gpio0 7 GPIO_ACTIVE_LOW>; };
    lis2dw12: accel@19 { compatible = "st,lis2dw12"; reg = <0x19>; irq-gpios = <&gpio0 5 GPIO_ACTIVE_HIGH>; };
    drv2605: haptic@5a { compatible = "ti,drv2605"; reg = <0x5a>; en-gpios = <&gpio0 10 GPIO_ACTIVE_HIGH>; };
  };
  &qspi { status = "okay"; w25q512: w25q512nw@0 { compatible = "nordic,qspi-nor"; reg = <0>;
    sck-frequency = <48000000>; jedec-id = [ef 60 20]; size = <DT_SIZE_M(512)>; /* bits */ … }; };
  &pdm0 { status = "okay"; pinctrl-0 = <&pdm0_default>; clock-source = "PCLK32M"; };
  &usbd { status = "okay"; cdc_acm_uart0: cdc_acm_uart0 { compatible = "zephyr,cdc-acm-uart"; }; };
  ```
  Every compatible string, property and JEDEC ID above is from memory or a placeholder. *Verify* each against the bindings in the pinned NCS/Zephyr version (e.g. the regulator node names for LOADSW, the DRV2605 binding's existence, the TMAG5273 binding name) and against the W25Q512NW datasheet.
- **Partitions:** a Partition Manager static file `pm_static.yml` that puts MCUboot's secondary slot on external flash. This keeps internal flash free for the app, and the LittleFS recordings partition gets the rest of external flash, with the size fixed so OTA never touches recordings.
- **Network core:** the stock `ipc_radio` / HCI controller image from NCS via sysbuild.

### 4.4 Firmware additions for the puck
- **`qi.c`:** PG/CHG interrupts → `charge_source=QI`; charger classification for snap detection:
  - PG asserts within 2 s of a SNAPPED event → **likely a charger** (MagSafe charger or Qi pad).
  - PG asserts but BLE sees that the phone we're bonded to is close (RSSI > −45 dBm) → **likely Pixel Battery Share** → treat as snapped onto that phone.
  - This is a heuristic, recorded in the measurements.
- **Fuel gauge:** the nPM1300 fuel-gauge library (`nrf_fuel_gauge`) with the battery model generated for the chosen cell. Use Nordic's battery modeling procedure, or the generic model at first. Reports `battery_pct` in `DeviceStatus`.
- **JEITA temperature limits** set in the nPM1300 charger config according to the cell datasheet.
- **Mic power:** LOADSW1 is on only while RECORDING (plus a 50 ms settle time before capture starts).
- **Production APPROTECT:** in release builds, enable APPROTECT + SECUREAPPROTECT through UICR. Stage 7 adds the provisioning procedure (*not* on bring-up boards).

### 4.5 Power targets (whole device, measured from the battery with the PPK2 in source mode at 3.8 V)
| State | Target | Notes |
|---|---|---|
| Ship mode | < 1 µA | nPM1300 ship |
| SLEEP (wake-on-motion, no advertising) | ≤ 20 µA | Accelerometer in low-power wake-up, Hall off, RTC running |
| ADV_SLOW | ≤ 60 µA avg | 1 s interval |
| RECORDING | ≤ 6 mA avg | Two mics + Opus + flash writes |
| Sync (BLE L2CAP) | ≤ 10 mA avg | |
| **Full-day test** | 8 h with 2 h of recording, 20 snaps, 5 syncs, the rest in SLEEP | ≥ 50% battery left |

### 4.6 Audio validation
- **Recordings:** in a quiet room, at a meeting table (3 speakers at 1–2 m), and in a pocket while walking.
- **Comparisons:** the puck vs the DK breakout vs a phone's voice memo app, using WER from stage 2's harness on the same scripted reading.
- **Decide here:** whether to store stereo (stage 1, §8). If meeting-table WER improves by ≥ 15% relative with stereo and host-side beamforming, enable stereo at 24 kbps for recordings marked as "meeting mode". Meeting mode would be a button gesture: hold 2 s when starting.

## 5. Task breakdown
**Board support**
- [ ] M: board definition + DTS + pinctrl + `pm_static.yml`
- [ ] S: blinky / shell / i2c-scan test images

**Bring-up** (repeat per board; at least 3 of 5 fully done)
- [ ] M: steps 0–9
- [ ] M: steps 10–15
- [ ] M: steps 16–19

**Firmware features**
- [ ] M: `qi.c` + charger classification
- [ ] M: fuel gauge + JEITA config
- [ ] S: mic power gating; meeting-mode gesture (if adopted)

**Measurements and decisions**
- [ ] M: power profile + full-day test
- [ ] M: RF/throughput while attached to phones
- [ ] M: audio comparison + the stereo decision
- [ ] S: `rev-a-issues.md`, with each issue mapped to a Rev B change

## 6. Testing and verification
- **Hardware-in-the-loop (HIL) script** (`firmware/tests/hil/run.sh`): flashes the board, runs `micnote-cli` against it over USB and BLE, records → syncs → verifies the hash, and reads `DeviceStatus` fields. It runs nightly on a bench Raspberry Pi with a puck attached (optional, nice to have).
- Twister builds for `micnote_puck` are added to firmware CI (build-only).
- **Exit:** every row of §4.2 passes on at least 3 boards, and the full-day test passes on at least one.

## 7. Risks and mitigations
| Risk | Mitigation |
|---|---|
| A board-killing fault (shorted rail) | The current-limited supply plus the step order; 5 boards ordered |
| The module antenna underperforms when attached to a phone | Measure; the u.FL variant on spare boards; a matching network |
| Qi heating over the limit | Lower the BQ51013B ILIM; increase the ferrite; enclosure venting is limited; worst case, Qi is documented as "slow charge" |
| Snap false positives | Tune thresholds from logged traces; require the accelerometer shock + field persistence |
| A DTS binding doesn't exist for a part (e.g. DRV2605) | Write a minimal out-of-tree driver in `firmware/drivers/` (I²C register writes) |

## 8. Open questions
- Stereo meeting mode (decided in §4.6).
- Pixel Battery Share detection reliability: is the heuristic good enough, or should it just be ignored?
- Battery model: use Nordic's generic Li-ion model, or characterize the actual cell?

## 9. Out of scope
- Enclosure (6c).
- Production key provisioning (7).
