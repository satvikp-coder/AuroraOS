# Aurora Glass: performance profile (M8-H)

Everything below was **measured** with `tests/perf/profile.js` (run it again: `node tests/perf/profile.js --runs 7 --tier clear --out result.json`). Raw results are in
`docs/agents/perf/`. Nothing here is estimated, and nothing here is a phone number.

## Read this first: what these numbers are and are not

- **Environment:** headless Chromium 131 on a 13th-gen Intel i7-1355U laptop (12 logical CPUs, 16 GB RAM, about 1.8-2.2 GB free during runs), Windows 11, Node 24, **software
  rendering**, viewport 390x844, cold cache, local server without compression. It is **not** a phone, has no GPU raster, and was run while the machine was under memory
  pressure.
- **This machine is noisy.** The same code, measured in different runs, moved by up to about 40%: cold-load DOMContentLoaded was 311, 340 and 231 ms (the last number is
  from a run whose only difference is a tier switch made *after* the load, so it cannot be a real effect). Treat every small number (single-digit to ~25 ms) as "about a
  frame or less". **Only differences of several times are trustworthy** (the Vault change below was confirmed in two interleaved before/after pairs).
- Interaction times include style and layout (a forced flush), not paint.
- The browser prototype is not the native OS. **Do not read any of this as an AuroraOS or phone performance claim.**

## Baseline (the shipped prototype, measured)

| Measure | Result |
| --- | --- |
| Files served | 42 requests: 29 JS (388,618 B), 12 CSS (96,976 B), 1 HTML (18,216 B) = **503,810 B raw, 155,304 B gzip (level 9)**. Largest: app.js 44 KB, broker.js 32 KB, shell.css 29 KB, vault.js 28 KB, components.css 26 KB, review.js 24 KB. The dev server does not compress, so raw is what a local run transfers. |
| Cold load (Soft, the app default; 7 runs) | DOMContentLoaded 311-340 ms median across runs, first contentful paint about 245-252 ms, script 58-80 ms, layout 69-80 ms, style 37-46 ms, JS heap about 2.1 MB. Run-to-run spread is large (see above). |
| Document size | About 1,316 elements on Home and 1,339-1,366 on the other screens (every screen's markup is in the document, hidden ones are not laid out); Chromium counts about 2,300 nodes including text. |
| Open an app (warm, median over repeated opens) | Roughly 2-22 ms across runs for Notes, Clock, Calculator, Trail, Settings and Vault (the high end is from the noisiest run); Passport 13-28 ms. A CPU profile of Passport alone showed most of its time is `focus()` and layout (accessibility focus management), so it was **left alone**. |
| Index keystroke (14 queries) | median 2.6-5.9 ms, worst 6.3-18.6 ms across runs |
| Context menu open | median 3-7 ms |
| Frames while scrolling a 12,000 px list | Clear and Soft Glass were **indistinguishable here**: mean interval 16.7-16.9 ms (vsync-bound), 0-4 frames over 20 ms per 150. This does **not** show that Soft Glass is cheap on a phone: software rendering in headless Chrome says nothing about GPU blur cost. It remains **unmeasured on real hardware**. |
| `backdrop-filter` use | 5 lines in `components.css` (2) and `shell.css` (3) (static count) |

**Clear mode as baseline:** the interaction numbers were repeated with the tier set to Clear (`--tier clear`, file `after-vault-fix-clear-tier.json`): within noise of the Soft runs, as the frame test also showed. No Clear-vs-Soft cost is claimed.

## The one real finding: Vault cost grows with the number of files

The prototype ships 10 sample files, where Vault is fast (open about 9-27 ms, select click about 16-25 ms). But the list is rebuilt on every selection, search keystroke and
open, and the cost grew roughly linearly: at 1,000 files a single click cost about 350-450 ms.

A CPU profile (200 files, 20 select clicks) attributed the time: about **48% forced style and layout of the rebuilt list**, about 12% sorting (the comparator called
`localeCompare(..., options)` on every comparison, which builds a collator each time), and the rest building DOM nodes.

### Changes (small, no behaviour change)

1. **`vault-model.js`: one shared `Intl.Collator`** for name ordering instead of `localeCompare` with options per comparison. Same order by construction; a new unit test
   compares it against the old `localeCompare` on accents, case, digits, empty names and id tie-breaks, and fails if the numeric option is removed.
2. **`vault.js` / `vault.css`: `content-visibility: auto`** on rows, **only when the list has more than 60 files** (class `is-large`). Off-screen rows skip layout and paint.
   The normal 10-file Vault is byte-for-byte the same layout as before (a test asserts rows are fully rendered at 10 and at exactly 60).

### Measured effect (median ms, two interleaved before/after runs; "after" is the same machine minutes later)

| Vault, files | Action | Before (run 1 / run 2) | After (run 1 / run 2) |
| --- | --- | --- | --- |
| 200 | open screen | 89.4 / 111.3 | 70.1 / 61.5 |
| 200 | search keystroke | 40.3 / 34.4 | 37.8 / 30.2 |
| 200 | select click | 91.5 / 65.3 | 52.8 / 17.2 |
| 1000 | open screen | 559.4 / 538.6 | 194 / 149.9 |
| 1000 | search keystroke | 402.4 / 284.6 | 117 / 60.3 |
| 1000 | select click | 453.6 / 350.6 | 139.6 / 93.4 |
| 10 | all three | unchanged within noise | |

At 1,000 files that is roughly 3-4x faster; at 200 files it is a smaller, less certain gain. Selection and search still rebuild the whole list: the remaining cost is DOM
construction, and the proper fix (keyed updates or virtualised rows) was **not** done because the risk to Vault's keyboard/selection behaviour outweighs a gain nobody
sees at the shipped 10 files. It is the next step if Vault ever holds hundreds of files.

## Deliberately not changed (measured, no cheap safe win)

- Cold load: 42 synchronous script/style requests, about 80 ms of script and 70-80 ms of layout. Reordering or deferring scripts risks init order and was not attempted
  without a failing user-visible symptom. If load time matters: bundle for production (not done; the prototype is intentionally unbundled for readability) and serve
  gzip/brotli (155 KB instead of 504 KB).
- Passport open (focus management), Index keystroke (already about one frame), context menu, Clear vs Soft frame cost (nothing measurable here).

## Not measured

Real devices, GPU blur cost, memory beyond JS heap, long sessions, network latency, Notes with many notes, battery. Add them when there is hardware to run on.
