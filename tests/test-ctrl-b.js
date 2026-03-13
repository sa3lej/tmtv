// Playwright test: Ctrl+B (tmux prefix) works in the web viewer.
//
// Usage: node test-ctrl-b.js <viewer-url> [screenshot-dir]
//
// Tests:
//   1. Terminal connects and renders content
//   2. Ctrl+B followed by "%" splits the pane (verified via DOM pane count)
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

// Extract session name from URL: /s/<name> or /j/<name>
const sessionMatch = url.match(/\/[sj]\/([^/?#]+)/);
if (!sessionMatch) {
  console.error('Cannot extract session name from URL: ' + url);
  process.exit(1);
}
const sessionName = sessionMatch[1];
// Build input URL: same origin, /ws/<name>/input (Caddy proxies to SSE port)
const origin = new URL(url).origin;
const inputUrl = origin + '/ws/' + sessionName + '/input';

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage();

  try {
    // Use 'domcontentloaded' — SSE connections are persistent, so
    // 'networkidle' would timeout waiting for the stream to close.
    await page.goto(url, { waitUntil: 'domcontentloaded', timeout: 30000 });

    // --- Step 1: Terminal connects and renders ---
    const terminal = page.locator('.xterm-screen');
    await terminal.first().waitFor({ state: 'visible', timeout: 30000 });
    await page.screenshot({ path: screenshotDir + '/ctrl-b-1-connected.png' });
    console.log('PASS step 1: terminal connected');

    // Count xterm instances (each pane gets one .xterm container)
    const panesBefore = await page.locator('.xterm').count();
    console.log('  panes before: ' + panesBefore);

    // --- Step 2: Send Ctrl+B then "%" to split pane ---
    // POST directly to the input endpoint (same as viewer.js does).
    // Playwright keyboard simulation doesn't reliably trigger xterm.js
    // onData in headless mode, so we use fetch() within the page context.
    console.log('  input URL: ' + inputUrl);

    // Send Ctrl+B (0x02)
    const resp1 = await page.evaluate(async (u) => {
      const r = await fetch(u, {
        method: 'POST', body: '\x02',
        headers: { 'Content-Type': 'text/plain', 'X-Tmtv-Input': '1' }
      });
      return r.status;
    }, inputUrl);
    console.log('  POST ctrl+b: HTTP ' + resp1);
    await page.waitForTimeout(500);

    // Send % to trigger vertical split
    const resp2 = await page.evaluate(async (u) => {
      const r = await fetch(u, {
        method: 'POST', body: '%',
        headers: { 'Content-Type': 'text/plain', 'X-Tmtv-Input': '1' }
      });
      return r.status;
    }, inputUrl);
    console.log('  POST %: HTTP ' + resp2);

    // Wait for pane layout update via SSE
    await page.waitForTimeout(3000);
    await page.screenshot({ path: screenshotDir + '/ctrl-b-2-after-split.png' });

    const panesAfter = await page.locator('.xterm').count();
    console.log('  panes after: ' + panesAfter);

    if (panesAfter > panesBefore) {
      console.log('PASS step 2: Ctrl+B prefix split the pane (' + panesBefore + ' -> ' + panesAfter + ')');
    } else {
      throw new Error('Step 2: pane count did not increase (' + panesBefore + ' -> ' + panesAfter + ')');
    }

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
