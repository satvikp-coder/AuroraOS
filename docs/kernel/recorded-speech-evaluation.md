# Evaluating recorded offline speech

Aurora currently has host-tested voice activity detection and command routing. The following experiment measures a host decoder and provides a protocol for evaluating the three proposed commands before connecting a recognizer to Aura.

## Host reference

PocketSphinx is a candidate for a bounded English command recognizer. It provides a C decoder, an English acoustic model and dictionary, and grammar or keyword search. Keep the decoder, model and dictionary notices with their exact versions. The installed English acoustic model carries Alpha Cephei's two-clause redistribution terms. [PocketSphinx](https://github.com/cmusphinx/pocketsphinx) and [acoustic model notice](https://github.com/cmusphinx/pocketsphinx/blob/main/model/en-us/en-us/README).

On x86-64 Linux, the installed Debian libpocketsphinx3 0.8+5prealpha+1-15+b4 and pocketsphinx-en-us 0.8+5prealpha+1-15 transcribed the upstream recording as **go forward ten meters**. The input was 89,160 bytes of mono signed 16-bit little-endian PCM at 16 kHz, SHA-256 `f15c60ec54059d8b66e410d0064945a0b0a04ea56e1ddca1958e493c0cf70e71`. One warm-host observation using a Python ctypes harness and the full packaged English language model took 0.30 s for decoder initialization and 0.44 s to decode 2.79 s of audio. Peak RSS of the entire process was 112,636 KiB. These figures describe one host invocation. [Pinned recording](https://github.com/cmusphinx/pocketsphinx/blob/f20ff1b7a5db64c5e892a798479e20df86e79a35/test/data/goforward.raw).

Whisper.cpp is a candidate for a host transcription baseline when general dictation is needed. Upstream lists 75 MiB on disk and approximately 273 MB of memory for its tiny model. The original Whisper code and weights use the MIT license. An Aurora EL0 memory and platform port requires separate measurement. [Whisper.cpp memory table](https://github.com/ggml-org/whisper.cpp#memory-usage) and [Whisper license](https://github.com/openai/whisper#license).

## Held-out command protocol

Recruit six consenting speakers for an initial smoke corpus. Assign two speakers to development and four to evaluation before recording. Per speaker, record two independent takes of each command and six negative utterances including negations, mentions of commands in conversation, and similar-sounding words. Add room tone as a separately reported noise group. Preserve the original take as a complete utterance. Record consent for the intended distribution and keep the speaker key private. Store a pseudonymous speaker ID, recording or dataset source and license, split, expected command, audio format and SHA-256 for every clip. Use stable clip IDs. Retain the evaluation speakers for a single frozen model, grammar and rejection threshold. Report device and noise conditions with the scores. This small corpus can reveal regressions; its results do not estimate population accuracy.

For public negatives, isolated words from the official Speech Commands evaluation split can supplement recorded negatives. Isolated keywords cannot validate the multiword commands. Record the upstream release, original path, digest and attribution. [Speech Commands dataset and evaluation splits](https://www.tensorflow.org/datasets/catalog/speech_commands) and [dataset paper](https://arxiv.org/abs/1804.03209).

Export a labelled TSV with columns `clip` and `expected` and a decoder TSV with columns `clip` and `transcript`. Use literal tab separators. Expected values are `open home`, `open notes`, `cancel timer`, or `none`. Include one transcript row for every clip, including empty hypotheses. Run:

```sh
python3 tests/tools/score-speech.py labels.tsv transcripts.tsv
```

The scorer counts correct, missed and wrong commands, correct rejections and false accepts. Matching folds case and whitespace only. It compares complete utterances. Run separately for recorded speech, recorded noise and synthetic test vectors. Report the number of negatives with the false-accept count and the number of positives with the command counts.

## Proposed native boundary

Use a bounded utterance of mono 16 kHz signed PCM. The trusted capture service owns permission, the visible listening state and a capture session tied to the requester generation. A decoder service in EL0 receives read-only samples and verified model assets and returns a supported command ID or no match for that utterance. The kernel attests the requester and session when accepting a result. Reject stale results after revocation, generation change or session completion, and reject unsupported formats, overlong inputs and unavailable models. Deliver accepted IDs through Aura validation and the existing Action Broker. Measure peak memory, CPU time, end-of-utterance latency and negative false accepts before choosing a native backend. No recorded Aurora command or native model was evaluated in the host reference above.

## Public recorded negative controls

We selected one recording of the spoken digit zero from each of the six speakers in the Free Spoken Digit Dataset (FSDD), using take index 0 for everyone. The upstream test split contains indices 0 through 4. This is a recording-level holdout; FSDD includes the same speakers in its training split. Independence from a decoder's own pretraining data is unknown. Credit the FSDD contributors for the original clips. The dataset is distributed under CC BY-SA 4.0. [Pinned dataset, split and license](https://github.com/Jakobovski/free-spoken-digit-dataset/blob/26eb9aaf76e81b692f806f9140c2d2777410d7a1/README.md) and [license terms](https://creativecommons.org/licenses/by-sa/4.0/).

The table records the original WAV digests and hypotheses from the host PocketSphinx configuration above. Each clip used a fresh decoder. We converted mono signed PCM from 8 to 16 kHz by retaining each original sample and inserting the rounded-down average of adjacent samples, repeating the final sample. No audio is redistributed here. [Pinned recordings](https://github.com/Jakobovski/free-spoken-digit-dataset/tree/26eb9aaf76e81b692f806f9140c2d2777410d7a1/recordings).

| Clip | SHA-256 | Host transcript |
| --- | --- | --- |
| `0_george_0.wav` | `228ab63fccdf262d2e05817b6ec918b15e7d9e4bfb6bb20183c46ae088405240` | you know |
| `0_jackson_0.wav` | `eea86018ce1730baaf7f5dd6ec88c1f727dd90203521a9115b489310a248ea05` | you are |
| `0_lucas_0.wav` | `1d942e79c200ae1f84618314f2669d9c154718276b38ccf341630635073f7c06` | here are |
| `0_nicolas_0.wav` | `527b1d27c2b931204b24995f035580dd248b908978a27756b4698ce0aa7e1aa8` | no |
| `0_theo_0.wav` | `d5833196a91e5da8703a8afe1f944cbf0d0bae55e5af6a4eb7e92e62070bf280` | you are |
| `0_yweweler_0.wav` | `c2d53137898d6d32ae86e9703676bcaf595080a1f4e2e3efa5ede5a49f54b6a5` | you're all |

Using the complete-utterance command vocabulary, the host decoder produced **0 false accepts in 6 recorded negatives**. It transcribed zero correctly in 0 of 6 clips. This measures rejection of a single out-of-vocabulary word. Aurora command recall, rejection of confusable phrases, and native recognition remain unmeasured.

A separate host replay used the Aurora feature matcher and the three synthetic command templates from [the evaluated revision](https://github.com/satvikp-coder/AuroraOS/blob/f4887e87d44e635a6a0d845fb90b222efdf83b83/tests/kernel-host/speech_eval_test.c). We reused its template registration, VAD configuration, shell identity, broker setup and `sa_run_turn`. Each resampled recording started with fresh state and was divided into consecutive 320-sample frames, with zero padding on the final partial frame. We counted a frame when the result was `SA_OK` and the broker returned `BP_ALLOW_NOW`. No action executor ran.

| Speaker | Frames | False home decisions, distance 40 | False home decisions, distance 20 |
| --- | ---: | ---: | ---: |
| george | 15 | 15 | 14 |
| jackson | 33 | 31 | 23 |
| lucas | 32 | 16 | 14 |
| nicolas | 22 | 21 | 12 |
| theo | 20 | 3 | 0 |
| yweweler | 20 | 13 | 0 |
| **Total** | **142** | **99** | **63** |

**All six clips produced a false activation at distance 40. Four of six still produced a false activation at distance 20.** Every false decision proposed home. Adjacent frames within a clip are correlated. These six negative observations concern the host fixture's synthetic templates and shell requester. PocketSphinx ran independently and its exact-transcript scorer rejected all six clips. Neither distance setting was selected using these recordings.

To reproduce on a Linux host with Python 3 and a C compiler, place the six original WAVs listed above in a directory containing just those clips. Verify their digests against the table. From a checkout containing the replay tool, run the following with `clips` set to that directory. Each command builds the same harness against the named revision and emits one row per recording. The test makes broker decisions in memory.

```sh
git worktree add --detach ../speech-distance40 f084dea0f97f037a8baa279c71b1d6722129439f
git worktree add --detach ../speech-distance20 f4887e87d44e635a6a0d845fb90b222efdf83b83
python3 tests/tools/replay-speech-negatives.py ../speech-distance40 "$clips"/*.wav
python3 tests/tools/replay-speech-negatives.py ../speech-distance20 "$clips"/*.wav
```

To score by condition, append an optional third column named `group` to the labels TSV and pass `--breakdown` to the scorer. Each row of the breakdown includes the clip count and five outcome counts for one group and expected command. For a command row, recall is correct commands divided by clips. For a negative row, the false-accept fraction is false accepts divided by clips. An absent row has no observations.
