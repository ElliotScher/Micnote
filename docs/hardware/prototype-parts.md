# Prototype parts (stage 1)

> **In plain terms.** Before Micnote has its own circuit board, it's developed on off-the-shelf boards. This page is the shopping list: what to buy, where, roughly what it costs, and what each part is for.
>
> The two main boards are the **Thingy:53**, a small ready-made gadget with the same main chip, a microphone, sensors and a battery, which acts as the first walking prototype of the puck; and the **nRF5340-DK**, a development board with a built-in debugger (a tool for loading software onto chips and looking inside them while they run). Small *breakout boards* (tiny boards carrying one chip each) wire the puck's real microphones and sensors to the DK so they can be tried before the custom board is designed.
>
> You don't need any of this to work on the app or core: those run on an ordinary computer.
>
> *Unfamiliar words are explained in the [glossary](../glossary.md).*

What to buy for [stage 1](../stages/01-firmware-and-sync.md) (§2, hardware needed): a Thingy:53 as the walking prototype, and an nRF5340-DK with breakout boards that carry the puck's real sensors. The puck's own PCB parts are in [stage 6a](../stages/06a-pcb-rev-a.md).

**Prices** are single-unit snapshots in USD from the listed store, checked 2026-11-18, before tax and shipping. They change often and vary a lot by country, so check the live page. Prices marked ≈ were converted from another currency or came from a third-party listing.

## Order first

Enough to flash and debug both boards; nothing else in stage 1 can start without them.

| Item | Part / SKU | Qty | Price | Buy | Needed for | Notes |
|---|---|---|---|---|---|---|
| Nordic Thingy:53 | `THINGY53` | 1 | $61.25 | [DigiKey](https://www.digikey.com/en/products/detail/nordic-semiconductor-asa/THINGY53/15211700) | Stage 1 PR 2 on | nRF5340, VM3011 PDM mic, 8 MB QSPI flash, BMM150 magnetometer, accelerometers, nPM1100 PMIC, 1350 mAh battery, RGB LED, USB-C. The firmware already builds for it (`thingy53/nrf5340/cpuapp`). Also at [Mouser](https://www.mouser.com/c/?q=thingy53) (≈ $55–57) |
| Nordic nRF5340-DK | `NRF5340-DK` | 1 | $47.53 | [DigiKey](https://www.digikey.com/en/products/detail/nordic-semiconductor-asa/NRF5340-DK/13544603) | Stage 1 PR 2 on | Bench board, and the on-board J-Link that flashes the Thingy. 8 MB QSPI flash on board. The firmware already builds for it (`nrf5340dk/nrf5340/cpuapp`) |
| SWD cable, 10-pin 2×5 socket-socket, 1.27 mm | Adafruit `1675` (150 mm) | 1 | $2.95 | [Adafruit](https://www.adafruit.com/products/1675) | Flashing the Thingy | Connects the DK's Debug out to the Thingy's debug header ([Nordic's guide](https://nrfconnectdocs.nordicsemi.com/ncs/latest/nrf/app_dev/device_guides/thingy53/building_thingy53.html)). It may not come in the Thingy box; it's cheap enough to buy anyway. Must be 1.27 mm pitch: 0.1" cables don't fit |
| USB cables | USB-C (Thingy), micro-USB (DK) | 1 each | — | any | Power, USB CDC, flashing | Check the DK's connector on its listing; data cables, not charge-only |

## DK breakouts

The puck's real sensors, wired to the DK's pin headers. Needed for stage 1 PRs 2–4 (audio, UI, snap detection).

| Item | Part / SKU | Qty | Price | Buy | Needed for | Notes |
|---|---|---|---|---|---|---|
| PDM mics (the puck's part) | Infineon `KITIM73D122V01FLEXTOBO1` (IM73D122 evaluation board) | 1 | ≈ $95 (£76.45) | [DigiKey UK](https://www.digikey.co.uk/en/products/detail/infineon-technologies/KITIM73D122V01FLEXTOBO1/16999573) | PR 2: audio quality tests | The mic the puck uses, so audio quality is judged on it. **Poor availability:** a 57-week lead time on one regional site and "discontinued" on [DigiKey Canada](https://www.digikey.ca/en/products/detail/infineon-technologies/KITIM73D122V01FLEXTOBO1/16999573). Check how many mics the kit carries; stereo tests need 2 |
| PDM mics (fallback) | Adafruit `3492` (MP34DT01-M) | 2 | $4.95 each | [Adafruit](https://www.adafruit.com/products/3492) | PR 2: capture pipeline | Buy if the Infineon board is unavailable. Proves PDM capture, Opus encoding and stereo L/R wiring; quality testing then waits for the puck's mics |
| 3-axis Hall sensor | SparkFun `SEN-23880` (TMAG5273, Qwiic) | 1 | $4.95 | [SparkFun](https://sparkfun.com/products/23880) | PR 4: snap detection | The puck's exact part. Supply 2.3–3.6 V. The [mini version](https://sparkfun.com/products/23881) (`SEN-23881`, $5.50) has 0.1" pins for the breadboard |
| Haptic driver + LRA | Pimoroni `PIM452` (DRV2605L + linear resonant actuator) | 1 | ≈ $16 | [RobotShop](https://robotshop.com/products/pimoroni-drv2605l-linear-actuator-haptic-breakout) | PR 4: haptics | The puck's driver, with an LRA (the motor type the puck uses) included. The [Adafruit DRV2605L](https://www.adafruit.com/products/2305) (`2305`) is the same chip without a motor, and its examples use the other motor type (ERM) |
| Qwiic to breadboard cable | SparkFun `PRT-17912` | 3 | $1.95 each | [SparkFun](https://www.sparkfun.com/products/17912) | Wiring the Qwiic boards | The DK has no Qwiic connector |
| Jumper wires and a breadboard | female-female and male-female jumpers, half-size breadboard | 1 set | ≈ $10 | any | All breakouts | |
| Magnetic MagSafe ring, adhesive | 54–56 mm outer, about 46 mm inner diameter | 1–2 | ≈ $10–20 | e.g. [ESR HaloLock universal ring](https://www.esrtech.com/products/halolock-universal-ring-black-silver-6-pack) | PR 4: snap tuning on an iPhone and a Pixel | Stuck to the Thingy so it snaps onto phones. **Get one with magnets in it**, like the puck's N52 ring: many "MagSafe ring stickers" are plain steel and would give the Hall sensor a different field to tune against. The adhesive is one-use |

## Later in stage 1

| Item | Part / SKU | Qty | Price | Buy | Needed for | Notes |
|---|---|---|---|---|---|---|
| Nordic Power Profiler Kit II | `NRF-PPK2` | 1 | $89 at launch; ≈ $85–100 now | [DigiKey](https://www.digikey.com/en/products/detail/nordic-semiconductor-asa/NRF-PPK2/13557476) | PR 7: power measurements | Measures current in each power state; later used for per-rail measurements on the puck. Worth adding to the first order to save shipping |

## Deferred

| Item | Why not now |
|---|---|
| 64 MB 1.8 V QSPI flash (Winbond `W25Q512NW`) | No ready-made breakout exists: it's a WSON-8 (6×8 mm) chip that needs an adapter board, a 1.8 V supply (the DK's I/O voltage would have to match), and the DK's own flash disconnected from the QSPI pins. Both boards' 8 MB flash covers stage 1's storage work; the 64 MB chip arrives with the puck PCB (stage 6) |
| Wireless charging: TI BQ51013B evaluation module, Rx coils, ferrite sheet | Bench evaluation before the PCB schematic, in stage 6a |
| LIS2DW12 accelerometer, nPM1300 PMIC | The Thingy's own accelerometers and PMIC stand in during stage 1; the puck's parts are evaluated in stage 6a |

## Totals (approximate)

| Group | Total |
|---|---|
| Order first | ≈ $112 + cables |
| DK breakouts, with the Infineon mic board | ≈ $147 |
| DK breakouts, with the Adafruit fallback mics | ≈ $62 |
| PPK2 | ≈ $90 |
| **Everything, Infineon mics** | **≈ $350** |
| **Everything, Adafruit mics** | **≈ $265** |
