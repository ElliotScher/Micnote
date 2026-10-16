# Stage 6c: Rev B and enclosure

## 1. Goal
Fold the Rev A lessons into a Rev B board, and design the enclosure that turns the board into a pocketable puck. The finished puck snaps onto an iPhone and a Pixel, keeps its sound ports clear, charges on a Qi pad, and survives everyday handling.

**Exit criterion (from PLAN.md):** the puck snaps onto an iPhone and a Pixel, syncs, and charges on a Qi pad.

## 2. Depends on / unblocks
- **Depends on:** 6b (`rev-a-issues.md`, measurements, the stereo decision).
- **Unblocks:** v1 hardware done. Stage 7 tests the physical attack surface on Rev B.

## 3. Deliverables
```
hardware/puck/ (Rev B changes on a `hw-rev-b` branch, tagged hw-rev-b)
hardware/puck/fab/rev-b/
hardware/enclosure/
  puck-top.FCStd, puck-bottom.FCStd, button-cap.FCStd, light-pipe.FCStd
  exports/{puck-top.step, puck-bottom.step, *.stl, *.3mf}
  gaskets/{mic-gasket.dxf}            # die-cut / laser-cut PORON or silicone
  assembly.md                          # step-by-step build with photos
docs/hardware/validation-rev-b.md
```

## 4. Design detail

### 4.1 Rev B board changes
- Apply every item in `rev-a-issues.md`. Each one gets a one-line "change made" entry and a link to the commit.
- **Expected changes** (to be confirmed by 6b):
  - remove bring-up-only parts (some test points stay as pads; 0 Ω rail jumpers become solid copper);
  - a lower-profile USB-C if the stack is over budget;
  - adjusted Qi resonant capacitors after bench tuning;
  - antenna matching values filled in (or the move to u.FL);
  - mic port adjustments if 6b found acoustic leaks.
- The board outline stays the same unless the enclosure needs changes (§4.2). Lock the outline *before* starting enclosure CAD.

### 4.2 Enclosure
**Overall size:** about 57 mm diameter × 10.5–11.5 mm (*target*). This matches or slightly exceeds the 54 mm magnet ring, so the puck centers on the phone's MagSafe/Pixelsnap ring.

**Parts:**
| Part | Material / process | Notes |
|---|---|---|
| Bottom shell (phone side) | SLA resin (JLC3DP "8001" or a tough resin) for prototypes; later MJF PA12 or injection molding | Holds the magnet ring in a 54 × 46 mm recess. The back wall over the coil is ≤ 1.0 mm (no metal). A notch for the orientation magnet if one is used (helps alignment and RF placement) |
| Top shell | Same | Mic port holes on the rim, the USB-C cutout, the button aperture, the LED light pipe hole, grip texture |
| Button cap | SLA or TPU | Flexure or separate cap; ≥ 0.3 mm travel |
| Light pipe | Clear resin or PMMA rod | Over the RGB LED; also diffuses the SAS colors |
| Mic gaskets | PORON or silicone, 0.5–1.0 mm, die-cut | Seal the PCB port to the rim channel |
| Acoustic mesh | Hydrophobic acoustic membrane (e.g. Saati / GORE-style vents, *verify* the supplier) | Over each rim port: dust and splash protection |
| Fasteners | 3× M1.6 screws into heat-set inserts, or a snap fit | Screws for prototypes (easier to open) |
| Back finish | Thin non-slip pad (silicone, ≤ 0.3 mm) or bare | Must not weaken the magnetic hold much; test both |

**Acoustic channels:**
- From each PCB bottom-port mic: through the gasket, then a radial channel of about 1 mm × 1 mm cross-section to a rim hole of about 1 mm with a mesh. The channel stays short (≤ 6 mm) to avoid resonance peaks in the voice band.
- Validate by recording a sine sweep (§4.4).

**Magnetic attachment:**
- The ring is bonded into the bottom shell with structural adhesive (e.g. 3M VHB or epoxy).
- The ring's magnetic **polarity pattern must match MagSafe accessories**. An off-the-shelf "MagSafe-compatible" ring already has it; *verify* by testing attraction on an iPhone and a Pixel. A ring flipped upside down repels.

**Coil:**
- Centered inside the ring ID, with the ferrite above it (toward the battery).
- Coil-to-phone distance (back wall + skin) is ≤ 1.5 mm, for coupling with chargers and Battery Share.

**Thermal:**
- The battery and the coil are separated by ferrite plus about 0.3 mm of air or foam.
- No venting is needed, because this is a low-power device, but check the Qi charging temperature in the closed enclosure (§4.4).

**RF:**
- No metal paint or coatings near the antenna edge.
- Keep the antenna region ≥ 1 mm from the shell wall, and use a cutout or thinner wall over the antenna if needed.

**Assembly order:**
1. Bond the ring + coil + ferrite into the bottom shell.
2. Battery on foam tape.
3. Solder the coil and battery leads, or connect them.
4. Place the PCB, with the mic gaskets aligned by locating pins.
5. Fit the button cap and light pipe into the top shell.
6. Close with screws.

### 4.3 Design iterations
| Iteration | Purpose |
|---|---|
| E0: FDM print (fast) | Fit check against Rev A: outline, USB access, button feel, ring fit |
| E1: SLA print | Acoustics, light pipe, snap-on hold, Qi through the back wall |
| E2: SLA with Rev B board | Final validation (§4.4) |
| E3 (optional) | MJF PA12 or a small batch of resin-cast parts for daily use |

### 4.4 Validation (`validation-rev-b.md`)
| Test | Method | Pass |
|---|---|---|
| Snap hold | Spring scale, pulling perpendicular and in shear, on an iPhone (MagSafe) and a Pixel (Pixelsnap), with and without a MagSafe-compatible case | Perpendicular pull ≥ 5 N (*target*; compare with a MagSafe wallet); doesn't slide off when the phone is held upright and shaken lightly |
| Alignment | 20 snaps each | Self-centers within ±2 mm |
| Snap detection in the enclosure | 20× per phone | ≥ 19/20, no false positives during 1 h of daily carry |
| Acoustics | Sine sweep 100 Hz–8 kHz from a speaker at 1 m vs the bare board | Response within ±6 dB of the bare board over 200 Hz–6 kHz; no resonant peak > 6 dB |
| Speech | Stage 2 WER harness: scripted reading at 1 m, puck on a table | WER within 10% relative of the bare Rev B board |
| Rustle | Walking with the puck in a pocket while recording | Subjective; compare against firmware rustle filtering |
| Qi | MagSafe charger, Qi2 pad, Pixel Battery Share, enclosed puck, 60 min | Charges; enclosure surface ≤ 41 °C; battery within JEITA |
| RF | RSSI and throughput while attached to each phone, vs Rev A open air | Throughput ≥ 80% of open air |
| Drop | 1 m onto hardwood, 6 faces × 2 | Still works; shell may scuff but doesn't open; the ring stays bonded |
| Button | 1,000 presses (manual or a simple jig) | No degradation |
| Splash (informal) | Light water spray on the mesh ports | Still records afterwards (no IP rating claimed) |
| Full day | As in 6b §4.5, in the enclosure | ≥ 50% battery left |

## 5. Task breakdown
**Rev B board**
- [ ] M: apply Rev A issues; DRC/ERC; fill the review checklist again (6a §4.8, with the changed items)
- [ ] S: fab exports + order

**Enclosure**
- [ ] M: E0 CAD from the Rev A STEP; FDM print; fit check
- [ ] L: E1 CAD: acoustic channels, gasket design, light pipe, ring recess; SLA order
- [ ] M: gasket and mesh sourcing; assembly guide
- [ ] M: E2 with the Rev B board: full validation table

**Firmware**
- [ ] S: `micnote_puck` revision handling (`board@revB` with DTS deltas, if any pins changed)

## 6. Testing and verification
- Every row of §4.4 passes on 2 assembled pucks. Results, photos and failure notes go in `validation-rev-b.md`.
- CI builds firmware for both `micnote_puck@revA` and `@revB` until Rev A boards are retired.

## 7. Risks and mitigations
| Risk | Mitigation |
|---|---|
| Weak hold through phone cases | Recommend MagSafe-compatible cases; test common case types; a stronger ring (2.5 mm N52) if the stack allows |
| Acoustic resonances from channels | Short channels; sweep testing; tune the channel length/diameter in E1 |
| Resin shells crack in drops | Tough resin, or MJF PA12, for E3 |
| Qi heat inside the enclosure | Lower the charge current while Qi is the source (firmware), and add foam insulation between the coil and the battery |

## 8. Open questions
- Screw-closed (repairable, battery replaceable) or snap-closed (thinner)? Leaning screw-closed for v1, in keeping with the "own your device" spirit.
- Lanyard or keyring attachment point?
- Color/material for the LED light pipe so the SAS colors are easy to tell apart (test with colorblind-friendly palettes; the SAS alphabet may need to change accordingly; feeds back to stage 1 §4.6).

## 9. Out of scope
- Injection-molding tooling.
- Formal certifications (IP rating, drop standards, product-level FCC).
