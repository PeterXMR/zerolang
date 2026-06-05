#!/usr/bin/env -S node --experimental-strip-types --disable-warning=ExperimentalWarning
import { execFileSync } from "node:child_process";
import { existsSync, lstatSync, readFileSync, readdirSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";

const skippedDirs = new Set([
  ".git",
  ".next",
  ".turbo",
  ".zero",
  "dist",
  "node_modules",
  "target",
  "zig-cache",
  "zig-out",
]);

function usage() {
  console.error("usage: repository-graph-verify-sync [--root <path>]");
}

let root = process.cwd();
for (let i = 2; i < process.argv.length; i++) {
  const arg = process.argv[i];
  if (arg === "--root") {
    const value = process.argv[++i];
    if (!value) {
      usage();
      process.exit(2);
    }
    root = value;
  } else if (arg === "--help" || arg === "-h") {
    usage();
    process.exit(0);
  } else {
    usage();
    process.exit(2);
  }
}

const repoRoot = process.cwd();
const zeroBin = resolve(repoRoot, "bin/zero");
const absoluteRoot = resolve(root);

if (!existsSync(zeroBin)) {
  console.error("repository graph verify-sync requires bin/zero from the repository root");
  process.exit(1);
}

if (!existsSync(absoluteRoot)) {
  console.error(`repository graph verify-sync root does not exist: ${absoluteRoot}`);
  process.exit(1);
}

function findStores(path: string, stores: string[]) {
  const stat = lstatSync(path);
  if (stat.isSymbolicLink()) return;
  if (stat.isFile()) {
    if (path.endsWith("/zero.graph") || path === "zero.graph") stores.push(path);
    return;
  }
  if (!stat.isDirectory()) return;

  for (const entry of readdirSync(path, { withFileTypes: true })) {
    if (entry.isDirectory() && skippedDirs.has(entry.name)) continue;
    const child = join(path, entry.name);
    if (entry.isSymbolicLink()) continue;
    if (entry.isFile() && entry.name === "zero.graph") stores.push(child);
    else if (entry.isDirectory()) findStores(child, stores);
  }
}

const stores: string[] = [];
findStores(absoluteRoot, stores);
stores.sort();

if (stores.length === 0) {
  console.log("repository graph verify-sync ok (0 stores)");
  process.exit(0);
}

let failures = 0;
for (const store of stores) {
  const input = sourceInputForStore(store);
  const display = relative(repoRoot, input) || ".";
  console.log(`repository graph verify-sync: ${display}`);
  try {
    execFileSync(zeroBin, ["graph", "verify-sync", input], { stdio: "inherit" });
  } catch {
    failures++;
    console.error(`repository graph verify-sync failed: bin/zero graph verify-sync ${display}`);
  }
}

if (failures > 0) {
  console.error(`repository graph verify-sync failed (${failures}/${stores.length} stores)`);
  process.exit(1);
}

console.log(`repository graph verify-sync ok (${stores.length} ${stores.length === 1 ? "store" : "stores"})`);

function sourceInputForStore(store: string) {
  const root = dirname(store);
  if (existsSync(join(root, "zero.json"))) return root;

  const text = readFileSync(store, "utf8");
  const projection = text.match(/^projection path:"((?:\\.|[^"])*)" text:/m)?.[1];
  if (projection) {
    const path = JSON.parse(`"${projection}"`);
    const source = resolve(root, path);
    if (existsSync(source)) return source;
  }
  return root;
}
