# Ports

The application/core uses narrow synchronous clock and random-byte interfaces.
Phase 3 adds `IStateCommit`, which accepts already validated identity or timeline
candidates and reports whether each is durable. It exposes no filesystem or SQLite
types. A future transport port will remain similarly independent of a specific
chipset or operating system.
