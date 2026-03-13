// Playwright test: session lobby screen appears when a session ends.
//
// Usage: node test-session-lobby.js <url> <screenshot-dir> [max-wait-seconds]
//
// Tests:
//   1. Session ends -> error overlay shows session-form (token input)
//   2. Session-form input and button are visible and focusable
//   3. Submitting a token navigates to /s/<token>
//
// The test connects to a time-limited session and waits for it to end,
// then verifies the lobby UI elements are present and functional.
//
// For the "unavailable" case, pass a URL with a bogus token.
// Usage: node test-session-lobby.js <url> <screenshot-dir> unavailable
//
// Exits 0 if all checks pass. Exits 1 on failure.

const { chromium } = require('playwright');

const url = process.argv[2];
const screenshotDir = process.argv[3] || '/tmp';
const mode = process.argv[4] || 'ended';
const maxWait = parseInt(process.argv[5] || '60', 10) * 1000;

if (!url) {
  console.error('Usage: node test-session-lobby.js <url> <screenshot-dir> [ended|unavailable] [max-wait-seconds]');
  process.exit(1);
}

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage();

  try {
    await page.goto(url, { waitUntil: 'domcontentloaded', timeout: 15000 });

    if (mode === 'unavailable') {
      // For bad tokens, the error overlay should appear quickly
      const errorOverlay = page.locator('#error-overlay');
      await errorOverlay.waitFor({ state: 'visible', timeout: 20000 });
    } else {
      // Wait for session to end (TTL expiry or host disconnect)
      const errorOverlay = page.locator('#error-overlay');
      await errorOverlay.waitFor({ state: 'visible', timeout: maxWait });

      const titleText = await page.locator('#error-title').textContent();
      if (!titleText || !titleText.includes('Session ended')) {
        throw new Error('Step 1: expected "Session ended" title, got: ' + titleText);
      }
    }

    // --- Step 1: session-form is visible ---
    const sessionForm = page.locator('#session-form');
    await sessionForm.waitFor({ state: 'visible', timeout: 5000 });
    console.log('PASS step 1: session-form is visible on error overlay');

    // --- Step 2: input and button are visible ---
    const sessionInput = page.locator('#session-input');
    const sessionSubmit = page.locator('.session-submit');

    await sessionInput.waitFor({ state: 'visible', timeout: 3000 });
    await sessionSubmit.waitFor({ state: 'visible', timeout: 3000 });

    // Verify aria-label for accessibility
    const ariaLabel = await sessionInput.getAttribute('aria-label');
    if (!ariaLabel || ariaLabel.toLowerCase().indexOf('session') === -1) {
      throw new Error('Step 2: session-input missing aria-label, got: ' + ariaLabel);
    }
    console.log('PASS step 2: session input and button are visible with aria-label');

    await page.screenshot({ path: screenshotDir + '/lobby-1-form-visible.png' });

    // --- Step 3: submitting a token navigates ---
    await sessionInput.fill('test-lobby-token');
    await sessionSubmit.click();

    // Wait for navigation
    await page.waitForURL('**/s/test-lobby-token', { timeout: 5000 });
    console.log('PASS step 3: form submission navigates to /s/<token>');

    await page.screenshot({ path: screenshotDir + '/lobby-2-navigated.png' });

    console.log('PASS: all session lobby tests passed');
    await browser.close();
    process.exit(0);
  } catch (err) {
    try {
      await page.screenshot({ path: screenshotDir + '/lobby-fail.png' });
    } catch (_) {}

    console.error('FAIL: ' + err.message);
    await browser.close();
    process.exit(1);
  }
})();
