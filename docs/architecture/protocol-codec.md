# Host Protocol Codec V1

The Batch 000 protocol codec is a pure, bounded representation layer. It deliberately precedes named-pipe transport and dispatch so wire compatibility, malformed-input behavior, and allocation bounds can be proven without conflating them with Windows IPC lifecycle or authority execution.

## Boundary

The codec follows ADR-0009: native C++ object layout is not the IPC representation. Frames are encoded field-by-field with fixed integer widths and explicit little-endian byte order. No native struct is copied to or from the wire. V1 has a 16-byte header and a hard 256-byte total frame cap; payload size is therefore bounded to 240 bytes. Encoding and decoding use caller-owned fixed storage and perform no heap allocation.

The header contains the four-byte family magic `QVH0`, protocol version `1`, an explicit message kind, exact payload byte count, and a reserved field that must be zero. A decoder rejects truncated or oversized frames, bad magic, unknown versions, unknown message kinds, nonzero reserved fields, wrong fixed payload sizes, and trailing bytes. Typed decoders also validate canonical field values before publishing output; failed decode leaves the caller's output object unchanged.

## Normal protocol surface

V1 contains only the normal Host operations frozen by ADR-0029:

- `Acquire` request / response;
- `ExecuteTestOperation` request / response;
- `Release` request / response;
- `QueryStatus` request / response.

There is intentionally no `Reconcile` message kind. Recovery is a separate authority surface and cannot be added to this normal codec merely because the future pipe is owner-ACLed.

A `BrokerSessionId` is deliberately **not** part of the wire contract. It is a Host-owned identity for one accepted local IPC connection and must be generated or assigned by the future transport/dispatcher, then injected into `AuthorityBroker` calls out-of-band from decoded application messages. A client cannot choose, replay, or copy the broker-session identity that protects completion, release, disconnect, and competing-session detection.

An Acquire request carries only a nonzero per-response execution identity and bounded printable-ASCII scope/intent metadata. Scope is capped at 32 bytes and intent at 64 bytes; unused wire bytes are canonical zero padding. The client does not choose or transmit a lease identifier. A successful Acquire response returns the execution identity, Host-issued opaque lease, persistent generation, and fencing epoch. Lease generation belongs to the future Host dispatcher/authority path, not to the codec.

ExecuteTestOperation carries the execution/lease identities, fence, monotonic request sequence, operation identity, and an explicit test-operation code. V1 freezes only `NoOp`. The wire request does not carry the `AuthorityState` replay digest; the future dispatcher derives that digest from the canonical decoded request so replay identity is not caller-selected. The dispatcher also supplies the Host-owned broker session separately when calling `begin_operation` / `finish_operation`.

Release carries the execution/lease identities and fence. Its broker session is likewise supplied by the transport/dispatcher rather than by the client message. QueryStatus has an empty request and a bounded response containing protocol result, broker-started and persistence-fault flags, protocol authority phase, generation, fence, and next request sequence.

Protocol result values are their own stable wire vocabulary rather than raw casts of internal `AuthorityError`, `AuthorityBrokerError`, Win32 errors, or persistence-layer enums. The future dispatcher is responsible for an explicit mapping. Native error values are not part of V1.

## Canonical encoding and failure behavior

Opaque IDs required by requests must be nonzero. Fence and request sequence values required for active authority must be nonzero. Boolean fields accept only `0` or `1`; enums accept only declared V1 values; all reserved/padding bytes must be zero. Error responses are canonical: fields that are meaningful only on success must remain zero. These rules prevent multiple wire encodings from representing the same logical message and make request digesting deterministic later.

Structural or semantic decode failure produces a codec error and no partially decoded message. The codec never dispatches authority work. A later pipe/dispatcher checkpoint may close a client, return a protocol-level rejection, or quarantine according to its accepted contract, but this representation layer does not invent transport or authority policy.

## Out of scope

This checkpoint does not implement named pipes, Windows ACLs, broker-session generation, request dispatch, lease generation, operation digesting, protected operations, Runtime/DCR adapters, production filesystem/Git/process side effects, recovery, or WebAuthn. It does not change Batch 000/001 acceptance boundaries and does not re-enable mutating DCR.
