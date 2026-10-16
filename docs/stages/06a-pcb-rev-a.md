# Stage 6a: PCB Rev A design

## 1. Goal
Design the first custom puck board: final outline and size, every v1 function on board, plus extra test points and rail jumpers for bring-up. Send it to fab fully assembled, 5 boards. It should be correct enough that Rev B only needs fixes, not a redesign.

**Exit criterion (from PLAN.md):** ERC/DRC clean in CI, design review passes, boards ordered.

## 2. Depends on / unblocks
- **Depends on stage 1 results on the DK** (all recorded in `docs/measurements/stage1.md`):
  - the mic choice is confirmed (IM73D122 vs alternatives);
  - the QSPI part runs at 1.8 V;
  - the snap-detection thresholds and sensor placement work with a MagSafe ring;
  - the current budget is confirmed.
- **Needs bench evaluation first:** the BQ51013B evaluation module + Rx coil + ferrite + magnet ring, tested together on a MagSafe/Qi2 charger and on a Pixel with Battery Share.
- **Unblocks:** 6b (bring-up).

## 3. Deliverables
```
hardware/puck/
  puck.kicad_pro, puck.kicad_sch (root) + sheets: power.kicad_sch, mcu.kicad_sch, audio.kicad_sch,
    storage.kicad_sch, sensors.kicad_sch, ui_haptics.kicad_sch, wireless_charging.kicad_sch, connectors.kicad_sch
  puck.kicad_pcb, puck.kicad_dru (JLC 4-layer rules)
  fab/rev-a/{gerbers/, drill/, puck-bom.csv, puck-cpl.csv, puck.step, puck-schematic.pdf, assembly.pdf}
  REVIEW-rev-a.md                  # filled checklist (§4.8)
hardware/lib/micnote.kicad_sym, micnote.pretty/, 3dmodels/
hardware/enclosure/stack-rev-a.FCStd (mechanical stack check only; real enclosure is 6c)
docs/hardware/pin-map.md, docs/hardware/power-tree.md
```

## 4. Design detail

### 4.1 BOM (Rev A)
| Ref | Function | Part | Package (*verify* every footprint against its datasheet) | Notes |
|---|---|---|---|---|
| U1 | SoC module | Raytac MDBT53-1M (nRF5340, chip antenna) | module LGA | FCC/IC/CE modular cert. Plan B: the u.FL variant on the same footprint |
| U2 | PMIC | Nordic nPM1300 | QFN (5×5) | Charger (up to 800 mA; set to about 1C = 250 mA), BUCK1/2, LOADSW1/2, 3 LED sinks, fuel gauge via the SoC ADC + nrfxlib library, ship mode |
| U3 | Qi receiver | TI BQ51013B | VQFN-20 (RHL) | 5 V out, WPC v1.2 BPP. Thermal pad must be soldered |
| U4 | Power mux | TI TPS2116 | SOT-583 | Priority on VBUS (USB) over Qi; output → nPM1300 VBUS |
| U5 | Flash | Winbond W25Q512NW (1.8 V, 512 Mbit) | WSON-8 8×6 | QSPI, up to 133 MHz (the nRF5340 QSPI runs at ≤ 96 MHz) |
| U6, U7 | Mics | Infineon IM73D122 | LGA (bottom port) | PDM, 1.8 V; L/R select tied differently on each |
| U8 | 3D Hall | TI TMAG5273 (A1 variant, ±40/±80 mT) | SOT-23-6 | I²C; choose the range variant after the stage 1 field measurements |
| U9 | Accelerometer | ST LIS2DW12 | LGA-12 2×2 | I²C, wake-up INT1, tap INT2 |
| U10 | Haptic driver | TI DRV2605L | VSSOP-10 | I²C, LRA mode, auto-resonance; supplied from VSYS |
| M1 | LRA | 0832-class coin LRA (about 8 mm, about 170 Hz) | wired / pads | Placed at least 5 mm from the magnet ring |
| D1 | RGB LED | small side- or top-view RGB (e.g. 1.6×1.6 mm) | | Anodes to VSYS; cathodes to nPM1300 LED0–2 sinks |
| SW1 | Button | side-actuated tactile switch, about 2.6 mm height | SMD | To an SoC GPIO plus nPM1300 SHPHLD (wake from ship mode). *Verify* that sharing one button between them works per the nPM1300 docs |
| J1 | USB-C | 16-pin USB 2.0 receptacle, mid-mount | | CC1/CC2 5.1 kΩ to GND; ESD on D+/D−/VBUS |
| U11 | ESD | TI TPD2EUSB30 (or similar) | DRT-3 | Low capacitance on D+/D− |
| L1 | Qi Rx coil | Würth WE-WPCC receiver coil, about 30–35 mm OD (exact P/N *TBD from the bench eval*) | wire / pads | Must fit inside the 46 mm magnet ring inner diameter with clearance |
| FB | Ferrite sheet | Würth WE-FSFS or similar, 0.1–0.3 mm | | Between the coil and the battery/PCB |
| BT1 | LiPo | about 250 mAh pouch with protection circuit (e.g. a 402535-class cell, *verify fit*) | JST-SH 2-pin or solder pads | 10 kΩ NTC (B = 3380) to the nPM1300 NTC pin |
| RT1 | NTC | 10 kΩ, if the cell has no built-in NTC | 0402 | |
| J2 | SWD | Tag-Connect TC2030-NL footprint | | VTref, SWDIO, SWDCLK, nRESET, GND, (SWO) |
| TP* | Test points | 1 mm pads | | Every rail, I²C, PDM CLK/DATA, QSPI CSN/SCK, UART TX/RX, BUTTON, VBUS, Qi OUT |
| JP* | Rail jumpers | 0 Ω 0402 (or a solder-bridge footprint) | | One per rail into its load group, for PPK2 measurements |
| — | Magnet ring | N52 MagSafe-compatible ring, 54 × 46 mm | — | Lives in the enclosure, not on the PCB |

**Passives:** 0402 by default. Bulk decoupling per the module, nPM1300 and BQ51013B reference designs. Copy the values from the reference designs (nPM1300 EK and the BQ51013B EVM). Don't invent them.

### 4.2 Power tree
```
USB-C VBUS ─(ESD)─┐
                  ├─ TPS2116 (prio IN1=VBUS) ─► nPM1300 VBUS
Qi coil ─ BQ51013B OUT(5V) ┘
nPM1300:
  VBAT ◄─► LiPo (NTC)
  VSYS ──► DRV2605L VDD, RGB LED anodes, BUCK inputs (internal)
  BUCK1 1.8 V ──► MDBT53 VDD/VDDH*, W25Q512NW, TMAG5273, LIS2DW12, I²C pull-ups
  LOADSW1 (from BUCK1) ──► mic VDD (U6, U7)   # firmware mute + off in sleep
  BUCK2 (off in v1; reserved, routed to TP)
  GPIO0 ─► SoC (PMIC interrupt), GPIO1 ─► (spare)
```
- \* **Module supply:** the nRF5340 can run with VDDH from VSYS (it has an internal high-voltage regulator) or with VDD directly at 1.8 V. **Choose VDD = 1.8 V from BUCK1** with VDDH tied according to the module datasheet's "VDD supply" configuration. That keeps everything on one rail and lowers power. *Verify* against Raytac MDBT53's pin options and Nordic's nRF5340 reference schematic for the external-1.8 V configuration.
- **Qi status:** the BQ51013B `PG` (power good) and `CHG` signals are open-drain into SoC GPIOs with pull-ups to 1.8 V. The firmware uses them for "on charger" classification (stage 1 §4.12).

### 4.3 Pin map (draft; finalize against the MDBT53 pinout)
| Signal | nRF5340 pin | Notes |
|---|---|---|
| QSPI SCK / CSN / IO0–IO3 | P0.17 / P0.18 / P0.13–P0.16 | Dedicated QSPI pins (*verify*); short, length-matched to within 2 mm |
| USB D+ / D− | dedicated | 90 Ω differential |
| PDM CLK | P0.25 (any GPIO) | Ground guard on both sides; ≤ 30 mm to each mic |
| PDM DIN | P0.26 | Both mics share it (L/R) |
| I²C SCL / SDA | P1.02 / P1.03 | 4.7 kΩ pull-ups to 1.8 V; devices: nPM1300 (0x6B), TMAG5273 (0x35 default), LIS2DW12 (0x18/0x19), DRV2605L (0x5A). *Verify* addresses, no conflicts |
| PMIC INT (nPM1300 GPIO0) | P0.04 | |
| Accel INT1 / INT2 | P0.05 / P0.06 | Wake-up / tap |
| Hall INT | P0.07 | |
| Button | P0.23 | Plus SHPHLD on the PMIC |
| Qi PG / CHG | P0.08 / P0.09 | |
| DRV2605L EN | P0.10 | |
| UART TX / RX (debug) | P0.20 / P0.22 | Test points only |
| SWDIO / SWDCLK / nRESET | dedicated | To the TC2030 |

Pin numbers are placeholders chosen for clean routing. Fix them once the module footprint is placed. The devicetree in 6b must match this table, and `docs/hardware/pin-map.md` is the authoritative copy.

### 4.4 Mechanical stack and outline
- **Board:** about 50 mm round, 1.0 mm thick. Components on the top side only.
- **Cutouts and placement:**
  - USB-C at the rim (a notch for the mid-mount receptacle);
  - the button at the rim, at 90° from USB;
  - the module's antenna end at the rim, opposite USB, away from the LRA.
- **Stack** (height budget ≤ 11.5 mm total, *target*):
  | Layer | Height |
  |---|---|
  | Top shell | 1.2 mm |
  | Tallest parts (USB-C / button) | ≈ 3.2 mm |
  | PCB | 1.0 mm |
  | Adhesive + battery | 4.0 mm |
  | Ferrite | 0.3 mm |
  | Coil (inside the ring ID; the ring is 1.5–2.5 mm thick and the coil sits within that height) | 1.2 mm |
  | Back wall | 1.0 mm |
  | **Total** | ≈ 11.9 mm; tighten in 6c (thinner battery, low-profile USB-C) |
- **Mounting:** 3× M1.6 holes, or snap ledges, at 120°, outside the antenna keep-out.
- **Coil connection:** the coil leads come up through a slot in the ferrite to pads near U3.

### 4.5 Stack-up and rules
- **JLCPCB 4-layer, 1.0 mm** (stack-up JLC04101H-7628 or equivalent; *verify* the current offering). Inner copper 0.5 oz, outer 1 oz.
- **Layers:**
  - L1: signals + components;
  - L2: solid GND (no splits);
  - L3: power pours (VSYS, 1.8 V, MIC_VDD) + QSPI;
  - L4: GND pour + slow signals.
- **Impedance:** USB D+/D− at 90 Ω differential, using JLC's impedance calculator for the chosen stack.
- **Minimums:** 0.1 mm track/space (prefer 0.127 mm), 0.2 mm drill / 0.45 mm via pads. No via-in-pad except under QFN thermal pads (filled and capped if JLC allows; otherwise tented thermal vias next to the pad).

### 4.6 Layout rules
1. **Antenna:**
   - Copy the module's keep-out area *exactly*: no copper on any layer and no parts there.
   - Ground stitching vias every 2–3 mm around the module's ground edge.
   - The magnet ring runs under the outer annulus of the board (diameter 46–54 mm). Put the antenna above the ring's gap or orientation-magnet notch if the enclosure can align that, otherwise anywhere, and measure.
   - Reserve a π-match footprint (0402 DNP pads) on the u.FL Plan B variant.
2. **Mics:**
   - Port holes through the PCB (diameter per the IM73D122 datasheet, typically 0.5–0.8 mm), with a solid ground ring around each port on all layers; no vias inside the gasket footprint.
   - The mics sit at opposite rim positions (≈ 45 mm apart), and the enclosure channels sound from the rim (6c).
   - Keep the mics ≥ 10 mm from the LRA, and away from the BQ51013B and the coil leads.
3. **Qi section:**
   - Keep the BQ51013B, its resonant capacitors and the coil pads in one corner; short, wide traces for the AC1/AC2 coil connections.
   - No sensitive traces (PDM, I²C, Hall) on L1 or L4 directly above the coil area.
   - Thermal: a copper pour + thermal vias under U3 into L2; keep the battery's NTC on the opposite side.
4. **PMIC:** follow the nPM1300 layout guide. Buck inductors close by, with tight input/output capacitor loops. The SW node pours are kept minimal.
5. **Hall sensor:** TMAG5273 at the board center (the center of the magnet ring's field), with no ferromagnetic parts (inductors, shields) within 5 mm. *Note:* the nPM1300 buck inductors are magnetic, so keep them ≥ 8 mm from U8.
6. **Accelerometer:** near the center and away from the board edges (less flex).
7. **LRA:** at the rim, opposite the antenna, ≥ 5 mm from the ring's inner edge if possible. The ring is at the rim too, so in practice the LRA is placed *inward* of the ring footprint (radius < 23 mm).
8. **ESD:** USB ESD protection right at the connector; a TVS on the button line (exposed metal).
9. **Silkscreen:** board name, revision and date; test point labels; pin 1 marks; "TOP".

### 4.7 Schematic sheet notes
| Sheet | Key points |
|---|---|
| power | nPM1300 reference circuit; TPS2116; rail jumpers; test points; ship-mode button |
| mcu | MDBT53 + decoupling per Raytac; 32 MHz/32 kHz crystals are *inside* the module (*verify*; if the 32.768 kHz is external, add an LFXO with load capacitors); SWD; UART test points |
| audio | 2× IM73D122; LOADSW-switched VDD; 100 nF + 1 µF at each mic; L/R straps; series resistor (33 Ω) on PDM CLK near the SoC |
| storage | W25Q512NW; 100 nF + 1 µF; 10 kΩ pull-up on CS; IO2/IO3 (WP#/HOLD#) handled per the QSPI-mode datasheet |
| sensors | TMAG5273, LIS2DW12, pull-ups, INT lines |
| ui_haptics | DRV2605L + LRA, RGB LED to PMIC sinks, button + TVS |
| wireless_charging | BQ51013B per the EVM (resonant capacitors Cs/Cd sized for the chosen coil, COMM caps, clamp, ILIM resistor, TS/CTRL handling), PG/CHG to the SoC |
| connectors | USB-C, ESD, CC resistors, TC2030, battery connector, coil pads |

### 4.8 Design review checklist (`REVIEW-rev-a.md`)
- [ ] Every IC's power, ground, decoupling and EN/reset pins match its reference design
- [ ] The nRF5340 supply configuration matches one of Nordic's documented configurations
- [ ] Every I²C address is unique; pull-ups are present exactly once
- [ ] Every interrupt line goes to a GPIO that can wake the SoC (GPIOTE/SENSE)
- [ ] QSPI is on the dedicated pins; the flash runs at 1.8 V
- [ ] USB: CC resistors present, VBUS ESD + bulk capacitor ≤ 10 µF (USB inrush rule), 90 Ω pair
- [ ] Battery: protection on the cell or the PMIC; NTC wired; charge current is ≤ 1C; ship mode is reachable with the button
- [ ] Qi: resonant capacitor values computed for the actual coil inductance (bench-measured); the thermal pad is soldered
- [ ] Antenna keep-out respected on every layer; stitching vias present
- [ ] Mic ports: drill size, ground ring, no vias in the gasket area
- [ ] Hall sensor ≥ 8 mm from the buck inductors and the LRA
- [ ] Test points on every rail, I²C, PDM, QSPI CSN/SCK, UART; TC2030 footprint orientation checked against the cable
- [ ] Rail jumpers allow measuring each group's current with the PPK2
- [ ] Footprints: every LCSC part number checked for stock; every footprint matches the datasheet's recommended land pattern; 3D models fit inside the stack height
- [ ] Mechanical: board outline, mounting holes, USB/button positions and the antenna edge exported to FreeCAD and checked in the stack model
- [ ] ERC/DRC clean in CI; JLC DFM check (their online tool) passes
- [ ] Independent review: post schematic PDFs for a peer review (e.g. Nordic DevZone or r/PrintedCircuitBoard) before ordering

### 4.9 Fab and assembly
- **Order:** JLCPCB 4-layer 1.0 mm, ENIG finish (better for LGA and mic pads), 5 or 10 boards, top-side PCBA.
- **Sourcing:** parts not stocked at JLC (the module, the coil, sometimes the nPM1300) go through global sourcing or consignment. Hand-solder the coil, battery and LRA.
- **Exports:**
  ```sh
  kicad-cli sch export pdf hardware/puck/puck.kicad_sch -o fab/rev-a/puck-schematic.pdf
  kicad-cli pcb export gerbers --layers F.Cu,In1.Cu,In2.Cu,B.Cu,F.Mask,B.Mask,F.Silkscreen,F.Paste,Edge.Cuts -o fab/rev-a/gerbers/ hardware/puck/puck.kicad_pcb
  kicad-cli pcb export drill -o fab/rev-a/drill/ hardware/puck/puck.kicad_pcb
  kicad-cli pcb export pos --side front --format csv --units mm -o fab/rev-a/puck-cpl.csv hardware/puck/puck.kicad_pcb
  kicad-cli sch export bom … -o fab/rev-a/puck-bom.csv     # fields: Comment, Designator, Footprint, LCSC
  kicad-cli pcb export step -o fab/rev-a/puck.step hardware/puck/puck.kicad_pcb
  ```
  These are wrapped in `hardware/scripts/export-fab.sh` and run in CI on tags `hw-rev-a*`.
- **Rotations:** check part rotations in JLC's CPL preview. QFN/LGA rotations are a common error source.

## 5. Task breakdown
**Bench evaluation (before the schematic)**
- [ ] M: BQ51013B EVM + candidate coils + ferrite + magnet ring on a MagSafe charger, a Qi2 pad and Pixel Battery Share. Pick the coil; measure inductance in place; log temperatures
- [ ] S: measure the magnet ring's field at the Hall sensor location with the TMAG5273 breakout; pick the range variant

**Schematic**
- [ ] L: all eight sheets, using reference designs
- [ ] S: ERC clean; pin map + power tree docs

**Layout**
- [ ] M: outline, mechanical placement (USB, button, antenna, mics, Hall, coil pads)
- [ ] L: placement + routing (QSPI, USB pair, PDM, power)
- [ ] M: pours, stitching, keep-outs, test points, silkscreen
- [ ] S: DRC clean; JLC DFM

**Review and order**
- [ ] M: stack check in FreeCAD; fill in the review checklist; external review
- [ ] S: fab exports; order; record the order number and BOM cost in `REVIEW-rev-a.md`

## 6. Testing and verification
- **CI:** `kicad-cli sch erc --exit-code-violations` and `kicad-cli pcb drc --exit-code-violations --schematic-parity`, both returning 0.
- **JLC DFM:** passes with no unresolved errors.
- **Mechanical:** the STEP model fits inside the stack model with ≥ 0.3 mm clearance everywhere.
- The checklist in §4.8 is fully ticked, with notes for every exception.

## 7. Risks and mitigations
| Risk | Mitigation |
|---|---|
| Antenna detuned by the magnet ring or the phone | u.FL Plan B footprint; RSSI test in 6b; a matching-network footprint |
| Qi coil + ring don't couple well | Bench evaluation before the schematic; ring/coil concentricity enforced by the enclosure |
| The magnetometer saturates from its own ring | Field measured during the bench evaluation; choose the ±80 mT variant if needed; center placement |
| Part availability (module, nPM1300) | Check stock before layout; keep alternates (Fanstel BT40-series module footprint as a backup in the library) |
| Stack height over budget | Choose low-profile parts early (mid-mount USB-C, a 0.8 mm-high LED); thinner battery in Rev B |

## 8. Open questions
- Does the 32.768 kHz crystal live inside the module? This depends on the MDBT53 variant; *verify*.
- Can the button be shared between the SoC GPIO and nPM1300 SHPHLD, or should it get a separate wake path?
- Is BPP 5 W receive enough, or should a part with Qi2 MPP be considered later? BPP is enough for a 250 mAh cell.

## 9. Out of scope
- Final enclosure design (6c).
- Certification beyond relying on the module's modular cert (any later product-level FCC testing is not planned for a personal build).
