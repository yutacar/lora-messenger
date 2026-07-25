# UI font subset regeneration

The normal configure and build never download fonts and do not require a font
subsetting tool. The two generated OTF files under `assets/fonts/` are committed
runtime assets. Regenerate them only when the production translation catalog
changes.

## Fixed inputs

- HarfBuzz: `hb-subset (HarfBuzz) 14.2.1`
- Noto CJK release: `Sans2.004`
- Pinned commit: `523d033d6cb47f4a80c58a35753646f5c3608a78`
- `NotoSansCJKjp-Medium.otf` SHA-256:
  `dd523e580e3413c480b2d701bf64e534c20f8419e3cfb6a44c2bdcd8d2a6c052`
- `NotoSansCJKsc-Medium.otf` SHA-256:
  `ca094f6b0001fb048ca39ddd797a0cdb0179e1e55c6561e111c49c3e6a61d7b7`
- `tools/font/ui-glyphs.txt` SHA-256:
  `7d24ea8db35d015db195f2150693da7a830bd50cafd42fcc96aacce66dd48925`

The original OTF files are deliberately not stored in this repository. Download
them from the immutable official URLs recorded in
`assets/licenses/THIRD_PARTY_NOTICES.md`, verify these hashes, and place them in
a temporary directory.

`ui-glyphs.txt` must contain exactly the union of all 70 production `StringId`
values in all three locales, the locale codes and display names, printable ASCII
U+0020 through U+007E, and U+25A1 WHITE SQUARE. The C++ coverage test is the
authoritative check; it fails if either generated font misses a catalog
character.

## Commands

Run from the repository root. This example assumes the verified originals are
in `/private/tmp`.

```sh
cmake --build build/darwin-arm64 --config Debug \
  --target lora_messenger_font_coverage_test
build/darwin-arm64/Debug/lora_messenger_font_coverage_test \
  --export-glyphs tools/font/ui-glyphs.txt

shasum -a 256 \
  /private/tmp/NotoSansCJKjp-Medium.otf \
  /private/tmp/NotoSansCJKsc-Medium.otf

hb-subset \
  --font-file=/private/tmp/NotoSansCJKjp-Medium.otf \
  --text-file=tools/font/ui-glyphs.txt \
  '--name-IDs=*' \
  '--name-languages=*' \
  '--layout-features=*' \
  '--layout-scripts=*' \
  --name-legacy \
  --notdef-outline \
  --glyph-names \
  --output-file=assets/fonts/lora-ui-ja.otf

hb-subset \
  --font-file=/private/tmp/NotoSansCJKsc-Medium.otf \
  --text-file=tools/font/ui-glyphs.txt \
  '--name-IDs=*' \
  '--name-languages=*' \
  '--layout-features=*' \
  '--layout-scripts=*' \
  --name-legacy \
  --notdef-outline \
  --glyph-names \
  --output-file=assets/fonts/lora-ui-zh-hans.otf

shasum -a 256 \
  tools/font/ui-glyphs.txt \
  assets/fonts/lora-ui-ja.otf \
  assets/fonts/lora-ui-zh-hans.otf
```

Expected generated files:

| File | Size | SHA-256 |
|---|---:|---|
| `lora-ui-ja.otf` | 192,528 bytes | `056b8fefacf817a1f5484a6a43b31d80bfefa8c8537a3d32b155e0302cd8e751` |
| `lora-ui-zh-hans.otf` | 190,996 bytes | `90efd9ceb2d92116168da7f341dcd90ab446f14e4bdd17ec586f8cc6ee8941ea` |

Running the commands twice with the fixed inputs and HarfBuzz version above
must produce byte-identical files. Font inspection must report the internal
families `Noto Sans CJK JP` and `Noto Sans CJK SC`, style `Medium`, and version
2.004.

The generated fonts remain under the SIL Open Font License 1.1. The complete
license is `assets/licenses/Noto-CJK-OFL-1.1.txt`; provenance and source/output
hashes are in `assets/licenses/THIRD_PARTY_NOTICES.md`.
