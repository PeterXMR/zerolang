# Changelog

## 0.1.4

<!-- release:start -->

- Adopts canonical `.0` text as the current Zero source surface across parsing, import resolution, package manifests, command workflows, docs, fixtures, formatting, and artifact contracts.
- Rebuilds checker internals around TypeCore, binder-aware unification, generic inference, static interface validation, provenance substitution, and shared call-resolution facts.
- Hardens direct emitters and native artifacts through MIR verifier contracts, direct specialization metadata, target buildability checks, and shared x64, AArch64, ELF, Mach-O, and COFF emission helpers.
- Adds clearer direct backend selection, target readiness, and buildability reporting so command JSON and failed native builds expose deterministic blocker facts.
- Removes stale wasm, compiler-zero, and superseded parser surfaces while refreshing public docs, README guidance, and zerolang branding around the current system.
- Strengthens compiler structure guardrails, metrics budgets, TypeScript-based repo scripts, docs build infrastructure, CI workspace commands, and eval harness isolation.
- Renames bundled version-matched skills to the current flat skill names while keeping `zero skills` discovery focused on the native compiler.

### Contributors

- @ctate

<!-- release:end -->

## 0.1.3

- Adds hosted HTTP client runtime support, including runtime packaging fixes, JSON byte helpers for wasm, HTTP final-header parsing fixes, and direct array span lowering repairs.
- Parses `use` declarations into syntax graph facts and reports import diagnostics, fix plans, and graph edges against the specific import source ranges.
- Adds structured backend blocker and target-readiness facts to command JSON so checks, build previews, and direct backend failures explain unsupported targets more deterministically.
- Expands agent-facing diagnostics with BOR001 borrow trace facts, target-neutral CGEN004 wording, and NAM004 generic type-name shadowing rejection.
- Hardens native compiler allocation paths across shared buffers, parser, checker, IR lowering, emitters, source handling, targets, and driver state for deterministic allocation failures.
- Updates the public site and docs with current guidance, docs chat rate limiting, and pnpm-based repository workflows.
- Restores native versioned skill guidance, keeps the repo wrapper on the native compiler, and removes the legacy `zero skills path` command.

### Contributors

- @ctate
- @PeterXMR
- @h4ckf0r0day

## 0.1.2

- Rebuilds borrow provenance tracking across references, fields, subpaths, assignments, control-flow joins, receiver side effects, generic methods, return summaries, and unreachable paths.
- Tightens borrow conflict checking by deriving conflicts from provenance and comparing concrete places, with additional conformance coverage for provenance joins and edge cases.
- Fixes checker regressions around borrow origins, reassignment, aggregate values, explicit reference fields, unknown identifiers, and fallibility propagation through wrappers.
- Fixes dynamic CLI strings and Darwin executable UUID emission.
- Adds Apache-2.0 licensing and lockfile license metadata.
- Updates the public site with an install toggle, GitHub star count, and a mobile header Docs link.

### Contributors

- @ctate
- @badlogic
- @chenrui333
- @heylakatos
- @onevcat

## 0.1.1

- Adds the public installer at `https://zerolang.ai/install.sh`, with platform selection, GitHub release downloads, checksum verification, and `$HOME/.zero/bin/zero` installation.
- Adds `zero run` for the everyday edit loop: build a host executable, run it, pass program arguments after `--`, forward stdout/stderr, and return the program exit status.
- Updates README, homepage, getting started, install, and CLI docs around the curl install path, copyable commands, and `zero run`.
- Reworks public docs to be more scannable and current, including stronger language, diagnostics, testing, target, package, optimization, and standard library references.
- Removes placeholder module docs that described surfaces not ready for users and adds current module docs for `std.crypto`, `std.http`, and `std.net`.
- Adds version-matched agent guidance through `zero skills`, including focused workflows for Zero syntax, diagnostics, builds, packages, standard library use, testing, and agent edit loops.
- Keeps the installable Zero skill as a thin bootstrap so external skill managers discover one Zero skill while the compiler serves the richer guidance for the installed version.
- Updates the `zero skills` CLI contract to serve bundled flat skill data while preserving list, get, path, and JSON workflows.

### Contributors

- @ctate
- @mvanhorn

## 0.1.0

- Initial public release of Zero as the programming language for agents.
- Includes the native compiler, examples, documentation site, and validation fixtures.
- Supported workflows use direct Zero emitters for the documented examples and targets.
