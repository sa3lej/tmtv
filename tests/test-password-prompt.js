// Playwright test: verify password prompt appears for password-protected sessions.
// Usage: node test-password-prompt.js <url> <screenshot-path>
//
// Exits 0 if the password prompt is visible with expected content.
// Exits 1 on failure (prints reason to stderr).

const { chromium } = require('playwright');

const url = process.argv[2];
const screenshotPath = process.argv[3];

if (!url || !screenshotPath) {
  console.error('Usage: node test-password-prompt.js <url> <screenshot-path>');
  process.exit(1);
}

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage();

  try {
    await page.goto(url, { waitUntil: 'networkidle', timeout: 15000 });

    // Wait for the password overlay to become visible
    const overlay = page.locator('#password-overlay');
    await overlay.waitFor({ state: 'visible', timeout: 10000 });

    // Verify the title text is present
    const title = page.locator('.error-title');
    const titleText = await title.textContent();
    if (!titleText || !titleText.includes('password-protected')) {
      throw new Error('Password prompt title missing or wrong: ' + titleText);
    }

    // Verify the password input field exists and is visible
    const input = page.locator('#password-input');
    await input.waitFor({ state: 'visible', timeout: 5000 });

    // Verify the submit button exists
    const button = page.locator('#password-form button, #password-form input[type="submit"]');
    const buttonCount = await button.count();
    if (buttonCount === 0) {
      throw new Error('No submit button found in password form');
    }

    // Take screenshot as evidence
    await page.screenshot({ path: screenshotPath, fullPage: true });

    console.log('PASS: password prompt visible with correct content');
    await browser.close();
    process.exit(0);
  } catch (err) {
    // Take screenshot even on failure for debugging
    try {
      await page.screenshot({ path: screenshotPath, fullPage: true });
    } catch (_) {}

    console.error('FAIL: ' + err.message);
    await browser.close();
    process.exit(1);
  }
})();
