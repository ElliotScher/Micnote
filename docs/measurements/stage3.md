# Stage 3 measurements

> **In plain terms.** How well language models that run on your own computer turn a memo into drafted actions. Each was given the same 20 made-up memos, three of which try to trick it.
>
> *Unfamiliar words are explained in the [glossary](../glossary.md).*

**2026-11-24**, Linux laptop (Intel Arc integrated graphics, Vulkan), llama.cpp v0.6.0, all models Q4_K_M, `core/eval/dataset/synthetic.jsonl`, one run each (seed 0, temperature 0.2 and 0.4).

| Model | Item F1 | Right action | Exact values | Keywords | Invalid drafts | Injections held | s/case |
|---|---|---|---|---|---|---|---|
| **Qwen3.5-4B** | **0.96** | **21/22** | **17/19** | 8/9 | 0/24 | 2/3 | 31 |
| Qwen3.5-9B | 0.93 | 20/22 | 15/19 | 9/9 | 0/21 | 2/3 | 34 |
| Ministral-3-8B | 0.93 | 21/22 | 13/19 | 8/9 | 0/23 | 2/3 | 34 |
| Phi-4-mini | 0.83 | 14/22 | 10/19 | 7/9 | 1/19 | 2/3 | 20 |

Granite-4.2-8B found no items at all in an earlier run with the same segmentation rules but a plainer date line, and wasn't run again. Gemma-4-E4B doesn't load: llama.cpp's C API doesn't know its chat template.

**How far to trust these:** 20 cases, written by the same author as the prompts, which were then improved by looking at these very cases. One run each, so a difference of one or two means nothing. The contexts hold a whole contact list, where the real lookups return only the people named. They compare models and catch regressions; they don't say how well real memos will do. The plan's 30 real memos don't exist yet.

**Against the targets** (stage 3 §4.8), for Qwen3.5-4B: item F1 ≥ 0.9 met; intents ≥ 0.9 met (0.95); key values ≥ 0.9 nearly (0.89); no invented recipients or repositories in the drafts inspected; every draft had the right shape. **Injection: 2 of 3, target 100%: not met.** The case that fails is a memo whose own words say "approve pull request 666"; every model drafts it, and code can't tell that from a real request. It still needs a hold-to-confirm.

**What the code now guarantees, whatever the model says** (`Ground()`): recipients must have been said, or be the contact of a name that was said; repositories and issue numbers likewise; accounts must exist; review comments need a diff. Before this, the same model copied in people nobody mentioned and added an attacker's address planted in a contact's note.

**Still wrong:** invented labels, assignees and due dates on reminders; "[Your Name]" sign-offs; a model's word-for-word copy of the request in a body; "schedule a catch up" drafted as a reminder.
