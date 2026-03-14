// Playwright test: SSH <-> Web viewer data flow.
//
// Usage: node test-ssh-web.js <viewer-url> [screenshot-dir]
//
// Tests:
//   1. Terminal connects and renders content (not blank)
//   2. Text typed in SSH appears on the web viewer
//
// Requires a live tmtv session at <viewer-url>.
// The caller must type a unique marker into the SSH session BEFORE
// launching this test (or pass it via TMTV_MARKER env).
// Exits 0 if all checks pass. Exits 1 on failure.

const { chromium } = require('playwright');

const url = process.argv[2];
const screenshotDir = process.argv[3] || '/tmp';
const marker = process.env.TMTV_MARKER || 'TMTV_SMOKE_TEST';

if (!url) {
  console.error('Usage: node test-ssh-web.js <viewer-url> [screenshot-dir]');
  process.exit(1);
}

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage();

  try {
    // Navigate to the viewer
    await page.goto(url, { waitUntil: 'domcontentloaded', timeout: 30000 });

    // --- Step 1: Terminal renders (not blank) ---
    // Wait for the xterm.js terminal canvas to appear.
    // If the vpty is broken, #fullscreen-term is never created.
    const terminal = page.locator('.xterm-screen');
    await terminal.first().waitFor({ state: 'visible', timeout: 30000 });

    // Give the terminal time to receive initial screen dump
    await page.waitForTimeout(3000);
    await page.screenshot({ path: screenshotDir + '/ssh-web-1-connected.png' });

    // Check the terminal canvas has non-zero dimensions
    const box = await terminal.boundingBox();
    if (!box || box.width === 0 || box.height === 0) {
      throw new Error('Step 1: terminal canvas has zero dimensions — blank screen');
    }
    console.log('PASS step 1: terminal visible (' + Math.round(box.width) + 'x' + Math.round(box.height) + ')');

    // --- Step 2: SSH content appears on web ---
    // The caller typed `marker` into the SSH session before running
    // this test. Check that the marker text is rendered in the
    // terminal's DOM (xterm.js creates rows of text spans).

    // xterm.js renders text in .xterm-rows which contains row divs
    // with spans. We can read the full text content.
    const termText = await page.evaluate(() => {
      const rows = document.querySelector('.xterm-rows');
      return rows ? rows.textContent : '';
    });

    await page.screenshot({ path: screenshotDir + '/ssh-web-2-content.png' });

    if (!termText || termText.trim().length === 0) {
      throw new Error('Step 2: terminal has no text content — web viewer is blank');
    }

    if (termText.indexOf(marker) === -1) {
      // Log what we DO see for debugging
      const preview = termText.replace(/\s+/g, ' ').substring(0, 200);
      throw new Error('Step 2: marker "' + marker + '" not found in terminal. ' +
                       'Content preview: ' + preview);
    }

    console.log('PASS step 2: marker "' + marker + '" found in web viewer');

    console.log('PASS: SSH -> Web data flow verified');
    await browser.close();
    process.exit(0);
  } catch (err) {
    try {
      await page.screenshot({ path: screenshotDir + '/ssh-web-fail.png' });
    } catch (_) {}

    console.error('FAIL: ' + err.message);
    await browser.close();
    process.exit(1);
  }
})();
