# APP-04 — Notes version history

## Goal

Document the Notes application's existing on-disk versioning scheme as a stable, formal interface. Notes already versions its notes (tests/programs/notes_core.h): a magic header, a version number, a monotonically increasing sequence, a CRC-32, and bounded text. A note written by a NEWER version of Notes is detected and never overwritten.

## Scope

- The existing notes_core.h format is the reference implementation: one file per slot ("note.a", "note.b"), two slots written alternately.
- On-disk format (little endian): magic "ANTE", version u8 (=1), flags u8 (=0), length u16 (<= NOTE_MAX), sequence u32, CRC-32 of bytes 4..11 and the body, then the body.
- Decode result classes: NOTE_OK, NOTE_E_SHORT, NOTE_E_MAGIC, NOTE_E_NEWER (version above this program's — never overwritten), NOTE_E_VERSION, NOTE_E_LEN, NOTE_E_FLAGS, NOTE_E_CRC, NOTE_E_CHARS.
- `note_encode()` writes with an explicit sequence; `note_decode()` validates magic, version, flags, length, CRC, character set and layout fit.

## Interface (tests/programs/notes_core.h — existing, documented)

```c
#define NOTE_VERSION 1              /* this program's format version; in[4] > NOTE_VERSION => NOTE_E_NEWER */
#define NOTE_MAX 288                /* max bytes of note text */
#define NOTE_COLS 36                /* max wrapped columns per row */
#define NOTE_ROWS 8                 /* max wrapped rows */
#define NOTE_HDR 16                 /* header size in bytes */
#define NOTE_FILE_MAX (NOTE_HDR + NOTE_MAX)

enum { NOTE_OK = 0, NOTE_E_SHORT = -1, NOTE_E_MAGIC = -2, NOTE_E_NEWER = -3,
       NOTE_E_VERSION = -4, NOTE_E_LEN = -5, NOTE_E_FLAGS = -6, NOTE_E_CRC = -7,
       NOTE_E_CHARS = -8 };

struct note { char buf[NOTE_MAX]; int len, cur; };

/* Encode *n into out (capacity NOTE_FILE_MAX); returns file length (NOTE_HDR + len). */
static int note_encode(const struct note *n, nc_u32 seq, nc_u8 *out);

/* Decode untrusted bytes. On NOTE_OK, *n holds the text (cursor at the end) and *seq the
 * sequence; otherwise n is cleared. A version above NOTE_VERSION returns NOTE_E_NEWER. */
static int note_decode(const nc_u8 *in, int len, struct note *n, nc_u32 *seq);
```

## Properties

- **Version gating**: `in[4] > NOTE_VERSION` returns NOTE_E_NEWER — a note from the future is never opened or overwritten by an older build.
- **Sequence ordering**: the slot with the higher sequence wins on load; equal sequences prefer slot A.
- **CRC-32 guarded**: every decode validates the CRC over the version/flags/length/sequence header and the body.
- **Bounded**: NOTE_MAX bytes, NOTE_ROWS wrapped rows; insertions that would exceed either bound are refused, never truncated.

## Relation to APP-05

APP-04 defines the versioned on-disk format. APP-05 defines the safe restoration policy: how two alternating slots, read-back verification, and NEWER/damaged handling combine so a note is never lost.
