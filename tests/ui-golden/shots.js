/**
 * tests/ui-golden/shots.js — where screenshots go.
 * Routine runs write to Playwright's per-test output folder (test-results/, git-ignored) so a test run never
 * changes tracked files. The curated set in aurora-glass/screenshots/ is regenerated deliberately:
 *   AURORA_SHOTS=1 npx playwright test --workers=1 <spec>
 * (not a test file: the name does not match *.test.js, so Playwright does not run it).
 */
const path = require("path");

function shotPath(testInfo, name) {
  return process.env.AURORA_SHOTS === "1" ? path.join("aurora-glass", "screenshots", name) : testInfo.outputPath(name);
}

module.exports = { shotPath };
