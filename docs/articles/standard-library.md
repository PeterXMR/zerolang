## Standard Library Reference

Zero's standard library is pay-as-used, capability-aware, and graph-backed.
Importing memory helpers does not pull in hosted filesystem helpers.

Hosted APIs report their target requirements in `zero inspect` and `zero size`.

## Graph-Backed Modules

The standard library compile path uses binary `std/*.graph` stores. Sibling
`std/*.0` files are human-readable projections so people can review what the
graph contains, but they are not the compiler source of truth. Agents should
learn the callable surface from `zero skills get stdlib`, inspect package use
with `zero query` or `zero inspect`, and patch user programs through the graph.

Module pages include Zero snippets because `.0` projections are still the most
compact way for humans to read examples. Treat those snippets as reviewable
projections of graph-authored programs. When a human rarely edits a projection,
run `zero import <package>` before checking or building so the graph store is
current again.

Runnable modules:

- `std.mem`: spans, byte equality, copy/fill, fixed-buffer allocators, byte buffers, and arena-style reset helpers.
- `std.collections`: fixed-capacity collection operations over caller-owned storage and explicit lengths.
- `std.search`: scalar span search, lower-bound, and binary-search helpers.
- `std.sort`: in-place insertion sort and sortedness checks over caller-owned scalar storage.
- `std.ascii`: byte predicates, ASCII case conversion, and digit value helpers.
- `std.fmt`: caller-buffer formatting for booleans and integer text.
- `std.text`: byte-backed ASCII and UTF-8 validation and counting.
- `std.io`: buffered reader/writer metadata, cursor writes, line scanning, and byte copy helpers over caller-owned storage.
- `std.args`: hosted process argument count, lookup, option search, fallback, and typed `u32` parsing.
- `std.cli`: hosted flag and option helpers for command-line programs.
- `std.env`: hosted environment lookup, fallback, and typed bool/`u32` parsing.
- `std.fs`: hosted file lifecycle helpers, owned file handles, byte reads/writes, explicit file copy, remove, rename, and close.
- `std.math`: fixed-width integer min/max/clamp, checked and saturating arithmetic, and small number-theory routines.
- `std.path`: fixed-buffer lexical path helpers.
- `std.str`: allocation-free byte-string helpers over spans and caller-owned storage.
- `std.testing`: Bool-returning helpers for test blocks and byte-output checks.
- `std.parse`: allocation-free byte scanners and integer/bool parsers.
- `std.codec`: byte-oriented integer encoding, endian reads/writes, varint, base64, hex, and CRC-32 helpers.
- `std.json`: validation, structured status codes, field lookup, explicit-allocator parsing, and caller-buffer writing.
- `std.log`: explicit-buffer JSON Lines record formatting.
- `std.url`: lexical URL splitting, percent/query encoding, query lookup, and append helpers.
- `std.time`: duration construction, conversion, comparison, and target-gated clock helpers.
- `std.rand`: explicit deterministic random sources, random bits, and target entropy helpers.
- `std.proc`: host process status helpers behind the process capability.
- `std.crypto`: small hash, keyed hash, constant-time equality, and entropy helpers.
- `std.net`: network capability metadata, localhost/loopback address builders, timeouts, and bootstrap connection/listener handles.
- `std.http`: HTTP status helpers, request/response envelope writers, request parsers, client/server metadata, TLS-boundary helpers, hosted fetch, response-body borrowing, and header-value lookup.

Each module page documents target support, allocation behavior, error behavior,
ownership notes, and runnable examples.

Use the CLI to inspect what a graph-backed program actually retains. Start with
readable output for agent context; add `--json` when automation needs stable
fields:

| Command | Shows |
| --- | --- |
| `zero query <graph-input>` | Module, function, call, reference, and node handles for graph edits. |
| `zero inspect <graph-input>` | Required capabilities and imported helpers in readable form. |
| `zero inspect --json <graph-input>` | Required capabilities and imported helpers. |
| `zero size --json <graph-input>` | Helper metadata and retained helper cost. |
| `zero mem --json <graph-input>` | `memoryBudgets`, `allocatorFacts`, `allocationInstrumentation`, and `collectionFacts`. |

The `stdlibHelpers` and `usedStdlibHelpers` JSON entries include `module`,
`effects`, `allocationBehavior`, `targetSupport`, `errorBehavior`,
`ownershipNotes`, `example`, and `apiStability` for each public helper.

## Metadata Contract

Public standard library symbols document the fields agents need to call them
safely:

```text
symbol: std.fs.readAllOrRaise
effects: fs
allocation behavior: caller allocator
target support: host
error behavior: `raises [NotFound, TooLarge, Io]`
ownership notes: returns owned<ByteBuf>
example: examples/readall-cli/
```

Module pages may group related symbols when their metadata is identical.

Keep these labels visible: effects, allocation behavior, target support, error
behavior, ownership notes, and example.
