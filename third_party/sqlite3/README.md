# Pinned SQLite reader

Public-domain SQLite 3.53.1 amalgamation, copied byte-for-byte from the committed
CheatRunner dependency at 59dcb9efaba71af00dc29d6cfe35da7d1ea43651. The source
identifies Fossil c88b22011a54b4f6fbd149e9f8e4de77658c. Per-file hashes are
enforced from dependencies.lock.json; no working-tree changes were present in
the copied files. The copyright disclaimer is retained in both source files.

Used for worker-only read-only native title inventory. Extension loading and
URI filenames are disabled, temporary storage is in memory, and mmap is disabled.
The adapter serializes all connections through a nonblocking process-wide gate.
Actual SDK v0.43 compilation and native read-only queries passed on FW13.60; see
results/20261006-title-database. This does not qualify future other SQLite callers.
