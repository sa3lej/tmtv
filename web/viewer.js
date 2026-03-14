/*
 * Copyright (c) 2026 Lars-Erik Jonsson <l@jonsson.es>
 * ISC license — see LICENSE for details.
 *
 * tmtv web viewer — SSE-based terminal viewer with full-screen streaming.
 */
(function() {
  'use strict';

  var _dec = new TextDecoder();
  function toStr(v) {
    if (v instanceof Uint8Array) return _dec.decode(v);
    if (v && v.buffer instanceof ArrayBuffer) return _dec.decode(new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
    if (Array.isArray(v)) return String.fromCharCode.apply(null, v);
    return String(v);
  }

  var CTL_HEADER         = 0;
  var CTL_DEAMON_OUT_MSG = 1;
  var OUT_HEADER      = 0;
  var OUT_SYNC_LAYOUT = 1;
  var OUT_PTY_DATA    = 2;
  var OUT_FIN           = 8;
  var OUT_VIEWER_COUNT  = 14;
  var OUT_SESSION_MODE  = 15;

  var FONT = '"JetBrains Mono", "Fira Code", "SF Mono", "Menlo", monospace';
  /* macOS Terminal.app "Clear Dark" palette */
  var THEME = {
    background: '#191d27',
    foreground: '#e0e0e0',
    cursor: '#67b5ed',
    cursorAccent: '#191d27',
    selectionBackground: 'rgba(39, 61, 76, 0.75)',
    black: '#191d27',
    red: '#b45648',
    green: '#6caa71',
    yellow: '#c4ac62',
    blue: '#6d96b4',
    magenta: '#bd7bcd',
    cyan: '#7ccbcd',
    white: '#dee5eb',
    brightBlack: '#465c6d',
    brightRed: '#df6c5a',
    brightGreen: '#79be7e',
    brightYellow: '#e5c872',
    brightBlue: '#67b5ed',
    brightMagenta: '#d389e5',
    brightCyan: '#84dde0',
    brightWhite: '#e5eff5'
  };

  var serverCols = window.serverCols = 80;
  var serverRows = window.serverRows = 24;
  var contentRows = 24;
  var term = null;              /* single xterm.js instance */
  var cellW = 0, cellH = 0;
  var fontSize = 14;
  var cellRatioW = 0, cellRatioH = 0;
  var container = document.getElementById('terminal-container');
  var searchAddon = null;
  var searchVisible = false;

  /* Connection quality tracking */
  var lastDataTime = 0;
  var connQuality = 'unknown'; /* good, fair, poor, disconnected */
  var connQualityTimer = null;

  function measureCellRatio() {
    if (cellRatioW > 0) return;
    var tmp = document.createElement('div');
    tmp.style.position = 'absolute';
    tmp.style.visibility = 'hidden';
    tmp.style.left = '-9999px';
    document.body.appendChild(tmp);
    var t = new Terminal({
      fontFamily: FONT, fontSize: 14, lineHeight: 1.15,
      cols: 10, rows: 2, allowProposedApi: true
    });
    t.open(tmp);
    try {
      var dims = t._core._renderService.dimensions;
      if (dims && dims.css && dims.css.cell) {
        cellRatioW = dims.css.cell.width / 14;
        cellRatioH = dims.css.cell.height / 14;
      }
    } catch (e) {}
    t.dispose();
    document.body.removeChild(tmp);
    if (!cellRatioW || !cellRatioH) { cellRatioW = 0.6; cellRatioH = 1.15; }
  }

  var params = new URLSearchParams(location.search);
  var currentTheme = params.get('theme') || '';
  if (currentTheme) document.body.className = 'theme-' + currentTheme;

  var TITLEBAR_H = 38;

  function computeFontSize() {
    measureCellRatio();
    var availW, availH;
    if (currentTheme === 'tv') {
      availW = (window.innerWidth - 80) * 0.72 - 48;
      availH = window.innerHeight - 140 - 48;
    } else {
      availW = window.innerWidth - 32;
      availH = window.innerHeight - 32 - TITLEBAR_H;
    }
    var maxFromW = availW / (serverCols * cellRatioW);
    var maxFromH = availH / (contentRows * cellRatioH);
    fontSize = Math.max(10, Math.floor(Math.min(maxFromW, maxFromH)));
    cellW = fontSize * cellRatioW;
    cellH = fontSize * cellRatioH;
  }

  function createTerminal(sx, sy) {
    var disableInput = sessionReadonly || !webInputEnabled;
    return new Terminal({
      fontFamily: FONT, fontSize: fontSize, lineHeight: 1.15,
      theme: THEME, cols: sx, rows: sy, scrollback: 0,
      cursorBlink: !disableInput, cursorStyle: 'block',
      disableStdin: disableInput, allowProposedApi: true
    });
  }

  function initTerminal() {
    if (term) return;

    /* serverRows is the pane area; add 1 for the tmux status bar row. */
    var cols = serverCols || 80;
    var rows = (serverRows || 24) + 1;
    contentRows = rows;
    computeFontSize();

    term = createTerminal(cols, rows);

    var el = document.createElement('div');
    el.id = 'fullscreen-term';
    el.style.position = 'absolute';
    el.style.left = '0';
    el.style.top = '0';
    el.style.width = '100%';
    el.style.height = '100%';
    container.appendChild(el);

    term.open(el);

    /* Load search addon */
    if (window.SearchAddon && window.SearchAddon.SearchAddon) {
      try {
        searchAddon = new window.SearchAddon.SearchAddon();
        term.loadAddon(searchAddon);
      } catch (e) {}
    }

    /* Load image addon for SIXEL graphics support */
    if (window.ImageAddon && window.ImageAddon.ImageAddon) {
      try {
        var imageAddon = new window.ImageAddon.ImageAddon();
        term.loadAddon(imageAddon);
      } catch (e) {}
    }

    /* Bind input if web input is active */
    if (!sessionReadonly && webInputEnabled) {
      term.options.disableStdin = false;
      term._inputBound = true;
      term.onData(function(data) { queueInput(data); });
    }

    /* Handle text selection for copy */
    term.onSelectionChange(function() {
      var sel = term.getSelection();
      if (sel) {
        /* Store selection for copy shortcut */
        term._lastSelection = sel;
      }
    });

    /* Prevent browser from intercepting terminal keys */
    term.attachCustomKeyEventHandler(function(ev) {
      /* Ctrl+F: open search */
      if (ev.ctrlKey && !ev.shiftKey && ev.key === 'f' && ev.type === 'keydown') {
        ev.preventDefault();
        toggleSearch(true);
        return false;
      }
      /* Escape: close search */
      if (ev.key === 'Escape' && searchVisible && ev.type === 'keydown') {
        toggleSearch(false);
        return false;
      }
      /* Ctrl+Shift+C: copy selection */
      if (ev.ctrlKey && ev.shiftKey && ev.key === 'C' && ev.type === 'keydown') {
        copySelection();
        return false;
      }
      if (ev.ctrlKey) {
        /* Allow Ctrl+Shift+V for paste (browser convention) */
        if (ev.shiftKey && ev.key === 'V') return true;
        ev.preventDefault();
        return true;
      }
      /* Arrow keys, Home, End, PgUp, PgDn — prevent browser scroll/nav */
      var navKeys = ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight',
                     'Home', 'End', 'PageUp', 'PageDown', 'Tab'];
      if (navKeys.indexOf(ev.key) >= 0) {
        ev.preventDefault();
        return true;
      }
      return true;
    });

    /* Defer sizing until xterm.js has rendered so getActualTerminalDims()
     * can read the real canvas height.  Without this, the fallback
     * estimated height clips the status bar row. */
    requestAnimationFrame(function() {
      requestAnimationFrame(function() { sizeToServer(); });
    });

    /* Focus terminal on touch (mobile) */
    container.addEventListener('touchstart', function() {
      if (term) term.focus();
    }, { passive: true });
  }

  /* --- Status bar fix: use actual xterm.js dimensions --- */
  function getActualTerminalDims() {
    if (!term) return null;
    try {
      var dims = term._core._renderService.dimensions;
      if (dims && dims.css && dims.css.canvas) {
        return {
          width: Math.ceil(dims.css.canvas.width),
          height: Math.ceil(dims.css.canvas.height)
        };
      }
    } catch (e) {}
    return null;
  }

  function sizeToServer() {
    computeFontSize();

    var effectiveCols = serverCols;
    var effectiveRows = term ? term.rows : contentRows;

    /* Update font size on the terminal if it changed */
    if (term && term.options.fontSize !== fontSize) {
      term.options.fontSize = fontSize;
    }

    /* Try to use actual xterm.js rendered dimensions for pixel-perfect sizing.
     * This prevents the status bar from being clipped due to rounding errors
     * in our cell ratio approximation. */
    var actualDims = getActualTerminalDims();
    var contentW, contentH;
    if (actualDims) {
      contentW = actualDims.width;
      contentH = actualDims.height;
    } else {
      contentW = Math.ceil(effectiveCols * cellW);
      contentH = Math.ceil(effectiveRows * cellH);
    }

    var wrap = document.getElementById('terminal-wrap');

    if (currentTheme === 'tv') {
      var cabinetW = Math.ceil(contentW / 0.72) + 48;
      var cabinetH = contentH + 48;
      var maxW = window.innerWidth - 48;
      var maxH = window.innerHeight - 140;
      wrap.style.width = Math.min(cabinetW, maxW) + 'px';
      wrap.style.height = Math.min(cabinetH, maxH) + 'px';
      container.style.height = contentH + 'px';
    } else {
      var maxW = window.innerWidth - 32;
      var maxH = window.innerHeight - 32;
      var wrapH = contentH + TITLEBAR_H;
      wrap.style.width = Math.min(contentW, maxW) + 'px';
      wrap.style.height = Math.min(wrapH, maxH) + 'px';
      container.style.height = contentH + 'px';
    }
  }

  window.addEventListener('resize', function() { sizeToServer(); });

  var statusEl = document.getElementById('status');
  var fadeTimer = null;

  function setStatus(msg, cls) {
    statusEl.textContent = msg;
    statusEl.className = 'status ' + (cls || '');
    if (fadeTimer) clearTimeout(fadeTimer);
    if (cls === 'connected') {
      fadeTimer = setTimeout(function() { statusEl.classList.add('fade'); }, 2000);
    }
  }

  var pathMatch = location.pathname.match(/^\/[sj]\/([^\/]+)/);
  var sessionToken = pathMatch ? pathMatch[1] : null;

  var ssePort = params.get('port');
  var sseUrl = ssePort
    ? 'http://' + (location.hostname || 'localhost') + ':' + ssePort + '/' + (sessionToken || '')
    : '/ws/' + (sessionToken || '');

  var sessionStart = null;
  var webViewers = 0;
  var sshViewers = 0;
  var durationTimer = null;

  if (sessionToken) {
    var displayName = sessionToken.substring(0, 8);
    document.getElementById('titlebar-label').textContent = 'tmtv \u2014 ' + displayName;
    document.title = 'tmtv \u2014 ' + displayName;
  }

  function updateMeta() {
    var parts = [];
    if (sessionStart) {
      var elapsed = Math.floor((Date.now() - sessionStart) / 1000);
      var m = Math.floor(elapsed / 60);
      var s = elapsed % 60;
      parts.push((m < 10 ? '0' : '') + m + ':' + (s < 10 ? '0' : '') + s);
    }
    var metaEl = document.getElementById('titlebar-meta');
    if (metaEl) metaEl.textContent = parts.length ? parts.join(' \u00b7 ') : '';

    /* Update viewer count overlay */
    var countEl = document.getElementById('viewer-count');
    if (countEl) {
      var total = webViewers + sshViewers;
      if (total > 0) {
        countEl.textContent = total;
        countEl.title = (sshViewers ? sshViewers + ' SSH' : '') +
                        (sshViewers && webViewers ? ', ' : '') +
                        (webViewers ? webViewers + ' web' : '') + ' viewer' +
                        (total !== 1 ? 's' : '');
        countEl.classList.remove('hidden');
      } else {
        countEl.classList.add('hidden');
      }
    }
  }

  var reconnectAttempts = 0;
  var maxReconnect = 5;
  var sessionEnded = false;
  var everConnected = false;
  var sessionReadonly = true;     /* assume RO until server says otherwise */
  var webInputEnabled = false;
  var inputInFlight = false;
  var inputBatch = '';

  function flushInput() {
    if (!inputBatch) return;
    var data = inputBatch;
    inputBatch = '';
    inputInFlight = true;
    var url = buildSseUrl().replace(/\?.*$/, '') + '/input';
    var pw = sessionPassword;
    if (pw) url += '?password=' + encodeURIComponent(pw);
    fetch(url, {
      method: 'POST',
      body: data,
      headers: { 'Content-Type': 'text/plain', 'X-Tmtv-Input': '1' }
    }).then(function() {
      inputInFlight = false;
      flushInput();
    }).catch(function() {
      inputInFlight = false;
    });
  }

  /* Filter out terminal query responses that xterm.js generates
   * automatically (DA, DSR, OSC 10/11, DECRPM, CPR, etc.).
   * These must never be sent back to the server as user input. */
  var termResponseRe = /\x1b\[[\?>=!]?[\d;]*[cnRySut]|\x1b\]1[01];[^\x07\x1b]*(?:\x07|\x1b\\)|\x1bP[^\x1b]*\x1b\\/g;

  function queueInput(data) {
    if (sessionReadonly || !webInputEnabled) return;
    data = data.replace(termResponseRe, '');
    if (!data) return;
    inputBatch += data;
    if (!inputInFlight) {
      flushInput();
    }
  }

  function updateSessionModeBadge() {
    var badge = document.getElementById('session-mode');
    if (!badge) return;
    if (sessionReadonly) {
      badge.textContent = 'view-only';
      badge.className = 'session-mode ro';
    } else if (webInputEnabled) {
      badge.textContent = 'interactive';
      badge.className = 'session-mode rw';
    } else {
      badge.textContent = 'view-only';
      badge.className = 'session-mode ro';
    }
    badge.classList.remove('hidden');
  }

  function enableTerminalInput() {
    if (term) {
      term.options.disableStdin = false;
      if (!term._inputBound) {
        term._inputBound = true;
        term.onData(function(data) { queueInput(data); });
      }
    }
  }

  var sessionPassword = sessionStorage.getItem('tmtv_pw_' + (sessionToken || '')) || '';

  function showPasswordPrompt(isRetry) {
    var overlay = document.getElementById('password-overlay');
    var errEl = document.getElementById('password-error');
    var input = document.getElementById('password-input');
    if (overlay) overlay.classList.remove('hidden');
    if (errEl) {
      if (isRetry) errEl.classList.remove('hidden');
      else errEl.classList.add('hidden');
    }
    if (input) { input.value = ''; input.focus(); }

    var form = document.getElementById('password-form');
    if (form && !form._bound) {
      form._bound = true;
      form.addEventListener('submit', function(e) {
        e.preventDefault();
        var pw = document.getElementById('password-input').value;
        if (!pw) return;
        sessionPassword = pw;
        sessionStorage.setItem('tmtv_pw_' + (sessionToken || ''), pw);
        overlay.classList.add('hidden');
        reconnectAttempts = 0;
        connect();
      });
    }
  }

  function hidePasswordPrompt() {
    var overlay = document.getElementById('password-overlay');
    if (overlay) overlay.classList.add('hidden');
  }

  function showErrorOverlay(msg, isSessionEnd) {
    var overlay = document.getElementById('error-overlay');
    var msgEl = document.getElementById('error-msg');
    var titleEl = document.getElementById('error-title');
    var iconEl = document.getElementById('error-icon');
    var sessionForm = document.getElementById('session-form');
    var sessionInput = document.getElementById('session-input');
    if (overlay) overlay.classList.remove('hidden');
    if (msgEl && msg) msgEl.textContent = msg;
    if (titleEl) titleEl.textContent = isSessionEnd ? 'Session ended' : 'Session unavailable';
    if (iconEl) {
      iconEl.innerHTML = isSessionEnd
        ? '<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><rect width="18" height="18" x="3" y="3" rx="2"/><path d="m15 9-6 6"/><path d="m9 9 6 6"/></svg>'
        : '<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><rect width="18" height="11" x="3" y="11" rx="2"/><path d="M7 11V7a5 5 0 0 1 9.9-1"/><line x1="12" x2="12" y1="15" y2="17"/></svg>';
    }
    /* Show session lobby form for both session-end and unavailable states */
    if (sessionForm) {
      sessionForm.classList.remove('hidden');
      var divider = document.getElementById('session-divider');
      if (divider) divider.classList.remove('hidden');
      bindSessionForm();
      if (sessionInput) sessionInput.focus();
    }
  }

  function bindSessionForm() {
    var form = document.getElementById('session-form');
    if (!form || form._bound) return;
    form._bound = true;
    form.addEventListener('submit', function(e) {
      e.preventDefault();
      var input = document.getElementById('session-input');
      if (!input) return;
      var value = (input.value || '').trim();
      if (!value) return;
      /* Extract token from full URL or bare token */
      var urlMatch = value.match(/\/[sj]\/([^\/?#]+)/);
      var token = urlMatch ? urlMatch[1] : value;
      location.href = '/s/' + encodeURIComponent(token);
    });
  }

  function buildSseUrl() {
    if (sessionPassword) {
      var sep = sseUrl.indexOf('?') >= 0 ? '&' : '?';
      return sseUrl + sep + 'password=' + encodeURIComponent(sessionPassword);
    }
    return sseUrl;
  }

  /* --- Seamless reconnect overlay --- */
  function showReconnectOverlay() {
    var el = document.getElementById('reconnect-overlay');
    if (el) el.classList.remove('hidden');
  }

  function hideReconnectOverlay() {
    var el = document.getElementById('reconnect-overlay');
    if (el) el.classList.add('hidden');
  }

  /* --- Connection quality indicator --- */
  function updateConnQuality(quality) {
    if (quality === connQuality) return;
    connQuality = quality;
    var el = document.getElementById('conn-quality');
    if (!el) return;
    el.className = 'conn-quality ' + quality;
    var labels = { good: 'Good connection', fair: 'Slow connection', poor: 'Unstable connection', disconnected: 'Disconnected' };
    el.title = labels[quality] || '';
    el.setAttribute('aria-label', labels[quality] || '');
  }

  function startConnQualityMonitor() {
    if (connQualityTimer) clearInterval(connQualityTimer);
    connQualityTimer = setInterval(function() {
      if (!lastDataTime || sessionEnded) return;
      var age = Date.now() - lastDataTime;
      if (age < 5000) updateConnQuality('good');
      else if (age < 15000) updateConnQuality('fair');
      else if (age < 30000) updateConnQuality('poor');
      else updateConnQuality('disconnected');
    }, 2000);
  }

  /* --- Search --- */
  function toggleSearch(show) {
    var bar = document.getElementById('search-bar');
    var input = document.getElementById('search-input');
    if (!bar || !searchAddon) return;

    searchVisible = show;
    if (show) {
      bar.classList.remove('hidden');
      if (input) {
        input.value = '';
        input.focus();
      }
    } else {
      bar.classList.add('hidden');
      searchAddon.clearDecorations();
      if (term) term.focus();
    }
  }

  function doSearch(direction) {
    if (!searchAddon || !searchVisible) return;
    var input = document.getElementById('search-input');
    if (!input || !input.value) return;
    var opts = { regex: false, wholeWord: false, caseSensitive: false };
    if (direction === 'prev') {
      searchAddon.findPrevious(input.value, opts);
    } else {
      searchAddon.findNext(input.value, opts);
    }
  }

  /* Bind search UI events */
  (function() {
    var bar = document.getElementById('search-bar');
    if (!bar) return;
    var input = document.getElementById('search-input');
    var prevBtn = document.getElementById('search-prev');
    var nextBtn = document.getElementById('search-next');
    var closeBtn = document.getElementById('search-close');

    if (input) {
      input.addEventListener('input', function() { doSearch('next'); });
      input.addEventListener('keydown', function(e) {
        if (e.key === 'Enter') {
          e.preventDefault();
          doSearch(e.shiftKey ? 'prev' : 'next');
        }
        if (e.key === 'Escape') {
          toggleSearch(false);
        }
      });
    }
    if (prevBtn) prevBtn.addEventListener('click', function() { doSearch('prev'); });
    if (nextBtn) nextBtn.addEventListener('click', function() { doSearch('next'); });
    if (closeBtn) closeBtn.addEventListener('click', function() { toggleSearch(false); });
  })();

  /* --- Copy text --- */
  function copySelection() {
    if (!term) return;
    var sel = term.getSelection();
    if (!sel) return;
    if (navigator.clipboard) {
      navigator.clipboard.writeText(sel).then(showCopyToast).catch(function() {
        fallbackCopy(sel);
        showCopyToast();
      });
    } else {
      fallbackCopy(sel);
      showCopyToast();
    }
  }

  function fallbackCopy(text) {
    var ta = document.createElement('textarea');
    ta.value = text;
    ta.style.position = 'fixed';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.select();
    try { document.execCommand('copy'); } catch (e) {}
    document.body.removeChild(ta);
  }

  function showCopyToast() {
    var toast = document.getElementById('copy-toast');
    if (!toast) return;
    toast.classList.remove('hidden');
    toast.classList.add('show');
    setTimeout(function() {
      toast.classList.remove('show');
      toast.classList.add('hidden');
    }, 1500);
  }

  /* Bind copy button */
  var copyBtn = document.getElementById('copy-btn');
  if (copyBtn) {
    copyBtn.addEventListener('click', function() { copySelection(); });
  }

  function connect() {
    setStatus('Connecting...');

    /* Skip the HEAD probe on reconnect — we already know the password
     * status from the first connection.  The HEAD probe creates a full
     * ws_client through IPC on the server, doubling connection load
     * and generating "handshake failed" noise on every reconnect. */
    if (everConnected) {
      connectEventSource();
      return;
    }

    /* Probe the SSE endpoint with HEAD to detect password gates.
     * HEAD validates the token and password without creating a real
     * SSE connection or spawning the virtual PTY on the server.
     * EventSource does not expose HTTP status codes, so we cannot
     * distinguish a 403 from a network error without this probe. */
    fetch(buildSseUrl(), { method: 'HEAD' }).then(function(resp) {
      if (resp.status === 403) {
        /* HEAD 403 has no body — check X-Tmtv-Reason header instead */
        var reason = resp.headers.get('X-Tmtv-Reason');
        if (reason === 'password_required') {
          showPasswordPrompt(false);
        } else if (reason === 'wrong_password') {
          sessionPassword = '';
          sessionStorage.removeItem('tmtv_pw_' + (sessionToken || ''));
          showPasswordPrompt(true);
        } else {
          showErrorOverlay('Access denied.', false);
        }
        return;
      }
      connectEventSource();
    }).catch(function() {
      /* fetch itself failed (network error) — fall through to EventSource
       * which has its own retry logic */
      connectEventSource();
    });
  }

  function connectEventSource() {
    var url = buildSseUrl();
    var es = new EventSource(url);
    var dataReceived = false;
    var silenceTimer = null;

    es.onopen = function() {
      setStatus('Connected', 'connected');
      hidePasswordPrompt();
      hideReconnectOverlay();
      updateConnQuality('good');
      startConnQualityMonitor();
      if (!sessionStart) {
        sessionStart = Date.now();
        durationTimer = setInterval(updateMeta, 1000);
      }
      /* Watchdog: if no data arrives shortly after connecting,
       * the session is likely dead (server holds connection open
       * but child process is gone). Treat as connection error.
       * Use a shorter timeout on reconnects — we already had data
       * before, so the screen dump should arrive quickly. */
      var watchdogMs = everConnected ? 3000 : 10000;
      everConnected = true;
      silenceTimer = setTimeout(function() {
        if (!dataReceived && !sessionEnded) {
          es.close();
          es.onerror();
        }
      }, watchdogMs);
    };

    es.onmessage = function(evt) {
      dataReceived = true;
      reconnectAttempts = 0;
      lastDataTime = Date.now();
      if (silenceTimer) { clearTimeout(silenceTimer); silenceTimer = null; }
      var binary = atob(evt.data);
      var bytes = new Uint8Array(binary.length);
      for (var i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
      try {
        var iter = MessagePack.decodeMulti(bytes);
        var result = iter.next();
        while (!result.done) {
          handleMessage(result.value);
          result = iter.next();
        }
      } catch (e) {}
    };

    es.onerror = function() {
      if (silenceTimer) { clearTimeout(silenceTimer); silenceTimer = null; }
      es.close();
      updateConnQuality('disconnected');
      if (sessionEnded) {
        setStatus('Session ended', 'error');
        if (durationTimer) clearInterval(durationTimer);
        showErrorOverlay('The host ended this terminal session.', true);
        return;
      }
      if (reconnectAttempts < maxReconnect) {
        reconnectAttempts++;
        var delay = Math.min(1000 * Math.pow(2, reconnectAttempts - 1), 5000);
        setStatus('Reconnecting (' + reconnectAttempts + '/' + maxReconnect + ')...', 'error');
        /* Seamless reconnect: keep terminal visible, show subtle overlay */
        showReconnectOverlay();
        setTimeout(connect, delay);
      } else {
        setStatus('Connection lost', 'error');
        if (durationTimer) clearInterval(durationTimer);
        if (!everConnected) {
          showErrorOverlay('This session may have ended or the token is invalid.', false);
        } else {
          showErrorOverlay('Connection lost. The session may no longer be available.', false);
        }
      }
    };
  }

  function handleMessage(msg) {
    if (!Array.isArray(msg) || msg.length < 2) return;
    if (msg[0] === CTL_DEAMON_OUT_MSG) handleDaemonMsg(msg[1]);
  }

  function handleDaemonMsg(inner) {
    if (!Array.isArray(inner) || inner.length < 1) return;
    var type = inner[0];

    switch (type) {
      case OUT_PTY_DATA:
        if (inner.length >= 3) {
          var paneId = inner[1];
          var ptyData = inner[2];
          if (paneId === -1) {
            if (!term) initTerminal();
            if (term && ptyData) {
              term.write(ptyData instanceof Uint8Array ? ptyData : new Uint8Array(ptyData));
            }
          }
          /* Ignore per-pane data (paneId >= 0) — not supported */
        }
        break;
      case OUT_SYNC_LAYOUT:
        if (inner.length >= 3) {
          var sx = inner[1], sy = inner[2];
          if (sx > 0 && sy > 0) {
            serverCols = sx; serverRows = sy;
            window.serverCols = sx; window.serverRows = sy;
          }
          /* Resize the terminal to match.
           * +1 row for the tmux status bar rendered in-terminal. */
          if (term && sx > 0 && sy > 0) {
            var newRows = sy + 1;
            contentRows = newRows;
            if (term.cols !== sx || term.rows !== newRows) {
              term.resize(sx, newRows);
            }
          }
          /* Defer so xterm.js render dimensions are up to date */
          requestAnimationFrame(function() { sizeToServer(); });
        }
        break;
      case OUT_VIEWER_COUNT:
        if (inner.length >= 4) {
          sshViewers = inner[2] || 0;
          webViewers = inner[3];
          updateMeta();
        }
        break;
      case OUT_SESSION_MODE:
        if (inner.length >= 3) {
          sessionReadonly = !!inner[1];
          webInputEnabled = !!inner[2];
          updateSessionModeBadge();
          if (!sessionReadonly && webInputEnabled) enableTerminalInput();
        }
        break;
      case OUT_FIN:
        sessionEnded = true;
        setStatus('Session ended', 'error');
        if (durationTimer) clearInterval(durationTimer);
        showErrorOverlay('The host ended this terminal session.', true);
        break;
    }
  }

  connect();
})();
