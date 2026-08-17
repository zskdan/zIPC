# Buffer API walkthrough

This focused Linux example demonstrates the logical data window inside one
owned zIPC buffer:

```text
complete 256-byte storage
+---------------------------+----------------------------------------+
| headroom | logical frame  | tailroom, including metadata at 128    |
+---------------------------+----------------------------------------+
            [hdr]hello[tail]              ^ absolute storage offset 128
```

The producer:

1. Allocates an empty buffer with explicit headroom and tailroom requirements.
2. Writes metadata through `zipc_buffer_at()` at absolute storage offset 128.
3. Appends a body and trailer.
4. Prepends a header without reallocating the buffer.
5. Sends the buffer and verifies that successful send consumes local ownership.

The consumer receives the same slot, validates the logical frame and absolute
metadata, demonstrates bounds checking with an invalid absolute range, trims
the header and trailer without moving bytes, and releases ownership. The fixed
metadata remains at offset 128 while prepend and trim operations move only the
logical data window.

`zipc_buffer_at()` addresses storage but does not reserve it: the metadata is
still part of the tailroom reported by the buffer API. The example therefore
defines offset 128 as an application protocol boundary and checks before every
append that the logical frame cannot overlap it. A real protocol must enforce
the same kind of layout rule when combining fixed absolute regions with a
growing logical data window.

Build and run:

```sh
make buffer-api
./build/examples/zipc-buffer-api
```

The example prints size, headroom, and tailroom after every operation and ends
with a `PASS` line after validating all content and ownership transitions.
