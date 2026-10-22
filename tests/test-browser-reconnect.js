// Actual production viewer + host/relay/SSE, inside the offline E2E container.
// Launched by run-relay-e2e.py --browser; no live service or credentials used.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const { chromium } = require('playwright-core');
const root = '/tmp/e2e/browser';
fs.mkdirSync(root, { recursive: true });
const [rw, ro] = process.argv.slice(2);
const origin = 'http://127.0.0.1:8080';
const assets = JSON.parse(fs.readFileSync('/test/web/assets.json'));
const reset = '\x1b[0m\x1b[2J\x1b[H';
const link = 'https://example.invalid/browser-link';
const red = '\x1bP0;1q"1;1;12;12#0;2;100;0;0!12~-!12~\x1b\\';
const green = red.replace('100;0;0', '0;100;0');
let passes = 0;
function pass(label) { console.log('PASS browser: ' + label); passes++; }
async function main() {
  const browser = await chromium.launch({ executablePath: '/test/chromium/chrome',
    args: ['--no-sandbox', '--disable-dev-shm-usage'] });
  const context = await browser.newContext({ viewport: { width: 1440, height: 1000 } });
  const errors = [];
  context.on('requestfailed', req => fs.appendFileSync(root + '/requests.log', req.url() + ': ' + req.failure()?.errorText + '\n'));
  await context.route('**/*', async route => {
    const url = route.request().url();
    if (assets[url]) return route.fulfill({ path: '/test/web/' + assets[url] });
    if (url.startsWith(origin + '/')) return route.continue();
    if (url === link) return route.fulfill({ body: 'Local hyperlink test destination' });
    return route.abort(); // external fonts and any unpinned/unexpected resources
  });
  await context.addInitScript(() => {
    // Passive observation of the real terminal, never substitute its parser,
    // EventSource or production drawing paths. DOM/pixels are asserted below.
    // Reproduce the pinned image addon's async decoder race deterministically.
    // This delays real WASM initialization, never replaces the decoder/parser.
    const Source = window.EventSource;
    window.EventSource = new Proxy(Source, { construct(target, args) {
      const source = Reflect.construct(target, args);
      source.addEventListener('message', event => {
        if (event.data !== 'heartbeat') window.__testLastFrameAt = performance.now();
      });
      return source;
    } });
    const instantiate = WebAssembly.instantiate.bind(WebAssembly);
    WebAssembly.instantiate = (...args) => new Promise(resolve => setTimeout(resolve, 500))
      .then(() => instantiate(...args));
    let ctor;
    Object.defineProperty(window, 'Terminal', {
      configurable: true,
      get: () => ctor,
      set: value => { ctor = new Proxy(value, {
        construct(target, args) {
          const instance = Reflect.construct(target, args);
          window.__testTerminal = instance;
          return instance;
        }
      }); }
    });
  });
  const page = await context.newPage();
  page.on('pageerror', e => errors.push(e.message));
  page.on('console', msg => fs.appendFileSync(root + '/console.log', msg.type() + ': ' + msg.text() + '\n'));
  page.on('dialog', dialog => dialog.accept());
  async function control(action, value = {}) {
    const response = await context.request.post(origin + '/_test/' + action, { data: value, timeout: 60000 });
    assert.equal(response.status(), 200, await response.text());
  }
  async function emit(value) { await control('emit', { hex: Buffer.from(value).toString('hex') }); }
  async function text(value, target = page) {
    await target.waitForFunction(v => document.querySelector('.xterm-rows')?.textContent.includes(v), value);
  }
  async function styles() {
    return page.evaluate(() => {
      const result = {};
      for (const span of document.querySelectorAll('.xterm-rows span')) {
        const name = span.textContent.trim();
        if (!name) continue;
        const css = getComputedStyle(span);
        result[name] = { color: css.color, background: css.backgroundColor,
          style: css.fontStyle, weight: css.fontWeight, decoration: css.textDecorationLine,
          decorationStyle: css.textDecorationStyle, decorationColor: css.textDecorationColor,
          visibility: css.visibility };
      }
      return result;
    });
  }
  async function pixels(colour, target = page) {
    await target.waitForFunction(rgb => {
      for (const canvas of document.querySelectorAll('.xterm-screen canvas')) {
        const ctx = canvas.getContext('2d');
        if (!ctx || !canvas.width || !canvas.height) continue;
        const data = ctx.getImageData(0, 0, canvas.width, canvas.height).data;
        for (let i = 0; i < data.length; i += 4)
          if (data[i] === rgb[0] && data[i+1] === rgb[1] && data[i+2] === rgb[2] && data[i+3] === 255) return true;
      }
      return false;
    }, colour);
  }
  try {
    await page.goto(origin + '/s/' + rw);
    await page.locator('.xterm-screen').waitFor({ state: 'visible' });
    await text('#');
    pass('production Astro viewer and all pinned terminal addons load over authenticated SSE');
    const styled = reset + '\x1b[38;2;18;52;86;48;2;101;67;33;1;3;9mRGB_STYLE\x1b[0m\r\n' +
      '\x1b[38;5;196;48;5;22mINDEXED_STYLE\x1b[0m\r\n' +
      '\x1b[4:2;58:2::12:34:56mDOUBLE_UNDERLINE\x1b[0m\r\n' +
      '\x1b[4:3;58:2::12:34:56mCURL_UNDERLINE\x1b[0m\r\n' +
      '\x1b[53mOVERLINE\x1b[0m\r\n' +
      'UNICODE_e\u0301_界_ø' + '\u0301'.repeat(15) + '\r\n' +
      '\x1b]8;id=browser;' + link + '\x1b\\CLICK_LINK\x1b]8;;\x1b\\';
    await emit(styled);
    await text('CLICK_LINK');
    await page.screenshot({ path: root + '/styles-before.png' });
    const checkStyles = async () => {
      const actual = await styles();
      assert.equal(actual.RGB_STYLE.color, 'rgb(18, 52, 86)');
      assert.equal(actual.RGB_STYLE.background, 'rgb(101, 67, 33)');
      assert.equal(actual.RGB_STYLE.style, 'italic');
      assert.equal(actual.RGB_STYLE.weight, '700');
      assert.match(actual.RGB_STYLE.decoration, /line-through/);
      assert.equal(actual.INDEXED_STYLE.color, 'rgb(255, 0, 0)');
      assert.equal(actual.INDEXED_STYLE.background, 'rgb(0, 95, 0)');
      assert.equal(actual.DOUBLE_UNDERLINE.decorationStyle, 'double');
      assert.equal(actual.CURL_UNDERLINE.decorationStyle, 'wavy');
      assert.equal(actual.CURL_UNDERLINE.decorationColor, 'rgb(12, 34, 56)');
      assert.match(actual.OVERLINE.decoration, /overline/);
      await text('UNICODE_e\u0301_界_ø' + '\u0301'.repeat(15));
    };
    await checkStyles();
    pass('actual DOM computed RGB/256 colours, bold, italics, strike, overline and coloured double/curly underline');
    async function checkLink() {
      // The fixture's pane is idle here. Wait for the final redraw before
      // clicking; a redraw during mousedown/up invalidates xterm's link handle.
      await page.waitForFunction(() => performance.now() - window.__testLastFrameAt > 750);
      await page.bringToFront();
      await page.mouse.move(0, 0);
      const span = page.locator('.xterm-rows span').filter({ hasText: /^CLICK_LINK$/ });
      const bounds = await span.boundingBox();
      assert(bounds);
      // Visit a different terminal row first. xterm caches provider results
      // per row, including an empty row seen during a reconnect redraw.
      const screen = await page.locator('.xterm-screen').boundingBox();
      await page.mouse.move(screen.x + 1, screen.y + 1);
      await page.mouse.move(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
      await page.waitForFunction(uri =>
        window.__testTerminal._core.linkifier.currentLink?.link.text === uri, link);
      const popupPromise = context.waitForEvent('page', { timeout: 5000 });
      // Shift bypasses application mouse reporting while opening a link.
      await page.keyboard.down('Shift');
      await page.mouse.click(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
      await page.keyboard.up('Shift');
      const popup = await popupPromise;
      await popup.waitForURL(link, { waitUntil: 'commit' });
      assert.equal(popup.url(), link);
      await popup.close();
    }
    await checkLink();
    pass('OSC 8 link activates the exact portable URI');
    await control('reconnect');
    await emit('\x1b[12;1HRECONNECTED_LIVE');
    await text('RECONNECTED_LIVE');
    await text('CLICK_LINK');
    await checkStyles();
    await checkLink();
    await page.screenshot({ path: root + '/styles-reconnect.png' });
    pass('existing browser reconnects after real host TCP loss with styles and hyperlink preserved');
    await page.reload();
    await text('CLICK_LINK');
    await checkStyles();
    pass('fresh browser reconstructs styled snapshot after reload');
    await emit(reset + 'BASE_IMAGE_STATE\x1b[3;5H' + red);
    await text('BASE_IMAGE_STATE');
    await pixels([255, 0, 0]);
    await page.screenshot({ path: root + '/sixel-before.png' });
    pass('SIXEL produces opaque red pixels on the actual image canvas');
    await control('reconnect');
    await emit('\x1b[12;1HRECONNECTED_IMAGE');
    await text('RECONNECTED_IMAGE');
    await text('BASE_IMAGE_STATE');
    await pixels([255, 0, 0]);
    await page.reload();
    await text('BASE_IMAGE_STATE');
    await pixels([255, 0, 0]);
    pass('SIXEL red bitmap survives host reconnect and reload with intentionally delayed decoder initialization');
    await emit('\x1b[?1049h\x1b[HALT_IMAGE_STATE\x1b[3;5H' + green);
    await text('ALT_IMAGE_STATE');
    await pixels([0, 255, 0]);
    await control('reconnect');
    await emit('\x1b[12;1HRECONNECTED_ALT');
    await text('RECONNECTED_ALT');
    await text('ALT_IMAGE_STATE');
    await pixels([0, 255, 0]);
    await page.reload();
    await text('ALT_IMAGE_STATE');
    await pixels([0, 255, 0]);
    await emit('\x1b[?1049l');
    await text('BASE_IMAGE_STATE');
    await pixels([255, 0, 0]);
    await page.screenshot({ path: root + '/sixel-restored.png' });
    pass('alternate green SIXEL and saved normal red SIXEL survive reconnect and alternate exit');
    for (const viewport of [{ width: 768, height: 1024 }, { width: 375, height: 667 }]) {
      await page.setViewportSize(viewport);
      await text('BASE_IMAGE_STATE');
      await pixels([255, 0, 0]);
      await page.waitForFunction(() => {
        const box = document.querySelector('.xterm-screen').getBoundingClientRect();
        const container = document.getElementById('terminal-container');
        const clipped = box.right > innerWidth + 1;
        return box.width > 0 && box.height > 0 && box.left >= 0 &&
          (!clipped || getComputedStyle(container).overflowX === 'auto');
      });
      await page.evaluate(() => {
        const container = document.getElementById('terminal-container');
        container.scrollLeft = container.scrollWidth;
      });
      await page.waitForFunction(() => {
        const container = document.getElementById('terminal-container');
        const box = document.querySelector('.xterm-screen').getBoundingClientRect();
        return box.right <= container.getBoundingClientRect().right + 1;
      });
      await page.screenshot({ path: root + '/viewport-' + viewport.width + '.png' });
      pass('visible SIXEL and rightmost terminal columns reachable at ' + viewport.width + 'px viewport');
    }
    await page.setViewportSize({ width: 1440, height: 1000 });
    await page.goto(origin + '/s/' + rw + '?theme=tv');
    await text('BASE_IMAGE_STATE');
    await pixels([255, 0, 0]);
    assert(await page.locator('body.theme-tv').count());
    await page.screenshot({ path: root + '/tv-theme.png' });
    pass('TV theme preserves actual terminal and SIXEL rendering');
    const readonly = page;
    await readonly.goto(origin + '/s/' + ro);
    await text('BASE_IMAGE_STATE', readonly);
    await pixels([255, 0, 0], readonly);
    await readonly.waitForFunction(() => window.__testTerminal.options.disableStdin);
    const forbidden = await context.request.post(origin + '/ws/' + ro + '/input', {
      headers: { 'X-Tmtv-Input': '1' }, data: 'echo forbidden\r'
    });
    assert.equal(forbidden.status(), 403);
    pass('read-only browser restores SIXEL and retains disabled input/server-side 403 isolation');
    const failed = page;
    await failed.addInitScript(() => { WebAssembly.instantiate = () => new Promise(() => {}); });
    await failed.goto(origin + '/s/' + rw);
    await failed.waitForFunction(() => window.__testTerminal && !window.__testTerminal.options.disableStdin);
    await failed.waitForFunction(() =>
      document.querySelector('#error-msg')?.textContent.includes('Reload this page to try again.') &&
      !document.querySelector('#error-overlay').classList.contains('hidden'));
    assert(await failed.evaluate(() => window.__testTerminal.options.disableStdin));
    pass('decoder startup failure gives an actionable error and disables terminal input');
    assert.deepEqual(errors, []);
    pass('no browser JavaScript exceptions');
    console.log(`${passes} browser checks passed`);
  } catch (error) {
    await page.screenshot({ path: root + '/failure.png' }).catch(() => {});
    console.log('CANVASES ' + JSON.stringify(await page.evaluate(() => Array.from(document.querySelectorAll('canvas')).map(c => ({width:c.width,height:c.height,html:c.outerHTML})))));
    console.log('FAIL DOM ' + JSON.stringify(await styles().catch(() => ({}))));
    throw error;
  } finally { await browser.close(); }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
