# Preparing consented recordings for Aurora command evaluation

The SLURP official test manifest at revision `8eb16545762be97ace75334109d73824217311f1` contains 2,974 annotated utterances. A full-utterance comparison after lowercasing and collapsing whitespace found zero occurrences of **open home**, **open notes**, or **cancel timer**. Its authors license text under CC BY 4.0 and audio under CC BY-NC 4.0. We used only the annotations for this check. [Pinned test annotations](https://github.com/pswietojanski/slurp/blob/8eb16545762be97ace75334109d73824217311f1/dataset/slurp/test.jsonl) and [upstream licensing](https://github.com/pswietojanski/slurp/tree/8eb16545762be97ace75334109d73824217311f1#license).

## Collecting the exact phrases

Before collecting anything, tell each volunteer who will have access to their voice, whether audio or transcripts will be published, how long each will be retained, and whom to contact about withdrawal. Obtain an affirmative choice for local evaluation and a separate choice for redistribution if needed. Keep contact details and the consent record outside the evaluation directory. Use an opaque reference to that record in the private manifest. The tool checks only that the reference is present. The corpus custodian verifies the actual authorization.

Assign a pseudonymous speaker to one split before recording. For the initial smoke corpus, use two development speakers and four evaluation speakers. Ask each to record two complete takes per command plus negatives including conversational mentions, negation and similar-sounding phrases. Shuffle prompt order. Keep mistakes and rerecordings distinct. A human reviewer should listen and independently label the whole utterance before freezing the corpus. Keep all takes, rooms and devices for each speaker within that speaker's split. Exclude TTS output and edited word concatenations from recorded command results.

## Preparing and scoring

Place consented WAVs and a private TSV manifest together locally. Use this exact header, with literal tab separators.

```text
clip	speaker	split	expected	consent_ref	sha256
```

Use relative clip paths, anonymized speaker IDs, `development` or `evaluation`, and one of the three command strings or `none`. Supply the lowercase SHA-256 of the original mono 16 kHz signed 16-bit PCM WAV. Keep the manifest, private audio and decoder configuration under versioned local custody. Record the model digest and freeze its rejection threshold before opening the evaluation split.

```sh
python3 tests/tools/prepare-heldout-speech.py /local/corpus/manifest.tsv > labels.tsv
python3 tests/tools/score-speech.py labels.tsv transcripts.tsv
```

The first command checks the source files and split assignments before emitting evaluation labels. It rejects exact PCM duplicates even when WAV metadata differs. The output excludes speaker and consent references. Capture one complete decoder result per evaluation clip, including an empty transcript for a rejection. Keep labels out of the decoder input. Report positives and negatives by expected command, speaker and condition, along with sample counts. A rejected or missing corpus provides no recognition measurement. This workflow reads local files and makes no broker calls.
