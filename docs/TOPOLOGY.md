# zIPC topology configuration

zIPC uses one canonical in-memory topology model regardless of where the
configuration originates. The application data path is therefore independent
of configuration source.

```text
static C/header ----\
                    +--> zipc_topology_config_t --> validator/registry --> zipc_link_open()
INI file/string ----/
```

## Resource binding versus topology

Runtime resources contain process-local objects and pointers, so they cannot be
serialized directly into a portable configuration file. zIPC therefore binds
those objects once under stable names:

```c
zipc_topology_bind_pool("main", &pool);
zipc_topology_bind_transport("ab-ring", &ab_transport);
```

A topology then refers only to the stable names `main` and `ab-ring`. Both a
compiled header and an INI file use exactly the same names and resolve through
the same registry.

This split also keeps platform details out of application code. A FreeRTOS or
bare-metal BSP can bind a preallocated DDR pool and IPI transport, while Linux
can bind POSIX SHM and a shared ring, without changing the declarative link
model.

## Static/header configuration

A generated or hand-written `zipc_config.h` can contain:

```c
static const zipc_topology_link_config_t my_links[] = {
    {
        .name = "ab",
        .link_id = 0x1001,
        .pool_name = "main",
        .transport_name = "ab-ring",
        .local_component = COMP_A,
        .remote_component = COMP_B,
    },
};

static const zipc_topology_config_t my_topology = {
    .links = my_links,
    .link_count = sizeof(my_links) / sizeof(my_links[0]),
};
```

After platform resources have been bound:

```c
zipc_topology_register_config(&my_topology,
                              ZIPC_TOPOLOGY_REJECT_DUPLICATES);
zipc_link_open(&ab, "ab");
```

This is the recommended path for FreeRTOS, bare-metal and fixed embedded Linux
deployments.

## Runtime file configuration

Hosted/Linux builds may load the equivalent topology from a file:

```ini
[link.ab]
id = 0x1001
pool = main
transport = ab-ring
local = 1
remote = 2
hop_limit = 0
default_deadline_ns = 0
timeout_ticks = 0
```

```c
zipc_topology_load_file("/etc/zipc/zipc.conf",
                        ZIPC_TOPOLOGY_REJECT_DUPLICATES);
zipc_link_open(&ab, "ab");
```

`zipc_topology_load_string()` provides the same parser for configuration already
held in memory. It is useful for tests and for systems that obtain
configuration from another management service.

The file parser currently accepts `[link.NAME]` and `[link NAME]` sections.
Recognized keys are `id`/`link_id`, `pool`, `transport`, `local`, `remote`,
`local_epoch`, `hop_limit`, `default_deadline_ns`, and `timeout_ticks`.
Unknown keys are rejected rather than silently ignored.

## Shared logical links between processes

The registry is process-local. Therefore CompA and CompB can each define a
local entry named `ab` with the same logical link ID:

```text
CompA topology                 CompB topology
name = ab                      name = ab
id   = 0x1001                  id   = 0x1001
local = A                      local = B
remote = B                     remote = A
```

Both applications simply call:

```c
zipc_link_open(&link, "ab");
```

A single-process integration test that simulates both endpoints may use two
local names such as `ab-tx` and `ab-rx`.

## Validation and merge policy

`zipc_topology_validate()` checks names, non-zero link IDs, component IDs,
endpoint direction, and that referenced pool and transport resources have been
bound. Duplicate names or IDs inside one declarative configuration are
rejected.

The component namespace has 256 numeric entries, but only IDs 1 through 254 are
usable. IDs 0 and 255 are reserved and rejected by static, string/file, legacy,
and explicit link paths. `ZIPC_TOPOLOGY_MAX_LINKS` remains 64 and is unrelated
to component namespace size.

Registration/load APIs take one explicit policy:

- `ZIPC_TOPOLOGY_REJECT_DUPLICATES` — safest default; fail on an existing name
  or link ID.
- `ZIPC_TOPOLOGY_EXTEND` — add new entries but still reject conflicts.
- `ZIPC_TOPOLOGY_OVERRIDE` — explicitly replace matching entries.

There is no silent precedence between a compiled topology and a file. An
operator who wants a file to override compiled defaults must request
`ZIPC_TOPOLOGY_OVERRIDE` explicitly.

## Lifetime

The declarative topology registry copies link names and scalar configuration.
The pool and transport bindings refer to runtime resources and therefore those
resources must remain valid for links opened from the registry.

Call `zipc_topology_reset()` during controlled teardown/tests to clear the
process-local topology and all resource bindings. Existing already-opened
`zipc_link_t` objects retain their own runtime configuration and must still be
destroyed normally.
