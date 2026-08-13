# Buffer ownership and protection

zIPC uses exclusive ownership: at any instant a slot is free, owned by one
component, or being transferred to one next owner.

## API enforcement

`zipc_buffer_t` is opaque. Successful send/release invalidates the local object,
as does `ZIPC_ERR_TRANSPORT_PUBLISHED`: the send reported an error, but its
descriptor was already visible and sender ownership is consumed. Ordinary
`ZIPC_ERR_TRANSPORT` is pre-publication, so send rolls back and leaves the local
buffer valid. These rules catch use-after-send, double-send, use-after-release,
and double-release through the public API. Generation counters reject stale
handles after slot reuse.

A retained raw pointer returned earlier by `zipc_buffer_data()` is different: in
normal mode it can still address the shared mapping after ownership transfer.
Applications must treat the pointer lifetime as ending at send/release.

## Guard pages

`ZIPC_POOL_F_GUARD_PAGES` places a `PROT_NONE` page after each page-aligned slot
on supported platforms. A linear overrun reaches the guard before the next slot.
This detects bounds corruption but does not revoke access to bytes that remain
inside a valid slot.

## Strict ownership protection

`ZIPC_POOL_F_STRICT_OWNERSHIP` is an optional Linux/debug-oriented mode. Payload
slots are `PROT_NONE` while the local process does not own them. Allocation or
receive changes the owned slot to read/write; send or release revokes it again.
Consequently a retained pointer used after send/release faults with SIGSEGV or
SIGBUS.

Protection changes complete before ownership state publication: release and
recovery revoke access before `FREE`, and low-level
`zipc_buffer_prepare_transfer()` revokes access before publishing `TRANSFER`.
Low-level and high-level allocation enable access before publishing `OWNED`,
while unpublished-send rollback restores access before `OWNED`. Failed
restoration does not return apparent ownership; the local buffer is invalidated
and administrative `CLAIMING` recovery is required.

Requirements:

- payload backend supports page protection;
- payload offset and slot capacity are page aligned;
- the mode is disabled by default;
- mprotect/TLB activity adds transfer latency and should be measured before use
  in a production low-latency path.

Guard pages and strict ownership are independent and may be combined when their
layout requirements are satisfied.

## Offset access and logical-window changes

`zipc_buffer_at(buffer, offset, length)` is valid only while the local process
owns the buffer. Its offset is absolute from buffer storage offset zero. It is
therefore stable across `zipc_buffer_prepend()`, `zipc_buffer_trim_front()`, and
`zipc_buffer_trim_back()` operations, which change only the logical data window.

A successful `zipc_send()` invalidates the local `zipc_buffer_t`; subsequent
`zipc_buffer_at()` calls return `NULL`. In optional strict-ownership mode, a raw
pointer retained before the transfer is additionally protected by the platform
page-permission mechanism when supported.
