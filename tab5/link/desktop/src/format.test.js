import { test } from "node:test";
import assert from "node:assert/strict";
import { ago, clock, duration, freshness, inboxActions, linkState, MOVE, pairCode, sessionState } from "./format.js";

test("durations", () => {
  assert.equal(duration(9), "9s");
  assert.equal(duration(75), "1m 15s");
  assert.equal(duration(3960), "1h 06m");
  assert.equal(duration(null), "—");
});

test("media clock", () => {
  assert.equal(clock(83), "1:23");
  assert.equal(clock(3725), "1:02:05");
  assert.equal(clock(undefined), "–:––");
});

test("relative times", () => {
  const now = Date.parse("2026-09-24T12:00:00Z");
  assert.equal(ago("2026-09-24T11:59:50Z", now), "just now");
  assert.equal(ago("2026-09-24T11:56:00Z", now), "4 min ago");
  assert.equal(ago("2026-09-24T09:00:00Z", now), "3 h ago");
  assert.equal(ago("2026-09-23T12:00:00Z", now), "yesterday");
  assert.equal(ago(null, now), "never");
});

test("pairing codes print as pairing.py prints them", () => {
  assert.equal(pairCode("482913"), "482 913");
  assert.equal(pairCode("12"), "12");
});

test("session and link states have words", () => {
  assert.equal(sessionState("waiting_for_input").dot, "warn");
  assert.equal(linkState({ state: "running", port: 8765 }).detail, "serving on :8765");
  assert.equal(linkState({ state: "setup" }).dot, "warn");
});

test("inbox moves follow the Link's transitions", () => {
  assert.deepEqual(inboxActions("open"), ["claim", "done", "reject"]);
  assert.deepEqual(inboxActions("claimed"), ["release", "done", "reject"]);
  assert.deepEqual(inboxActions("done"), []);
  assert.equal(MOVE.release, "open");
});

test("freshness", () => {
  const now = Date.parse("2026-09-24T12:00:00Z");
  assert.equal(freshness("2026-09-24T11:59:50Z", now), "ok");
  assert.equal(freshness("2026-09-24T11:52:00Z", now), "info");
  assert.equal(freshness("2026-09-20T11:50:00Z", now), "");
});
