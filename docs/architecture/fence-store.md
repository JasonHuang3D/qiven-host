# FenceStore

`FenceStore` is the Host Authority Kernel's durable authority-state substrate. It is not the execution journal.

Each durable slot uses an explicit fixed-width binary format with magic/version/size fields, an internal storage revision, `PersistentGeneration`, `FencingEpoch`, `AuthorityPhase`, reserved fields, and CRC32 corruption detection. C++ object layout is never persisted.

The internal storage revision advances on every durable state write and exists only to select the newest A/B slot after restart. It is deliberately separate from `PersistentGeneration`: normal authority phase changes may be persisted without minting a new authority generation. `FencingEpoch` never decreases, and any fence advance requires an authority-generation advance.

Windows writes use `CreateFileW`, `FILE_FLAG_WRITE_THROUGH`, `WriteFile`, and `FlushFileBuffers`, then re-read and verify the complete record before reporting success. The inactive/older slot is written first, so a torn newest write leaves the previous valid slot recoverable.

Bootstrap is fail-closed. Only a nonexistent store root is considered never initialized. Once the root exists, missing/corrupt slots are treated as corruption rather than silently recreating epoch zero. This also means a crash after creating the root but before establishing the first valid slot requires explicit trusted recovery rather than automatic reinitialization.

CRC32 detects accidental corruption; it is not cryptographic authentication and is not treated as recovery authority.
