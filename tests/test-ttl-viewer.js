// Playwright test: /j/ short URL and TTL session expiry in the web viewer.
//
// Usage: node test-ttl-viewer.js <url> <screenshot-dir> [max-wait-seconds]
//
// Tests:
//   1. /j/<token> URL loads the viewer and connects to the terminal
//   2. Session expires → "Session ended" overlay appears
//
// The test connects to a time-limited session via /j/ and waits for the
// server to terminate it (TTL expiry). Server checks TTL every 30s, so
// max-wait-seconds defaults to 60 to allow for timer granularity.
//
// Exits 0 if all checks pass. Exits 1 on failure (prints reason to stderr).

const { chromium } = require('playwright');

const url = process.argv[2];
const screenshotDir = process.argv[3] || '/tmp';
const maxWait = parseInt(process.argv[4] || '60', 10) * 1000;

if (!url) {
  console.error('Usage: node test-ttl-viewer.js <url> <screenshot-dir> [max-wait-seconds]');
  process.exit(1);
}

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage();

  try {
    await page.goto(url, { waitUntil: 'domcontentloaded', timeout: 15000 });

    // --- Step 1: /j/ URL connects to the terminal ---
    const terminal = page.locator('#fullscreen-term .xterm-screen');
    await terminal.waitFor({ state: 'visible', timeout: 15000 });

    // Verify status shows connected (not an error state)
    const status = page.locator('#status');
    const statusText = await status.textContent();
    if (!statusText || !statusText.toLowerCase().includes('connected')) {
      throw new Error('Step 1: expected "Connected" status, got: ' + statusText);
    }

    await page.screenshot({ path: screenshotDir + '/ttl-1-connected.png' });
    console.log('PASS step 1: /j/ URL connects to terminal');

    // --- Step 2: Wait for TTL expiry → "Session ended" overlay ---
    const errorOverlay = page.locator('#error-overlay');
    await errorOverlay.waitFor({ state: 'visible', timeout: maxWait });

    const errorTitle = page.locator('#error-title');
    const titleText = await errorTitle.textContent();
    if (!titleText || !titleText.includes('Session ended')) {
      throw new Error('Step 2: expected "Session ended" title, got: ' + titleText);
    }

    const errorMsg = page.locator('#error-msg');
    const msgText = await errorMsg.textContent();
    if (!msgText || !msgText.includes('ended')) {
      throw new Error('Step 2: expected session ended message, got: ' + msgText);
    }

    // Verify status pill also reflects the ended state
    const endStatus = await status.textContent();
    if (!endStatus || !endStatus.toLowerCase().includes('ended')) {
      throw new Error('Step 2: expected "Session ended" in status pill, got: ' + endStatus);
    }

    await page.screenshot({ path: screenshotDir + '/ttl-2-expired.png' });
    console.log('PASS step 2: session expired with "Session ended" overlay');

    console.log('PASS: all TTL viewer tests passed');
    await browser.close();
    process.exit(0);
  } catch (err) {
    try {
      await page.screenshot({ path: screenshotDir + '/ttl-fail.png' });
    } catch (_) {}

    console.error('FAIL: ' + err.message);
    await browser.close();
    process.exit(1);
  }
})();
