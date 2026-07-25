# Application layer

`MessengerState` implements the synchronous, UI-independent commands for
identity initialization/restore/rename, local composition, accepted received posts,
and outbound terminal-state notifications. It injects clock and random ports and
keeps every rejected command transactional. Phase 3 optionally injects a synchronous
state-commit port. Candidate history is persisted before adoption; compose reserves
the identity sequence first, so a later database failure creates a safe gap rather
than reusing a sequence. The synchronous ViewModel invokes these commands on the UI
thread. Background-worker coordination remains later work.
