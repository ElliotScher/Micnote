# Micnote stage plans

`../PLAN.md` is the master plan. These docs expand each milestone into something you can implement directly. If a stage doc and `PLAN.md` disagree, update both rather than letting them drift.

Every stage doc has the same nine sections: Goal, Depends on / unblocks, Deliverables, Design detail, Task breakdown, Testing and verification, Risks and mitigations, Open questions, Out of scope.

Task sizes: **S** ≈ under half a day, **M** ≈ 1–3 days, **L** ≈ a week or more (estimates for one person working part-time on the project).

## Stages

| # | Stage | Doc | Status |
|---|---|---|---|
| 0a | Dev environment | [00a-dev-environment.md](00a-dev-environment.md) | Not started |
| 0b | Scaffolding and CI | [00b-scaffolding-ci.md](00b-scaffolding-ci.md) | Not started |
| 1 | Firmware and sync | [01-firmware-and-sync.md](01-firmware-and-sync.md) | Not started |
| 2 | Core: transcription and store | [02-core-transcription.md](02-core-transcription.md) | Not started |
| 3 | Planner | [03-planner.md](03-planner.md) | Not started |
| 4 | Desktop app and in-app actions | [04-desktop-app-and-actions.md](04-desktop-app-and-actions.md) | Not started |
| 5 | Mobile apps and auto-sync | [05-mobile-and-autosync.md](05-mobile-and-autosync.md) | Not started |
| 5b | Installers and releases | [05b-distribution.md](05b-distribution.md) | Not started |
| 6a | PCB Rev A design | [06a-pcb-rev-a.md](06a-pcb-rev-a.md) | Not started |
| 6b | Rev A bring-up | [06b-bringup.md](06b-bringup.md) | Not started |
| 6c | Rev B and enclosure | [06c-rev-b-enclosure.md](06c-rev-b-enclosure.md) | Not started |
| 7 | Hardening | [07-hardening.md](07-hardening.md) | Not started |

## Dependency graph

```
0a ──► 0b ──► 1 ──► 2 ──► 3 ──► 4 ──► 5 ──► 5b
              │                 │
              │                 └──────────────► 7 (after 5b; parts can start once 1 lands)
              │
              └──► 6a ──► 6b ──► 6c
                   (6a can start after 1 proves the audio and snap designs on the Thingy:53;
                    6b needs 1's firmware; 6c needs 6b's findings)
```

**Parallel tracks:**
- **Software:** 2 → 3 → 4 → 5 → 5b.
- **Hardware:** 6a → 6b → 6c. It starts once stage 1 has confirmed the mic, flash, snap-detection and power numbers on real hardware.
- **Security:** 7 runs continuously. Fuzz targets are added in each stage, and the formal review comes at the end.

## Conventions used across stages
- **Protocol schema:** `proto/micnote.proto` (package `micnote.v1`). Generated code is in `proto/gen/` and committed.
- **C ABI:** `core/include/micnote.h`; every symbol uses the `mn_` prefix.
- **Pinned versions:** `tools/versions.env` is the only place tool versions are pinned.
- **Branches:** one branch and one PR per task group in each doc's task breakdown.
- **verify:** text marked *verify* is a fact to confirm against the vendor datasheet or docs during that stage.
