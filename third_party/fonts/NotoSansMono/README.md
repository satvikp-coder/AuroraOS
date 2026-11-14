# Noto Sans Mono (bitmap derivative) - provenance and licence

AuroraOS draws native text with bitmaps derived from **Noto Sans Mono**.

| Item | Value |
|---|---|
| Upstream project | https://github.com/notofonts/latin-greek-cyrillic |
| Release used | `NotoSansMono-v2.014` (2023-09-30), release asset `NotoSansMono-v2.014.zip`, files `googlefonts/ttf/NotoSansMono-Regular.ttf` and `-Bold.ttf` |
| Release archive SHA-256 | `090cf6c5e03f337a755630ca888b1fef463e64ae7b33ee134e9309c05f978732` |
| NotoSansMono-Regular.ttf SHA-256 | `74fd536351d0f30a73410e1bd223a0ebf763dd4808eb844aa2aed18c0d6e3c84` |
| NotoSansMono-Bold.ttf SHA-256 | `4d063be01a00694bf7183d6799d5611760121dcfd526f8b07280ba79115ce02a` |
| Copyright | Copyright 2022 The Noto Project Authors (https://github.com/notofonts/latin-greek-cyrillic) |
| Licence | SIL Open Font License, Version 1.1, `OFL.txt` (SHA-256 `cee9892f9f0cc8fe882c9e9537ee6a89621d86ee7ceaf70b02e2b2b1c25c061a`), byte-identical to the file inside the release archive and to `OFL.txt` in the upstream repository at the release tag |
| Reserved Font Name | none declared by the upstream copyright notice |
| Authors | `AUTHORS.txt` (copied from the release archive) |

The TrueType files themselves are **not** stored in this repository. The generator refuses any input whose SHA-256 differs from the values above and also checks the licence strings stored inside the font (name IDs 13 and 14) for "SIL Open Font License" and "1.1".

## What AuroraOS ships

`kernel/gfx/font_noto_mono.c` is a **modified version** of the font software: the outlines of printable ASCII (U+0020 to U+007E) rasterized to 1-bit bitmaps at 12, 14, 16, 18 and 24 pixels, regular (400) and bold (700), with glyphs scaled horizontally by at most about 4 percent so they fit an integer character pitch. Under the OFL this derivative stays under the OFL; the file header says so and points to `OFL.txt`. The font is not sold on its own, and the upstream name is used only to say where the data came from, not as the name of a product. The rest of AuroraOS is not covered by the OFL.

## Reproducing the data

```
python3 tools/fontgen/gen_font.py --regular NotoSansMono-Regular.ttf --bold NotoSansMono-Bold.ttf --out kernel/gfx/font_noto_mono.c
```

Standard library only. The output is byte-identical on repeated runs and across Python interpreters (checked on Windows Python 3.13 and WSL Python 3.14). `tests/kernel-host/gfx_text_test.c` checks that the committed file carries the pinned hashes and notices.

## Not supported

Hindi, Gujarati and every non-ASCII character are not in the atlas and are drawn as a visible missing-glyph box. There is no shaping, no bidirectional text, no combining marks and no screen-reader or text-to-speech path in the native stack.
