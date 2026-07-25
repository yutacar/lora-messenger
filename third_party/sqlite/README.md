# SQLite amalgamation

This directory vendors the official SQLite 3.53.3 amalgamation so desktop and
CardputerZero builds use the same database implementation.

- Upstream: <https://www.sqlite.org/>
- Archive:
  <https://www.sqlite.org/2026/sqlite-amalgamation-3530300.zip>
- Downloaded: 2026-07-23
- Archive SHA3-256:
  `d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9`
- `sqlite3.c` SHA3-256:
  `28e484abdaa43630e34040ef6ed92be973a1ad54107803d8af5145b889c23ed7`

The archive hash and `sqlite3.c` hash were verified before import. Only
`sqlite3.c`, `sqlite3.h`, and `sqlite3ext.h` are retained from the archive.
SQLite is dedicated to the public domain; see
[`assets/licenses/SQLite-Public-Domain.txt`](../../assets/licenses/SQLite-Public-Domain.txt).

The project build should compile `sqlite3.c` as C with:

- `SQLITE_DQS=0`
- `SQLITE_DEFAULT_MEMSTATUS=0`
- `SQLITE_DEFAULT_WAL_SYNCHRONOUS=2`
- `SQLITE_ENABLE_API_ARMOR`
- `SQLITE_OMIT_LOAD_EXTENSION`
- `SQLITE_THREADSAFE=1`

Do not replace these files from an unverified package-manager copy.
