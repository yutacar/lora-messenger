# Development Notes

This is the append-only engineering record for phase gates. `PLAN.md` remains the
scope and decision source of truth.

## 2026-07-22 — Phase 0 approved and completed

### Authorization and scope

- User authorization: exact reply `推奨案で開始` on 2026-07-22.
- Authorized work: Phase 0 only.
- No hardware purchase, RF transmission, GitHub remote/repository, push, PR,
  AppStore submission, or publication was performed.
- Resulting status: Phase 0 complete; Phase 1 is not started.

### Upstream provenance recheck

Rechecked immediately before implementation:

| Input | URL | Resolved revision |
| --- | --- | --- |
| Standalone Template | `https://github.com/CardputerZero/Template` | `c08ca040af5a88f5d7c36b4ce0dd128b67a92e4f` |
| AppBuilder | `https://github.com/CardputerZero/AppBuilder` | `6716c0988700388d4c78c4a4f66585b02d5a3aa8` |
| AppStore packages | `https://github.com/CardputerZero/packages` | `bfaf7c85fecbc2507a0de9c1b0ed5f7049958662` |

Template import details:

- Git tree: `8791e261c5fe50fb391bf3613409419fac5be4dc`
- Deterministic `git archive --format=tar` SHA-256:
  `5f0fa35e944164e315a499b189db755bdaea568708d5ec821aae89493c42a7a7`
- The snapshot was imported without the upstream `.git` directory. The upstream
  commit is not treated as a signed release attestation.
- LVGL tag `v9.5.0` resolved during the clean configure to
  `85aa60d18b3d5e5588d7b247abf90198f07c8a63`.

### Host/tool snapshot

- Host: macOS 26.5.2, arm64.
- CMake 4.4.0, Ninja 1.13.2, Apple Clang 21.
- SDL2 2.32.70, FreeType 2.14.3, fmt 12.2.0, libpng 1.6.58,
  libjpeg-turbo 3.2.0.
- No CardputerZero hardware or cross toolchain was used.

### Implementation decisions

- IDs are separated: CMake target `lora_messenger`, executable/package
  `lora-messenger`, display name `LoRa Messenger`.
- `lora_messenger_core` has an explicit source list and standard-library-only
  dependency graph. `BUILD_UI=OFF` returns before LVGL fetch/discovery.
- The initial screen is an explicitly nonfunctional public-timeline preview with
  `SIMULATOR`, `RADIO DISABLED`, and `PHASE 0` labels.
- Real keyboard events and `APP_SCRIPT` both use `platform::route_key()`.
- Shutdown order is deterministic: clear key callback, cancel script timer,
  destroy the screen/fonts, delete inputs/display, stop SDL, deinitialize LVGL,
  then flush/reset the logger. Repeated stop requests are harmless.
- Screenshot output is fixed to `screenshot/` and the stem is restricted to
  `[A-Za-z0-9._-]`. PNG data is closed in a same-directory temporary file before
  atomic rename; failure preserves the old final file, removes the temporary file,
  propagates through `RunState`, and exits non-zero.
- Packaging installs into APPLaunch paths and contains no systemd unit. The Debian
  Maintainer value is an explicit non-publishable placeholder and must be replaced
  before Phase 5/publication.
- `app-builder.json` intentionally omits the `store` block in Phase 0. Final icon,
  320x170 store captures, localized store text, source repository, and publish
  ownership metadata are Phase 5/7 gates.

### Verification record

| Check | Command/evidence | Result |
| --- | --- | --- |
| Core configure | `cmake --preset core-only --fresh` | Passed in 4.0 s; no FetchContent or UI dependency discovery |
| Core Debug | `cmake --build --preset core-only-dbg` + `ctest --preset core-only-dbg` | Passed, 1/1 tests, 0 failures, 0.48 s test time |
| Core Release | `cmake --build --preset core-only-rel` + `ctest --preset core-only-rel` | Passed, 1/1 tests, 0 failures, 0.48 s test time |
| Core dependency scan | `rg -i 'lvgl|sdl|src/platform|radio|/opt/homebrew' build/core-only/compile_commands.json` + `otool -L build/core-only/Debug/lora_messenger_core_smoke` | No search matches; executable links only libc++ and libSystem |
| Simulator clean configure | `cmake --preset darwin-arm64 --fresh` | Passed in 30.5 s; a clean non-shallow fetch checked out LVGL `85aa60d18...` |
| Simulator Debug | `cmake --build --preset darwin-arm64-dbg -- -j4` + `ctest --preset darwin-arm64-dbg` | Passed, 3/3 tests, 0 failures, 0.22 s final test time |
| Simulator Release | `cmake --build --preset darwin-arm64-rel -- -j4` + `ctest --test-dir build/darwin-arm64 -C Release --output-on-failure` | Passed, 3/3 tests, 0 failures, 0.21 s final test time |
| Negative capture tests | CTest `script_rejects_empty_screenshot` and `script_reports_screenshot_io_failure` | Wrapper asserted exit 1, the case-specific diagnostic, and the common APP_SCRIPT failure-propagation diagnostic in Debug and Release |
| Home + capture | `APP_SCRIPT='WAIT,SHOT=phase0-home,HOME' APP_SCRIPT_INTERVAL_MS=80 SDL_VIDEODRIVER=dummy build/darwin-arm64/Debug/lora-messenger` | Exit 0; atomic capture and common teardown completed |
| Home exit | `APP_SCRIPT='HOME' APP_SCRIPT_INTERVAL_MS=20 SDL_VIDEODRIVER=dummy build/darwin-arm64/Debug/lora-messenger` | Exit 0; common teardown completed |
| Esc exit | `APP_SCRIPT='ESC' APP_SCRIPT_INTERVAL_MS=20 SDL_VIDEODRIVER=dummy build/darwin-arm64/Debug/lora-messenger` | Exit 0; common teardown completed |
| Window close | `APP_SCRIPT='CLOSE' APP_SCRIPT_INTERVAL_MS=20 SDL_VIDEODRIVER=dummy build/darwin-arm64/Debug/lora-messenger` | Exit 0; SDL close callback reached common teardown |
| Screenshot dimensions | `file screenshot/phase0-home.png` | PNG RGBA, exactly 320x170 |
| Screenshot hash | SHA-256 | `0b94965ee208562e7b87a5edcd7d419577bb4b43baefc51968d21158346d0dbc` |
| Visual inspection | Opened `screenshot/phase0-home.png` at original resolution | Passed: no clipping, overlap, missing glyph, fake radio state, or window chrome |
| Atomic failure preservation | Run from `/private/tmp/lora-atomic-preserve.1qpUg6` with a read-only `screenshot/`: `APP_SCRIPT='SHOT=keep' APP_SCRIPT_INTERVAL_MS=20 SDL_VIDEODRIVER=dummy /Users/yutacar/work/LoraMessenger/build/darwin-arm64/Debug/lora-messenger` | Exit 1; `cmp` proved the existing final file unchanged and `find` found no `*.tmp-*` residue |
| Install staging | `cmake --install build/darwin-arm64 --config Debug --prefix /private/tmp/lora-phase0-install.XEOywp` | 12 app-owned files only; absolute APPLaunch `Exec`; no `.service`, systemd, LVGL header, or LVGL library payload |
| RF implementation scan | `rg -n 'RadioLib|#include[^\n]*SX126|\.transmit\(|startTransmit\(|transmit\(' src CMakeLists.txt cmake` | No match; no RF driver or transmission path found |

Final repository-boundary, `git diff --check`, remote-empty, and commit checks are
recorded below after the verified staging tree is copied into the target repository.

### Problems found and resolved

1. The unmodified template failed against Homebrew fmt 12 because its logger used
   `fmt::format` while including only `fmt/core.h`. The logger now includes
   `fmt/format.h`; the clean Debug build passes.
2. An initial install rehearsal included LVGL headers/static libraries because
   `FetchContent_MakeAvailable` registered dependency install rules. LVGL is now
   added with `EXCLUDE_FROM_ALL`; a clean install rehearsal contains only app-owned
   payloads.
3. The template defaulted to a per-app systemd service and `/usr/bin`. Both were
   removed in favor of a user-launched APPLaunch binary/wrapper layout.
4. The first APPLaunch desktop entry used a relative `Exec`, so launch depended on
   an unspecified working directory. It now uses the absolute
   `/usr/share/APPLaunch/bin/lora-messenger-launch` path, verified in the generated
   and staged-install desktop files.
5. The BSP contains FreeType/libpng headers in dedicated include directories and a
   fmt runtime without fmt headers. CM0 lookup now includes the dedicated paths and
   follows the official fmt 10.1.1 FetchContent fallback, including Debian
   multiarch `bits/` headers. The real ARM64 build remains a later gate.
6. Pinning LVGL by commit while retaining `GIT_SHALLOW` can fail in a clean clone.
   Shallow mode was removed; a fresh configure cloned and checked out the exact
   recorded commit.
7. `APP_SCRIPT` originally ignored screenshot failure and wrote directly to the
   tracked final PNG, allowing a stale or partial file to pass later checks. Failure
   now exits non-zero, same-directory temporary output is atomically renamed only
   after successful close, and both empty-name and I/O-failure tests are automated.

### Strict review outcome

- Three read-only reviews covered architecture/package boundaries, lifecycle/timer
  safety, automation, screenshot integrity, UI layout, RF absence, and records.
- No P0 issue was found. P1 findings were fixed for APPLaunch path resolution,
  CM0 dependency fallback, clean FetchContent behavior, screenshot failure/atomic
  replacement, and completion records.
- The architecture/package reviewer completed a same-scope re-review with no
  remaining P0/P1. The lifecycle reviewer found no P0/P1 and executed Home, Esc,
  Close, and delayed Close successfully. The integrated reviewer reran verbose
  Debug/Release suites at 3/3, verified the expected diagnostics, and confirmed the
  failure wrapper rejects unrelated exit 1 and exit 0 processes. Final result: no
  remaining P0/P1.

### Explicitly not verified in Phase 0

- ARM64 native/cross build or `.deb` contents.
- CardputerZero display, keyboard mapping, APPLaunch session ABI, or removal flow.
- LoRa adapter compatibility, antenna, regional frequency/power/duty-cycle policy,
  receive, transmit, airtime, range, or two-device behavior.
- Persistence, identity, message model, protocol, localization, privacy controls,
  final icon/store metadata, or publication.

### Final repository gate

- Repository top level: `/Users/yutacar/work/LoraMessenger`.
- `git remote -v`: no output; no remote was configured.
- `git diff --cached --check`: passed immediately before the implementation commit.
- `git status --short`: no output immediately after the implementation commit.
- Initial implementation commit:
  `eca3e1f216a6c4e603f165b2035fe961789824be feat: bootstrap LoRa Messenger phase 0`.
- The implementation commit contains 50 intentional project files. Build caches,
  generated install manifests, dependencies, and temporary test fixtures are
  ignored and were not committed.
- This repository-gate record is committed separately so it can cite the immutable
  implementation commit. Its own documentation commit hash is reported in the
  Phase 0 completion handoff.

## 2026-07-23 — Phase 1 authorized and completed

- User authorization: exact reply `次へ進んでください` on 2026-07-23.
- Authorized work: Phase 1 pure-C++ core model and local rules only.
- UI expansion, persistence, protocol framing, transports, RF/hardware work,
  packaging, remote creation, push, PR, AppStore submission, and publication remain
  unapproved and were not started by this authorization.
- Execution gate: Debug/Release core suites, Address/UndefinedBehavior sanitizer,
  dependency isolation, strict P0/P1 review, documentation, and intentional local
  commits.

### Phase 1 implementation decisions

- `InstallId` and `MessageId` are distinct strong values around valid UUIDv4 data.
  UUID generation requests 16 bytes exactly once from `IRandomBytes`, then applies
  the RFC version/variant bits. Failure is explicit and never silently retried.
- `UserId` and `PostBody` validate encoded UTF-8 byte length without locale or
  Unicode normalization. The policy rejects malformed encodings, whitespace-only
  input, NUL/C0/C1 controls, bidi controls, U+2028/U+2029, and Unicode
  noncharacters. Bodies allow LF; user IDs also reject edge whitespace.
- Identity stores `last_issued_sender_sequence` with zero meaning none issued.
  Sender sequence and local receive order issue 1 through `UINT64_MAX`, commit only
  after success, and never wrap.
- `PostPayload` contains only sender/post data. `TimelineEntry` owns local receive
  order and a variant origin: received, or local `Queued`/`Broadcast`/`Failed`/
  `Unknown`. `Delivered` is intentionally absent.
- The timeline is bounded to 256 entries. It rejects identical retained message IDs
  as duplicates, differing payloads under one retained ID as conflicts, and evicts
  only the oldest received or terminal local entry. Persisted/protocol dedupe remains
  Phase 4; an ID can be accepted again after its retained entry is evicted.
- The aggregate itself enforces the queued-post bound. Its effective queue capacity
  is `min(16, timeline capacity - 1)`, preserving one replaceable receive slot even
  in injected small-capacity configurations. Queued entries are never evicted.
- Received replies may have missing parents. Local compose requires the current
  parent and protects it from the same insertion's eviction. Borrowed timeline views
  are invalid after mutation; later UI code must retain IDs and re-query.
- Sender time is an optional opaque `int64` hint, accepts extreme/untrusted values,
  and never affects ordering. Phase 2 must apply an explicit safe display range and
  omit unsafe hints before platform date conversion.
- `MessengerState` is synchronous and intended for one application/UI thread. It
  validates structural failures before consuming random/clock ports and commits the
  identity sequence only after the timeline insertion succeeds.
- The original instruction template listed GoogleTest/CTest. The accepted Phase 1
  design uses CTest plus a small dependency-free named-case/CHECK runner so a fresh
  core-only configure stays offline, Release checks survive `NDEBUG`, and no test
  framework enters the product dependency graph. CTest owns discovery, labels,
  ten-second timeouts, and process failure status.
- Encoded payload, fragment, inbound-frame, reassembly, and timeout values remain
  Phase 4 planning defaults in `PLAN.md`; Phase 1 does not publish dummy protocol
  constants or claim behavioral tests for code that does not exist.

### Implemented surface

- Core value objects: result, limits, UUID/strong IDs, strict UTF-8 text values.
- Models: identity, immutable validated post payload/draft, bounded timeline,
  dedupe/conflict and delivery-state transitions.
- Ports: random bytes and optional Unix-seconds wall clock.
- Application commands: initialize, restore, rename, compose, receive, mark
  broadcast, and mark failed.
- Build: separate pure-C++ `lora_messenger_core` and
  `lora_messenger_application` targets before the UI/FetchContent boundary;
  dedicated ASan+UBSan preset.
- Tests: eight CTest executables. The seven named Phase 1 suites execute 90 cases
  and 735 explicit checks, plus the existing core smoke executable.

### Final verification record

| Check | Command/evidence | Result |
| --- | --- | --- |
| Core clean configure | `cmake --preset core-only --fresh` | Passed; returns before UI dependency discovery/FetchContent |
| Core Debug | `cmake --build --preset core-only-dbg` + `ctest --preset core-only-dbg` | Canonical repository passed, 8/8 executables, 0 failures, 2.60 s |
| Named checks | `ctest --preset core-only-dbg -V` | 90 named cases and 735 checks plus core smoke; 0 failures, ten-second timeout visible |
| Core Release | `cmake --build --preset core-only-rel` + `ctest --preset core-only-rel` | Canonical repository passed, 8/8, 0 failures, 2.54 s; runner checks remain active with `NDEBUG` |
| Sanitizers | `cmake --preset core-only-sanitize --fresh` + build/test matching preset | Canonical repository passed, 8/8, 0 failures, 3.42 s with ASan+UBSan and no recovery |
| Strict warnings | Single-config Debug with `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` | Source-identical staging tree built and passed 8/8, 0 failures, 2.69 s |
| Static analysis | `xcrun clang++ --analyze` over UUID/text/model/application sources | Exit 0, no diagnostic |
| Dependency scan | forbidden include/path search in core compile commands plus `otool -L` | No LVGL/SDL/platform/view/BlueZ/RadioLib/Homebrew/FetchContent match; test executables link only libc++ and libSystem |
| Simulator Debug regression | `cmake --build --preset darwin-arm64-dbg -- -j4` + `ctest --preset darwin-arm64-dbg` | Canonical repository passed 10/10, 0 failures, 3.23 s |
| Simulator Release regression | `cmake --build --preset darwin-arm64-rel -- -j4` + Release CTest | Canonical repository passed 10/10, 0 failures, 4.17 s |
| Simulator normal exit | `SDL_VIDEODRIVER=dummy APP_SCRIPT=HOME APP_SCRIPT_INTERVAL_MS=20 build/darwin-arm64/Debug/lora-messenger` | Exit 0 through the existing common teardown path |

The core and simulator results above were rerun after copying the reviewed source to
the canonical repository. No UI changed, so Phase 1 does not claim a new screenshot
or visual-design review; the Phase 0 image remains the current simulator preview.

### Review findings addressed before final re-review

Initial independent reviews found no P0. Their P1 findings were addressed as
follows:

1. Expanded UTF-8 tests to fixed valid scalar boundaries and 25 named malformed
   classes, checking both the generic decoder and field factories.
2. Added reverse/equal `INT64_MIN`/`INT64_MAX` sender-time tests proving that only
   local receive order determines newest entries.
3. Limited the Phase 1 gate to Phase 1-owned behavioral limits and removed future
   protocol constants from the public core API.
4. Moved queued-post enforcement into `Timeline`, derived small-capacity queue
   limits to reserve a receive slot, and tested the aggregate and application paths.
5. Clarified and documented the CTest runner choice, retained-entry dedupe boundary,
   local/received reply rules, borrowed-view lifetime, and sender-time display rule.
6. Added N-1/minimum boundaries, sender-sequence maximum, application transition/
   exhaustion mappings, late reply-parent resolution, reply-parent retention, and
   ten-second CTest timeouts.

### Strict review outcome

- Three initial independent reviews found no P0 and identified contract/test/docs
  P1 findings summarized above.
- The architecture/invariant reviewer confirmed the queue reservation, local reply
  policy, retained-only dedupe, borrowed-view lifetime, and sender-time contract;
  no P0/P1 remains.
- The test/build reviewer confirmed all previous findings, 90 cases/735 checks,
  Release `NDEBUG`, ASan+UBSan propagation, timeout/labels, and dependency isolation;
  no P0/P1 or remaining in-scope P2 remains.
- The requirements reviewer confirmed implementation/API scope and all documented
  decisions. Its sole conditional finding was the intentionally pending completion
  status; `README.md`, `PLAN.md`, and this record are now finalized together.

### Explicitly still outside Phase 1

- UI/view-model integration, new keyboard flows, localized screens, or new captures.
- Settings/identity/history persistence, migrations, restart recovery, and SQLite.
- Wire codec, frames, fragmentation/reassembly, transport queues, retransmission,
  checksum, loopback/multi-process E2E, or persistent dedupe.
- LoRa/BLE/Wi-Fi adapters, hardware access, RF transmission, regulatory selection,
  ARM64 package/device validation, remote publication, PR, or AppStore submission.

### Final repository gate

- Canonical repository: `/Users/yutacar/work/LoraMessenger`.
- Implementation commit:
  `dccb6e41bb42a70bde406c66efd5fce06f070bcd feat: implement phase 1 messaging core`.
- `git diff --cached --check` passed immediately before that commit. The commit
  contains 28 intentional source/build/test files; build products and dependency
  caches remain ignored.
- `git remote -v` has no output. No remote, push, PR, AppStore action, hardware
  access, or RF transmission was performed.
- Final documentation is committed separately so it can record the immutable
  implementation commit. That documentation commit hash is reported in the Phase 1
  completion handoff.

## 2026-07-23 — Phase 2 authorized and completed

- User authorization: exact reply `次のフェーズに進んでください` on 2026-07-23.
- Authorized work: Phase 2 keyboard UI, ephemeral local demonstration state,
  deterministic input/capture automation, and EN/JA/zh-Hans UI resources and font
  coverage.
- Explicit boundary: no settings/history persistence, protocol framing, transport
  bus, sockets, LoRa/RF, hardware work, ARM64/`.deb` production or package-readiness
  claims, remote creation, push, PR, AppStore submission, or publication.
- Execution gate: preserve Phase 1 core isolation and tests; add pure UI-state and
  localization tests; verify Debug/Release simulator flows and common teardown;
  open every required 320x170 capture at original size; complete same-scope P0/P1
  reviews before marking the phase complete.

### Phase 2 implementation decisions

- `MessengerViewModel`, `TextEditor`, localization, and the bounded script parser
  are pure C++ targets. They use the Phase 1 aggregate synchronously and have no
  LVGL, SDL, OS, storage, network, or radio dependency.
- `DemoScenario` creates one deterministic local identity and three deterministic
  received posts. It is startup fixture data, not a loopback or simulated
  transport. The injected clock returns no time, and the UI neither displays
  sender-time hints nor invokes platform date conversion.
- Navigation retains `MessageId` values and re-queries the aggregate after every
  mutation. Detail scrolling exposes an exact two-line logical window; conservative
  13-scalar rows prevent LVGL from rewrapping wide Latin or CJK text.
- Compose editing preserves UTF-8 scalar boundaries and the 160-byte limit. Its
  viewport follows the caret at the limit. Unsupported received content is replaced
  with U+25A1 WHITE SQUARE for the active bundled font.
- Timeline, detail, compose, mentions, and session settings are keyboard-only.
  Status, error, discard, and exit modals have visible safe defaults. `Home` always
  opens confirmation; OS close and scripted close use the common teardown path.
- Compose and mentions are one draft-editing context, so printable `N`/`R`/`M`/`S`
  remain text there. The same letters are global shortcuts on non-entry screens.
  Selected peers evicted from the timeline remain visible until deselected.
- English is the launch default. Japanese and Simplified Chinese language changes,
  identity, draft, and history reset on every launch. The header, settings, and
  queued modal explicitly state local demo, radio disabled, no persistence, and no
  delivery confirmation. Composed posts remain `Queued`.
- `APP_SCRIPT` now has bounded keys, percent-encoded UTF-8 text, `EXPECT`, `AWAIT`,
  atomic `SHOT`, waits, and terminal `CLOSE`. A configured sequence must consume
  every action and emit its completion diagnostic or exit non-zero. The UI runner
  may remove only named evidence files inside a validated strict child test folder.
- Phase 2 implements display output only. Sound notification remains a later
  device-specific gate after the actual CardputerZero audio path is known.
- Existing host install rules carry the Phase 2 runtime fonts, licenses, and linked
  documentation. This is a regression rehearsal only; ARM64/`.deb` production,
  package inspection, and publication remain outside Phase 2.

### Implemented surface

- Pure state: localized ViewModel snapshots, exact selection/scroll bounds, compose
  editor, reply and mention workflows, modal state, and safe return paths.
- LVGL view: a single 320x170 messenger screen with locale-specific fonts, visible
  focus, clipped editor viewport, deterministic detail rows, and compact key guides.
- Platform automation: deterministic key/text injection, semantic focus probes,
  bounded polling, atomic screenshots, explicit failure propagation, and common
  timer/input/display teardown.
- Tests: 12 core-only CTests. The eleven named suites contain 139 cases and 1,956
  explicit checks plus the smoke executable. Phase 2 adds 49 cases and 1,221 checks
  for i18n, editing, ViewModel behavior, and script parsing.
- Assets: two reproducible Noto Sans CJK Medium subsets, complete OFL/MIT notices,
  authoritative glyph exporter, and 18 committed reviewed Phase 2 flow captures.

### Final verification record

| Check | Command/evidence | Result |
| --- | --- | --- |
| Core clean configure | `cmake --preset core-only --fresh` | Canonical repository passed; returned before UI dependency discovery and FetchContent |
| Core Debug | `cmake --build --preset core-only-dbg` + `ctest --preset core-only-dbg -V` | Passed 12/12, 0 failures, 4.10 s |
| Named checks | Debug verbose output | 139 named cases and 1,956 checks plus core smoke; Phase 2 is 49/1,221 |
| Core Release | `cmake --build --preset core-only-rel` + `ctest --preset core-only-rel` | Passed 12/12, 0 failures, 4.08 s; checks active under `NDEBUG` |
| Sanitizers | Fresh `core-only-sanitize` configure, build, and CTest | Passed 12/12, 0 failures, 5.54 s with ASan+UBSan and no recovery |
| Simulator clean configure | `cmake --preset darwin-arm64 --fresh` | Passed with pinned LVGL `85aa60d18b3d5e5588d7b247abf90198f07c8a63` |
| Simulator Debug | Debug build + `ctest --preset darwin-arm64-dbg` | Passed 21/21, 0 failures, 15.24 s |
| Simulator Release | Release build + Release CTest | Passed 21/21, 0 failures, 14.01 s |
| Desktop suite composition | CTest list and labels | 12 core/unit, 1 font coverage, 6 expected script failures, 1 keyboard flow, and 1 window-close teardown |
| Strict warnings | All unique product `src/` compile commands plus `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror -fsyntax-only` | Passed 21/21 translation units |
| Font coverage | Direct coverage executable | Both CJK fonts cover 370 required scalars; both Inter faces cover 96 English scalars including U+25A1 |
| Font reproducibility | Authoritative exporter and two HarfBuzz 14.2.1 subset runs | Glyph file and both repeated subsets were byte-identical to the committed files |
| Screenshot automation | Debug and Release keyboard flows plus `file`, checksum comparison, and original-size inspection | 18/18 flow captures are 320x170 and byte-identical across configs; window-close evidence is also 320x170 and byte-identical |
| Script path guard | Deliberately set the source root as `TEST_WORKING_DIRECTORY` | Exit 1 before cleanup: `TEST_WORKING_DIRECTORY must be a strict child of ALLOWED_TEST_ROOT` |
| Dependency scan | Core compile-command forbidden-dependency search plus `otool -L` | No LVGL, SDL, Homebrew, RadioLib, SQLite, curl, or OpenSSL match; sampled tests link only libc++ and libSystem |
| RF/transport scan | Driver/socket/transport symbol search over product/build/test sources | No RadioLib, SX126x, socket, transmit, loopback, or fault-injection implementation found |
| Host install rehearsal | Debug `cmake --install` into `/private/tmp/lora-phase2-canonical-install.Y7EVds` | 24 app-owned files; honest radio-disabled description, coherent doc/license links, and no `.service` file |

The clean canonical Debug and Release flow captures match the committed files by
content; only generated timestamps differ. The normalized manifest digest from
`cd screenshot` followed by `shasum -a 256 phase2-*.png | shasum -a 256` is
`1ec4065be6e607641697e528b845db37e8fce2520e8ebe6b4ee8bba53a430789`.
The lifecycle-only `phase2-window-close.png` is generated in both configuration
test folders rather than committed as a duplicate of the English timeline.

Font generation evidence:

| Input/output | Bytes | SHA-256 |
| --- | ---: | --- |
| Original `NotoSansCJKjp-Medium.otf` | — | `dd523e580e3413c480b2d701bf64e534c20f8419e3cfb6a44c2bdcd8d2a6c052` |
| Original `NotoSansCJKsc-Medium.otf` | — | `ca094f6b0001fb048ca39ddd797a0cdb0179e1e55c6561e111c49c3e6a61d7b7` |
| `tools/font/ui-glyphs.txt` | 921 | `a93280ea5e9ea21c322d5544b0366c9a1185b19b2aa5a45c5493b70e22f285b8` |
| `assets/fonts/lora-ui-ja.otf` | 182,728 | `bc388b782f6bdd37524065586d909f2e7b9f382771bfaa951a896066051f09c5` |
| `assets/fonts/lora-ui-zh-hans.otf` | 181,884 | `496e2ec9803d66a73926a07a6d4d59473ec532e549a8cf119411ac185d7b4cd8` |

### Reviewed visual evidence

All files below were opened at original 320x170 resolution:

- English: `phase2-en-timeline.png`, `phase2-en-detail.png`,
  `phase2-en-detail-scrolled.png`, `phase2-en-detail-wide.png`,
  `phase2-en-detail-wide-bottom.png`, `phase2-en-compose-empty.png`,
  `phase2-en-compose.png`, `phase2-en-compose-long.png`,
  `phase2-en-mentions.png`, `phase2-en-settings.png`,
  `phase2-en-status.png`, `phase2-en-error.png`, `phase2-en-exit.png`, and
  `phase2-en-discard.png`.
- Japanese: `phase2-ja-timeline.png` and `phase2-ja-settings.png`.
- Simplified Chinese: `phase2-zh-hans-timeline.png` and
  `phase2-zh-hans-settings.png`.

Inspection passed for focus visibility, modal legibility, line and card bounds,
long-compose caret visibility, language glyphs, unsupported-user-content square
fallback, honest queued/radio/delivery wording, and absence of overlap or clipping.
The wide-detail evidence changes from `O/W` at the top to `M/Q` at the bottom, so
reachability is visually and behaviorally distinct.

### Review findings addressed before final re-review

Three independent reviews found no P0. Their P1 findings were fixed as follows:

1. Replaced hard-coded detail scroll limits with exact logical-line bounds, added
   LF-heavy and wide-glyph boundary tests, and made top/bottom evidence distinct.
2. Added a clipped compose viewport that follows the actual LVGL caret position at
   the exact 160-byte limit.
3. Made no-delivery-confirmation and radio-disabled wording explicit in all locales,
   localized the persistent `Home` exit cue, and kept global shortcuts out of the
   active draft/mention context.
4. Preserved selected evicted mention IDs until the user can deselect them, and
   tested safe detail/settings return paths.
5. Made premature `APP_SCRIPT` termination fail non-zero, replaced duplicated focus
   evidence with semantic widget focus, added live `AWAIT` success/timeout coverage,
   and restricted cleanup to a validated strict child path and named files.
6. Added authoritative catalog glyph export, Inter fallback coverage, reproducible
   CJK subsets, source/output hashes, full licenses, and coherent installed-doc
   links.
7. Moved the final Settings notice within its card and recaptured all locales to
   remove lower-edge clipping.

The architecture/state, lifecycle/script, and localization/font/UI reviewers each
rechecked their fixes against the final source and captures. No P0/P1 remains.

### Verification anomalies investigated

- An initial attempt to run Debug and Release builds concurrently against the same
  multi-config build directories caused Ninja lock/CMake regeneration races, while
  sandboxed canonical writes were also denied. The invalid concurrent attempt was
  discarded; each configuration was then cleanly configured and run sequentially,
  producing the passing results above.
- The first host install copy reached the temporary prefix but the sandbox denied
  writing `install_manifest.txt` in the canonical build tree. The same command was
  rerun with the required write authorization and completed. Neither event was a
  product-test failure or a flaky rerun.

### Explicitly still outside Phase 2

- Settings, identity, language, draft, or history persistence; crash recovery,
  migration, retention, SQLite, or uninstall-data behavior.
- Wire codec, fragmentation/reassembly, transport queues, loopback/multi-process
  simulation, networking, receive workers, delivery ACKs, or terminal send states.
- Sound notification, CardputerZero audio integration, LoRa/BLE/Wi-Fi adapters,
  hardware access, RF transmission, antenna/region selection, or device validation.
- ARM64 native/cross build claims, `.deb` production inspection, remote creation,
  push, PR, AppStore submission, or publication. Phase 3 is not authorized.

### Final repository gate

- Canonical repository: `/Users/yutacar/work/LoraMessenger`.
- Implementation commit:
  `5b6b740ada817c27ab855abfb2b870c26a48d486 feat: implement phase 2 keyboard UI`.
- `git diff --cached --check` passed immediately before the commit. It contains 63
  intentional source/build/documentation/evidence files; generated build products,
  dependencies, install manifests, and temporary test fixtures remain ignored.
- `git status --short` and `git remote -v` had no output immediately after the
  implementation commit. No remote, push, PR, AppStore action, hardware access, or
  RF transmission was performed.
- This completion record is committed separately so it can cite the immutable
  implementation commit. Its documentation commit hash is reported in the Phase 2
  completion handoff.

## 2026-07-23 — Phase 3 authorized and started

- User authorization: exact reply `次へ進んでください` on 2026-07-23.
- Authorized work: crash-safe local identity/settings and message-history
  persistence, migrations, bounded retention, corrupt-data recovery, confirmed
  local-data deletion, and cancellation-safe shutdown.
- Explicit boundary: no protocol, transport bus, sockets, LoRa/RF, hardware,
  sound, ARM64/`.deb` production, remote creation, push, PR, AppStore submission,
  or publication.
- Execution gate: restart persistence, deterministic interrupted-write and corrupt
  data recovery, old-schema migration, retention bounds, uninstall-data
  documentation, Debug/Release/sanitizer coverage, and same-scope P0/P1 reviews.

## 2026-07-23 — Phase 3 implementation and verification complete

### Implemented scope

- Added canonical bounded settings JSON with installation identity, validated user
  ID, sender-sequence high-water mark, locale, generation, and an explicit
  history-initialized marker.
- Added same-directory 0600 temp/write/fsync/rename/directory-fsync settings
  replacement with deterministic failpoints at every material write boundary.
- Vendored the official SQLite 3.53.3 amalgamation and added a STRICT schema for
  metadata, posts, and ordered mentions. Unsigned 64-bit counters are stored as
  fixed-width big-endian blobs.
- Added exact canonical schema verification, integrity/foreign-key checks,
  transactional v1-to-v2 migration, a migration failpoint, owner/single-link leaf
  validation, identity binding, and strict local sender-sequence monotonicity.
  Verification enumerates every `sqlite_schema` row and permits only the expected
  tables and exact SQLite-created autoindexes, rejecting hidden triggers including
  reserved-name injections.
- Existing history is validated in the deterministic owner-only sibling directory
  `history.sqlite3.probe/`, using copies of the main database, committed WAL, and
  rollback journal, before the original can be migrated, switched to WAL, or
  chmodded. Tests prove committed WAL-only state is observed while the original
  main/WAL/SHM bytes remain unchanged on rejection. A forced-stop residue is removed
  on the next data-locked startup and by confirmed local-data deletion.
- Added distinct config-tree and data-tree lifetime locks. This prevents two
  sessions that share either XDG side from racing full-snapshot writes or reusing a
  receive order.
- Made identity and timeline changes persistence-first. Compose reserves its sender
  sequence before history replacement; a failure can leave a gap but cannot reuse a
  sequence. Restored `Queued` entries become `Unknown` and are never retransmitted
  at startup.
- Added persistent locale, explicit simulator-only demo seeding, restart restore,
  recovery, safe-default confirmed deletion, and settings-first deletion ordering.
  Cancel preserves the authoritative settings/temp/database/WAL/SHM/journal leaves
  byte-for-byte; confirmed deletion also removes the managed validation probe.
- Added localized local-only/radio-disabled/delivery-unconfirmed wording and
  320x170 restart, recovery, and delete-confirmation evidence.

### Verification record

| Check | Evidence | Result |
| --- | --- | --- |
| Core Debug | `cmake --build --preset core-only-dbg` and Debug CTest | 19/19 passed |
| Named checks | Verbose Debug output | 208 named cases, 2,694 checks, plus smoke |
| Core Release | `cmake --build --preset core-only-rel` and Release CTest | 19/19 passed; checks active under `NDEBUG` |
| Sanitizers | `core-only-sanitize` build and CTest | 19/19 passed with ASan+UBSan |
| Desktop Debug | `darwin-arm64-dbg` build and Debug CTest | 31/31 passed, 0 failures, 15.76 s |
| Desktop Release | `darwin-arm64-rel` build and Release CTest | 31/31 passed, 0 failures, 15.12 s |
| Strict warnings | All unique product compile commands plus `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror -fsyntax-only` | 26/26 translation units passed |
| Atomic settings | Direct suite | 12 cases / 187 checks; FIFO rejected without blocking |
| SQLite history | Direct suite | 8 cases / 92 checks; includes WAL-only identity mismatch with unchanged originals |
| SQLite migration | Direct suite | 10 cases / 98 checks; every schema row, hidden-trigger rejection, and failpoint rollback |
| Persistent session | Direct suite | 12 cases / 98 checks; marker, split-XDG locks, stale-probe cleanup, recovery, deletion |
| UI persistence | Restart/recovery/delete CTests | Three flows passed in Debug and Release with teardown/completion diagnostics |
| Static SQLite | Release `otool -L` | No SQLite dylib; amalgamation is linked statically |
| Scope scan | Product/test symbol scan | No socket, protocol, transport, radio driver, or RF implementation |

The three reviewed Phase 3 captures are 320x170 and byte-identical between Debug
and Release:

| Capture | SHA-256 |
| --- | --- |
| `phase3-restart-ja-timeline.png` | `2a875c1620b06d84cdeb35d8d0061ffbba6bfdc0400812ab2777a752b2846764` |
| `phase3-recovery.png` | `5dd63c3539277f21149b8c2399967c5350bdb9175f1aa375e90d731f406b8da8` |
| `phase3-delete-confirmation.png` | `cf9cc623537bb717a11cb01bf2b7547d4a94435a8d2d226f04d8c1006459775f` |

Supply-chain and localized-glyph evidence:

| Input/output | SHA-256 / SHA3-256 |
| --- | --- |
| `third_party/sqlite/sqlite3.c` | SHA3-256 `28e484abdaa43630e34040ef6ed92be973a1ad54107803d8af5145b889c23ed7` |
| `tools/font/ui-glyphs.txt` | SHA-256 `7d24ea8db35d015db195f2150693da7a830bd50cafd42fcc96aacce66dd48925` |
| `assets/fonts/lora-ui-ja.otf` | SHA-256 `056b8fefacf817a1f5484a6a43b31d80bfefa8c8537a3d32b155e0302cd8e751` |
| `assets/fonts/lora-ui-zh-hans.otf` | SHA-256 `90efd9ceb2d92116168da7f341dcd90ab446f14e4bdd17ec586f8cc6ee8941ea` |

### Review findings resolved

The first independent crash/data and SQLite reviews found no P0. Their P1 findings
were resolved before the final gate:

1. Rejected/mismatched databases are now fully inspected away from the originals;
   migration, WAL changes, and chmod happen only after identity and row validation.
2. Separate settings/history locks prevent split-XDG concurrent sessions and lost
   full-snapshot updates.
3. The explicit initialization marker disambiguates a genuinely interrupted first
   launch from a lost empty or received-only history. Settings-first confirmed
   deletion cannot silently recreate an old identity after a partial reset.
4. Schema-only databases are rejected. Every `sqlite_schema` row is enumerated;
   only the expected tables and exact SQLite-created autoindexes are allowed, so
   extra columns, constraints, indexes, hidden triggers, and reserved-name
   injections are rejected. Local sender sequences must be unique and strictly
   increasing.
5. FIFO settings input is opened nonblocking; database and sidecar hard links are
   rejected before SQLite access.
6. Validation uses the deterministic owner-only sibling
   `history.sqlite3.probe/`; forced-stop residue is removed on the next startup
   under the data-tree lock and during confirmed local-data deletion.

The same two reviewers rechecked the final implementation and reported no remaining
P0/P1.

### Verification anomalies and design corrections

- The first restart CMake driver represented an `APP_SCRIPT` as a CMake list, so
  semicolons split the value. `string(CONCAT ...)` now constructs each script
  atomically; restart automation then passed in both configurations.
- A candidate read-only SQLite probe using URI `immutable=1` was explicitly tested
  and discarded because it ignores committed, uncheckpointed WAL state. The final
  managed-copy probe in `history.sqlite3.probe/` observes WAL state and has a
  regression test proving that the original database and sidecars are unchanged on
  rejection. Its deterministic location lets the next data-locked startup and
  confirmed deletion remove residue left by a forced stop.
- Generated LVGL dependency state was reused only after CMake regenerated the
  desktop build graph for the Phase 3 source set. Core-only validation remained
  offline and independent of that cache.

### Explicitly still outside Phase 3

- Wire frames, codecs, fragmentation/reassembly, simulated or real transport,
  sockets, networking, receive workers, ACK/retry policy, and multi-node E2E.
- LoRa/BLE/Wi-Fi adapters, radio profile or region selection, RF transmission,
  sound, hardware access, and physical-device validation.
- ARM64 native/cross validation claims, `.deb` production inspection, remote
  creation, push, PR, AppStore submission, and publication.

### Final repository gate

- Canonical repository: `/Users/yutacar/work/LoraMessenger`.
- Implementation commit:
  `176a719ecb9d980853ff3ac7304d980f38cc2de7 feat: implement phase 3 local persistence`.
- `git diff --cached --check` passed immediately before the commit. It contains 57
  intentional source/build/documentation files. A narrow `.gitattributes`
  exception preserves the byte-verified upstream SQLite amalgamation while
  excluding only its pre-existing trailing spaces from this check.
- Immediately after the implementation commit, `git status --short` contained
  only the intentional `PLAN.md` and `NOTES.md` completion-record edits. No build
  products or test fixtures were staged.
- `git remote -v` has no output. No remote, push, PR, AppStore action, hardware
  access, RF transmission, or publication was performed.
- This completion record is committed separately so it can cite the immutable
  implementation commit. Its documentation commit hash is reported in the Phase 3
  completion handoff.

## 2026-07-23 — Phase 4 authorized and started

- User authorization: exact reply `次へ進んでください` on 2026-07-23.
- Authorized work: a frozen version-1 canonical post and DATA-frame protocol,
  bounded fragmentation/reassembly/deduplication/scheduling, SQLite-backed
  canonical receive dedupe, deterministic in-process fault simulation, and a
  bounded two-process local-IPC convergence gate.
- Explicit boundary: the product simulator remains transport-detached. No socket,
  IP network, LoRa/BLE/Wi-Fi adapter, real radio, RF transmission, hardware access,
  delivery ACK, ARM64/package claim, remote creation, push, PR, AppStore action, or
  publication is authorized.
- Execution gate: golden/malformed/boundary vectors, all five frozen MTUs, 10,000
  deterministic seeds and every frozen fault class, two simultaneous processes,
  crash-safe schema-v3 receive persistence, Debug/Release/strict/sanitizer suites,
  and independent protocol/lifecycle/persistence P0/P1 review.

## 2026-07-23 — Phase 4 implementation and verification complete

### Implemented scope

- Added a canonical Post v1 codec with an exact 46-to-316-byte range and rejection
  before allocation beyond the existing model's logical maximum.
- Added a fixed 28-byte DATA header with application magic, version/type, 64-bit
  message tag, full unsigned sender sequence, fragment metadata, exact lengths, and
  CRC-32/ISO-HDLC. Frozen transport MTUs are 48/51/64/128/255 bytes; the 316-byte
  maximum fits the hard 16-fragment limit even at MTU 48.
- Added bounded fragmentation, eight-slot/60-second reassembly, a 64-frame ingress
  queue, a 128-entry recent-frame window, and a 32-entry quarantine. Malformed,
  conflicting, corrupt, duplicate, late, and incomplete input cannot commit a
  second post or extend an assembly indefinitely.
- Added a 16-message outbound scheduler with deterministic injected policy. Each
  frame may be offered at most twice and every message has a finite deadline.
  Transport acceptance may set `Broadcast`; no ACK, receiver count, implicit
  restart retransmission, or `Delivered` state is inferred.
- Added pure `IDatagramTransport` and `IRadioPolicy` ports. The local
  `SimulatedRadioBus` and POSIX-pipe helper are linked only by tests; product UI and
  runtime code do not attach a transport.
- Migrated SQLite history to schema v3 with a bounded 2,048-entry ledger keyed by
  message ID and full canonical payload. A received post and its dedupe record are
  one transaction; duplicate/conflicting input cannot advance local order or
  persistence generation. Migration, write-boundary rollback, restart, and the
  exact 2,048/2,049 eviction boundary are covered.

### Verification record

All tests ran on Apple Silicon macOS in the local canonical/staging trees. The
core-only variants use no UI, network fetch, radio library, or hardware:

| Check | Command/evidence | Result |
| --- | --- | --- |
| Core Debug | `cmake --build --preset core-only-dbg`; `ctest --preset core-only-dbg` | 27/27 passed; 15.89 s |
| Core Release | `cmake --build --preset core-only-rel`; `ctest --preset core-only-rel` | 27/27 passed; 18.74 s |
| Strict warnings | Debug core build with `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`, then full CTest | Build passed; 27/27 passed; 29.00 s |
| Sanitizers | `cmake --build --preset core-only-sanitize`; `ctest --preset core-only-sanitize` | ASan+UBSan 27/27 passed; 28.39 s |
| Desktop Debug | `cmake --build --preset darwin-arm64-dbg`; `ctest --test-dir build/darwin-arm64 -C Debug --output-on-failure` | 39/39 passed; 30.41 s |
| Desktop Release | `cmake --build --preset darwin-arm64-rel`; `ctest --test-dir build/darwin-arm64 -C Release --output-on-failure` | 39/39 passed; 25.25 s |
| Final MTU-alias gate | Debug and Release `ctest ... -R 'limits_contract\|phase4_simulation'` in the canonical tree | 2/2 in each configuration; 8.66 s / 1.80 s |
| Phase 4 direct gate | Canonical `phase4_simulation_test` executable | 4 cases / 422 checks |
| Deterministic matrix | Seeds `0..9999`, MTUs 48/51/64/128/255, every frozen profile | Passed with no crash, duplicate commit, or bound violation |
| Persistence boundaries | SQLite migration/store/session direct suites | Schema v3 rollback, failpoints, restart, conflict, and 2,048/2,049 boundaries passed |
| Dependency/scope audit | include graph, product/test reference scan, strict `nm`, Release `otool -L` | No product transport attachment, socket/RF symbols, RadioLib/LVGL/SDL dependency, or dynamic SQLite |

The deterministic matrix covers clean, loss, duplicate, corruption, delay, forced
reorder, disconnect/reconnect, mixed, and permanent one-way-loss profiles. Full
convergence compares sorted full canonical post bytes when every fragment remains
deliverable. Permanent loss proves a non-empty, corruption-free canonical subset.
The integration gate spawns two simultaneous helper processes for every MTU; both
receive both expected canonical posts, duplicate replay adds no commit, and timeout,
HUP, restart, shutdown, and child reaping are bounded.

Independent protocol/lifecycle, SQLite/persistence, integration, documentation, and
dependency audits were rerun after their fixes. The final dependency re-audit also
ran eight strict related tests (8/8 passed in 7.10 s). No P0/P1/P2 remains.

### Review findings and design corrections resolved

1. The integration gate now uses two actual simultaneous helper processes and
   compares complete canonical bytes, not only message IDs.
2. Pipe descriptors are close-on-exec and teardown uses bounded child reaping.
   HUP, timeout, restart, duplicate replay, and clean shutdown have direct coverage.
3. Forced reorder is directly observed through fragment-index descent. Permanent
   loss is one-way and must leave an explicitly non-empty valid subset.
4. Scheduler tests enforce the two-offer ceiling, finite logical-clock deadlines,
   and all frozen MTUs rather than wall-clock or distribution-dependent behavior.
5. Persistence tests inject failures at both timeline and dedupe write boundaries,
   verify exact marker/high-water/queued semantics, reject lower high-water marks,
   and prove durable dedupe behavior at 2,048 and 2,049 records across restart.
6. The raw SQLite failpoint pointer lifetime and trusted single-owner monotonic
   snapshot boundary are documented; product `PersistentSession` satisfies both.
7. The minimum/maximum MTUs have one source of truth in the transport port.
   Protocol limits alias those constants with static assertions; the dependency
   remains one-way with no reverse include or concrete-adapter coupling.

### Explicitly still outside Phase 4

- Product transport attachment, receive workers, socket/IP networking, LoRa,
  BLE/Wi-Fi adapters, radio-driver integration, RF transmission, antenna/region
  selection, sound, hardware access, and physical-device validation.
- ACK protocol, confirmed receiver counts, `Delivered` state, background service,
  or implicit retransmission of restored `Queued` history.
- ARM64 native/cross-build readiness, `.deb` production inspection, AppLaunch/store
  metadata, remote creation, push, PR, AppStore submission, or publication.
- Phase 5 and later work remain unstarted and require the next phase authorization.

### Final repository gate

- Canonical repository: `/Users/yutacar/work/LoraMessenger`.
- Implementation commit:
  `f5faeca69bcc071641f19bd5d31081459219f8ad feat: implement phase 4 protocol simulation`.
- `git diff --cached --check` passed immediately before the commit. It contained 38
  intentional source, test, build-configuration, and documentation files with
  8,262 insertions and 103 deletions; generated dependencies and build products
  remained untracked or ignored.
- `git remote -v` had no output. No remote, push, PR, AppStore action, hardware
  access, RF transmission, socket/network access, or publication was performed.
- This completion record is committed separately so it can cite the immutable
  implementation commit. Its documentation commit hash is reported in the Phase 4
  completion handoff.

## 2026-07-23 — Phase 5 authorized and started

- User authorization: exact reply `次のフェーズに進んでください` on 2026-07-23.
- Authorized work: reproducible CardputerZero ARM64 Debug/Release cross-builds,
  a no-service APPLaunch `.deb`, icons and localized store metadata, ELF and
  dependency inspection, clean Debian package validation, and local read-only
  reproduction of current `czdev`/`packages` checks.
- Explicit boundary: no device execution, LoRa/RF or other hardware access,
  verified Maintainer identity selection, OAuth/login, repository publication,
  push, PR, AppStore submission, or publish command.
- The first current-source refresh resolved Template
  `c08ca040af5a88f5d7c36b4ce0dd128b67a92e4f`, AppBuilder
  `aac6074ea3d0123ed6b401e11cecf56a21c12bb4`, packages
  `93529f9377fd36d2d1494741c2640f75beaa9776`, and AppStore
  `3f5cf47b87d7c7231d9745e36900a87bfed95833`.
- The installed Rust `czdev 0.1.0` has no read-only `preflight` command. Current
  Python `czdev publish` performs remote authentication and mutation after inline
  checks, so it will not be invoked as a linter. Its pure checks and the current
  packages Actions safety gates will be reproduced locally instead.

## 2026-07-24 — Phase 5 implementation and verification complete

### Implemented scope

- Added pinned CardputerZero AArch64 Debug/Release presets and a toolchain file
  that requires the exact SDK BSP archive SHA-256
  `e51b6eb803ed08f450e459efbfe62dd0341440846f3be9d01da861fe6cfdebb0`.
  The unpacked v0.0.4 Debian trixie/AArch64 sysroot is validated before configure,
  including its multiarch headers, target compiler, loader, and libraries.
- Pinned LVGL 9.5.0 to
  `85aa60d18b3d5e5588d7b247abf90198f07c8a63` and fmt to
  `f5e54359df4c26b6230fc61d38aa294581393084`; target builds do not embed a host
  asset path.
- Added a no-service CPack/APPLaunch package with the official relative icon path,
  a three-line `exec` wrapper, four runtime fonts, the application icons, README,
  copyright, and complete third-party notices. Internal `PLAN.md` and `NOTES.md`
  remain source-only so the artifact hash can be recorded without a self-reference.
- Added honest `legacy-deb-only` store metadata: four exact 320x170 screenshots,
  English/Japanese/`zh-CN` entries, a 512x512 store icon, and an exact permission
  map limited to keyboard input and app-data filesystem access.
- Added configure-time metadata validation and a CPack post-build `.deb` validator.
  It checks canonical control metadata, hashes, safe archive members and paths,
  APPLaunch layout, exact wrapper/desktop/icon invariants, modes, AArch64 ELF,
  loader, NEEDED libraries, RPATH/RUNPATH absence, and GLIBC/GLIBCXX/CXXABI bounds
  against the pinned BSP.
- Added a publication-strict Maintainer mode. Normal local packaging warns about
  `noreply@example.invalid`; `--require-publishable-maintainer` rejects it
  mechanically. A user-verified replacement identity and separate publication
  approval are still required.

### Final icon

The later user request superseded the intermediate blue/red/yellow palette request:
restore the pre-color-change icon and change only its background to blue. The
built-in image generation edit used the exact prior generated image
`call_wUWjDFb0psy1JMKyb7dL2zZe.png` as its edit target; the selected background-only
result is `call_lh2qmM8cVGKFsJ01PKgI7DEn.png`. Independent visual comparison
confirmed that the cyan radio arcs, white envelope, navy seam, amber rays, positions,
geometry, spacing, and proportions remain those of the prior icon.

| Distributed icon | Dimensions | Alpha | Size | SHA-256 |
| --- | ---: | --- | ---: | --- |
| `assets/images/lora-messenger-store.png` | 512x512 | none | 250,299 bytes | `ed29b45f5d8d648199ad33ebadccc1f880e48049717d9eccd8c9d5af91c07214` |
| `assets/images/lora-messenger.png` | 100x100 | none | 11,608 bytes | `098b41a548b538e72912144fd83e60bf266fb0f2f3192751afa0d24897b8ba17` |
| `assets/images/lora-messenger_100.png` | 100x100 | none | 11,608 bytes | `098b41a548b538e72912144fd83e60bf266fb0f2f3192751afa0d24897b8ba17` |
| `assets/images/lora-messenger_80.png` | 80x80 | none | 7,916 bytes | `366385e100d3723fc212d3c24359211f4bc9ce82e7237a5fe2ae9758ad94a986` |

### Refreshed upstream inputs

| Input | Commit |
| --- | --- |
| CardputerZero Template | `c08ca040af5a88f5d7c36b4ce0dd128b67a92e4f` |
| CardputerZero AppBuilder | `aac6074ea3d0123ed6b401e11cecf56a21c12bb4` |
| CardputerZero packages | `93529f9377fd36d2d1494741c2640f75beaa9776` |
| CardputerZero AppStore | `3f5cf47b87d7c7231d9745e36900a87bfed95833` |
| CardputerZero APPLaunch | `8de9706a0b4fa7623f71f2c82b9b601f4864aa3f` |
| CardputerZero developer portal | `8e2d00598996b06f57199fce4c67c5fd4fd2fd6f` |
| M5Stack Linux libraries | `8c412407623badbc24ed710b55f7a80b6a8d2fb3` |
| M5Stack Linux device overlays | `0da80268fe0fdc4aecd42aa15991047e79e3f384` |

The product documentation still describes CardputerZero as work in progress.
These commits are evidence for this phase, not a claim that the platform ABI or
publication workflow is final.

### Authoritative package

The final artifact was built without network access in the fixed
`lora-messenger-phase5-env:20260723` Debian trixie amd64 environment using CMake
3.31.6 and `aarch64-linux-gnu-g++ 14.2.0-19`. Pinned LVGL/fmt sources and the BSP
sysroot were mounted read-only.

| Property | Final value |
| --- | --- |
| File | `dist/lora-messenger_0.1.0-1_arm64.deb` |
| Size | 2,220,502 bytes |
| SHA-256 | `0a2ef1edb3bebf1659d1e8575ab1c78d7561337d61dcfb5d57561705682aed53` |
| Architecture | `arm64` |
| Payload | 36 regular files |
| ELF interpreter | `/lib/ld-linux-aarch64.so.1` |
| ELF NEEDED | `libc.so.6`, `libfreetype.so.6`, `libgcc_s.so.1`, `libpng16.so.16`, `libstdc++.so.6` |
| `dpkg-shlibdeps` Depends | `libc6 (>= 2.34), libfreetype6 (>= 2.2.1), libgcc-s1 (>= 4.5), libpng16-16t64 (>= 1.6.46), libstdc++6 (>= 14)` |
| GLIBCXX | required max `3.4.29`; BSP max `3.4.33` |
| CXXABI | required max `1.3.15`; BSP max `1.3.15` |
| GLIBC | required max `2.34`; BSP max `2.41` |

The package has no RPATH/RUNPATH, link/device/setuid members, maintainer scripts,
systemd unit, or background service. The normal validator passes with the explicit
local-only Maintainer warning. The publication-strict validator fails with exactly
one expected error for the reserved `example.invalid` domain.

### Verification record

| Check | Result |
| --- | --- |
| Core Debug | 28/28 passed; 17.08 s |
| Core Release | 27 unaffected tests passed; the sandbox denied the two-process test's process inspection, and that exact test passed 1/1 outside the sandbox in 0.85 s |
| Core ASan+UBSan | 28/28 passed; 34.41 s |
| Desktop Debug | 40/40 passed; 29.54 s |
| Desktop Release | 40/40 passed; 24.13 s |
| AArch64 Debug/Release | both cross-builds completed; target CTest is intentionally disabled because the host cannot execute ARM64 tests |
| Package validator regression | 26/26 passed locally in 1.326 s and in the final network-disabled build environment in 1.576 s |
| Metadata preflight | passed with four screenshots, three locales, and the exact 12-key permission map |
| Final `.deb` validator | passed, including the BSP ABI comparison and exact safe wrapper |
| Publication-strict negative gate | failed as intended for `noreply@example.invalid` |

Phase 5 changes do not alter product C++ behavior. The Phase 4 strict-warning gate
therefore remains the product-code warning baseline. GCC 14 emitted its known
optimizer `-Wstringop-overread` diagnostic inside the pinned upstream SQLite
amalgamation during the cross-build; the exact upstream bytes were retained and all
storage, sanitizer, desktop, cross-build, package, and install gates passed.

The final `.deb` was installed into a fresh ARM64 Debian trixie container with
dependencies resolved by `apt`. The gate verified:

- exact APPLaunch binary, wrapper, desktop, and relative icon lines;
- wrapper byte-for-byte equality and `sh -n`;
- an AArch64 PIE with the expected loader and no RPATH/RUNPATH;
- `ldd` with no missing library and `dpkg -V` with no difference;
- the exact package/version/architecture and generated dependency list;
- current Phase 5 README content, with source-only `PLAN.md`/`NOTES.md` absent;
- successful package removal and no remaining APPLaunch executable, wrapper,
  desktop entry, runtime data tree, or packaged README.

Debian's slim base image excludes most `/usr/share/doc` content. The first install
probe removed the wrong spelling of that image-specific rule, so only the expected
copyright file was installed and `dpkg -V` reported the excluded documentation.
The gate was corrected to remove the exact `path-exclude /usr/share/doc/*` rule
before installation; the complete second run passed. This was a disposable
container policy issue, not a package defect.

The first local `py_compile` check attempted to create its cache below the
sandbox-forbidden macOS `~/Library/Caches/com.apple.python` tree and failed before
compiling either file. Re-running the same two-file check with the task-specific
`PYTHONPYCACHEPREFIX=/private/tmp/lora-messenger-pycache` passed. The network-free
26-test validator suite also passed independently of bytecode caching.

### Review findings and corrections

Independent package, requirements, build, licensing, and image reviews found no P0.
Their P1 findings were corrected:

1. Replaced inaccurate hard-coded Debian dependency names with Linux-host
   `dpkg-shlibdeps`; the macOS fallback is explicitly inspection-only.
2. Rebuilt the authoritative artifact with Debian GCC 14 instead of treating a
   macOS GCC 15 cross-build as the release package, and added BSP ABI maxima checks.
3. Disabled target CTest for cross-builds and moved metadata validation into
   mandatory configure/CPack gates.
4. Added actual `.deb` archive, layout, ELF, dependency, and sysroot validation.
5. Strengthened the wrapper check to exact bytes and permissions to the complete
   approved key/value map.
6. Added deterministic malicious `.deb` fixtures for traversal, links, device
   nodes, setuid/setgid, systemd, control scripts, wrapper modification, sysroot
   escape, and CPack failure propagation.
7. Added a publication-strict Maintainer gate instead of describing an unenforced
   warning as a blocker.
8. Preserved the official APPLaunch relative icon form while validating a temporary
   absolute-icon copy with the generic desktop validator.
9. Removed internal mutable records from the package, eliminating stale docs and
   final-artifact hash self-reference.
10. Replaced the first blue-background edit after review showed that it had also
    changed the foreground scale and geometry. The final edit passed an independent
    comparison against the exact prior icon.
11. Propagated sysroot/multiarch variables through CMake `try_compile` and rejected
    incomplete or hash-mismatched BSP inputs.

The build/package, requirements/documentation, and package-security reviewers each
rechecked the final source plus SHA-256 `0a2ef1ed...aed53` artifact and reported no
remaining P0/P1. The icon re-review also compared the prior and final foreground
bounding boxes and centroids; the maximum centroid shift was 0.43 pixels before
resizing, consistent with the requested preserved geometry.

### Explicitly still outside Phase 5

- Running the application on CardputerZero or any other physical device.
- LoRa/BLE/Wi-Fi transport attachment, radio-driver integration, RF transmission,
  antenna/region selection, sound, and hardware acceptance.
- Selection or verification of a real Maintainer identity.
- Login, OAuth, repository creation, remote changes, push, PR, AppStore submission,
  `czdev publish`, or any publication action.

### Final repository gate

- Canonical repository: `/Users/yutacar/work/LoraMessenger`.
- Implementation commit:
  `39f28a24c54b003d7b50f3da4e0474bd8e253be6 feat: add phase 5 ARM64 packaging`.
- `git diff --cached --check` passed immediately before the implementation commit.
  It contained 24 intentional build, validation, metadata, license, documentation,
  and image files with 2,760 insertions and 45 deletions. Build products, dependency
  caches, Python bytecode, and the ignored `.deb` were not staged.
- The canonical ignored artifact is
  `dist/lora-messenger_0.1.0-1_arm64.deb`, exactly 2,220,502 bytes with SHA-256
  `0a2ef1edb3bebf1659d1e8575ab1c78d7561337d61dcfb5d57561705682aed53`.
- Immediately after the implementation commit, `git status --short` contained only
  the intentional `PLAN.md` and `NOTES.md` completion-record edits.
- `git remote -v` has no output. No remote, push, PR, AppStore action, device or
  hardware access, RF transmission, login/OAuth, or publication was performed.
- This completion record is committed separately so it can cite the immutable
  implementation commit. Its documentation commit hash is reported in the Phase 5
  completion handoff.

## 2026-07-24 — Phase 6 software implementation complete; physical gate open

### Authorization and approved hardware

- User authorization: exact reply `次のステップに進んで`.
- User clarification: `Cap LoRaは日本でも発売しているので問題ないです。`
  and the Zero-compatible Cap LoRa-1262 documentation URL.
- Approved device-side scope: M5Stack Cap LoRa-1262 (SKU U214, SX1262,
  868–923 MHz) in its CardputerZero-compatible orientation, Japan profile, and
  supplied/approved Japanese-market antenna.
- No hardware purchase, physical device access, RF transmission, login/OAuth,
  remote, push, PR, AppStore, or publication action occurred.

Primary sources used:

| Subject | Source |
| --- | --- |
| Cap LoRa-1262, pins, PI4IOE5V6408, antenna warning, TCXO | `https://docs.m5stack.com/en/cap/Cap_LoRa-1262` |
| CardputerZero EXT pinout | `https://docs.m5stack.com/en/CardputerZero` |
| Zero accessory availability | `https://shop.m5stack.com/blogs/news/m5stack-launches-cardputerzero-a-pocket-sized-linux-computer-for-makers-and-developers` |
| Current Meshtastic Japan range/power reference | `https://raw.githubusercontent.com/meshtastic/firmware/master/src/mesh/RadioInterface.cpp` |

The connector mapping used by the implementation is RST GPIO26, IRQ GPIO23, BUSY
GPIO22, SCK GPIO11, MOSI GPIO10, MISO GPIO9, NSS/CS1 GPIO7, I²C SDA GPIO2, and I²C
SCL GPIO3. The fixed profile is 920.8 MHz center, 125 kHz, SF9, CR 4/7, 13 dBm,
12-symbol preamble, private sync `0x12`, and -90 dBm listen-before-talk. The
calculated occupied channel is constrained to 920.5–923.0 MHz so it remains inside
both the selected Japan range and the Cap's documented 923 MHz upper bound.

### Implementation

- Added a bounded `ICapLora1262Radio` seam and `CapLora1262Transport` with one
  inbound datagram buffer, explicit overload metrics, asynchronous polling,
  fail-closed status mapping, and idempotent shutdown.
- Added `Japan920RadioPolicy`, including exact LoRa airtime calculation, a default
  6,000 ms/60,000 ms token bucket, 100 ms minimum gap, invalid-profile rejection,
  and fail-closed clock-rollback handling.
- Added the Linux Cap implementation for `/dev/spidev0.1`, `/dev/gpiochip0`, and
  `/dev/i2c-1`. It controls GPIO26/23/22, enables PI4IOE5V6408 P0 at I²C address
  `0x43`, initializes SX1262 with a 3.0 V TCXO, handles TX/RX/error/timeout IRQs,
  performs listen-before-talk, returns to continuous receive, sleeps the radio,
  and disables the antenna switch on shutdown.
- Added `RadioRuntime`, a bounded single-threaded bridge among the durable session,
  existing transmission scheduler, protocol fragment/reassembly path, radio
  policy, and transport. Local sends transition durably to `Broadcast` or `Failed`;
  inbound posts and their dedupe records commit atomically.
- The Linux product runtime constructs the radio only after exact
  `LORA_MESSENGER_ANTENNA_ATTACHED=1`. SPI/GPIO/I²C path overrides are optional;
  all absent permissions, node errors, configuration faults, and invalid profiles
  leave the radio disabled.
- Added EN/JA/zh-Hans radio-ready and queued-for-broadcast disclosures. The header
  distinguishes `LOCAL` from `JP LORA`; all languages continue to state that peer
  delivery is not confirmed.
- Kept the desktop simulator hardware-free and the core/application/protocol
  layers independent of Linux and radio headers.

### Verification

| Check | Result |
| --- | --- |
| Core Debug | 31/31 passed; 8.07 s |
| Core Release | 31/31 passed; 12.06 s |
| Core ASan+UBSan | 31/31 passed; 18.08 s |
| Desktop Debug | 43/43 passed; 19.84 s |
| Desktop Release | 43/43 passed; 22.09 s |
| Cap transport/policy unit tests | passed in every core and desktop configuration |
| Two-session radio runtime integration | send/receive and persistent duplicate suppression passed |
| Strict standalone driver compile | AArch64 GCC with `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` passed |
| Native ARM64 Linux build | complete; linked Linux SPI/GPIO/I²C driver |
| Official BSP cross-build | Debian trixie amd64, GCC 14.2.0, Release AArch64 build complete |
| Package validator | passed archive/layout/ELF/dependency and BSP ABI checks |
| Clean ARM64 package gate | complete install, `ldd`, empty `dpkg --verify`, remove passed |

The final artifact was produced in Debian trixie amd64 with GCC 14 and
`dpkg-shlibdeps`, using the pinned official v0.0.4 BSP sysroot and fixed local
LVGL/fmt sources:

| Property | Final value |
| --- | --- |
| File | `dist/lora-messenger_0.1.0-1_arm64.deb` |
| Size | 2,257,094 bytes |
| SHA-256 | `5533a24b37629979a273577b733526c400311f03bc07c926bc516558815b228c` |
| Architecture | `arm64` |
| Payload | 37 regular files |
| ELF interpreter | `/lib/ld-linux-aarch64.so.1` |
| ELF NEEDED | `libc.so.6`, `libfreetype.so.6`, `libgcc_s.so.1`, `libm.so.6`, `libpng16.so.16`, `libstdc++.so.6` |
| `dpkg-shlibdeps` Depends | `libc6 (>= 2.34), libfreetype6 (>= 2.2.1), libgcc-s1 (>= 4.5), libpng16-16t64 (>= 1.6.46), libstdc++6 (>= 14)` |
| GLIBCXX | required max `3.4.29`; BSP max `3.4.33` |
| CXXABI | required max `1.3.15`; BSP max `1.3.15` |
| GLIBC | required max `2.34`; BSP max `2.41` |

Intermediate issues were resolved rather than bypassed: new localized glyphs were
reworded to the curated font set; RX timeout now clears the IRQ and restarts
receive; minimal ARM64 package containers gained the Python/readelf dependencies
required by the validator; the Debian cross package environment gained the
explicit `libstdc++6:arm64` package metadata required by `dpkg-shlibdeps`; and the
slim-container documentation exclusion was removed before the complete
`dpkg --verify` gate. The known optimizer diagnostic inside the unchanged vendored
SQLite amalgamation remains the only cross-build warning.

### Remaining Phase 6 gate

Software implementation is complete, but Phase 6 itself is not marked complete.
Two physical CardputerZero + Cap LoRa-1262 units must pass
`docs/phase6-cap-lora-1262.md`: antenna/label inspection, normal-user device-node
access, minimum and fragmented payloads, duplicate persistence, congestion and
listen-before-talk behavior, bounded faults, restart/cancellation, and safe
shutdown. Compilation, simulation, packaging, and container installation are not
evidence of physical RF operation or legal compliance for an arbitrary unit.

## 2026-07-24 — Phase 8B Wi-Fi LAN preview complete; BLE/device gates open

### Authorization and scope

- User authorization: exact reply `この計画で進めてください`.
- Implemented a private-protocol IPv4 UDP LAN broadcast preview on port 42425.
  This is IP Messenger-like serverless same-LAN behavior, not IP Messenger
  UDP/2425 wire compatibility.
- Kept the feature device-only and opt-in with
  `LORA_MESSENGER_WIFI_BROADCAST=1`. During this preview, an explicit Wi-Fi
  selection takes precedence over LoRa; simultaneous fan-out and independent
  per-transport persistence remain Phase 8D.
- No CardputerZero, Wi-Fi AP, BlueZ controller, BLE advertisement, RF
  transmission, remote, push, PR, AppStore, login/OAuth, or publication action
  was accessed.

### Implementation

- Added a bounded `IUdpBroadcastSocket` seam and `UdpBroadcastTransport`, with
  explicit temporary-disconnect and fatal-close behavior, maximum datagram
  enforcement, nonblocking send/receive results, metrics, and idempotent shutdown.
- Added a POSIX IPv4 UDP implementation which binds `0.0.0.0:42425`, enables
  directed broadcast, rediscovers the selected interface address/netmask before
  sends, accepts only same-subnet sources on the project port, ignores local
  reflection, rejects truncation, and closes on fatal socket failures.
- Added `LanBroadcastPolicy`, a bounded byte token bucket with a minimum transmit
  gap, saturating arithmetic, and fail-closed clock-rollback behavior.
- Reused protocol v1, existing fragmentation/reassembly, CRC, scheduler, durable
  history, and canonical-payload duplicate suppression without introducing a
  second message format.
- Changed `RadioRuntime` so reversible Wi-Fi link loss leaves durable outbound
  work `Queued` for reconnect, while a permanently closed transport still reaches
  the scheduler failure path and terminates as `Failed`.
- Added `Wi-Fi LAN` transport state and explicit no-delivery-confirmation wording
  in English, Japanese, and Simplified Chinese. The UI never claims `Delivered`.
- Updated package permissions and metadata to disclose network and external
  hardware use, plus README, adapter documentation, plan, and the complete
  physical acceptance checklist in `docs/phase8-ble-wifi.md`.

### Verification

| Check | Final result |
| --- | --- |
| Core Debug | 34/34 passed; 8.60 s |
| Core Release | 34/34 passed; 1.99 s |
| Core ASan+UBSan | 34/34 passed; 18.72 s |
| Strict warnings | build passed with `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`; 34/34 tests passed; 7.22 s |
| Desktop Debug | 46/46 passed; 20.80 s |
| Desktop Release | 46/46 passed; 14.22 s |
| Wi-Fi adapter/policy tests | bounded transport, token bucket, POSIX socket filtering, disconnect, close, truncation, and validation tests passed in all applicable configurations |
| Radio runtime integration | two-session broadcast/deduplication, temporary disconnect/reconnect, and permanent-close failure tests passed |
| CardputerZero ARM64 | Debug and Release official-BSP cross-builds completed |
| Debian package | metadata, archive, layout, ELF dependency, and BSP ABI validator passed |
| Physical Wi-Fi/BLE | pending; no matching CardputerZero was present in the inspected USB inventory |

The first desktop run found that the initial Simplified Chinese LAN wording used
glyphs outside the curated embedded font. The wording was changed to the supported
`Wi-Fi LAN` form and the final font-coverage and full desktop suites passed. The
first targeted CTest command also omitted the multi-configuration `-C Debug`
selector and therefore reported tests as not run; the corrected command passed
5/5. A later parallel strict-build approval review timed out before execution, so
the strict gate was rerun sequentially and passed.

The final local ARM64 package is:

| Property | Final value |
| --- | --- |
| File | `dist/lora-messenger_0.1.0-1_arm64.deb` |
| Size | 2,314,752 bytes |
| SHA-256 | `db0525d9555879c9235b7c9709b386ed7f0507aa321ea88140af56b808b845ca` |
| Architecture | `arm64` |
| Payload | 37 regular files |
| ELF interpreter | `/lib/ld-linux-aarch64.so.1` |
| ELF NEEDED | `libc.so.6`, `libfreetype.so.6`, `libgcc_s.so.1`, `libjpeg.so.62`, `libm.so.6`, `libpng16.so.16`, `libstdc++.so.6`, `libz.so.1` |

The normal local package gate continues to warn that the Maintainer uses the
reserved `example.invalid` placeholder. This does not block local validation, but
publication-strict validation still requires a verified identity.

### Remaining physical gates

- Phase 8A must record the actual OS, kernel, interface, BlueZ/controller,
  normal-user permissions, simultaneous scan/advertise behavior, legacy
  service-data capacity, update rate, loss, latency, and current draw on two
  CardputerZero units.
- Phase 8B still requires two devices on the same AP, AP isolation, reconnect/DHCP
  change, malformed/off-subnet traffic, latency/loss, restart, and current-draw
  acceptance.
- Phase 8C BLE advertising is intentionally not implemented yet. The existing
  protocol minimum frame is 48 bytes, while BT 4.2 legacy advertisement data is at
  most 31 bytes before service-data overhead; a bounded second chunk layer is only
  justified after the physical update-rate and simultaneous scan/advertise gate.
- Phase 8D multi-transport fan-out/schema v4 and Phase 8E physical acceptance
  remain future authorized steps. A GATT connection, external BLE 5 controller,
  IP Messenger wire compatibility, router traversal, relay, encryption, or
  publication would require separate scope/approval.

### Final repository gate

- Canonical repository: `/Users/yutacar/work/LoraMessenger`.
- Implementation commit:
  `5f1b96cc166a434ee833d60a9353de92b06f2b62 feat: add phase 8 Wi-Fi LAN preview`.
- `git diff --cached --check` passed before the 26-file implementation commit.
- The ignored final `.deb` hash and size match the package table above.
- `git remote -v` has no output. No push, PR, or publication was performed.
- This completion-record update is committed separately so it can cite the
  immutable implementation commit; its hash is reported in the handoff.

## 2026-07-24 — Title menu, Talk routing, and startup skip implemented

### Authorization and behavior

- User authorization: `BattleShipのようにロゴ画面を作成し、起動時に表示し、設定はこの画面からメニューで行けるようにしてください`.
- Added a 320x170 branded first screen based on the local BattleShip menu
  structure: existing blue LoRa Messenger envelope icon, `LORA MESSENGER`,
  `OFFLINE BROADCAST`, and vertically selectable `Talk`/`Settings` entries.
- `Talk` opens the existing timeline. `Settings` opens from either the title menu
  or Talk (`S`) and returns to its caller with `Esc`. Talk `Esc` returns to the
  title menu; Home keeps the existing safe-exit confirmation.
- Added a persisted `Skip title` setting. OFF is the new-install and migration
  default. ON starts the next launch directly in Talk without removing access to
  Settings.
- Migrated settings JSON from schema v1 to v2 with strict canonical
  `ui.skip_title` boolean serialization. Existing v1 files are accepted as OFF;
  malformed, future, missing, wrong-type, and unknown fields remain fail-closed.

### Verification

| Check | Final result |
| --- | --- |
| Core Debug | 34/34 passed; 9.45 s |
| Core Release | 34/34 passed; 5.00 s |
| Core ASan+UBSan | 34/34 passed; 21.84 s |
| Strict warnings | build passed with warnings as errors; 34/34 passed; 10.29 s |
| Desktop Debug | 46/46 passed; 23.05 s |
| Desktop Release | 46/46 passed; 16.34 s |
| Routing/ViewModel | title→Talk, title→Settings, Talk→Settings, caller return, direct-Talk startup, and durable-commit failure/success paths passed |
| Persistence | schema v1 migration, canonical v2 round trip, first-launch OFF, persisted ON, and second-launch direct Talk passed |
| UI/font | title screenshot visually inspected; EN/JA/zh-Hans font coverage and keyboard flow passed |
| CardputerZero ARM64 | official-BSP Debug and Release cross-builds completed |
| Debian package | metadata, archive/layout, ELF dependency, and BSP ABI validator passed |

Visual evidence:

- `build/darwin-arm64/test-work/ui-phase2-Debug/screenshot/title-en-menu.png`
- `build/darwin-arm64/test-work/ui-phase2-Debug/screenshot/phase2-en-settings.png`

The first font-coverage run rejected the Japanese wording because the curated
embedded subset lacked `起` and `時`. The setting label was shortened to the
unambiguous ASCII `Skip title` with localized ON/OFF context; the final font and
full desktop suites passed. A later negative-commit assertion initially inherited
the ViewModel test helper's legacy direct-Talk default and therefore expected the
wrong starting flag; the case now explicitly starts with skipping OFF and passes
in Debug and strict-warning builds. The screenshots were inspected at their exact
320x170 resolution: the logo, wordmark, two menu rows, selected border, settings
rows, and single-line footer are legible without overlap.

The updated local ARM64 package is:

| Property | Final value |
| --- | --- |
| File | `dist/lora-messenger_0.1.0-1_arm64.deb` |
| Size | 2,317,504 bytes |
| SHA-256 | `4b95f90b73be5a9885e8222cc4b5f6810527149d5b552c35538cf8745109c643` |
| Architecture | `arm64` |
| Payload | 37 regular files |

The package Maintainer placeholder warning remains unchanged and only blocks a
future publication-strict build. No hardware, RF transmission, remote, push, PR,
AppStore, login/OAuth, or publication action was performed.

### Final repository gate

- Implementation commit:
  `30fa42f4b577be6914805bae3c3ed3500b0460d1 feat: add title menu and Talk routing`.
- `git diff --cached --check` passed for the intentional 20-file commit.
- The ignored `.deb` hash and size match the package table above.
- This completion-record update is committed separately so it can cite the
  immutable implementation commit; its hash is reported in the handoff.

## 2026-07-24 — Title logo replaced with generated BattleShip-style wordmark

### Authorization and behavior

- User authorization: `起動画面のロゴ部分をBattleShipと同じような形にしてください
  アイコンは表示しなくて良いです` (title screen logo should match BattleShip's
  own shape; the icon does not need to be shown).
- The brand panel's `lora-messenger_80.png` app-icon image plus plain
  `"LORA\nMESSENGER"` `lv_label` were replaced with a single pre-generated
  3D isometric block-art PNG, produced by a new `tools/generate_title_logo.py`
  that mirrors the sibling BattleShip project's own script (5x7 dot-matrix
  glyphs, extruded with silhouette-edge bevels, supersampled and downscaled).
  Colors were tuned to LoRa Messenger's own fixed dark-navy palette
  (`view::app_palette()`, accent `0x65D6B4`) rather than reused verbatim.
- The generated PNG is two lines ("LORA" / "MESSENGER") stacked and centered,
  since a single-line 13-letter wordmark would not fit legibly in the 306px
  brand panel at 320x170. The app icon is no longer shown on this screen; it
  remains installed and used only for the OS-level APPLaunch/store icon.
- `cmake/cm0-package.cmake` gained an install rule for
  `assets/images/title_logo.png` into
  `${CMAKE_INSTALL_DATADIR}/lora-messenger/images/` — the app's own
  `AssetManager` runtime lookup root (`/usr/share/lora-messenger/images/` on
  device). This path previously had no install rule for any in-app runtime
  image beyond fonts, so a real device build would have silently fallen back
  to the plain-text title instead of loading the generated logo. This is the
  one genuine defect this pass found and fixed, not a hypothetical.

### Verification

| Check | Final result |
| --- | --- |
| Desktop Debug (`darwin-arm64-dbg`) | 46/46 passed |
| Desktop Release (`darwin-arm64-rel`) | 46/46 passed |
| UI/screenshot | `ui_phase2_keyboard_flow`'s `title-en-menu` capture visually inspected: new logo renders correctly, no overlap with the tagline or the menu rows below the brand panel |

No hardware, RF transmission, remote, push, PR, AppStore, login/OAuth, or
publication action was performed. The ad-hoc `title_logo_preview.png`
generated for visual review (light/dark-style side-by-side, unused by the
app) was deleted before commit rather than left in the tree.

### Final repository gate

- Implementation commit:
  `346f6ad8d37bd9435cf7aa4440f82c523c1332e9 feat: replace title screen icon with generated BattleShip-style wordmark`.
- `git diff --cached --check` passed for the intentional 6-file commit.
- This completion-record update is committed separately so it can cite the
  immutable implementation commit; its hash is reported in the handoff.

## 2026-07-24 — Repository hygiene aligned with the sibling BattleShip project

### Authorization and behavior

- User authorization: `BattleShipに従ってリポジトリを整理してください` (organize the
  repository following BattleShip's conventions), clarified to cover both the
  in-progress uncommitted change and repository-structure/accessory-file
  alignment.
- Added `deploy.sh`, a workflow-build-then-`scp` convenience script, adapted
  from BattleShip's own `deploy.sh` (same `cp0-cross-package` workflow preset
  name, same template origin) with the package glob and remote-dir default
  updated for `lora-messenger`. It never runs unless `REMOTE_USER`/
  `REMOTE_HOST` are set by the caller; no deploy was performed.
- Added `README.ja.md`, a full Japanese translation of `README.md` (this
  project's UI already ships English/Japanese/`zh-Hans`, unlike BattleShip's
  README this one is not shortened — the existing `README.md` is written as a
  precise technical/audit record, so the translation preserves every
  commitment, path, env var, and number rather than condensing it). Added the
  reciprocal `README.md` -> `README.ja.md` link matching BattleShip's own
  cross-link convention.
- Added a root-level `THIRD_PARTY_LICENSES.md`, a BattleShip-style component
  summary table (component / license / used-for / in-`.deb`?), which
  supplements rather than replaces the existing, more detailed
  `assets/licenses/THIRD_PARTY_NOTICES.md` (exact upstream commits, URLs, and
  SHA-256/SHA3-256 hashes) — that file was not moved, since NOTES.md's own
  "Final repository gate" records already cite its path and moving it would
  have broken that provenance trail for no benefit.
- Fixed a real gap found while doing this: `assets/licenses/THIRD_PARTY_NOTICES.md`
  had no section for the vendored SQLite 3.53.3 amalgamation
  (`third_party/sqlite/`, statically linked into the shipped binary) even
  though its license file (`SQLite-Public-Domain.txt`) was already present
  and already installed into the `.deb`. Added the missing section, citing
  the same hashes already recorded in `third_party/sqlite/README.md`.
- Considered and deliberately did **not** copy two more BattleShip top-level
  items: a `store/` screenshot directory (this project's `app-builder.json`
  already points its `store.screenshots` at curated `screenshot/phase2-*.png`
  files, itself allowlisted in `.gitignore` — a working, already-wired
  convention; adding a parallel `store/` tree would fragment it for no
  benefit) and BattleShip's `models/` directory (LLM model files — this
  project has no LLM feature).

### Verification

- `bash -n deploy.sh` passed.
- `ctest -R package_metadata` (Debug) passed 1/1 after the README/license
  changes.
- The full desktop Debug/Release suites (46/46 each) were already verified
  green earlier in this session for the code change these docs describe; no
  source or build-graph file changed in this pass, only documentation and a
  new standalone shell script.

No hardware, RF transmission, remote, push, PR, AppStore, login/OAuth, or
publication action was performed.

## 2026-07-24 — README screenshots added, matching BattleShip

### Authorization and behavior

- User authorization: `READMEにBattleshipと同様にスクショを入れた説明にしてください`
  (add a screenshot-illustrated description to the README, matching
  BattleShip's own).
- Added a "Screenshots" section to both `README.md` and `README.ja.md`,
  mirroring BattleShip's two-row, three-column table layout, placed right
  after the top intro. Re-used this project's own established convention
  (top-level `screenshot/`, already referenced by `app-builder.json` and
  allowlisted in `.gitignore`) rather than introducing BattleShip's separate
  `docs/screenshots/` directory.
- Selected six current English screens for the grid (Title menu, Timeline,
  Compose, Post detail, Mentions, Settings) plus a Japanese Settings shot in
  the `README.ja.md` version, and refreshed all of them from a fresh
  `ui_phase2_keyboard_flow` run against current `main` so the grid reflects
  today's generated title-logo change rather than the Jul 23 captures. Added
  `title-en-menu.png` to the tracked `screenshot/` set and to `.gitignore`'s
  allowlist (it did not match the existing `phase2-*` pattern).
- Deliberately excluded the `phase2-ja-*`/`phase2-zh-hans-*` timeline shots
  from the grid: both show a `□` fallback glyph for an intentionally
  unsupported demo character (the documented glyph-fallback behavior, not a
  bug), which would read as a rendering defect out of context in a README
  screenshot; `phase2-ja-settings.png` was used instead for the one
  non-English shot, since it has no such glyph.
- Left `screenshot/phase0-home.png` untouched and out of the grid: it predates
  the title-menu feature (shows a "PHASE 0" placeholder badge) and is cited
  as an immutable Phase 0 completion-record artifact by both `PLAN.md` and
  earlier `NOTES.md` entries, so it was not deleted or replaced.

### Verification

- Visual inspection of all newly captured/copied PNGs at their native
  320x170 resolution (no clipping, overlap, or missing glyphs in the six
  screens used).
- `git diff --check` passed.

No hardware, RF transmission, remote, push, PR, AppStore, login/OAuth, or
publication action was performed.

## 2026-07-24 — CJK input via the host OS IME (desktop)

### Authorization and behavior

- User authorization: `日本語や中国語が送れない場合には送れるようにしてください`
  (if Japanese or Chinese cannot be sent, make it possible to send them).
- Root cause: LVGL's SDL keyboard indev (`lv_sdl_keyboard.c`'s
  `sdl_keyboard_read()`) packs an entire UTF-8-encoded character -- not a
  Unicode codepoint -- into an indev's `key` value, least-significant byte
  first, whenever the character arrived via an `SDL_TEXTINPUT` event. That is
  exactly how the host OS's native IME delivers composed text (Japanese,
  Chinese, any non-ASCII input). `src/app/app.cpp`'s `semantic_key()` used to
  do a plain `static_cast<char32_t>(key)` on that value; for any multi-byte
  character (all Japanese/Chinese input is 3 bytes in UTF-8) this produced
  garbage, which `TextEditor::insert()` then re-encoded as garbage. LVGL's own
  `lv_textarea` widget already knows to unpack this convention
  (`lv_textarea_add_char()`); this app bypasses `lv_textarea` for its own
  `TextEditor` and had never done the equivalent unpacking. `TextEditor` and
  `is_editable_scalar()` themselves were already fully correct for arbitrary
  Unicode -- the bug was entirely in this one packed-key boundary. ASCII typing
  was unaffected (single-byte UTF-8 round-trips as its own codepoint), and
  `APP_SCRIPT`'s `TEXT=` injection was also unaffected, since it decodes
  percent-encoded UTF-8 into real codepoints itself
  (`src/platform/app_script_parser.cpp`) and never goes through the SDL
  indev's packed-key path at all -- which is why this was never caught by the
  existing screenshot/script test suite (all of which type ASCII messages).
- Fix: added `src/platform/key_codec.h`, a header-only, dependency-free
  `platform::decode_packed_utf8_key(std::uint32_t)` that reverses LVGL's
  packing (same algorithm `lv_textarea_add_char()`/`lv_text_encoded_next()`
  use internally). Applied it once, at the single real boundary shared by
  both live input sources -- `src/platform/linux_input.cpp`'s `key_event_cb()`
  (registered via `attach_key_router()` on both the desktop SDL keyboard
  indev and, on-device, every discovered evdev keypad) -- rather than at
  `semantic_key()`, since `semantic_key()` is also reached directly by
  `app_script.cpp`'s `route_key(route_value(action))` for `TEXT=`, which
  already hands it a real codepoint; decoding again there would have broken
  non-ASCII `APP_SCRIPT` injection instead of fixing anything (caught by
  first attempting the fix at `semantic_key()` itself, then correcting course
  once `ui_phase2_keyboard_flow` started failing after that first attempt --
  see Verification). The evdev keypad's own `map_evdev_key()` only ever
  produces single ASCII bytes, so it round-trips through the same decode
  function unchanged.
- Scope boundary (unchanged, still correctly gated): the physical
  CardputerZero's 46-key hardware keyboard has no CJK keys at all, so
  composing arbitrary Japanese/Chinese text on-device still requires a future
  on-device IME (candidate list plus romaji/pinyin conversion) -- this fix
  only restores correct desktop-build IME typing, matching what was already
  documented as working for `APP_SCRIPT`. Typing an *existing* Japanese/
  Chinese UI string with the physical keys (e.g. selecting a Settings option)
  is unaffected either way, since those keys already map to their own
  printed ASCII characters. Updated `README.md`/`README.ja.md` and
  `PLAN.md`'s Phase 2 checklist to state this distinction precisely instead
  of the previous blanket "remains a device acceptance gate" wording.

### Verification

| Check | Result |
| --- | --- |
| New unit test `key_codec` (`tests/unit/key_codec_test.cpp`) | ASCII round-trip, 2/3/4-byte UTF-8 (including U+3042 "あ" and U+4E2D "中"), and malformed-input fallback all pass |
| `core-only` Debug (35 tests) | 35/35 passed |
| `core-only-sanitize` (ASan+UBSan, 35 tests) | 35/35 passed |
| Desktop Debug (47 tests, includes the new unit test) | first attempt: 46/47 -- `ui_phase2_keyboard_flow` failed at `EXPECT=screen:mentions` after `TAB`, screenshot showed "That character cannot be inserted" instead of the Mentions screen |
| Root cause of that regression | a self-inflicted editing mistake while restructuring `semantic_key()`'s `default:` case: the `case LV_KEY_NEXT: return {UiKey::Tab, 0};` line was accidentally deleted in the same edit, so Tab (0x09) fell through to the character-insert path and was rejected by `is_editable_scalar()`. Found by temporarily inserting `SHOT=` actions around the failing `TAB` step (CMakeLists.txt, reverted before commit) and inspecting the captured screen. Restored the missing `case` line. |
| Desktop Debug (retest) | 47/47 passed |
| Desktop Release | 47/47 passed |

No hardware, RF transmission, remote, push, PR, AppStore, login/OAuth, or
publication action was performed.

## 2026-07-26 — History squashed and v0.1.0 early-test release published

### Authorization and behavior

- User authorization: squash the full repository history into a single commit
  and cut a `v0.1.0` GitHub release with a built `.deb` attached, for people
  who already own CardputerZero hardware to try ahead of the maintainer's own
  physical device arriving. Confirmed explicitly, in order: squash scope (full
  history, not just this session), force-push permission, and rebuild-and-attach
  for the `.deb` (rather than reusing the stale one), and separately confirmed
  the release should be marked GitHub pre-release rather than a normal latest
  release.
- Squashed all 22 prior commits (Phase 0 bootstrap through the 2026-07-24 CJK
  input fix) into one root commit via an orphan branch
  (`git checkout --orphan`, `git add -A`, one commit, then replacing `main`),
  confirmed byte-identical working tree (`ninja: no work to do` on rebuild) and
  the full 47/47 desktop Debug suite still green before force-pushing.
  `git push --force origin main` rewrote the previously-pushed history
  (`dc685dd...70d7b62`).
- Rebuilt the ARM64 `.deb` from this exact commit via
  `cmake --workflow --preset cp0-cross-package` (the stale `dist/` artifact
  predated both the title-logo and CJK-input-fix commits and was deleted
  first). CPack's post-build validator passed with zero errors (only the
  known, already-documented Maintainer-placeholder warning).
- Tagged `v0.1.0` on the squashed commit and published a GitHub pre-release
  (`gh release create --prerelease`) with the rebuilt `.deb` attached and
  release notes stating explicitly that physical-device acceptance (PLAN.md
  Phase 7) is still pending and this is a tester build, plus install
  instructions and known limitations (Maintainer placeholder, LoRa/Wi-Fi
  verified only in simulation so far, no on-device CJK IME).

### Verification

| Check | Result |
| --- | --- |
| Working tree identity after squash | `cmake --build --preset darwin-arm64-dbg` reported `ninja: no work to do`; `ctest -C Debug` 47/47 passed |
| ARM64 package rebuild | `dpkg`/`readelf`/BSP-ABI validation passed with zero errors; `lora-messenger_0.1.0-1_arm64.deb`, 2,329,808 bytes, SHA-256 `fcff8df76bdde7917f1f26fd4110a518b6708b8c8ded06e923cdb0416353c5fc` |
| `git push --force origin main` | succeeded; remote `main` now matches the single squashed commit `70d7b62ff9fa71e76be7e20e7b9cf9b813758b03` |
| `git push origin v0.1.0` / `gh release create` | tag and pre-release published: https://github.com/yutacar/lora-messenger/releases/tag/v0.1.0 |

No hardware, RF transmission, login/OAuth, or AppStore submission was
performed. This is a GitHub-only pre-release for existing-hardware testers,
not an AppStore/`czdev publish` action.
