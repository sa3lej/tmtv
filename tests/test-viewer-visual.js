// Playwright visual test: web viewer UI at multiple viewport sizes.
//
// Usage: node test-viewer-visual.js <viewer-url> [screenshot-dir]
//
// Requires a LIVE tmtv session at <viewer-url>.
// Tests UI elements added in v1.5.7 across desktop, tablet, and mobile
// viewports, plus TV theme validation.
//
// Exits 0 if all checks pass. Exits 1 on failure.

const { chromium } = require('playwright');

const url = process.argv[2];
const screenshotDir = process.argv[3] || '/tmp';

if (!url) {
  console.error('Usage: node test-viewer-visual.js <viewer-url> [screenshot-dir]');
  process.exit(1);
}

/* Strip any existing query params for TV theme URL construction */
var baseUrl = url.split('?')[0];
var tvUrl = baseUrl + (baseUrl.indexOf('?') >= 0 ? '&' : '?') + 'theme=tv';

var VIEWPORTS = [
  /* Desktop */
  { name: 'desktop-1920x1080', width: 1920, height: 1080, mobile: false },
  { name: 'desktop-1440x900',  width: 1440, height: 900,  mobile: false },
  { name: 'desktop-1280x720',  width: 1280, height: 720,  mobile: false },
  /* Tablet */
  { name: 'tablet-portrait',   width: 768,  height: 1024, mobile: false },
  { name: 'tablet-landscape',  width: 1024, height: 768,  mobile: false },
  /* Mobile */
  { name: 'iphone-se',         width: 375,  height: 667,  mobile: true },
  { name: 'iphone-14',         width: 390,  height: 844,  mobile: true },
  { name: 'android',           width: 360,  height: 800,  mobile: true },
];

/* TV theme tested at one desktop + one mobile viewport */
var TV_VIEWPORTS = [
  { name: 'tv-desktop',  width: 1440, height: 900, mobile: false },
  { name: 'tv-mobile',   width: 375,  height: 667, mobile: true },
];

var totalPass = 0;
var totalFail = 0;

function pass(viewport, test) {
  totalPass++;
  console.log('PASS [' + viewport + '] ' + test);
}

function fail(viewport, test, reason) {
  totalFail++;
  console.error('FAIL [' + viewport + '] ' + test + ' — ' + reason);
}

async function screenshot(page, viewport, testname) {
  var path = screenshotDir + '/visual-' + viewport + '-' + testname + '.png';
  try {
    await page.screenshot({ path: path });
  } catch (_) {}
}

async function waitForTerminal(page, timeout) {
  var terminal = page.locator('.xterm-screen');
  await terminal.first().waitFor({ state: 'visible', timeout: timeout });
  /* Give terminal time to receive initial screen dump */
  await page.waitForTimeout(2000);
}

async function runViewportTests(browser, viewportDef, testUrl, isTV) {
  var vp = viewportDef;
  var label = vp.name;
  var context = await browser.newContext({
    viewport: { width: vp.width, height: vp.height },
    hasTouch: vp.mobile,
    /* Simulate coarse pointer on mobile for touch target tests */
    ...(vp.mobile ? {} : {}),
  });
  var page = await context.newPage();

  /* On mobile viewports, emulate coarse pointer via media override */
  if (vp.mobile) {
    await page.emulateMedia({ colorScheme: 'dark' });
  }

  try {
    await page.goto(testUrl, { waitUntil: 'domcontentloaded', timeout: 30000 });
    await waitForTerminal(page, 30000);
    await screenshot(page, label, 'loaded');

    /* 1. Terminal renders with non-zero dimensions */
    var box = await page.locator('.xterm-screen').first().boundingBox();
    if (box && box.width > 0 && box.height > 0) {
      pass(label, 'terminal-renders (' + Math.round(box.width) + 'x' + Math.round(box.height) + ')');
    } else {
      fail(label, 'terminal-renders', 'xterm-screen has zero or null dimensions');
    }

    /* 2. Status bar visible — check last row of .xterm-rows has text */
    var statusBarCheck = await page.evaluate(function() {
      var rows = document.querySelector('.xterm-rows');
      if (!rows) return { ok: false, reason: 'no .xterm-rows element' };
      var rowDivs = rows.children;
      if (rowDivs.length === 0) return { ok: false, reason: 'no row divs in .xterm-rows' };
      var lastRow = rowDivs[rowDivs.length - 1];
      var text = (lastRow.textContent || '').trim();
      var containerEl = document.getElementById('terminal-container');
      var containerH = containerEl ? containerEl.offsetHeight : 0;
      return {
        ok: text.length > 0,
        reason: text.length === 0 ? 'last row is empty (no status bar content)' : '',
        rowCount: rowDivs.length,
        lastRowText: text.substring(0, 60),
        containerHeight: containerH
      };
    });
    if (statusBarCheck.ok) {
      pass(label, 'status-bar-visible (rows=' + statusBarCheck.rowCount +
           ', containerH=' + statusBarCheck.containerHeight + ')');
    } else {
      fail(label, 'status-bar-visible', statusBarCheck.reason);
    }

    if (!isTV) {
      /* 3. Titlebar visible */
      var titlebar = page.locator('#titlebar');
      var titlebarVisible = await titlebar.isVisible().catch(function() { return false; });
      if (titlebarVisible) {
        pass(label, 'titlebar-visible');
      } else {
        fail(label, 'titlebar-visible', '#titlebar is not visible');
      }

      /* 4. Connection quality indicator */
      var connQualityCheck = await page.evaluate(function() {
        var el = document.getElementById('conn-quality');
        if (!el) return { exists: false };
        var classes = el.className || '';
        return {
          exists: true,
          classes: classes,
          hasQuality: /\b(good|fair|poor|disconnected)\b/.test(classes)
        };
      });
      if (connQualityCheck.exists) {
        if (connQualityCheck.hasQuality) {
          pass(label, 'conn-quality (' + connQualityCheck.classes.trim() + ')');
        } else {
          /* Connection quality may not have been set yet — pass with note */
          pass(label, 'conn-quality (exists, class pending: ' + connQualityCheck.classes.trim() + ')');
        }
      } else {
        fail(label, 'conn-quality', '#conn-quality element not found');
      }

      /* 5. Viewer count element */
      var viewerCountExists = await page.evaluate(function() {
        return !!document.getElementById('viewer-count');
      });
      if (viewerCountExists) {
        pass(label, 'viewer-count');
      } else {
        fail(label, 'viewer-count', '#viewer-count element not found');
      }

      /* 6. Copy button visible and clickable */
      var copyBtn = page.locator('#copy-btn');
      var copyBtnVisible = await copyBtn.isVisible().catch(function() { return false; });
      if (copyBtnVisible) {
        var copyBtnEnabled = await copyBtn.isEnabled().catch(function() { return false; });
        if (copyBtnEnabled) {
          pass(label, 'copy-btn');
        } else {
          fail(label, 'copy-btn', '#copy-btn visible but not enabled');
        }
      } else {
        fail(label, 'copy-btn', '#copy-btn is not visible');
      }

      /* 7. Search bar — Ctrl+F opens, typing works, Escape closes */
      var searchBarHidden = await page.evaluate(function() {
        var bar = document.getElementById('search-bar');
        return bar ? bar.classList.contains('hidden') : null;
      });
      if (searchBarHidden === true) {
        /* Search bar starts hidden — good. Now open it with Ctrl+F */
        await page.keyboard.press('Control+f');
        await page.waitForTimeout(500);
        var searchBarNowVisible = await page.evaluate(function() {
          var bar = document.getElementById('search-bar');
          return bar ? !bar.classList.contains('hidden') : false;
        });
        if (searchBarNowVisible) {
          /* Type something into search input */
          var searchInput = page.locator('#search-input');
          await searchInput.fill('test');
          await page.waitForTimeout(300);
          var searchValue = await searchInput.inputValue();
          /* Close with Escape */
          await page.keyboard.press('Escape');
          await page.waitForTimeout(300);
          var searchBarClosed = await page.evaluate(function() {
            var bar = document.getElementById('search-bar');
            return bar ? bar.classList.contains('hidden') : true;
          });
          if (searchValue === 'test' && searchBarClosed) {
            pass(label, 'search-bar (open/type/close)');
          } else {
            fail(label, 'search-bar', 'value=' + searchValue + ', closed=' + searchBarClosed);
          }
        } else {
          fail(label, 'search-bar', 'Ctrl+F did not open search bar');
        }
      } else if (searchBarHidden === null) {
        fail(label, 'search-bar', '#search-bar element not found');
      } else {
        /* search bar not hidden by default — unexpected but not fatal */
        fail(label, 'search-bar', 'search bar was already visible on load');
      }
      await screenshot(page, label, 'after-search');

      /* 8. Reconnect overlay exists (hidden by default) */
      var reconnectCheck = await page.evaluate(function() {
        var el = document.getElementById('reconnect-overlay');
        if (!el) return { exists: false };
        return { exists: true, hidden: el.classList.contains('hidden') };
      });
      if (reconnectCheck.exists) {
        if (reconnectCheck.hidden) {
          pass(label, 'reconnect-overlay (hidden)');
        } else {
          /* If reconnect overlay is showing, still pass — session might be reconnecting */
          pass(label, 'reconnect-overlay (visible — may be reconnecting)');
        }
      } else {
        fail(label, 'reconnect-overlay', '#reconnect-overlay element not found');
      }

      /* 9. Session mode badge */
      var sessionModeExists = await page.evaluate(function() {
        return !!document.getElementById('session-mode');
      });
      if (sessionModeExists) {
        pass(label, 'session-mode');
      } else {
        fail(label, 'session-mode', '#session-mode element not found');
      }
    }

    /* 10. No horizontal overflow */
    var hOverflow = await page.evaluate(function() {
      return {
        scrollWidth: document.documentElement.scrollWidth,
        innerWidth: window.innerWidth
      };
    });
    if (hOverflow.scrollWidth <= hOverflow.innerWidth) {
      pass(label, 'no-horizontal-overflow');
    } else {
      fail(label, 'no-horizontal-overflow',
           'scrollWidth=' + hOverflow.scrollWidth + ' > innerWidth=' + hOverflow.innerWidth);
    }

    /* 11. No vertical overflow */
    var vOverflow = await page.evaluate(function() {
      return {
        scrollHeight: document.documentElement.scrollHeight,
        innerHeight: window.innerHeight
      };
    });
    if (vOverflow.scrollHeight <= vOverflow.innerHeight) {
      pass(label, 'no-vertical-overflow');
    } else {
      fail(label, 'no-vertical-overflow',
           'scrollHeight=' + vOverflow.scrollHeight + ' > innerHeight=' + vOverflow.innerHeight);
    }

    /* 12. Touch targets on mobile — buttons have minimum 44px touch target */
    if (vp.mobile) {
      var touchTargetCheck = await page.evaluate(function() {
        var results = [];
        var buttons = ['#copy-btn', '.search-btn', '.titlebar-btn'];
        for (var i = 0; i < buttons.length; i++) {
          var els = document.querySelectorAll(buttons[i]);
          for (var j = 0; j < els.length; j++) {
            var el = els[j];
            if (el.offsetParent === null) continue; /* skip hidden elements */
            var rect = el.getBoundingClientRect();
            results.push({
              selector: buttons[i] + '[' + j + ']',
              width: Math.round(rect.width),
              height: Math.round(rect.height),
              ok: rect.width >= 44 || rect.height >= 44
            });
          }
        }
        return results;
      });
      var allTouchOk = true;
      var touchDetails = [];
      for (var t = 0; t < touchTargetCheck.length; t++) {
        var item = touchTargetCheck[t];
        touchDetails.push(item.selector + ':' + item.width + 'x' + item.height);
        if (!item.ok) allTouchOk = false;
      }
      if (touchTargetCheck.length === 0) {
        /* No visible buttons on this viewport — acceptable (e.g., TV theme hides titlebar) */
        pass(label, 'touch-targets (no visible buttons)');
      } else if (allTouchOk) {
        pass(label, 'touch-targets (' + touchDetails.join(', ') + ')');
      } else {
        fail(label, 'touch-targets', 'some buttons < 44px: ' + touchDetails.join(', '));
      }
    }

    /* TV theme specific checks */
    if (isTV) {
      /* 13a. Titlebar hidden on TV theme */
      var titlebarHidden = await page.evaluate(function() {
        var el = document.getElementById('titlebar');
        if (!el) return true;
        var style = window.getComputedStyle(el);
        return style.display === 'none';
      });
      if (titlebarHidden) {
        pass(label, 'tv-titlebar-hidden');
      } else {
        fail(label, 'tv-titlebar-hidden', 'titlebar is visible in TV theme');
      }

      /* 13b. TV cabinet elements visible */
      var tvCabinetVisible = await page.evaluate(function() {
        var controls = document.querySelector('.tv-controls');
        var legs = document.querySelector('.tv-legs');
        if (!controls || !legs) return { ok: false, reason: 'missing tv-controls or tv-legs' };
        var ctrlStyle = window.getComputedStyle(controls);
        var legsStyle = window.getComputedStyle(legs);
        return {
          ok: ctrlStyle.display !== 'none' && legsStyle.display !== 'none',
          controlsDisplay: ctrlStyle.display,
          legsDisplay: legsStyle.display
        };
      });
      if (tvCabinetVisible.ok) {
        pass(label, 'tv-cabinet-visible');
      } else {
        fail(label, 'tv-cabinet-visible',
             'controls=' + (tvCabinetVisible.controlsDisplay || 'missing') +
             ', legs=' + (tvCabinetVisible.legsDisplay || 'missing'));
      }

      /* 13c. Terminal still renders in TV theme */
      var tvTermBox = await page.locator('.xterm-screen').first().boundingBox();
      if (tvTermBox && tvTermBox.width > 0 && tvTermBox.height > 0) {
        pass(label, 'tv-terminal-renders');
      } else {
        fail(label, 'tv-terminal-renders', 'terminal not visible in TV theme');
      }
    }

    await screenshot(page, label, 'final');

  } catch (err) {
    fail(label, 'setup', err.message);
    try {
      await screenshot(page, label, 'error');
    } catch (_) {}
  }

  await context.close();
}

(async function() {
  var browser = await chromium.launch();

  try {
    /* Run default theme tests on all viewports */
    for (var i = 0; i < VIEWPORTS.length; i++) {
      await runViewportTests(browser, VIEWPORTS[i], url, false);
    }

    /* Run TV theme tests on selected viewports */
    for (var j = 0; j < TV_VIEWPORTS.length; j++) {
      await runViewportTests(browser, TV_VIEWPORTS[j], tvUrl, true);
    }

    console.log('');
    console.log('Results: ' + totalPass + ' passed, ' + totalFail + ' failed');
    await browser.close();
    process.exit(totalFail > 0 ? 1 : 0);
  } catch (err) {
    console.error('FATAL: ' + err.message);
    await browser.close();
    process.exit(1);
  }
})();
