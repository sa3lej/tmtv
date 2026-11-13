// Browser checks for the public website. Build site/ and serve site/dist first, e.g.
//   npm run build --prefix site && npx --prefix site astro preview --port 4187
//   node tests/website.mjs http://127.0.0.1:4187
// PLAYWRIGHT_MODULE can point to an existing Playwright installation. No production
// sessions are opened: /s/ and /j/ navigations are only checked by URL.
import assert from 'node:assert/strict';
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || 'playwright');
const base = (process.argv[2] || 'http://127.0.0.1:4187').replace(/\/$/, '');
const routes = ['/', '/releases/', '/releases/3-0/'];
const browser = await chromium.launch(process.env.CHROME_BIN ? { executablePath: process.env.CHROME_BIN } : {});
try {
  for (const viewport of [
    { width: 320, height: 640 }, { width: 375, height: 812 }, { width: 812, height: 375 },
    { width: 1024, height: 768 }, { width: 1440, height: 900 },
  ]) {
    const page = await browser.newPage({ viewport });
    const errors = [];
    const external = [];
    page.on('pageerror', (e) => errors.push(e.message));
    page.on('console', (m) => m.type() === 'error' && errors.push(m.text()));
    page.on('request', (r) => !r.url().startsWith(base) && external.push(r.url()));
    for (const route of routes) {
      await page.goto(base + route);
      const where = `${viewport.width}x${viewport.height}${route}`;
      assert.equal(await page.locator('h1').count(), 1, `one h1 at ${where}`);
      assert(await page.locator('header nav a[href="/#join"]').isVisible(), `join link visible at ${where}`);
      assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `page overflow at ${where}`);
      const small = await page.$$eval('header a, main button, main input', (els) =>
        els.filter((e) => e.getBoundingClientRect().height < 44).map((e) => e.outerHTML.slice(0, 60)));
      assert.deepEqual(small, [], `tap targets under 44px at ${where}`);
      for (const selector of ['title', 'meta[name="description"]', 'link[rel="canonical"]', 'meta[property="og:image"]']) {
        assert.equal(await page.locator(`head ${selector}`).count(), 1, `${selector} at ${where}`);
      }
      for (const json of await page.$$eval('script[type="application/ld+json"]', (s) => s.map((x) => x.textContent))) {
        assert.equal(JSON.parse(json)['@context'], 'https://schema.org', `JSON-LD at ${where}`);
      }
    }
    assert.deepEqual(errors, [], `console errors at ${viewport.width}`);
    assert.deepEqual(external, [], `third-party requests at ${viewport.width}`);
    await page.close();
  }

  const context = await browser.newContext({ permissions: ['clipboard-read', 'clipboard-write'] });
  const page = await context.newPage();
  await page.route(/\/(s|j)\//, (route) => route.fulfill({ status: 200, body: 'viewer stub' }));
  await page.goto(base + '/');
  await page.click('.copy >> nth=0');
  assert.equal(await page.evaluate(() => navigator.clipboard.readText()), 'curl -fsSL https://tmtv.se/install.sh | sh');
  assert.match(await page.textContent('.copy-status'), /Copied/);
  assert(await page.evaluate(async () => {
    await document.fonts.ready;
    return [...document.fonts].some((f) => f.family.includes('IBM Plex Sans') && f.status === 'loaded');
  }), 'self-hosted IBM Plex Sans loads');

  const origin = new URL(base).origin;
  for (const [input, path] of [
    ['demo', '/s/demo'], [`${origin}/s/abc123`, '/s/abc123'], [`${origin}/j/demo`, '/j/demo'],
    ['ssh abc@tmtv.se', '/s/abc'], ['ssh ro-demo@tmtv.se', '/s/ro-demo'], ['  xyz-1  ', '/s/xyz-1'],
  ]) {
    await page.goto(base + '/');
    await page.fill('#token', input);
    await Promise.all([page.waitForURL((u) => u.pathname === path), page.click('#session-form button')]);
  }
  for (const [input, message] of [['', /Paste/], ['hello world', /does not look/], ['https://share.example.com/s/abc', /another relay/]]) {
    await page.goto(base + '/');
    await page.fill('#token', input);
    await page.click('#session-form button');
    assert.match(await page.textContent('#join-error'), message, `error for "${input}"`);
    assert.equal(await page.getAttribute('#token', 'aria-invalid'), 'true');
    assert.equal(await page.evaluate(() => document.activeElement.id), 'token');
    await page.type('#token', 'x');
    assert.equal(await page.textContent('#join-error'), '');
    assert.equal(await page.getAttribute('#token', 'aria-invalid'), null);
  }
  console.log('website checks passed');
} finally {
  await browser.close();
}
