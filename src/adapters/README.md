# Adapters

Phase 3 adds two synchronous local-storage adapters:

- strict versioned settings JSON written through a same-directory, fsync-before-
  rename atomic file operation;
- bounded SQLite history using the verified vendored SQLite 3.53.3 amalgamation,
  transactions, WAL, foreign keys, integrity checks, exact canonical schema
  verification, and transactional schema migration. Verification enumerates every
  `sqlite_schema` row and accepts only the expected tables and exact SQLite-created
  autoindexes; hidden triggers, including reserved-name injections, are rejected.

An existing history is bound to the loaded installation identity only after a
probe in the deterministic owner-only sibling `history.sqlite3.probe/` directory
of its main database, committed WAL, rollback journal, metadata, and all rows
succeeds. Rejected databases are not migrated, switched to WAL, chmodded, or
otherwise changed. The managed probe is normally removed immediately; forced-stop
residue is removed on the next startup while the data-tree lock is held and by
confirmed local-data deletion. Regular-file, single-link, and ownership checks also
keep SQLite from following or modifying symlink/hard-link aliases.

They are isolated in the `lora_messenger_storage` target and do not enter the pure
core/application/ViewModel link graph.

Phase 6 adds an independently testable Cap LoRa-1262 boundary:

- `cap_lora_1262_radio.*` adapts a bounded, asynchronous SX1262 seam to
  `IDatagramTransport`;
- `japan_920_radio_policy.*` validates the fixed 920.8 MHz/SF9/125 kHz/13 dBm
  profile and enforces calculated-airtime and minimum-gap limits;
- `linux_cap_lora_1262_radio.*` owns Linux SPI/GPIO/I²C operations for the
  Zero-compatible Cap, including the PI4IOE5V6408 antenna switch.

The pure Cap transport and policy compile in core-only tests. The Linux hardware
implementation is linked only by non-desktop Linux product builds. It requires the
explicit antenna acknowledgement, fails closed on every device/configuration
error, and performs no RF work in desktop or test builds. The simulated datagram
adapter remains test-only.

Phase 8B adds `adapters/network`:

- `UdpBroadcastTransport` maps a bounded nonblocking socket seam into
  `IDatagramTransport`;
- `PosixUdpBroadcastSocket` binds an unprivileged IPv4 UDP port, rediscovers the
  selected interface broadcast address, and filters receive sources to the current
  local subnet;
- `LanBroadcastPolicy` rate-limits bytes and enforces a minimum gap so scheduler
  retries cannot flood the LAN.

The product path is explicitly opt-in while physical acceptance is open. The
desktop still does not open a network transport; its POSIX implementation is only
compiled and unit-tested.
