// Playwright test: full interactive password prompt flow.
//
// Usage: node test-password-prompt.js <url> <password> <screenshot-dir>
//
// Tests:
//   1. Password overlay appears with correct title and form elements
//   2. Wrong password → "Wrong password" error message appears
//   3. Correct password → overlay hides, terminal connects
//
// Exits 0 if all checks pass. Exits 1 on failure (prints reason to stderr).

const { chromium } = require('playwright');

const url = process.argv[2];
const password = process.argv[3];
const screenshotDir = process.argv[4] || '/tmp';

if (!url || !password) {
  console.error('Usage: node test-password-prompt.js <url> <password> <screenshot-dir>');
  process.exit(1);
}

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage();

  try {
    await page.goto(url, { waitUntil: 'networkidle', timeout: 15000 });

    // --- Step 1: Password prompt appears ---
    const overlay = page.locator('#password-overlay');
    await overlay.waitFor({ state: 'visible', timeout: 10000 });

    // Verify title
    const title = overlay.locator('.error-title');
    const titleText = await title.textContent();
    if (!titleText || !titleText.includes('password-protected')) {
      throw new Error('Step 1: title wrong: ' + titleText);
    }

    // Verify input and button exist
    const input = page.locator('#password-input');
    await input.waitFor({ state: 'visible', timeout: 5000 });
    const button = page.locator('#password-form button[type="submit"]');
    await button.waitFor({ state: 'visible', timeout: 5000 });

    await page.screenshot({ path: screenshotDir + '/pw-1-prompt.png' });
    console.log('PASS step 1: password prompt visible');

    // --- Step 2: Wrong password → error message ---
    await input.fill('definitely-wrong-password');
    await button.click();

    // After wrong password, the overlay should reappear with error
    await overlay.waitFor({ state: 'visible', timeout: 10000 });
    const errorMsg = page.locator('#password-error');
    await errorMsg.waitFor({ state: 'visible', timeout: 10000 });

    const errorText = await errorMsg.textContent();
    if (!errorText || !errorText.toLowerCase().includes('wrong')) {
      throw new Error('Step 2: error message wrong: ' + errorText);
    }

    await page.screenshot({ path: screenshotDir + '/pw-2-wrong.png' });
    console.log('PASS step 2: wrong password shows error');

    // --- Step 3: Correct password → terminal connects ---
    const input2 = page.locator('#password-input');
    await input2.fill(password);
    await button.click();

    // Overlay should become hidden
    await overlay.waitFor({ state: 'hidden', timeout: 15000 });

    // Full-screen terminal should render — wait for any .xterm-screen element
    // (#fullscreen-term is created dynamically when the first vpty data arrives,
    // which can take several seconds on a slow staging server)
    const terminal = page.locator('.xterm-screen');
    await terminal.first().waitFor({ state: 'visible', timeout: 30000 });

    await page.screenshot({ path: screenshotDir + '/pw-3-connected.png' });
    console.log('PASS step 3: correct password connects to terminal');

    console.log('PASS: all password prompt tests passed');
    await browser.close();
    process.exit(0);
  } catch (err) {
    try {
      await page.screenshot({ path: screenshotDir + '/pw-fail.png' });
    } catch (_) {}

    console.error('FAIL: ' + err.message);
    await browser.close();
    process.exit(1);
  }
})();
