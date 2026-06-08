## Getting Started

Zero is the programming language for agents. The normal user workflow is an
agent conversation over a graph-first package, not hand-writing source text.
Ask the agent to inspect and patch `zero.graph`; export `.0` only when humans
need a readable projection for review, audit, or an occasional manual edit.

## Install The Compiler

```sh
curl -fsSL https://zerolang.ai/install.sh | bash
export PATH="$HOME/.zero/bin:$PATH"
zero --version
```

The installer downloads the latest matching binary from the GitHub release and
writes it to `$HOME/.zero/bin/zero`.

## Ask An Agent For Hello World

Start by telling the agent to use Zero's bundled skills and to write through
the graph. The agent should learn the local compiler surface from `zero skills`
before it edits anything:

```text
Check out the agent, graph, and language skills for Zero:
zero skills list
zero skills get agent
zero skills get graph
zero skills get language

Create a hello world Zero program. Use the graph to read and write. `.0` files
are for humans only; export them from the graph after the graph checks and runs.
```

A typical agent conversation should look like this:

```text
You:
  Create a hello world Zero program. Use the graph to read and write.
  `.0` files are for humans only.

Agent:
  I will initialize a graph-first package, patch zero.graph, validate it, run it,
  then export the `.0` projection for review.

Agent runs:
  zero init hello
  cd hello
  zero patch --op 'addMain' --op 'addCheckWrite fn="main" text="hello from zero\n"'
  zero check .
  zero run .
  zero export .
  zero verify-projection .

Agent reports:
  The package compiles from zero.graph, zero run . prints "hello from zero",
  and src/main.0 was regenerated from the graph for human review.
```

Expected output from the run:

```text
hello from zero
```

## What The Agent Created

The graph-first package has these important files:

```text
hello/
  zero.toml
  zero.graph
  src/
    main.0
```

`zero.graph` is the repository graph store and the normal compiler input for
this package. Agents should usually inspect and patch that graph with
`zero query` and `zero patch`. Its default encoding is binary so the compiler
can load typed graph tables directly. Use `--format text` only when you
specifically want a readable repository store artifact for debugging. Binary
graph artifacts are also supported for explicit graph outputs. The standard
library already uses binary `std/*.graph` stores for compilation while keeping
`std/*.0` projections for human review.

`src/main.0` is the human-readable projection. It is deliberately readable and
bidirectional: humans can review it, and humans may edit it directly when that
is the right workflow. After a human edit, run `zero import .`
to refresh `zero.graph`. Agents should not normally hand-write `.0` files.

After `zero export .`, the projection should look like this:

```zero
pub fn main(world: World) -> Void raises {
    check world.out.write("hello from zero\n")
}
```

That file is useful for human trust. The graph remains the normal agent
read/write surface, and most users should interact with it by asking an agent
to make a checked graph edit.

## Check, Run, And Build

From inside the graph-first package:

```sh
zero check .
zero run .
zero build --target linux-musl-x64 --out .zero/out/hello .
```

Normal compiler commands compile from `zero.graph`. They report source
projection state, but they do not rewrite `.0` files. Export explicitly when
the human projection needs to be refreshed.

## Ask For The Next Change

Keep the same graph-first instruction in the prompt:

```text
Add a function add(x, y) that returns x + y, and add a test for it.
Use the graph to read and write. Do not hand-edit `.0`; export the projection
from the graph after checks pass.
```

The agent should use `zero patch` operations, validate with `zero check .` and
`zero test .`, then run `zero export .` only for the human
projection.

## Learn The Core Syntax

The examples are still useful for humans learning how Zero projections read.
Run the graph sidecars, not the `.0` files:

```sh
zero check examples/hello.graph
zero check examples/hello-let.graph
zero check examples/functions.graph
zero check examples/branch.graph
zero check examples/point.graph
zero check examples/result-choice.graph
```

They cover:

- entry points and output
- `let` bindings
- functions and return values
- conditionals
- `type` data declarations
- `enum`, `choice`, and `match`

For agent-authored programs, use those files as projections and examples, not
as the default write path.

## Inspect A Package

Use graph query to inspect a package in slices:

```sh
zero query .
zero query --fn main .
zero query --find write .
```

The manifest records package metadata; the checked-in `zero.graph` beside it is
the compiler input:

```toml
[package]
name = "hello"
version = "0.1.0"
license = "MIT"

[targets.cli]
kind = "exe"
main = "src/main.0"
```

After the first graph-backed build or run, Zero may also create
`.zero/cache/native/mir-*.zmir`. That file is not an authoring surface. It is a
derived final-MIR cache that the compiler memory-maps and verifies before
codegen so later graph builds can stay closer to the compiler.

## Next Steps

- Load `zero skills get agent`, `zero skills get graph`, and
  `zero skills get stdlib` before asking an agent for larger changes.
- Use the examples index to pick the next concept by projection.
- Use the CLI reference for `zero patch`, `zero query`, `zero import`, and
  `zero export`.
- Use Building From Source when you want to validate a local checkout.
