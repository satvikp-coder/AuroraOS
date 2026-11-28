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
