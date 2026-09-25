// The Catalyst identity is copied, not imported, so it can drift. FrcCatalyst's docs/assets hold the
// originals (identity.css, motion.js); Catalyst Console and CatalystApp carry copies, and so does this
// app. This is the same check they run: `npm test` and `npm run build` (and `tauri build`, through
// beforeBuildCommand) fail when a copy differs from the library's.
//
//   node scripts/check-identity.mjs            # report drift, exit 1 if there is any
//   node scripts/check-identity.mjs --write    # copy the library's originals over ours
//   node scripts/check-identity.mjs path/to/docs/assets
//   CATALYST_IDENTITY_DIR=path/to/docs/assets node scripts/check-identity.mjs
//
// With no FrcCatalyst checkout on the machine it says so and exits 0: someone without the library
// should still be able to build, and a check nobody can act on gets ignored.

import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { homedir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");

const args = process.argv.slice(2);
const write = args.includes("--write");
const given = args.find((a) => !a.startsWith("--")) || process.env.CATALYST_IDENTITY_DIR;

// This app lives three levels into the Bezel repo (tab5/link/desktop), in a checkout under ~/dev or a
// worktree under ~/dev/_worktrees; the library is found beside either. The 2.0 line first, as Console.
const dev = join(homedir(), "dev");
const names = [
  join("_worktrees", "FrcCatalyst-systemcore"),
  join("_worktrees", "FrcCatalyst-alpha6"),
  join("_worktrees", "FrcCatalyst-alpha7"),
  "FrcCatalyst",
];
const bases = [dev, resolve(root, "../../../.."), resolve(root, "../../../../..")];
const candidates = given
  ? [resolve(given)]
  : [...new Set(bases.flatMap((b) => names.map((n) => join(b, n, "docs", "assets"))))];

const FILES = [
  ["identity.css", join("src", "styles", "identity.css")],
  ["motion.js", join("src", "motion.js")],
];

const source = candidates.find((p) => FILES.every(([name]) => existsSync(join(p, name))));
if (!source) {
  console.log("check-identity: no FrcCatalyst checkout found, leaving the identity alone");
  console.log("  looked in:", candidates.join(", "));
  process.exit(0);
}

let drifted = 0;
for (const [name, rel] of FILES) {
  const from = join(source, name);
  const to = join(root, rel);
  // Line endings aside: a Windows checkout with core.autocrlf hands out CRLF for the same bytes git holds.
  const theirs = readFileSync(from, "utf8").replace(/\r\n/g, "\n");
  const ours = existsSync(to) ? readFileSync(to, "utf8").replace(/\r\n/g, "\n") : null;
  if (ours === theirs) continue;
  drifted++;
  if (write) {
    writeFileSync(to, theirs); // LF, as the repo keeps it
    console.log(`check-identity: copied ${name} from ${source}`);
  } else {
    console.error(`check-identity: ${rel} differs from ${from}`);
  }
}

if (!drifted) {
  console.log(`check-identity: identity matches ${source}`);
} else if (!write) {
  console.error("Run `npm run identity` to copy the library's originals over these.");
  process.exit(1);
}
