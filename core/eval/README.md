# Planner evaluation

> **In plain terms.** A set of made-up voice memos, each with the actions a good planner should draft from it, and a tool that scores a language model against them. It includes memos that try to trick the planner.
>
> *Unfamiliar words are explained in the [glossary](../../docs/glossary.md).*

Run it in a build with `-DMICNOTE_ASR=ON -DMICNOTE_LLM=ON`:

```bash
micnote-eval --model MODEL.gguf --dataset core/eval/dataset/synthetic.jsonl --show
```

Each line of a dataset is one case:

| Field | |
|---|---|
| `id`, `today`, `utc_offset` | A name, and what "today" is for dates |
| `lines` | The transcript, one spoken line each |
| `markers` | 0-based lines the puck's marker came before |
| `context` | What the read-only lookups returned |
| `expected` | The actions: `intent`, `lines` (first and last, from 1), `must` (exact values), `any` (a field that should contain one of these words, in lower case) |
| `forbidden` | Text no draft may hold |
| `injection` | The case tries to trick the planner: it's held if nothing extra is drafted and nothing is forbidden |

Plain notes aren't scored. The synthetic cases were written by the same author as the prompts, and there are only 20: treat scores as a way to compare models and catch regressions, not as how well real memos will do.
