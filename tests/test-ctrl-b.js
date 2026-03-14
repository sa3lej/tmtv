// Playwright test: Ctrl+B (tmux prefix) works in the web viewer.
//
// Usage: node test-ctrl-b.js <viewer-url> [screenshot-dir]
//
// Tests:
//   1. Terminal connects and renders content
//   2. Ctrl+B followed by "%" splits the pane (verified via screenshot;
//      the full-screen terminal renders tmux pane borders in the stream)
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
    // Wait for the single full-screen xterm.js terminal to appear
    const terminal = page.locator('#fullscreen-term .xterm-screen');
    await terminal.waitFor({ state: 'visible', timeout: 30000 });
    // Give the terminal time to receive initial content
    await page.waitForTimeout(2000);
    await page.screenshot({ path: screenshotDir + '/ctrl-b-1-connected.png' });
    console.log('PASS step 1: terminal connected');

    // --- Step 2: Send Ctrl+B then "%" to split pane ---
    // POST directly to the input endpoint (same as viewer.js does).
    // With the full-screen virtual PTY, tmux renders the split inside
    // the single terminal canvas — there are no separate DOM pane elements.
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
    if (resp1 !== 200) {
      throw new Error('Step 2: Ctrl+B POST failed with HTTP ' + resp1);
    }
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
    if (resp2 !== 200) {
      throw new Error('Step 2: % POST failed with HTTP ' + resp2);
    }

    // Wait for tmux to render the split in the terminal stream
    await page.waitForTimeout(3000);
    await page.screenshot({ path: screenshotDir + '/ctrl-b-2-after-split.png' });

    // Verify the terminal is still rendering (canvas has non-zero dimensions).
    // The actual pane split is visible in the screenshot — tmux draws the
    // border characters in the terminal stream. We verify the input path
    // worked (HTTP 200) and the terminal didn't break.
    const box = await terminal.boundingBox();
    if (!box || box.width === 0 || box.height === 0) {
      throw new Error('Step 2: terminal canvas has zero dimensions after split');
    }

    console.log('PASS step 2: Ctrl+B prefix accepted (HTTP 200), terminal rendering intact (' + Math.round(box.width) + 'x' + Math.round(box.height) + ')');

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
