// Build site/ and serve site/dist before running. No production sessions are opened.
// PLAYWRIGHT_MODULE can point to an existing Playwright installation.
import assert from 'node:assert/strict';
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || 'playwright');
const base = process.argv[2] || 'http://127.0.0.1:4187';
const browser = await chromium.launch({ executablePath: process.env.CHROME_BIN || '/usr/bin/google-chrome' });
try {
  for (const width of [320, 390, 680, 768, 1440]) {
    const page = await browser.newPage({ viewport: { width, height: 900 } });
    const errors = []; page.on('pageerror', e => errors.push(e.message));
    for (const route of ['/', '/releases/', '/releases/3-0/']) {
      await page.goto(base + route);
      assert(await page.locator('.nav-join').isVisible(), `join visible at ${width}${route}`);
      assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `overflow at ${width}${route}`);
    }
    assert.deepEqual(errors, []); await page.close();
  }
  const context = await browser.newContext({ permissions: ['clipboard-read', 'clipboard-write'], reducedMotion: 'reduce' });
  const page = await context.newPage();
  await page.clock.install();
  await page.goto(base);
  // No autoplay; user controls every replay, including with reduced motion.
  await page.clock.fastForward(12000);
  assert.equal(await page.locator('[aria-current="step"]').textContent(), '4');
  await page.click('[data-demo-play]');
  assert.equal(await page.locator('[aria-current="step"]').textContent(), '1');
  await page.clock.fastForward(3000);
  assert.equal(await page.locator('[aria-current="step"]').textContent(), '2');
  await page.click('[data-demo-play]');
  await page.clock.fastForward(6000);
  assert.equal(await page.locator('[aria-current="step"]').textContent(), '2', 'paused');
  await page.click('[data-demo-play]');
  await page.clock.fastForward(3000);
  assert.equal(await page.locator('[data-host-screen]').textContent(), await page.locator('[data-guest-screen]').textContent());
  await page.clock.fastForward(3000);
  assert.match(await page.locator('[data-demo-play]').textContent(), /Replay/);
  await page.locator('[data-demo-step="0"]').focus(); await page.keyboard.press('Enter');
  assert.equal(await page.locator('[aria-current="step"]').textContent(), '1');
  await page.locator('[data-copy]').first().click();
  assert.equal(await page.evaluate(() => navigator.clipboard.readText()), 'curl -fsSL https://tmtv.se/install.sh | sh');
  await page.click('.nav-join');
  assert.equal(new URL(page.url()).hash, '#join');
  await page.fill('#token', 'not a token'); await page.click('#session-form button');
  assert.match(await page.textContent('#join-error'), /without a URL or spaces/);
  await page.route('**/s/demo-test', route => route.fulfill({ status: 200, body: 'viewer route fixture' }));
  await page.fill('#token', 'demo-test'); await page.click('#session-form button');
  await page.waitForURL('**/s/demo-test');
  await context.close();
  const noJS = await browser.newContext({ javaScriptEnabled: false });
  const staticPage = await noJS.newPage(); await staticPage.goto(base);
  assert(await staticPage.locator('[data-host-screen]').isVisible());
  assert(!(await staticPage.locator('.demo-controls').isVisible()));
  await noJS.close();
  console.log('PASS: 15 route/viewport checks, demo play/pause/replay/keyboard, reduced motion, copy/join, no-JS fallback');
} finally { await browser.close(); }
