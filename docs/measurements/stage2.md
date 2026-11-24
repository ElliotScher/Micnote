# Stage 2 measurements

> **In plain terms.** How accurate speech recognition is, and how fast. Accuracy is the *word error rate* (WER): the share of words it gets wrong. Speed is the *real-time factor* (RTF): the time it takes, divided by the length of the audio, so 0.1 means an hour of audio takes 6 minutes and anything above 1 is slower than listening to it.
>
> *Unfamiliar words are explained in the [glossary](../glossary.md).*

**2026-11-23 and 24**, on a laptop: Intel Core Ultra 7 165H (6P+8E+2LP cores), Intel Arc integrated graphics (Vulkan, Mesa), 32 GB RAM, NixOS, on mains power with the `balance_power` setting (cores around 2 GHz under load), desktop apps open. gcc 16, RelWithDebInfo, AVX2. whisper.cpp v1.9.5, beam 5, 8 threads.

**Data:** 100 utterances of LibriSpeech test-clean (every 26th), 753 s, 4-15 s each for the most part. "Real recording" means through Opus with the puck's settings (16 kbps, complexity 3) and with VAD on (Silero v6.2.0).

**Reproduce:** `micnote-wer --model MODEL --librispeech DIR --backend cpu|vulkan [--codec --vad SILERO]` (`core/bench/`).

| Model | WER, clean (Vulkan / CPU) | WER, real recording (Vulkan / CPU) | RTF, Vulkan, 2 runs | RTF, CPU |
|---|---|---|---|---|
| base.en q5_1 | 5.08% / 5.13% | 6.20% / 6.25% | 0.060, 0.074 | 0.185 |
| small.en q5_1 | 3.61% / 3.71% | 3.91% / 4.05% | 0.106, 0.171 | 0.632 |
| large-v3-turbo q5_0 | 2.10% / 2.05% | 2.78% / not run | 0.265, 0.411 | 3.306 |

RTF is for clean audio; codec and VAD changed it by -27% to +43% on Vulkan (0.078, 0.121, 0.282 in the first run; 0.106, 0.185, 0.300 in the second) and +10% to +15% on the CPU.

**How far to trust these**
- **Speed varies by about 50% between runs** on this machine (heat, power limits, other programs). The first Vulkan run was before the CPU side used AVX2, and was still the faster one.
- **RTF is pessimistic for real recordings:** whisper always processes 30 s windows, and these clips are much shorter.
- **LibriSpeech is clean read-aloud English.** Meetings will have more errors. There are no measurements on our own recordings (none exist yet), on a Mac (Metal), on a discrete GPU (CUDA or Vulkan) or on phones.

**Against the targets** (stage 2 §6)
- WER ≤ 8% with large-v3-turbo: **met** (2.1%; 2.8% as a real recording).
- RTF ≤ 0.1 on a desktop GPU: **not measured** (integrated graphics here: 0.27-0.41).
- RTF ≤ 0.5 on a laptop CPU: met by base.en only. large-v3-turbo, then the default for any desktop with 8 GB of RAM, is **more than 3 times slower than real time without a GPU**. Since 2026-11-24 it is the default only with one; small.en is the default otherwise.
