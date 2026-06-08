## Human Review Surface

`.0` files are human-readable projections of the graph.

They exist for trust. A human can read the program, review a change, and make a
manual edit when needed. They are also useful for diagnostics because source
maps can point to familiar line and column spans.

They are not the normal agent write surface.

## Explicit Sync Model

Zero uses explicit commands so graph and projection state cannot silently drift:

```sh
zero export
zero import
zero verify-projection
zero status
```

Use `zero export` when a human needs the latest `.0` review text. Use
`zero import` after a human intentionally edits projection text and wants to
reconstruct the graph. Use `zero verify-projection` in review or CI when you
want a no-write drift check.

```json-render
{
  "messages": [
    {
      "role": "user",
      "text": "show me the projection so i can review it"
    },
    {
      "role": "assistant",
      "text": "I’ll export the current projection and verify it is in sync."
    },
    {
      "role": "tools",
      "calls": [
        {
          "command": "zero export",
          "output": "repository graph export ok\nwrote: ./src/main.0"
        },
        {
          "command": "zero verify-projection",
          "output": "repository graph verify-projection ok"
        }
      ]
    }
  ]
}
```

## No Silent Divergence

`zero status` reports whether the projection is clean, missing, stale,
conflicting, or unavailable. Normal graph-first commands compile from
`zero.graph`; they do not silently choose edited text because it happened to be
present.

That separation prevents the worst ambiguity: an agent thinking it changed the
program by editing text while the compiler is actually using the graph.

## Human Escape Hatch

The escape hatch is deliberate. A project should remain reconstructable from
text projections. A human can edit `src/main.0`, reconcile it back into the
graph, and confirm the projection still matches:

```json-render
{
  "type": "flow",
  "title": "Human escape hatch: edit text, reconcile to the graph",
  "nodes": [
    { "id": "n1", "label": "human reviews src/main.0", "x": 0, "y": 0, "tone": "human" },
    { "id": "n2", "label": "human edits src/main.0", "x": 0, "y": 96, "tone": "human" },
    { "id": "n3", "label": "zero import", "x": 0, "y": 192, "tone": "compiler" },
    { "id": "n4", "label": "zero check", "x": 0, "y": 288, "tone": "compiler" },
    { "id": "n5", "label": "zero export", "x": 0, "y": 384, "tone": "graph" },
    { "id": "n6", "label": "zero verify-projection", "x": 0, "y": 480, "tone": "compiler" }
  ],
  "edges": [
    { "source": "n1", "target": "n2" },
    { "source": "n2", "target": "n3" },
    { "source": "n3", "target": "n4" },
    { "source": "n4", "target": "n5" },
    { "source": "n5", "target": "n6" }
  ]
}
```

This keeps the system auditable. The graph is first-class, but humans are not
locked out of a readable format.

## Review Rule

Use graph commands for authoring. Use projections for review. Import
projections only when a human intentionally edits them.
