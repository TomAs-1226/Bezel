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
// The 2.x line first, as Console does it: `upgrade/alpha-7` is the line that ships, and the two 2.x
// branches were merged into it on 2026-09-25, so it now carries the canonical identity. alpha6 stays
// on the list because a machine may still have only that worktree checked out, and `systemcore` is
// kept for a checkout made before the 2026-09-19 consolidation retired that branch name.
//
// `FrcCatalyst-v1.1.0` is here because the main checkout was renamed to it, and a bare `FrcCatalyst`
// no longer exists on this machine. Without it, a machine with only the main checkout finds nothing.
const dev = join(homedir(), "dev");
const names = [
  join("_worktrees", "FrcCatalyst-alpha7"),
  join("_worktrees", "FrcCatalyst-alpha6"),
  join("_worktrees", "FrcCatalyst-systemcore"),
  "FrcCatalyst",
  "FrcCatalyst-v1.1.0",
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
  // Exiting 0 here is the honest answer on a machine that has no library checkout — a contributor with
  // only this repo cannot be asked to clone another one to run the tests. But it used to be the ONLY
  // answer, which made this check fail open: rename or remove the worktree it resolves against and
  // `npm test` and `npm run build` go on passing forever without ever comparing anything, silently.
  //
  // So it is loud about it, and CI is told to treat it as a failure: identity drift is exactly the kind
  // of thing that is invisible until someone notices the app looks subtly wrong.
  const message = "check-identity: no FrcCatalyst checkout found, so NOTHING WAS CHECKED";
  console.log(message);
  console.log("  looked in:", candidates.join(", "));
  console.log("  set CATALYST_IDENTITY_DIR to a docs/assets folder to check against a specific one.");
  if (process.env.CI || args.includes("--require-source")) {
    console.error("check-identity: refusing to pass without a source (CI, or --require-source)");
    process.exit(2);
  }
  process.exit(0);
}
console.log(`check-identity: comparing against ${source}`);

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
