# Third-party and generated-asset notices

LoRa Messenger is licensed separately under the repository `LICENSE`. This
notice identifies third-party code, fonts, and project-generated image assets
present in the source tree or CardputerZero package. It does not replace any
upstream license text.

The CardputerZero package installs Inter Regular and Medium, the two Noto Sans
CJK UI subsets, and the application icon variants. Inter Semibold and Bold plus
Phosphor Icons remain auditable source-tree assets but are not installed by the
current package definition. Source archives are not required by the normal
build; immutable commits, URLs, and hashes below identify imported material.

## LVGL 9.5.0

- Upstream project: <https://github.com/lvgl/lvgl>
- Version/tag: `v9.5.0`
- Pinned commit: `85aa60d18b3d5e5588d7b247abf90198f07c8a63`
- Main license: MIT; see `LVGL-MIT.txt`.

LVGL is built into the application. The CardputerZero configuration enables
the built-in TLSF allocator, built-in `printf` implementation, and TJpgDec.
The pinned LVGL source/build graph also contains bundled ThorVG code; the
application disables LVGL vector-graphics features, but its notice is retained
conservatively.

| Bundled LVGL component | Copyright / origin | Terms |
|---|---|---|
| TLSF 3.1 | Copyright (c) 2006-2016, Matthew Conte | BSD 3-Clause-style notice; see `LVGL-TLSF-BSD.txt` |
| mpaland/printf | Copyright (c) 2014, Marco Paland | MIT; see `LVGL-SPRINTF-MIT.txt` |
| TJpgDec R0.03 | Copyright (C) 2021, ChaN | Upstream permissive notice; see `LVGL-TJPGD.txt` |
| ThorVG | Copyright (c) 2020-2025, ThorVG Project contributors | MIT; see `LVGL-THORVG-MIT.txt` |

## fmt 10.1.1

- Upstream project: <https://github.com/fmtlib/fmt>
- Version/tag: `10.1.1`
- Pinned commit: `f5e54359df4c26b6230fc61d38aa294581393084`
- Use: application logging and formatting. The CardputerZero fallback build
  compiles the pinned source when fmt is absent from the SDK sysroot.
- License: MIT with the upstream optional binary-object exception; see
  `fmt-MIT.txt`.

## SQLite 3.53.3 (amalgamation)

- Upstream project: <https://www.sqlite.org/>
- Archive: <https://www.sqlite.org/2026/sqlite-amalgamation-3530300.zip>
- Downloaded: 2026-07-23
- Archive SHA3-256:
  `d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9`
- `sqlite3.c` SHA3-256:
  `28e484abdaa43630e34040ef6ed92be973a1ad54107803d8af5145b889c23ed7`
- License: public domain; see `SQLite-Public-Domain.txt`.
- Full provenance and build flags: `third_party/sqlite/README.md`.

Only `sqlite3.c`, `sqlite3.h`, and `sqlite3ext.h` are vendored from the
archive, statically compiled into `lora_messenger_sqlite`
(`CMakeLists.txt`), and linked into the shipped application for local
message-history storage (`src/adapters/storage/sqlite_history_store.cpp`).

## Font assets

### Inter 18pt 4.001

Copyright 2016 The Inter Project Authors.

- Upstream project: <https://github.com/rsms/inter>
- Imported from the pinned CardputerZero Template commit
  `c08ca040af5a88f5d7c36b4ce0dd128b67a92e4f`.
- License: SIL Open Font License 1.1; see `Inter-OFL-1.1.txt`.
- License-file SHA-256:
  `f3cd9bba601cb377c66aee22cccc9e362f99b456aa3409f9f65cac865a11da1f`.
- These files are distributed unchanged from that pinned source.

| Distributed file | Immutable acquisition URL | SHA-256 |
|---|---|---|
| `inter-regular.ttf` | `https://raw.githubusercontent.com/CardputerZero/Template/c08ca040af5a88f5d7c36b4ce0dd128b67a92e4f/assets/fonts/inter-regular.ttf` | `3e5f90a0138b38de4cf4d779ad78391974ea1df776b9164842bdcbb60ce383c5` |
| `inter-medium.ttf` | `https://raw.githubusercontent.com/CardputerZero/Template/c08ca040af5a88f5d7c36b4ce0dd128b67a92e4f/assets/fonts/inter-medium.ttf` | `7c7206c451a89a8fa8f38f3c217f67e60b830db9e21645e36155cc62cd1f9903` |
| `inter-semibold.ttf` | `https://raw.githubusercontent.com/CardputerZero/Template/c08ca040af5a88f5d7c36b4ce0dd128b67a92e4f/assets/fonts/inter-semibold.ttf` | `a8b276e25bb13dfa39cface35cc92aff9a7d5f1b96143f0df8c66351ccfed2a4` |
| `inter-bold.ttf` | `https://raw.githubusercontent.com/CardputerZero/Template/c08ca040af5a88f5d7c36b4ce0dd128b67a92e4f/assets/fonts/inter-bold.ttf` | `30a5c45ec23a594af2effe8d3b589ad22c2dede27441050a1604d00ff82fd0dc` |

The embedded version record is `Version 4.001;git-66647c0bb`.

### Phosphor Icons 2.1

Copyright (c) 2020 Phosphor Icons.

- Upstream project: <https://phosphoricons.com>
- Imported from the pinned CardputerZero Template commit
  `c08ca040af5a88f5d7c36b4ce0dd128b67a92e4f`.
- Immutable acquisition URL:
  `https://raw.githubusercontent.com/CardputerZero/Template/c08ca040af5a88f5d7c36b4ce0dd128b67a92e4f/assets/fonts/Phosphor-Fill.ttf`
- Distributed file: `Phosphor-Fill.ttf`
- SHA-256: `a53f5d2630cab5e3b7536ecb9d69d71519a2190298c22b1f8d770dd37bc2940a`
- License: MIT; see `Phosphor-MIT.txt`.
- License-file SHA-256:
  `6918b72504641180600cbbd4a86b0dfa9dfccf788775694325b71b9a029f6eb4`.

The embedded font metadata reports Version 2.1 and points to the Phosphor
project and its MIT license.

### Noto Sans CJK 2.004 UI subsets

Copyright © 2014-2021 Adobe (<http://www.adobe.com/>).

- Official upstream: <https://github.com/notofonts/noto-cjk>
- Release tag: `Sans2.004`
- Pinned upstream commit: `523d033d6cb47f4a80c58a35753646f5c3608a78`
- License: SIL Open Font License 1.1; see `Noto-CJK-OFL-1.1.txt`.
- License-file SHA-256:
  `6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2`.
- Subsetter: `hb-subset (HarfBuzz) 14.2.1`.
- Generation input: `tools/font/ui-glyphs.txt`, SHA-256
  `7d24ea8db35d015db195f2150693da7a830bd50cafd42fcc96aacce66dd48925`.
- Exact regeneration commands are in `tools/font/README.md`.

The fixed generation commands, run from the repository root, are:

```sh
hb-subset --font-file=/private/tmp/NotoSansCJKjp-Medium.otf \
  --text-file=tools/font/ui-glyphs.txt '--name-IDs=*' \
  '--name-languages=*' '--layout-features=*' '--layout-scripts=*' \
  --name-legacy --notdef-outline --glyph-names \
  --output-file=assets/fonts/lora-ui-ja.otf

hb-subset --font-file=/private/tmp/NotoSansCJKsc-Medium.otf \
  --text-file=tools/font/ui-glyphs.txt '--name-IDs=*' \
  '--name-languages=*' '--layout-features=*' '--layout-scripts=*' \
  --name-legacy --notdef-outline --glyph-names \
  --output-file=assets/fonts/lora-ui-zh-hans.otf
```

| Original file (not distributed) | Immutable official URL | Source SHA-256 |
|---|---|---|
| `NotoSansCJKjp-Medium.otf` | `https://raw.githubusercontent.com/notofonts/noto-cjk/523d033d6cb47f4a80c58a35753646f5c3608a78/Sans/OTF/Japanese/NotoSansCJKjp-Medium.otf` | `dd523e580e3413c480b2d701bf64e534c20f8419e3cfb6a44c2bdcd8d2a6c052` |
| `NotoSansCJKsc-Medium.otf` | `https://raw.githubusercontent.com/notofonts/noto-cjk/523d033d6cb47f4a80c58a35753646f5c3608a78/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Medium.otf` | `ca094f6b0001fb048ca39ddd797a0cdb0179e1e55c6561e111c49c3e6a61d7b7` |
| `LICENSE` | `https://raw.githubusercontent.com/notofonts/noto-cjk/523d033d6cb47f4a80c58a35753646f5c3608a78/LICENSE` | `6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2` |

| Distributed modified font | Size | Output SHA-256 |
|---|---:|---|
| `lora-ui-ja.otf` | 192,528 bytes | `056b8fefacf817a1f5484a6a43b31d80bfefa8c8537a3d32b155e0302cd8e751` |
| `lora-ui-zh-hans.otf` | 190,996 bytes | `90efd9ceb2d92116168da7f341dcd90ab446f14e4bdd17ec586f8cc6ee8941ea` |

The subsets preserve the original Noto Sans CJK JP/SC Medium internal family,
version, copyright and license metadata. They contain the union of all shipped
English, Japanese and Simplified Chinese UI strings, printable ASCII, and
U+25A1 WHITE SQUARE for the documented unsupported-character display fallback.

## Application icon provenance

The following PNG files are project-generated application assets:

- `lora-messenger-store.png`
- `lora-messenger.png`
- `lora-messenger_100.png`
- `lora-messenger_80.png`

The icon was created specifically for LoRa Messenger in 2026 with OpenAI image
generation tooling. The final version restores the project's earlier cyan,
white, navy, and amber foreground design and changes only its background to
blue; no third-party artwork file was used as the edit reference. The smaller
runtime files are local resized derivatives of the opaque blue-background
store image.

This provenance record does not add a third-party license, imply endorsement by
the tool provider, or make a representation about copyrightability. Any project
grant is stated by the repository `LICENSE`.

No Kenney asset is present or distributed by this project.
