# Micnote stage plans

> **In plain terms.** Building Micnote is divided into *stages*: milestones that each deliver one working piece, in an order where each builds on the ones before. This page lists them and shows which depend on which. Each stage has its own detailed plan, linked in the table below.
>
> **How to read a stage document.** Every one has the same nine sections:
> 1. **Goal:** what the stage delivers, and its *exit criterion*, the single test that says it's done.
> 2. **Depends on / unblocks:** which earlier stages it needs, and which later ones wait for it.
> 3. **Deliverables:** the files and folders it creates.
> 4. **Design detail:** how it works, in full. The longest section.
> 5. **Task breakdown:** the work, as a checklist. A ticked box means done; *Partial* or *Note* explains anything done differently from the plan. Tasks are grouped into *PRs* (pull requests: one reviewed batch of changes each).
> 6. **Testing and verification:** how we'll know it works.
> 7. **Risks and mitigations:** what could go wrong, and the plan if it does.
> 8. **Open questions:** decisions not yet made.
> 9. **Out of scope:** what this stage deliberately leaves for later.
>
> Every stage plan also opens with a plain-language summary like this one.
>
> *Unfamiliar words are explained in the [glossary](../glossary.md); the whole system is explained without jargon in [How Micnote works](../how-it-works.md).*

`../PLAN.md` is the master plan. These docs expand each milestone into something you can implement directly. If a stage doc and `PLAN.md` disagree, update both rather than letting them drift.

Every stage doc has the same nine sections: Goal, Depends on / unblocks, Deliverables, Design detail, Task breakdown, Testing and verification, Risks and mitigations, Open questions, Out of scope.

Task sizes: **S** ≈ under half a day, **M** ≈ 1–3 days, **L** ≈ a week or more (estimates for one person working part-time on the project).

**Progress tracking:** stage status is the checklist under Milestones in `../PLAN.md`; task status is the checkboxes in each doc's task breakdown. Tick them in the same PR as the work, with a short note on any task that is partial, moved or done differently from the plan.

## Stages

| # | Stage | Doc |
|---|---|---|
| 0a | Dev environment | [00a-dev-environment.md](00a-dev-environment.md) |
| 0b | Scaffolding and CI | [00b-scaffolding-ci.md](00b-scaffolding-ci.md) |
| 1 | Firmware and sync | [01-firmware-and-sync.md](01-firmware-and-sync.md) |
| 2 | Core: transcription and store | [02-core-transcription.md](02-core-transcription.md) |
| 3 | Planner | [03-planner.md](03-planner.md) |
| 4 | Desktop app and in-app actions | [04-desktop-app-and-actions.md](04-desktop-app-and-actions.md) |
| 5 | Mobile apps and auto-sync | [05-mobile-and-autosync.md](05-mobile-and-autosync.md) |
| 5b | Installers and releases | [05b-distribution.md](05b-distribution.md) |
| 6a | PCB Rev A design | [06a-pcb-rev-a.md](06a-pcb-rev-a.md) |
| 6b | Rev A bring-up | [06b-bringup.md](06b-bringup.md) |
| 6c | Rev B and enclosure | [06c-rev-b-enclosure.md](06c-rev-b-enclosure.md) |
| 7 | Hardening | [07-hardening.md](07-hardening.md) |

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
- **Plain language:** every doc opens with an *In plain terms* summary that assumes no technical background, and every technical term the docs use is defined in [`../glossary.md`](../glossary.md). A change that adds a doc or a new term updates both, in the same PR.
