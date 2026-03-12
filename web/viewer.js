/*
 * Copyright (c) 2026 Lars-Erik Jonsson <l@jonsson.es>
 * ISC license — see LICENSE for details.
 *
 * tmtv web viewer — SSE-based terminal viewer with multi-pane support.
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
  var OUT_STATUS        = 5;
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
  var panes = {};
  var pendingData = {};
  var cellW = 0, cellH = 0;
  var fontSize = 14;
  var cellRatioW = 0, cellRatioH = 0;
  var container = document.getElementById('terminal-container');

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

  function computeFontSize() {
    measureCellRatio();
    var availW, availH;
    if (currentTheme === 'tv') {
      availW = (window.innerWidth - 80) * 0.72 - 48;
      availH = window.innerHeight - 140 - 48;
    } else {
      availW = window.innerWidth - 32;
      availH = window.innerHeight - 32 - 38 - 20;
    }
    var maxFromW = availW / (serverCols * cellRatioW);
    var maxFromH = availH / (contentRows * cellRatioH);
    fontSize = Math.max(6, Math.min(Math.floor(Math.min(maxFromW, maxFromH)), 22));
    cellW = fontSize * cellRatioW;
    cellH = fontSize * cellRatioH;
  }

  function createPaneTerminal(sx, sy) {
    var disableInput = sessionReadonly || !webInputEnabled;
    return new Terminal({
      fontFamily: FONT, fontSize: fontSize, lineHeight: 1.15,
      theme: THEME, cols: sx, rows: sy, scrollback: 0,
      cursorBlink: !disableInput, cursorStyle: 'block',
      disableStdin: disableInput, allowProposedApi: true
    });
  }

  function updatePaneLayout(windows, activeWinIdx) {
    if (!Array.isArray(windows)) return;
    var paneList = null;
    var activePaneId = -1;
    for (var i = 0; i < windows.length; i++) {
      var win = windows[i];
      if (!Array.isArray(win)) continue;
      if (win[0] === activeWinIdx) {
        paneList = win[2];
        activePaneId = win[3];
        break;
      }
    }
    if (!paneList || !Array.isArray(paneList)) return;

    var maxRow = 0;
    for (var i = 0; i < paneList.length; i++) {
      var p = paneList[i];
      if (Array.isArray(p) && p.length >= 5) {
        var bottom = p[4] + p[2];
        if (bottom > maxRow) maxRow = bottom;
      }
    }
    if (maxRow > 0) contentRows = maxRow;
    computeFontSize();

    var seen = {};
    var oldBorders = container.querySelectorAll('.pane-border');
    for (var i = 0; i < oldBorders.length; i++) oldBorders[i].remove();

    for (var i = 0; i < paneList.length; i++) {
      var p = paneList[i];
      if (!Array.isArray(p) || p.length < 5) continue;
      var id = p[0], sx = p[1], sy = p[2], xoff = p[3], yoff = p[4];
      seen[id] = true;

      var left = Math.round(xoff * cellW);
      var top = Math.round(yoff * cellH);
      var width = Math.round(sx * cellW);
      var height = Math.round(sy * cellH);

      if (panes[id]) {
        var pane = panes[id];
        pane.el.style.left = left + 'px';
        pane.el.style.top = top + 'px';
        pane.el.style.width = width + 'px';
        pane.el.style.height = height + 'px';
        if (pane.term.options.fontSize !== fontSize) pane.term.options.fontSize = fontSize;
        if (pane.term.cols !== sx || pane.term.rows !== sy) pane.term.resize(sx, sy);
        pane.sx = sx; pane.sy = sy;
      } else {
        var el = document.createElement('div');
        el.style.position = 'absolute';
        el.style.left = left + 'px';
        el.style.top = top + 'px';
        el.style.width = width + 'px';
        el.style.height = height + 'px';
        el.style.overflow = 'hidden';
        container.appendChild(el);
        var t = createPaneTerminal(sx, sy);
        t.open(el);
        panes[id] = { term: t, el: el, sx: sx, sy: sy, needsRefresh: true, _inputBound: false };
        /* Bind input handler if web input is active */
        if (!sessionReadonly && webInputEnabled) {
          t.options.disableStdin = false;
          panes[id]._inputBound = true;
          (function(pane) {
            pane.term.onData(function(data) { queueInput(data); });
          })(panes[id]);
        }
        if (pendingData[id]) {
          for (var j = 0; j < pendingData[id].length; j++) {
            var d = pendingData[id][j];
            t.write(d instanceof Uint8Array ? d : String(d));
          }
          delete pendingData[id];
        }
      }
      panes[id].term.options.cursorBlink = (id === activePaneId);

      if (xoff > 0) {
        var border = document.createElement('div');
        border.className = 'pane-border';
        border.style.left = (left - 1) + 'px';
        border.style.top = top + 'px';
        border.style.width = '1px';
        border.style.height = height + 'px';
        container.appendChild(border);
      }
      if (yoff > 0) {
        var border = document.createElement('div');
        border.className = 'pane-border';
        border.style.left = left + 'px';
        border.style.top = (top - 1) + 'px';
        border.style.width = width + 'px';
        border.style.height = '1px';
        container.appendChild(border);
      }
    }

    for (var id in panes) {
      if (!seen[id]) {
        panes[id].term.dispose();
        panes[id].el.remove();
        delete panes[id];
      }
    }
    paneCount = Object.keys(panes).length;
    updateMeta();
  }

  function sizeToServer() {
    computeFontSize();
    var contentW = Math.ceil(serverCols * cellW);
    var contentH = Math.ceil(contentRows * cellH);
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
      var totalH = 38 + contentH + 20;
      var maxW = window.innerWidth - 32;
      var maxH = window.innerHeight - 32;
      wrap.style.width = Math.min(contentW, maxW) + 'px';
      wrap.style.height = Math.min(totalH, maxH) + 'px';
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

  function updateWindowList(windows, activeIdx) {
    if (!Array.isArray(windows)) return;
    var parts = [];
    for (var i = 0; i < windows.length; i++) {
      var win = windows[i];
      if (!Array.isArray(win) || win.length < 2) continue;
      var idx = win[0];
      var name = toStr(win[1]);
      var marker = (idx === activeIdx) ? '*' : '-';
      parts.push(idx + ':' + name + marker);
    }
    var leftEl = document.getElementById('tmux-status-left');
    if (leftEl && parts.length > 0) leftEl.textContent = '[0] ' + parts.join(' ');
  }

  var pathMatch = location.pathname.match(/^\/s\/([^\/]+)/);
  var sessionToken = pathMatch ? pathMatch[1] : null;

  var ssePort = params.get('port');
  var sseUrl = ssePort
    ? 'http://' + (location.hostname || 'localhost') + ':' + ssePort + '/' + (sessionToken || '')
    : '/ws/' + (sessionToken || '');

  var sessionStart = null;
  var paneCount = 0;
  var webViewers = 0;
  var durationTimer = null;

  if (sessionToken) {
    var displayName = sessionToken.substring(0, 8);
    document.getElementById('titlebar-label').textContent = 'tmtv \u2014 ' + displayName;
    document.title = 'tmtv \u2014 ' + displayName;
  }

  function updateMeta() {
    var parts = [];
    if (webViewers > 0) parts.push('W:' + webViewers);
    if (paneCount > 0) parts.push(paneCount + (paneCount === 1 ? ' pane' : ' panes'));
    if (sessionStart) {
      var elapsed = Math.floor((Date.now() - sessionStart) / 1000);
      var m = Math.floor(elapsed / 60);
      var s = elapsed % 60;
      parts.push((m < 10 ? '0' : '') + m + ':' + (s < 10 ? '0' : '') + s);
    }
    var metaEl = document.getElementById('titlebar-meta');
    if (metaEl) metaEl.textContent = parts.length ? parts.join(' \u00b7 ') : '';
  }

  var reconnectAttempts = 0;
  var maxReconnect = 5;
  var sessionEnded = false;
  var everConnected = false;
  var sessionReadonly = true;     /* assume RO until server says otherwise */
  var webInputEnabled = false;
  var inputBatchTimer = null;
  var inputBatch = '';

  function sendInputBatch() {
    if (!inputBatch) return;
    var data = inputBatch;
    inputBatch = '';
    var url = buildSseUrl().replace(/\?.*$/, '') + '/input';
    var pw = sessionPassword;
    if (pw) url += '?password=' + encodeURIComponent(pw);
    fetch(url, {
      method: 'POST',
      body: data,
      headers: { 'Content-Type': 'text/plain' }
    }).catch(function() {});
  }

  function queueInput(data) {
    if (sessionReadonly || !webInputEnabled) return;
    inputBatch += data;
    if (!inputBatchTimer) {
      inputBatchTimer = setTimeout(function() {
        inputBatchTimer = null;
        sendInputBatch();
      }, 30);
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
    for (var id in panes) {
      var p = panes[id];
      if (p && p.term) {
        p.term.options.disableStdin = false;
        if (!p._inputBound) {
          p._inputBound = true;
          (function(pane) {
            pane.term.onData(function(data) { queueInput(data); });
          })(p);
        }
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
    if (overlay) overlay.classList.remove('hidden');
    if (msgEl && msg) msgEl.textContent = msg;
    if (titleEl) titleEl.textContent = isSessionEnd ? 'Session ended' : 'Session unavailable';
    if (iconEl) {
      iconEl.innerHTML = isSessionEnd
        ? '<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><rect width="18" height="18" x="3" y="3" rx="2"/><path d="m15 9-6 6"/><path d="m9 9 6 6"/></svg>'
        : '<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><rect width="18" height="11" x="3" y="11" rx="2"/><path d="M7 11V7a5 5 0 0 1 9.9-1"/><line x1="12" x2="12" y1="15" y2="17"/></svg>';
    }
  }

  function buildSseUrl() {
    if (sessionPassword) {
      var sep = sseUrl.indexOf('?') >= 0 ? '&' : '?';
      return sseUrl + sep + 'password=' + encodeURIComponent(sessionPassword);
    }
    return sseUrl;
  }

  function connect() {
    setStatus('Connecting...');

    /* Probe the SSE endpoint with fetch first to detect password gates.
     * EventSource does not expose HTTP status codes, so we cannot
     * distinguish a 403 from a network error without this probe. */
    fetch(buildSseUrl(), { method: 'GET' }).then(function(resp) {
      if (resp.status === 403) {
        resp.text().then(function(body) {
          if (body === 'password_required') {
            showPasswordPrompt(false);
          } else if (body === 'wrong_password') {
            sessionPassword = '';
            sessionStorage.removeItem('tmtv_pw_' + (sessionToken || ''));
            showPasswordPrompt(true);
          } else {
            showErrorOverlay('Access denied.', false);
          }
        });
        return;
      }
      /* Token is valid and no password gate (or password accepted).
       * Cancel the probe response body so it doesn't linger as a ghost
       * SSE connection that inflates the web viewer count. */
      if (resp.body && resp.body.cancel) {
        resp.body.cancel();
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

    es.onopen = function() {
      setStatus('Connected', 'connected');
      reconnectAttempts = 0;
      everConnected = true;
      hidePasswordPrompt();
      if (!sessionStart) {
        sessionStart = Date.now();
        durationTimer = setInterval(updateMeta, 1000);
      }
    };

    es.onmessage = function(evt) {
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
      es.close();
      if (sessionEnded) {
        setStatus('Session ended', 'error');
        if (durationTimer) clearInterval(durationTimer);
        showErrorOverlay('The host ended this terminal session.', true);
        return;
      }
      if (reconnectAttempts < maxReconnect) {
        reconnectAttempts++;
        var delay = Math.min(1000 * Math.pow(2, reconnectAttempts - 1), 8000);
        setStatus('Reconnecting (' + reconnectAttempts + '/' + maxReconnect + ')...', 'error');
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
          var pane = panes[paneId];
          if (pane) {
            pane.term.write(ptyData instanceof Uint8Array ? ptyData : String(ptyData));
            if (pane.needsRefresh) {
              pane.needsRefresh = false;
              (function(p) {
                setTimeout(function() { p.term.refresh(0, p.sy - 1); }, 50);
              })(pane);
            }
          } else {
            if (!pendingData[paneId]) pendingData[paneId] = [];
            pendingData[paneId].push(ptyData);
          }
        }
        break;
      case OUT_SYNC_LAYOUT:
        if (inner.length >= 3) {
          var sx = inner[1], sy = inner[2];
          if (sx > 0 && sy > 0) {
            serverCols = sx; serverRows = sy;
            window.serverCols = sx; window.serverRows = sy;
          }
          if (inner.length >= 5) {
            updateWindowList(inner[3], inner[4]);
            updatePaneLayout(inner[3], inner[4]);
          }
          sizeToServer();
        }
        break;
      case OUT_STATUS:
        if (inner.length >= 3) {
          var leftEl = document.getElementById('tmux-status-left');
          var rightEl = document.getElementById('tmux-status-right');
          if (leftEl && inner[1]) leftEl.textContent = toStr(inner[1]);
          if (rightEl && inner[2]) rightEl.textContent = toStr(inner[2]);
        }
        break;
      case OUT_VIEWER_COUNT:
        if (inner.length >= 4) {
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
