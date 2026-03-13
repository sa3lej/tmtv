// Playwright test: Ctrl+B (tmux prefix) works in the web viewer.
//
// Usage: node test-ctrl-b.js <viewer-url> [screenshot-dir]
//
// Tests:
//   1. Terminal connects and renders content
//   2. Ctrl+B followed by "%" splits the pane (verified via DOM pane count)
//   3. Ctrl+B is not intercepted by browser (preventDefault works)
//
// Requires a live tmtv session with web input enabled (no password).
// Exits 0 if all checks pass. Exits 1 on failure.

const { chromium } = require('playwright');

const url = process.argv[2];
const screenshotDir = process.argv[3] || '/tmp';

if (!url) {
  console.error('Usage: node test-ctrl-b.js <viewer-url> [screenshot-dir]');
  process.exit(1);
}

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage();

  try {
    // Use 'domcontentloaded' — SSE connections are persistent, so
    // 'networkidle' would timeout waiting for the stream to close.
    await page.goto(url, { waitUntil: 'domcontentloaded', timeout: 30000 });

    // --- Step 1: Terminal connects and renders ---
    const terminal = page.locator('.xterm-screen, #terminal-wrap canvas');
    await terminal.first().waitFor({ state: 'visible', timeout: 30000 });
    await page.screenshot({ path: screenshotDir + '/ctrl-b-1-connected.png' });
    console.log('PASS step 1: terminal connected');

    // Count xterm instances (each pane gets one .xterm container)
    const panesBefore = await page.locator('.xterm').count();
    console.log('  panes before: ' + panesBefore);

    // --- Step 2: Send Ctrl+B then "%" to split pane ---
    // Click the terminal to ensure it has focus
    await terminal.first().click();
    await page.waitForTimeout(500);

    // Send Ctrl+B (tmux prefix key)
    await page.keyboard.down('Control');
    await page.keyboard.press('b');
    await page.keyboard.up('Control');
    await page.waitForTimeout(500);

    // Send "%" to trigger vertical split
    await page.keyboard.press('Shift+5'); // % = Shift+5
    await page.waitForTimeout(2000);

    await page.screenshot({ path: screenshotDir + '/ctrl-b-2-after-split.png' });

    const panesAfter = await page.locator('.xterm').count();
    console.log('  panes after: ' + panesAfter);

    if (panesAfter > panesBefore) {
      console.log('PASS step 2: Ctrl+B prefix split the pane (' + panesBefore + ' -> ' + panesAfter + ')');
    } else {
      throw new Error('Step 2: pane count did not increase (' + panesBefore + ' -> ' + panesAfter + ')');
    }

    // --- Step 3: Verify Ctrl+B was not intercepted by browser ---
    // If the browser intercepted Ctrl+B, the split would not have happened.
    // The fact that step 2 passed means preventDefault() worked correctly.
    console.log('PASS step 3: Ctrl+B not intercepted by browser (split succeeded)');

    console.log('PASS: all Ctrl+B tests passed');
    await browser.close();
    process.exit(0);
  } catch (err) {
    try {
      await page.screenshot({ path: screenshotDir + '/ctrl-b-fail.png' });
    } catch (_) {}

    console.error('FAIL: ' + err.message);
    await browser.close();
    process.exit(1);
  }
})();
