# LoRa Messenger for CardputerZero — Development Plan

- Revision: 2.2 (Phase 6 software complete; physical gate open)
- Date: 2026-07-24
- Current status: Phase 0 through Phase 5 complete / Phase 6 software implemented /
  physical two-device acceptance pending
- Target work folder: `/Users/yutacar/work/LoraMessenger`
- Package name: `lora-messenger`
- Display name: `LoRa Messenger`
- License: MIT

Phase 0 was authorized by the user's exact reply `推奨案で開始` on 2026-07-22.
The local project bootstrap and simulator gate are complete. No hardware purchase,
RF transmission, GitHub publication, or AppStore submission has started. This file
is now the project's planning source of truth; exact results are recorded in
`NOTES.md`.

Phase 1 was authorized by the user's exact reply `次へ進んでください` on
2026-07-23 and completed the same day. Its scope was limited to pure-C++
domain/application rules and tests; UI flows, persistence, protocol frames,
transports, real radio code, packaging, hardware work, and publication remain
outside this phase.

Phase 2 was authorized by the user's exact reply `次のフェーズに進んでください`
on 2026-07-23 and completed the same day. Its scope is keyboard-operated local UI,
deterministic simulator scripts and captures, and EN/JA/zh-Hans bundled UI
rendering. At the Phase 2 gate, session state was deliberately ephemeral:
persistence, protocol frames, real or simulated network transport, LoRa/RF,
ARM64/`.deb` production, hardware work, and publication remained outside that
phase.

ARM64/`.deb` production, package inspection, and publication remain outside Phase
2. Existing host install rules are updated only to carry the Phase 2 runtime fonts,
licenses, and linked documentation.

Phase 3 was authorized by the user's exact reply `次へ進んでください` on
2026-07-23 and completed the same day. Its scope is crash-safe local
identity/settings and history persistence, schema migration, bounded retention,
recovery, confirmed local-data deletion, and cancellation-safe shutdown. Protocol,
simulated or real transport, RF/hardware, sound, ARM64/`.deb` production, remote
work, and publication remain outside this phase.

Phase 4 was authorized by the user's exact reply `次へ進んでください` on
2026-07-23 and completed the same day. Its scope is the bounded binary protocol,
fragmentation/reassembly, deduplication, retry scheduling, virtual-time
loopback/fault injection, and a local headless two-process simulated-radio gate.
The desktop UI remains detached from transport and does not imply delivery. Real
LoRa/RF, hardware, regional policy, BLE/Wi-Fi, packaging, remote work, and
publication remain outside this phase.

Phase 5 was authorized by the user's exact reply `次のフェーズに進んでください`
on 2026-07-23 and completed on 2026-07-24. Its scope is reproducible
CardputerZero ARM64 cross-builds, APPLaunch/CPack packaging, honest store
metadata, package/ELF/dependency inspection, and local reproduction of the current
read-only AppStore gates. Hardware execution, RF, login, OAuth, remote creation,
push, PR, publication, and AppStore submission remain outside this phase.

Phase 6 software work was authorized by the user's exact reply
`次のステップに進んで` and clarified on 2026-07-24 by the user's approval of the
Japan-market, CardputerZero-compatible Cap LoRa-1262. Its scope is the bounded
Linux SX1262 adapter, fixed Japan profile, airtime/congestion policy, persistent
send/receive integration, ARM64 package refresh, and non-hardware verification.
No physical device was connected and no RF transmission occurred. The phase gate
remains open until the documented two-device acceptance run is complete.

## 1. Requirements understood

### 1.1 MVP requirements

1. Send and receive short text posts over direct LoRa communication while fully
   offline.
2. Show every valid post physically received by the device in a public-timeline UI.
3. Support replies and mentions. A mention is addressed to an installation UUID,
   while the UI displays the sender's user ID plus a short UUID suffix when needed
   to disambiguate duplicate names.
4. Generate an installation UUID once on the first successful launch and persist it
   atomically. This intentionally resolves the literal "generate at every launch"
   wording, which would otherwise break mentions and history after every restart.
5. Keep no server or shared history. Each device stores only posts it sent itself or
   actually received over a transport.
6. Store settings, identity, history, and user data locally, with crash-safe writes
   and schema migration.
7. Support keyboard-only operation, safe shutdown, English as the default UI, and
   Japanese plus Simplified Chinese (`zh-Hans`) UI resources.
8. Build and test hardware-independent behavior on macOS before device integration.
9. Use the display for complete interaction. Sound notification is a requested
   output target, but its behavior remains deferred until the actual CardputerZero
   audio path and device UX can be approved and tested; Phase 2 is screen-only.

### 1.2 Explicit interpretation of privacy and delivery

LoRa broadcast is not private. Any compatible receiver in range can receive, record,
copy, or spoof an unencrypted post. Therefore "only the sender and receiver can see
past data" is implemented as local history without history synchronization; it is
not a confidentiality guarantee. Mentions and replies remain public posts, not
direct/private messages.

MVP does not claim end-to-end delivery. With no central coordinator and an unknown
number of broadcast listeners, receiver ACKs would create collisions and unbounded
airtime. The UI will use `Queued`, `Broadcast`, and `Failed`, never `Delivered`.
Authentication, encryption, private messages, key exchange, mesh relaying, edit,
delete propagation, and history synchronization require a separately approved scope.

The UUID, user ID, and message body are pseudonymous identifiers and user-generated
data. Documentation and store declarations will not state that the app handles no
user data, even though no account, cloud service, or real-name field is used.

### 1.3 Deferred and out of scope

- BLE and Wi-Fi transports are deferred optional work after the LoRa MVP.
- Cloud services, accounts, analytics, telemetry, advertisements, payments, and
  external publication are out of scope.
- LoRaWAN is not used in the recommended MVP; the transport is raw point-to-point
  LoRa in single-hop broadcast mode.
- AppStore submission, GitHub repository creation, push, PR, OAuth login, and publish
  remain approval-gated operations.

## 2. Official-information snapshot

Refreshed on 2026-07-23:

- M5Stack's product page marks CardputerZero as **Work in progress** and says final
  packaging and software development are incomplete. It confirms an aarch64
  Raspberry Pi CM0, 512 MB RAM, a 1.9-inch ST7789 display at 170x320 (used as
  320x170 landscape), a 46-key keyboard, Wi-Fi/BLE, and SPI/UART/I2C/GPIO expansion.
  Its Quick Start, SDK, and Kernel sections still say "coming soon".
  <https://docs.m5stack.com/en/CardputerZero>
- The M5Stack AppBuilder URL currently redirects to the `CardputerZero` GitHub
  organization, which describes itself as the "M5Stack CardputerZero Community".
  These repositories are the best available current application baseline, but they
  are not treated as a finalized M5Stack SDK contract.
  <https://github.com/CardputerZero>
- M5Stack's vendor SDK repository currently has HEAD
  `8c412407623badbc24ed710b55f7a80b6a8d2fb3` (2026-04-07, LVGL 9.5), and its
  Linux device-tree/driver repository has HEAD
  `0da80268fe0fdc4aecd42aa15991047e79e3f384`. They remain
  authoritative inputs for BSP/device integration even though the product Quick
  Start and SDK documentation are unfinished.
  <https://github.com/m5stack/M5Stack_Linux_Libs>
  <https://github.com/m5stack/m5stack-linux-dtoverlays>
- Current standalone C++ template HEAD:
  `c08ca040af5a88f5d7c36b4ce0dd128b67a92e4f` (2026-06-26). It uses CMake,
  LVGL 9.5, SDL2 desktop builds, aarch64 cross-build presets, and CPack `.deb`
  packaging, and currently targets a Debian 13/trixie-aligned sysroot. This will
  be pinned as the starting baseline.
  <https://github.com/CardputerZero/Template>
- Current AppBuilder HEAD:
  `aac6074ea3d0123ed6b401e11cecf56a21c12bb4`. It documents the 320x170
  emulator, `app-builder.json`, `.deb` builds, store metadata, and the mutating
  `czdev publish` flow.
  <https://github.com/CardputerZero/AppBuilder>
- Current AppStore packages HEAD:
  `93529f9377fd36d2d1494741c2640f75beaa9776`. The store accepts
  reviewed ARM64/all Debian packages with APPLaunch desktop metadata and 320x170
  screenshots, then verifies package hashes before installation.
  <https://github.com/CardputerZero/packages>
- Current AppStore client HEAD is
  `3f5cf47b87d7c7231d9745e36900a87bfed95833`. Current publishing
  ownership is keyed to the first uploader's GitHub login (`uploaded_by`), not to a
  verified Maintainer email. Current `czdev publish` has no non-mutating
  preflight-only mode: it authenticates and changes remote state after inline
  checks, so Phase 5 reproduces the pure checks locally instead of invoking it.
  <https://github.com/CardputerZero/AppStore>
- Current APPLaunch HEAD is
  `8de9706a0b4fa7623f71f2c82b9b601f4864aa3f`, and the developer portal HEAD is
  `8e2d00598996b06f57199fce4c67c5fd4fd2fd6f`. Their relative icon path and
  ownership/publication behavior are treated as volatile inputs and will be
  refreshed again before any approved publish.
  <https://github.com/CardputerZero/APPLaunch>
  <https://github.com/CardputerZero/dev-portal>
- Two community OS paths currently coexist: a pi-gen/APPLaunch/fbdev image path and
  a newer Raspberry Pi OS/Debian profile using DRM/KMS, Wayland/labwc, PAM, and
  logind. It is not yet established which ABI/path will ship. Device display/session
  assumptions will therefore be rechecked against the actual image instead of copied
  from either path.
  <https://github.com/CardputerZero/pi-gen/tree/cardputerzero_v0.6>
  <https://github.com/CardputerZero/cardputer-zero-os>
- M5Stack's Cap LoRa-1262 is SX1262-based, SPI-connected, and covers 868–923 MHz,
  but its official page specifies Cardputer-Adv rather than CardputerZero. It is not
  assumed compatible with CardputerZero until the exact adapter, connection, region,
  antenna, and regulatory profile are confirmed.
  <https://docs.m5stack.com/en/cap/Cap_LoRa-1262>
- IP Messenger is used only as a product reference for serverless/offline messaging;
  its LAN protocol and security model will not be copied.
  <https://ipmsg.org/>

Because the official product software is unfinished and the community repositories
are changing quickly, exact upstream commits and store checks will be re-verified
again before device integration or publication.

## 3. Recommended architecture

Dependency direction:

```text
LVGL View
  -> ViewModel / Application services
    -> Core models and protocol
      -> Port interfaces
        -> Storage / simulated transport / LoRa / platform adapters
```

The core and protocol remain standard C++ without LVGL, SDL, Linux, RadioLib, or a
specific radio chipset. Time, randomness, storage, transport, and radio policy are
injected ports. Background workers may only place bounded events onto an application
queue; LVGL is updated exclusively by the UI/main thread.

Recommended tree:

```text
src/
  app/                    startup, lifecycle, navigation, safe shutdown
  application/            use cases and event dispatch
  core/                   validation and domain rules
  model/                  Identity, Post, Timeline, settings
  protocol/               message codec, frames, reassembly, dedupe
  ports/                  transport, storage, clock, random, radio policy
  adapters/
    storage/              atomic settings and SQLite history
    transport/            loopback, fault injection, UDP test bus, LoRa
  platform/               macOS/Linux/CardputerZero integration
  view/                   LVGL screens/widgets/theme/i18n
  viewmodel/              UI-facing state and commands
tests/
  unit/
  integration/
  stress/
  e2e/
tools/                    deterministic UI and E2E scripts
assets/
  fonts/
  images/
  locales/
screenshot/
docs/
  adr/
cmake/
```

The standalone `CardputerZero/Template` is preferred over making the app dependent
on the newer `lvgl-dlopen` runtime ABI. It already supports the required macOS SDL
loop and ARM64 `.deb` flow. An `app-builder.json` will still provide AppStore
metadata, with `runtime: legacy-deb-only`. The template's optional per-app systemd
service will be disabled: this is a user-launched app, not a background daemon.

## 4. Data and identity model

```text
Identity
  install_uuid: UUIDv4, generated once and persisted
  user_id: editable UTF-8 display name, not unique or authenticated
  last_issued_sender_sequence: uint64, 0 means none issued

Post
  message_id: UUIDv4
  sender_uuid: UUIDv4
  sender_sequence: uint64
  sender_user_id_snapshot: UTF-8 string
  body: UTF-8 string
  mentioned_uuids: 0..4 UUIDs
  reply_to_message_id: optional UUIDv4
  sender_time: optional, untrusted display hint

TimelineEntry (local metadata, never encoded as Post)
  post: validated Post
  received_order: local monotonic sequence
  origin: Received | Local(Queued | Broadcast | Failed | Unknown)
```

Recommended initial limits, subject to tightening after radio airtime measurement:

- User ID: 24 UTF-8 bytes
- Body: 160 UTF-8 bytes
- Mentions: 4
- Encoded logical post: 316 bytes (the exact Phase 4 canonical maximum)
- Fragments per post: 16
- Outbound logical posts: 16
- Inbound raw frames: 64
- Concurrent reassemblies: 8
- Reassembly timeout: 60 seconds
- Timeline entries: 256

The timeline uses local receive order because offline devices have no trustworthy
clock synchronization. A received reply may reference an unavailable parent and is
rendered explicitly as such. Local compose requires the parent to remain in current
history, and the reply insertion protects that parent from its own retention
eviction. The peer/mention picker is populated only from identities observed in
local history.

Phase 1 validates UTF-8 strictly by encoded byte count and preserves accepted text
without normalization. Empty/whitespace-only values, malformed encodings, NUL,
C0/C1 controls, bidi controls, Unicode line/paragraph separators, and noncharacters
are rejected. Bodies allow LF as their only line break; user IDs allow no line break
and no leading/trailing Unicode whitespace. Identity and timeline counters start at
1, commit only after the corresponding operation succeeds, and fail closed at
`UINT64_MAX` without wrapping.

Duplicate message IDs with identical payloads are idempotently rejected as
duplicates; differing payloads under the same ID are conflicts. At capacity, the
oldest received or terminal local entry may be evicted, while queued local entries
are never evicted. Received posts retain a received origin even when their sender
UUID equals the local installation UUID. Repeated identical terminal notifications
are idempotent; conflicting terminal transitions are rejected.

Phase 1 duplicate/conflict detection covers entries retained in the in-memory
timeline. Persisted message-ID dedupe and a bounded fragment-dedupe window remain
Phase 4 protocol/storage work. Timeline pointers/references are borrowed views and
are invalid after any mutation; the Phase 2 ViewModel retains `MessageId` values and
re-queries the aggregate. Every `int64` sender-time value is treated as an opaque,
untrusted hint and never an ordering key. Phase 2 does not display sender time or
pass it to platform date formatting.

The encoded-size, fragment, inbound-frame, reassembly, and timeout values above were
Phase 4 planning defaults during Phase 1. Phase 4 freezes them as protocol limits
and exercises each at N-1/N/N+1.

## 5. Wire protocol and transport policy

MVP uses a versioned bounded binary protocol. A fixed frame header contains a magic
value/application ID, protocol version, message ID/tag, sender sequence, fragment
index/count, total encoded length, payload length, and checksum. The logical post
codec uses explicit fields and lengths, rejects non-canonical or invalid values, and
never allocates based on unbounded input.

- The transport exposes its maximum datagram size; fragmentation is transport-
  independent.
- CRC detects accidental corruption only; it is not authentication.
- Unknown versions, invalid UTF-8, bad lengths/counts, checksum failures, stale
  fragments, and excess resource use are rejected safely.
- Message-ID dedupe is persisted; fragment dedupe uses a bounded in-memory LRU.
- Each frame is broadcast at most twice with deterministic-testable random jitter,
  a deadline, and a radio-policy airtime budget. There is no infinite queue or retry.
- Incomplete sends are not silently retransmitted after restart; the UI marks their
  outcome unknown and lets the user explicitly resend.
- MVP is single-hop. Relay/mesh, discovery beacons, and history replay are excluded.

`IRadioPolicy` owns allowed channels, transmit power, duty/dwell constraints, LBT if
required, and airtime budgets. The real transport remains transmit-locked until the
user confirms the country/region, certified module, antenna, physical connection,
and legal radio profile.

## 6. Persistence and migration

- Settings and installation identity: versioned JSON in the per-user XDG config
  directory, written as temporary file + flush + atomic rename, with validation and
  safe defaults. Identity reset is an explicit confirmation action.
- Message history: SQLite with transactions, WAL, integrity checks, foreign keys,
  `user_version` migrations, and bounded retention. The SQLite amalgamation may be
  pinned into the build if the target sysroot cannot provide a compatible library.
- Data directory: per-user XDG data directory under `lora-messenger/`.
- No message body is written to diagnostic logs by default.
- Uninstall removes packaged files but intentionally leaves per-user settings and
  history. README will document their location and an in-app confirmed delete path.
- Corrupt settings fall back safely without silently rotating the installation UUID;
  identity recovery behavior will be covered by tests and documented.

## 7. UI, keys, and language

Logical resolution is fixed at 320x170. Main surfaces:

1. Timeline
2. Post detail/thread context
3. Compose
4. Mention picker
5. Identity/radio settings
6. Error, send-status, and safe-exit modals

Keyboard proposal:

| Key | Action |
|---|---|
| Arrow keys | Move focus, scroll, or change a value |
| Enter | Open/confirm/send when focused |
| Esc | Back or close modal |
| Home or long Esc | Safe-exit confirmation |
| `N` | New post |
| `R` | Reply to selected post |
| `M` | Mention selected sender |
| `S` | Settings |

The UI always shows current focus and a compact key guide, and never relies on color
alone. Body text targets at least 16 px, supporting information about 14 px. Compose
shows the remaining UTF-8 byte budget. Timeline rows are virtualized/bounded.

UI strings are EN, JA, and `zh-Hans`, with English fallback and automated missing-key
checks. Bundled UI strings must have no missing glyphs. Arbitrary received Unicode is
sanitized and displayed with a documented fallback when outside the bundled font
coverage. Japanese/Chinese UI display is in MVP; arbitrary CJK composition depends
on the final CardputerZero OS input-method support and is a device acceptance item.

## 8. Development phases and objective completion gates

### Phase 0 — Approved baseline and project bootstrap — complete 2026-07-22

- [x] Recheck upstream HEADs and record them in `NOTES.md`.
- [x] Create the target folder and a fresh local Git repository without a remote.
- [x] Import the pinned standalone template, rename package/app identifiers, disable the
  per-app systemd service, and add PLAN/NOTES/README/LICENSE.
- [x] Add the layer skeleton and an initial 320x170 screen.

Gate: macOS Debug config/build succeeds; initial screen launches at 320x170 and is
visually inspected; a core smoke target compiles without UI/OS/radio dependencies;
`git diff --check` is clean; no RF driver or real transmission exists.

Result: passed. The reviewed capture is `screenshot/phase0-home.png`; Debug UI and
Debug/Release simulator suites pass 3/3 tests each, including failed-capture
propagation, while Debug/Release core-only suites pass 1/1 each. No RF
implementation exists. See `NOTES.md` for commands, hashes, teardown checks, and
remaining hardware gates.

### Phase 1 — Core model and local rules — complete 2026-07-23

- [x] Canonical UUID parsing/formatting and injected-random UUIDv4 generation.
- [x] Strict UTF-8 validation and byte-bounded user ID/body rules.
- [x] Identity, post, mention, reply, local timeline, dedupe, and bounded-state
  models.
- [x] Injected clock/random ports and application commands for initialize, restore,
  rename, compose, receive, and outbound state transitions.
- [x] CTest unit suites for empty, invalid, boundary, maximum, duplicate, ordering,
  exhaustion, random failure, and invalid-transition cases.
- [x] Dependency isolation, Debug, Release, and Address/UndefinedBehavior sanitizer
  verification plus P0/P1 review.

Phase 1 keeps sender time as an optional untrusted display hint and orders the
timeline only by a local monotonic receive sequence. It accepts received replies
with missing parents but requires a current parent for local compose, rejects
duplicate/invalid mention UUIDs, caps mentions at 4, queued local posts at 16,
timeline entries at 256, user IDs at 24 UTF-8 bytes, and bodies at 160 UTF-8 bytes.
Smaller injected timeline capacities reduce the queue cap to `capacity - 1`, so a
valid received post can always enter by using or replacing the reserved non-queued
slot while local receive order remains issuable. There is deliberately no
`Delivered` state.

The original template named GoogleTest/CTest as the standard Phase 1 route. This
project uses CTest with a dependency-free named-case/CHECK runner instead: the
core-only gate must configure offline before `FetchContent`, checks must remain
active under `NDEBUG`, and the suite needs no third-party test runtime. CTest still
owns discovery, execution, failure status, labels, and timeouts; the runner reports
case/check counts and catches unexpected exceptions.

Gate: core-only macOS Debug and Release suites are green; the sanitizer suite is
green; every Phase 1-owned behavioral limit and invalid UTF-8 class is exercised,
while later protocol defaults receive only planning review; duplicate retained
messages do not mutate state; invalid transitions are rejected; injected
clock/random behavior is deterministic; and core compile/link evidence contains no
LVGL, SDL, OS, network, storage, or radio dependency. P0/P1 review must have no
remaining finding.

Result: passed. The canonical repository's core-only Debug, Release, and ASan+UBSan
suites each pass 8/8 executables; the seven named Phase 1 suites contain 90 cases
and 735 checks plus the original smoke executable. The unchanged SDL simulator
passes 10/10 in both Debug and Release. Compile/link scans show no forbidden core
dependency, three same-scope re-reviews report no remaining P0/P1, and no RF,
storage, protocol, transport, or publication work was introduced. Implementation
commit: `dccb6e41bb42a70bde406c66efd5fce06f070bcd`.

### Phase 2 — Keyboard UI and deterministic screenshots — complete 2026-07-23

Authorization and fixed scope:

- [x] User authorized this phase with the exact reply
  `次のフェーズに進んでください` on 2026-07-23.
- [x] Add a pure-C++ UI controller/view state that retains message IDs rather than
  borrowed timeline pointers, consumes the Phase 1 `MessengerState`, and exposes
  no LVGL/SDL/platform dependency.
- [x] Seed a clearly labeled, deterministic in-memory local demonstration session.
  Composed posts remain `Queued` because no transport accepts them in this phase;
  the UI must say that radio, persistence, and delivery confirmation are
  unavailable. It must not imply a real loopback/network transport, `Broadcast`,
  or `Delivered` state.
- [x] Implement timeline selection, post detail/reply context, compose with UTF-8
  byte budget, observed-peer mention picker, session settings, send-status/error
  modal, and safe-exit confirmation at 320x170.
- [x] Make every primary action keyboard-only: arrows move focus/change values,
  Enter opens/confirms/sends, Esc backs out, Home opens the exit confirmation, and
  N/R/M/S open new/reply/mention/settings when text entry is not active. Printable
  ASCII and Backspace edit compose text. On the desktop build, arbitrary CJK
  composition through the host OS's own IME is implemented (2026-07-24 follow-up,
  see NOTES.md "CJK input via the host OS IME"); on the physical CardputerZero
  hardware keyboard (no CJK keys at all), it remains a final on-device IME gate.
- [x] Extend deterministic `APP_SCRIPT` input with all Phase 2 keys and bounded text
  entry. Invalid/unknown script actions must fail visibly and non-zero; successful
  scripts must reach common teardown and leave no live script timer.
- [x] Add EN, JA, and zh-Hans keyed resources with English fallback, automated
  missing-key checks, and a redistributable bundled font/subset covering every
  shipped localized UI glyph. Unsupported received glyphs must render with a
  documented fallback instead of reaching platform date/text formatting unsafely.
- [x] Add pure state/localization tests plus Debug and Release simulator CTests for
  deterministic keyboard flows, script errors, screenshot output, and repeated
  shutdown.
- [x] Capture and open at original size at least: EN timeline/detail/compose/
  mentions/settings/status/error/exit, JA timeline/settings, and zh-Hans timeline/
  settings. Record dimensions and inspect focus, truncation, overlap, clipping,
  modal legibility, language glyphs, and honest local/radio status.
- [x] Complete independent lifecycle/state/script, localization/font, and integrated
  UI reviews; resolve every P0/P1 finding and rerun the same-scope reviews.

Phase 2 resets identity, language, draft, and history on each launch. Session
settings are not persisted. Seeded received posts and locally composed posts exist
only to exercise the Phase 1 aggregate and UI; no protocol codec, transport bus,
socket, radio adapter, or background receive worker is introduced. English remains
the launch default. Home never exits immediately: it opens a modal where Enter
confirms and Esc cancels; OS window close still reaches the common teardown path.
Phase 2 implements display output only; sound notification remains a later
device-specific gate.

Gate: core-only Debug/Release/sanitizer suites remain green and dependency-isolated;
new UI-state and localization limits have boundary/invalid-transition tests; Debug
and Release simulator suites and deterministic scripts are green; all primary flows
complete using only the keyboard; safe exit and script/window-close paths stop every
owned timer/input/display resource; and every listed 320x170 capture is opened and
visually checked with no clipping, overlap, false transport/delivery claim, or
missing bundled UI glyph. P0/P1 review must have no remaining finding.

Result: passed. The canonical repository's core-only Debug, Release, and ASan+UBSan
suites each pass 12/12 CTests. The eleven named suites contain 139 cases and 1,956
checks plus the smoke executable; Phase 2 contributes 49 cases and 1,221 checks.
The desktop Debug and Release suites each pass 21/21 CTests, including font coverage,
six negative-script cases, the keyboard flow, and window-close teardown. Eighteen
Phase 2 flow captures and the automated window-close capture are 320x170; the
reviewed Debug and Release captures are byte-identical. Strict compilation covers
21 product translation units. Same-scope re-reviews report no remaining P0/P1. No
persistence, protocol, transport, RF, hardware, or publication work was introduced.
Implementation commit: `5b6b740ada817c27ab855abfb2b870c26a48d486`.

### Phase 3 — Crash-safe settings and history — complete

- [x] User authorized this phase with the exact reply
  `次へ進んでください` on 2026-07-23.
- [x] Keep `core`, `application`, and `viewmodel` free of SQLite, filesystem, LVGL,
  SDL, transport, and RF dependencies. Add narrow injected commit ports and a
  separately linked POSIX/SQLite storage adapter.
- [x] Store a versioned settings JSON document containing the installation UUID,
  validated user ID, durable sender-sequence high-water mark, and locale. Accept
  only a bounded, duplicate-free schema; migrate the supported previous schema and
  reject corrupt or future schemas without consuming random bytes or rotating the
  UUID.
- [x] Resolve only absolute XDG paths, falling back to `$HOME/.config` and
  `$HOME/.local/share`. Write settings with a same-directory mode-0600 temporary
  file, complete-write loop, file `fsync`, atomic rename, and parent-directory
  `fsync`; deterministic failure injection must prove the previous final file is
  preserved at every pre-rename interruption point.
- [x] Persist validated timeline entries and the independent receive-order
  high-water mark in SQLite. Enable foreign keys and WAL, run integrity checks,
  migrate `user_version` transactionally, preserve every post field and mention
  order, and store unsigned 64-bit counters without signed narrowing. Enumerate
  every `sqlite_schema` row and accept only the exact expected tables and
  SQLite-created autoindexes; reject hidden objects, including reserved-name
  injections.
- [x] Enforce the 256-entry retention bound transactionally. Restore exact local
  order and delivery origin; convert a previously `Queued` local post to `Unknown`
  after restart so launch never silently retransmits it.
- [x] Make every in-memory mutation persistence-first. For compose, durably reserve
  the next sender sequence in settings before the SQLite transaction; a later DB
  failure may leave a safe gap but must never permit sequence reuse. Storage errors
  leave the candidate timeline uncommitted and surface a localized UI error.
- [x] Replace the normal seeded demo startup with a persistent session. Deterministic
  demo rows remain available only behind an explicit simulator-test environment
  flag and isolated XDG test directories.
- [x] Persist locale changes before changing visible ViewModel state. Update the UI
  to say `Saved locally`, `radio disabled`, and `delivery unconfirmed`; add
  localized recovery/storage errors and a Settings delete-data confirmation whose
  initial focus is Cancel.
- [x] On corrupt/inconsistent settings or history, preserve all files and enter a
  recovery surface. Cancel exits without mutation; confirmed reset removes only the
  exact app-owned settings/temp/database/WAL/SHM/journal files and the managed
  `history.sqlite3.probe/` directory, synchronizes their parent directories, and
  exits. A forced-stop probe residue is also removed on the next startup while the
  data-tree lock is held. A new identity is created only on a later clean launch.
- [x] Make normal exit/window close cancellation-safe and idempotent: stop script and
  input callbacks, close/checkpoint the database, then destroy UI/display/logger
  resources. Uninstall intentionally leaves per-user data; document exact default
  and XDG-overridden locations plus the confirmed in-app deletion route.
- [x] Add unit/integration/E2E coverage for JSON boundaries, atomic-write failures,
  first launch, two-launch restart, locale/identity/sequence persistence, all post
  fields, unsigned counter extremes, schema migration, invalid rows, corrupt files,
  retention, transaction rollback, recovery cancel/reset, data deletion, and
  repeated shutdown. UI automation must use isolated validated XDG directories.
- [x] Pass core-only Debug/Release/sanitizer, desktop Debug/Release, strict warnings,
  dependency/RF scans, restart/corruption scripts, 320x170 screenshot inspection,
  and independent same-scope review with no remaining P0/P1.

Gate: restart persistence, interrupted-write simulation, invalid/old settings,
database migration, corruption handling, retention limits, and uninstall-data
documentation tests are green in Debug and Release. Settings/history cross-file
ordering must prove that a crash can create a sender-sequence gap but never reuse a
sequence. Corrupt or inconsistent data must remain untouched until explicit
confirmation, and no recovery path may silently replace an installation UUID.

Result: passed. Core-only Debug, Release, and ASan+UBSan each pass 19/19 CTests;
the named suites contain 208 cases and 2,694 checks plus smoke. Desktop Debug and
Release each pass 31/31 CTests, including two-launch restart, six-leaf recovery
preservation/deletion, managed-probe cleanup, and confirmed Settings deletion.
Strict compilation passes 26/26 product translation units. Exact-schema,
hidden-trigger injection, transactional-migration, split-XDG locking, first-launch
interruption, schema-only database, uncheckpointed-WAL, identity mismatch,
local-sequence monotonicity, and no-mutation rejection tests are green. The reviewed
320x170 restart/recovery/delete captures are stable across Debug and Release.
Independent re-review found no remaining P0/P1. No protocol, transport, socket,
RF/hardware, remote, or publication work was introduced.
Implementation commit: `176a719ecb9d980853ff3ac7304d980f38cc2de7`.

### Phase 4 — Radio protocol and simulated multi-node E2E — complete

Authorization and frozen scope:

- [x] User authorized this phase with the exact reply `次へ進んでください` on
  2026-07-23.
- [x] Freeze a version-1 canonical post encoding. The existing model's maximum
  encoding is 316 bytes; the protocol rejects anything above that exact logical
  limit before allocation.
- [x] Freeze a 28-byte DATA frame header containing two-byte application magic,
  combined version/type, a 64-bit message tag derived from the full message UUID,
  the full unsigned 64-bit sender sequence, fragment index/count, total and fragment
  lengths, and CRC-32/ISO-HDLC. At MTU 48 this leaves 20 payload bytes, so the
  316-byte maximum fits in 16 fragments.
- [x] Implement canonical encode/decode, CRC golden vectors, bounded
  fragmentation/reassembly, malformed input rejection, an eight-slot 60-second
  reassembly table, a 64-frame ingress queue, and a bounded recent-frame LRU.
- [x] Implement a 16-message outbound scheduler. Each frame may be offered to the
  transport at most twice using injected deterministic jitter and a finite
  deadline. Transport acceptance may produce `Broadcast`; no ACK, `Delivered`
  state, implicit restart retransmission, or receiver-count claim is introduced.
- [x] Persist a bounded full-canonical-payload message-ID dedupe ledger in SQLite
  schema v3. A newly received post and its dedupe record must commit in the same
  transaction; retained duplicates and conflicting payloads must not advance local
  order or persistence generation.
- [x] Add pure `IDatagramTransport` and `IRadioPolicy` ports plus virtual-time
  Loopback/FaultInject adapters. The product simulator remains transport-detached;
  these adapters are exercised by headless tests only and cannot access RF.
- [x] Run seeds `0..9999`, cycling MTUs `48/51/64/128/255` and forced clean,
  loss, duplicate, corruption, delay, reorder, disconnect/reconnect, and mixed
  profiles. Fault decisions use a specified deterministic integer generator, not
  wall time or implementation-defined distributions.
- [x] Add a local two-process headless simulated-radio gate over bounded POSIX
  process IPC. This is test-only local IPC, never an IP or real-radio transport.
  Process exit/HUP, restart, timeouts, child reaping, and shutdown must be bounded.
- [x] Pass Debug, Release, ASan+UBSan, strict warnings, golden/malformed vectors,
  boundary N-1/N/N+1 tests, 10,000-seed stress, two-process E2E, and independent
  protocol/lifecycle P0/P1 review.

Convergence compares canonical `PostPayload` sets sorted by message ID, not complete
timeline rows: the sender correctly retains `Local(Broadcast)` while the peer stores
`Received`, and local receive order may differ under reordering. A case is
deliverable only when every fragment has at least one unmodified, timely accepted
copy in one reassembly window. Permanent loss must leave each receiver history a
valid corruption-free subset. Duplicate/corrupt fragments must never create a
second commit or extend a reassembly indefinitely.

Gate: golden vectors and malformed-frame tests are green; MTUs 48/51/64/128/255 and
loss/duplicate/corrupt/delay/reorder/disconnect/reconnect cases complete 10,000 fixed
seeds without crash, leak, duplicate commit, or bound violation. Full convergence is
required only when every fragment is eventually delivered; permanent loss must leave
the receiver history a valid, corruption-free subset. Two headless processes agree
on the expected canonical post set for deliverable cases.

Result: passed. Core-only Debug, Release, strict-warning, and ASan+UBSan builds each
pass 27/27 CTests. The Phase 4 gate runs seeds `0..9999` across all five frozen MTUs
and clean/loss/duplicate/corrupt/delay/reorder/disconnect-reconnect/mixed plus
permanent-loss profiles. It compares canonical post bytes, observes forced
reordering, proves a non-empty valid permanent-loss subset, and runs two
simultaneous helper processes through duplicate replay, timeout, HUP, restart,
shutdown, and bounded reaping (4 cases / 422 checks).
SQLite v3 atomically commits received posts with a 2,048-record canonical dedupe
ledger, including migration rollback, write failpoints, restart, conflict, and
2,048/2,049 eviction boundaries. Independent protocol/lifecycle and persistence
re-reviews found no remaining P0/P1. No socket, real radio, RF, hardware, remote, or
publication work was introduced.
Implementation commit: `f5faeca69bcc071641f19bd5d31081459219f8ad`.

### Phase 5 — ARM64 build and package readiness without hardware

- CardputerZero cross-build, `.deb`, APPLaunch entry, assets/locales, declared
  permissions, and store metadata.
- Inspect `dpkg-deb`, `readelf`, runtime dependencies, package contents, install/remove
  scripts, `Terminal=false`, and absence of an unwanted systemd daemon.
- Use the repository CPack route unless the then-current AppBuilder workflow is
  proven not to synthesize a service for `legacy-deb-only` applications.
- Run the current `czdev` local preflight and reproduce the current `packages`
  Actions checks; do not claim a separate official "prepublish check" unless one is
  published by then.

Gate: ARM64 Debug/Release builds succeed; package checks report zero errors; current
AppStore preflight is reproduced locally where possible; cross-build is recorded as
cross-build only, never as device validation.

Result: passed. The pinned BSP produced AArch64 Debug and Release binaries with the
expected `/lib/ld-linux-aarch64.so.1` interpreter and no RPATH/RUNPATH. The final
Debian trixie/GCC 14 package is
`lora-messenger_0.1.0-1_arm64.deb` (2,220,502 bytes, SHA-256
`0a2ef1edb3bebf1659d1e8575ab1c78d7561337d61dcfb5d57561705682aed53`).
`dpkg-shlibdeps` generated versioned dependencies for libc, FreeType, libgcc,
libpng, and libstdc++; the package validator proved its AArch64 ELF and BSP ABI
bounds, safe archive/layout, icons, and absence of maintainer scripts, services,
setuid files, links, and device nodes. A clean ARM64 Debian trixie environment
installed, verified, and removed it without residue in the APPLaunch package
paths. Core Debug and ASan+UBSan each passed 28/28 tests; the one Release
two-process test blocked by the macOS sandbox passed directly outside that
sandbox; desktop Debug and Release each passed 40/40 tests.

The final icon restores the previously approved cyan/white/navy/amber foreground
and changes only the background to blue; the store asset is an opaque 512x512 PNG,
with opaque 100x100 and 80x80 APPLaunch derivatives. Current Template, AppBuilder,
packages, AppStore, APPLaunch, developer-portal, M5Stack library, and device-tree
sources were refreshed and pinned in `NOTES.md`. Because current `czdev publish`
has no read-only preflight and authenticates/mutates remote state, its pure local
checks were reproduced without invoking it. The project publication-strict
validator mechanically rejects the intentionally invalid local Maintainer
placeholder. No device execution,
hardware/RF, login/OAuth, remote, push, PR, or publication action occurred.

### Phase 6 — Real LoRa adapter (hardware and regulatory gate)

Approved software baseline:

- M5Stack Cap LoRa-1262, SKU U214, in its CardputerZero-compatible orientation,
  with the supplied/approved Japanese-market antenna attached before power;
- CardputerZero EXT mapping: RST GPIO26, IRQ GPIO23, BUSY GPIO22, SCK GPIO11,
  MOSI GPIO10, MISO GPIO9, NSS/CS1 GPIO7, and I²C GPIO2/GPIO3;
- Japan profile: 920.8 MHz center, 125 kHz, SF9, CR 4/7, 13 dBm, 12-symbol
  preamble, private sync `0x12`, -90 dBm listen-before-talk;
- an additional application token bucket of 6 seconds calculated airtime per
  60 seconds and a 100 ms minimum transmit gap.

The adapter is implemented below `IDatagramTransport`/`IRadioPolicy` without
changing core rules. Linux SPI/GPIO/I²C access is opt-in through the exact
`LORA_MESSENGER_ANTENNA_ATTACHED=1` acknowledgement and fails closed on missing
nodes, permissions, invalid profile, queue overflow, or radio errors. The existing
bounded scheduler, reassembly, transactional receive, persistent duplicate
suppression, local delivery states, and safe-shutdown path are reused.

Gate: two physical devices pass send/receive, duplicate suppression, bounded-failure
display, congestion/airtime policy, restart, cancellation, and safe-shutdown checks.
The adapter is tested at its minimum usable payload and across the selected profile.

Software result: passed. The Cap transport and policy unit tests, two-session
send/receive and persistent-deduplication integration test, core
Debug/Release/ASan+UBSan suites, desktop Debug/Release suites, native ARM64 Linux
build, official-BSP cross-build, package validator, and clean ARM64
install/verify/remove gate are green. The final `.deb` includes the device driver.

Physical result: pending. No CardputerZero, Cap, antenna, GPIO/SPI/I²C device node,
or RF spectrum was accessed during this gate. Complete
`docs/phase6-cap-lora-1262.md` on two physical devices before marking Phase 6
complete or beginning Phase 7.

### Phase 7 — Device acceptance and publication readiness

- Full EN/JA/zh-Hans device walkthrough, performance/memory checks, clean-device
  install/start/remove, screenshots/store text, dependency and permissions review.
- Recheck official requirements and resolve all P0/P1 review findings.

Gate: the physical acceptance checklist is complete, documentation matches measured
behavior, and the user separately approves any GitHub push, PR, or AppStore publish.

### Phase 8 — BLE/Wi-Fi local transports

Authorized on 2026-07-24. The first supported Wi-Fi behavior is a private-protocol
LAN broadcast, not wire compatibility with IP Messenger. Exact IP Messenger
UDP/2425 interoperability, router traversal, cloud relay, attachments, encryption,
and authenticated identity remain separate scopes.

#### Phase 8A — CardputerZero capability gate

- Verify the current OS exposes the selected Wi-Fi interface and BlueZ `hci0` to
  the normal application user.
- Measure simultaneous BLE advertising/scanning, usable legacy-advertisement
  service-data bytes, advertisement replacement rate, loss, latency, and current
  draw. CardputerZero documents BT 4.2, so BLE 5 extended advertising is not a
  prerequisite or an assumed capability.
- Record OS image, kernel, BlueZ/controller versions, permissions, interface names,
  and all commands. Do not use root as a product workaround.

Gate: Wi-Fi and BLE results are measured on at least two physical CardputerZero
units. If connectionless BLE cannot carry a 160-byte post within an agreed latency
and loss target, stop and request approval before considering GATT or added
hardware.

#### Phase 8B — Wi-Fi LAN broadcast

- Add a bounded nonblocking IPv4 UDP adapter on an unprivileged project port.
  Rediscover the selected interface's directed broadcast address before each send,
  accept only same-subnet datagrams, ignore local reflection, reject truncation,
  and never pass off-subnet datagrams into the protocol.
- Reuse protocol v1, maximum logical MTU 255, scheduler semantics, CRC,
  persistence, and canonical-payload duplicate suppression.
- Apply a LAN byte-rate/minimum-gap policy so retries cannot flood the subnet.
- Keep the feature opt-in while hardware and privacy acceptance remain open.
  Device preview uses `LORA_MESSENGER_WIFI_BROADCAST=1` and defaults to `wlan0`;
  `LORA_MESSENGER_WIFI_INTERFACE` may select the measured OS interface.
- Report `Wi-Fi LAN`, `Broadcast`, and no-delivery-confirmation wording in EN/JA/
  zh-Hans. Never display `Delivered`.

Gate: pure adapter/policy tests, POSIX build, two-process fault coverage, desktop
regression, ARM64 build/package validation, and two-device same-AP tests pass.
Guest/client-isolated AP failure must remain bounded and visible.

#### Phase 8C — BLE advertisement proof of concept

- Define a standards-compliant service-data envelope with protocol ID, datagram
  tag, chunk index/count, CRC, and timeout. Fragment below `IDatagramTransport` so
  the versioned post/frame protocol remains unchanged.
- Use connectionless non-connectable advertising plus LE scanning; no pairing,
  GATT connection, ACK, or Bluetooth address identity.
- Bound active chunk sets, duplicate records, memory, retries, and expiry. Fuzz
  loss, reordering, duplication, malformed counts, conflicting chunks, CRC
  failures, and address rotation before device transmission.

Gate: Phase 8A proves that BlueZ/controller advertisement updates and simultaneous
scan are adequate; deterministic simulation and two-device tests pass.

#### Phase 8D — multi-transport fan-out and persistence

- Give LoRa, Wi-Fi, and BLE independent schedulers, policies, ingress queues, and
  reassemblers. Different MTUs must never share one partial reassembly.
- Reuse one canonical post/message ID across selected links, then feed completed
  posts into the existing shared persistent duplicate ledger.
- Migrate history schema v3 to v4 with bounded per-message/per-transport terminal
  states. Derive an aggregate status without treating local acceptance as peer ACK.
- Add persisted transport selection: LoRa, BLE, Wi-Fi, or all available. Existing
  installations retain their current LoRa-only behavior until the user opts in.

Gate: cross-link duplicates produce one durable timeline entry; partial success,
restart, cancellation, queue bounds, and every per-link failure are deterministic.

#### Phase 8E — physical acceptance

- Exercise two and three devices on an isolated AP and in BLE/LoRa range,
  including simultaneous links, disconnect/reconnect, DHCP change, congestion,
  malformed traffic, restart, and safe shutdown.
- Measure CPU, memory, latency, loss, battery/current, and UI responsiveness.
- Update permissions, store disclosures, screenshots, README, and package records
  only from measured results.

Gate: all physical checklists are complete and the user separately approves any
publication action.

### UI follow-up — title menu and Talk routing

Authorized on 2026-07-24. The normal first screen is a BattleShip-style branded
menu using the existing LoRa Messenger icon, product wordmark, and two keyboard
entries: `Talk` and `Settings`.

- `Talk` opens the existing timeline/messaging flow.
- `Settings` is reachable from the title menu and from Talk with `S`; `Esc`
  returns to the screen that opened it.
- Talk `Esc` returns to the title menu without exiting or changing history.
- Settings persist a `Skip title` ON/OFF flag. ON starts directly in Talk on the
  next launch; the setting remains reachable from Talk.
- Settings JSON migrates from schema v1 to v2. Existing v1 files load with
  `Skip title` OFF, then serialize canonically as v2 on the next settings write.
- The feature must preserve recovery-first behavior, safe Home exit, delete-data
  confirmation, locale persistence, radio state, and the hardware-free desktop
  simulator.

Gate: ViewModel routing, failed/successful setting commits, v1-to-v2 parsing,
restart persistence, EN/JA/zh-Hans font coverage, 320x170 screenshots, desktop
Debug/Release, core Debug/Release/sanitizers/strict warnings, ARM64 cross-build,
and package validation pass.

### UI follow-up — title logo matched to BattleShip's generated wordmark

Authorized on 2026-07-24. The title screen's brand panel previously showed the
`lora-messenger_80.png` app-icon image next to a plain `lv_label` wordmark
("LORA" / "MESSENGER"). Replaced with a single pre-generated 3D block-art PNG
(`tools/generate_title_logo.py`, `assets/images/title_logo.png`), matching the
sibling BattleShip project's own `tools/generate_title_logo.py` technique
(5x7 dot-matrix glyphs extruded with isometric bevels) rather than an LVGL
label, since LVGL's built-in fonts have no bevel/shading support. The app icon
image is no longer shown on this screen; it remains installed and used for the
OS-level APPLaunch/store icon only.

- `render_menu()` (`src/view/screens/messenger_screen.cpp`) now renders the
  generated PNG as the title, centered at the top of the brand panel, with the
  "OFFLINE BROADCAST" tagline centered below it. The old plain-text wordmark
  is kept only as a fallback if the PNG asset fails to resolve.
- `src/app/app.cpp` resolves `images/title_logo.png` instead of
  `images/lora-messenger_80.png` when constructing `MessengerScreen`.
- `cmake/cm0-package.cmake` installs `assets/images/title_logo.png` into
  `${CMAKE_INSTALL_DATADIR}/lora-messenger/images/` (the app's own
  `AssetManager` runtime lookup root), mirroring how BattleShip ships its own
  `title_logo.png` next to its app icon — this was previously missing for any
  in-app runtime image beyond the store icon and would have made the device
  build silently fall back to the plain-text title.

Gate: macOS Debug/Release full suites green (46/46), including the existing
`ui_phase2_keyboard_flow` screenshot test that captures the title screen
(`title-en-menu`), visually inspected.

## 9. Verification and records

Every completed phase must update `PLAN.md` and append to `NOTES.md`:

- exact commands, platform, upstream commits, test counts, duration, seed count, and
  failures;
- screenshot paths plus explicit visual inspection result;
- Debug, Release, sanitizer, cross-build, package, and device results kept distinct;
- discovered issues, root cause, fix, and remaining hardware gates;
- `git diff --check`, review outcome, and intentional commit hash.

P0/P1 review is required after protocol/lifecycle work and again before packaging.
The same reviewer will recheck any P0/P1 fix. Flaky tests are investigated rather
than merely rerun.

## 10. Risks and approval decisions

1. **Platform volatility:** M5Stack's product SDK and packaging are unfinished, while
   community tooling changes rapidly. Mitigation: pin, isolate platform code, and
   recheck at Phase 0, Phase 5, and publication.
2. **Radio compatibility/regulation:** the Zero-compatible Cap LoRa-1262 and fixed
   Japan software profile are approved, but exact in-hand labels, OS device access,
   RF behavior, and deployment compliance are not established by compilation.
   Mitigation: explicit antenna acknowledgement, fail-closed bounds, and the
   two-device physical checklist before completing Phase 6.
3. **Broadcast privacy:** local-only history cannot stop third-party reception or
   recording. Mitigation: explicit UI/README/store disclosure; cryptography is a new
   approved scope if confidentiality is required.
4. **Identity spoofing:** UUID/user ID is not authenticated. Mitigation: disclose it,
   show short UUID suffixes, and never present identity as verified.
5. **Congestion/airtime:** a social-style feed can exceed LoRa capacity, especially
   at slow radio profiles. Mitigation: strict message/queue/retry/airtime limits and
   measurement before freezing the device profile.
6. **CJK input/font size:** rendering bundled strings and composing arbitrary CJK are
   different problems. Mitigation: gate bundled UI coverage now; gate real IME and
   broad received-text coverage on the final OS image.
7. **No hardware:** macOS, stress, cross-build, and package results cannot establish
   radio compatibility or device operation. Those remain explicitly uncompleted.

Approval of this plan accepts these recommended decisions:

- persistent first-install UUID, not a new UUID on every launch;
- raw single-hop public LoRa broadcast, with no delivery ACK, encryption, or identity
  authentication in MVP;
- local-only history rather than confidential history;
- `zh-Hans` for ZH, with device IME acceptance deferred;
- CardputerZero community AppStore as the intended store;
- BLE/Wi-Fi deferred until after MVP;
- exact LoRa hardware and regional radio profile decided before Phase 6.

The user's reply **「推奨案で開始」** authorized and completed Phase 0. The reply
**「次へ進んでください」** authorized and completed Phase 1. The reply
**「次のフェーズに進んでください」** authorized and completed Phase 2. The
subsequent replies **「次へ進んでください」** authorized Phase 3 and then Phase
4 in order. The next reply **「次のフェーズに進んでください」** authorized and
completed Phase 5 package readiness. The later icon requests changed the visual
asset only; the final choice restores the pre-color-change icon foreground and
changes only its background to blue. Real LoRa/RF, hardware execution, verified
Maintainer selection, login/OAuth, remote publication, and AppStore actions remain
separately approval-gated.
