# Third-Party Licenses

This project's own code is MIT-licensed (see [`LICENSE`](LICENSE)). It builds on and links against
the third-party components listed below, each under its own license. "In `.deb`?" indicates whether
the component (or its output) is actually copied into the Debian package built from
[`cmake/cm0-package.cmake`](cmake/cm0-package.cmake), as opposed to being a build-time-only
dependency (fetched/compiled from source but not redistributed) or a system library the package
merely depends on (`Depends:`, resolved from the target's own `apt` repositories at install time).

| Component | License | Used for | In `.deb`? |
| --- | --- | --- | --- |
| [LVGL](https://github.com/lvgl/lvgl) v9.5.0 | MIT | Core GUI framework — all screens, widgets, rendering | Statically linked into the app binary |
| [SQLite](https://www.sqlite.org/) 3.53.3 (amalgamation) | Public domain | Local message-history storage (`src/adapters/storage/sqlite_history_store.cpp`) | Statically compiled into the app binary (`lora_messenger_sqlite`) |
| Noto Sans JP / Noto Sans SC (subset) | SIL OFL 1.1 | CJK glyph rendering for the Japanese and Simplified Chinese UI, subsetted at build time (`tools/font/README.md`) to only the glyphs actually used | Yes — `assets/fonts/lora-ui-{ja,zh-hans}.otf` are installed to `/usr/share/lora-messenger/fonts/` |
| Inter Regular / Medium | SIL OFL 1.1 | Latin UI font | Yes — installed to `/usr/share/lora-messenger/fonts/` |
| Inter Semibold / Bold | SIL OFL 1.1 | Inherited from the upstream template | No — auditable source-tree asset only, not installed by the current package definition |
| Phosphor Icons (`Phosphor-Fill.ttf`) | MIT | Inherited from the upstream template | No — auditable source-tree asset only, not currently used by any screen |
| [fmt](https://github.com/fmtlib/fmt) 10.1.1 | MIT | Logging and formatted strings | No — dynamically linked against the system `libfmt` package (`Depends:`) on both macOS (Homebrew) and the CardputerZero sysroot; only fetched and statically compiled from the pinned source as a fallback if the target sysroot lacks it |
| [SDL2](https://www.libsdl.org/) | zlib | Desktop simulator display/input backend (macOS/Linux development builds only) | No — desktop-only; the CardputerZero device build does not use SDL at all |
| [FreeType](https://freetype.org/) | FTL or GPLv2 (dual-licensed; this project uses the FTL option, compatible with MIT distribution) | Runtime TTF/OTF font rasterization (Latin + CJK) | No — dynamically linked against the system `libfreetype6` package (`Depends:`), not bundled |
| libpng, libjpeg-turbo, zlib | libpng license, IJG/BSD-style, zlib | Image decoding used by LVGL (PNG/JPEG assets) and general compression | No — dynamically linked against system packages (`Depends:`) |

## Notes

- **Template origin**: this repository imports several font assets (Inter, Phosphor Icons) from the
  CardputerZero official template (https://github.com/CardputerZero/template), which is itself
  MIT-licensed. See [`LICENSE`](LICENSE).
- **BLE**: a BlueZ/D-Bus transport is planned (`PLAN.md` Phase 8C, not yet implemented) but is not
  linked or shipped by the current `.deb` — this table will gain a BlueZ row once that lands, the
  same way BLE is documented in the sibling BattleShip project's own `THIRD_PARTY_LICENSES.md`.
- **Full provenance**: exact upstream commits/tags, download URLs, and SHA-256/SHA3-256 hashes for
  every component above (plus the project-generated icon assets) are kept in
  [`assets/licenses/THIRD_PARTY_NOTICES.md`](assets/licenses/THIRD_PARTY_NOTICES.md) — this file is
  a discoverable summary of that more detailed record, not a replacement for it.
