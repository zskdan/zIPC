# AGENTS.md — zIPC repository instructions

These instructions apply to the complete repository unless a more specific
`AGENTS.md` exists in a subdirectory.

## Project status

- Current release: **v0.1.9**.
- The project is an experimental chained zero-copy IPC protocol.
- The public API and shared-memory ABI are not stable before v1.0.
- The next planned milestone is v0.2, but do not implement it unless explicitly requested.

Read these files before changing code:

1. `docs/CODEX-HANDOFF.md`
2. `docs/ARCHITECTURE.md`
3. `docs/DECISIONS.md`
4. `docs/ROADMAP.md`
5. `README.md`
6. `CHANGELOG.md`

Treat them as the authoritative continuation of the previous design work.
When code and documentation disagree, report the mismatch before silently
changing the design.

## Required workflow

- Work on a feature branch; do not commit directly to `main`.
- Prefer small, reviewable commits with descriptive messages.
- Preserve backward compatibility within a patch release.
- Update `VERSION`, `CHANGELOG.md`, `README.md`, affected documentation, tests,
  and `MANIFEST.txt` when a release changes them.
- Add Doxygen comments for every new public API starting with v0.2.
- Do not claim hardware validation when only host stubs or simulations were run.
- Do not silently weaken atomics, ownership rules, cache requirements, or error handling.

## Build and validation

Primary Linux validation:

```sh
make clean
make test
make utilities
```

Target-adapter host integration:

```sh
make integration-targets
```

Strict compiler policy:

```text
-std=c11 -O2 -Wall -Wextra -Werror
```

For changes touching public structures or shared-memory layout, also inspect:

- structure size and alignment;
- 32-bit versus 64-bit assumptions;
- atomic field alignment;
- generation and sequence wraparound;
- endianness assumptions;
- ABI version handling.

## Core protocol invariants

1. Exactly one component owns a slot at a time.
2. A handle is `{generation, slot_id}` and must reject stale reuse.
3. Only authoritative state transitions and shared counters require atomics.
4. Non-atomic slot fields are protected by exclusive ownership and
   release/acquire state transitions.
5. `allocation_cursor` is only a relaxed atomic hint; slot claim is authoritative.
6. Pool formatting, reset, and shutdown require external serialization.
7. Control memory must support CPU read/write and 32-bit atomics.
8. Payload memory may be separate and need not support atomics.
9. Never place control metadata in PL BRAM unless atomic semantics are proven.
10. The local cookie is never transported or interpreted by zIPC.

## Platform rules

- Linux userspace, Linux kernel, FreeRTOS, bare metal, Xen, and PL-facing paths
  may use different transport implementations behind the same protocol.
- Cache maintenance is a platform responsibility for non-coherent mappings.
- Barriers do not replace cache clean/invalidate operations.
- Linux `/dev/mem` mappings are prototypes; production reserved-memory access
  should use a dedicated driver.
- Normal non-cacheable memory is preferred for initial R5 shared-memory bring-up,
  not Device memory.
- Linux hard IRQ and FreeRTOS ISR callbacks should normally defer work.

## Code style

- C11.
- No compiler warnings under the strict flags above.
- Validate all public arguments.
- Use explicit-width integer types for ABI-visible data.
- Keep platform-independent policy in `src/core/`.
- Keep OS/BSP mechanics in `platform/`.
- Avoid hidden allocation in deterministic or ISR-relevant paths.
- Document ownership changes at API boundaries.

## Pull request expectations

Every PR should state:

- scope and roadmap milestone;
- API or ABI impact;
- ownership/concurrency impact;
- platform impact;
- tests executed and their results;
- hardware validation performed, or explicitly not performed;
- remaining risks or follow-up work.
