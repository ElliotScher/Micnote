# Hardware

The Micnote puck's PCB, in KiCad 10 (`KICAD_MIN` in [`tools/versions.env`](../tools/versions.env)).

- `puck/`: the KiCad project. The root schematic `puck.kicad_sch` holds one sheet per block (power, mcu, audio, storage, sensors, ui_haptics, wireless_charging, connectors), all empty until [stage 6a](../docs/stages/06a-pcb-rev-a.md). The board is a placeholder 50 mm round outline on a 4-layer, 1.0 mm JLCPCB stack-up.
- `puck/puck.kicad_dru`: JLCPCB's manufacturing limits as custom DRC rules. The board minimums and the default net class live in `puck.kicad_pro` (Board Setup in KiCad).
- `lib/`: Micnote's own parts: symbols in `micnote.kicad_sym`, footprints in `micnote.pretty/`, 3D models in `3dmodels/`. The project's `sym-lib-table` and `fp-lib-table` load them as the `micnote` library. KiCad's standard libraries are used for everything else.

## Checks
CI ([`hardware.yml`](../.github/workflows/hardware.yml)) runs these in the pinned KiCad image. Locally, from `hardware/puck/`:

```sh
kicad-cli sch erc --exit-code-violations puck.kicad_sch
kicad-cli pcb drc --exit-code-violations puck.kicad_pcb
```

Both write a report file next to the input and exit non-zero on any violation.
