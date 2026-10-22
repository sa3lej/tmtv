"""Optional browser fixture; all live requests stay in the offline container.

Host-side prepare_browser copies an installed Playwright browser and caches only
pinned public CDN assets referenced by the production Astro viewer. No production
server, token, key or host directory is exposed to the browser.
"""
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import threading
import time
from urllib.request import urlopen


def prepare_browser(repo, root, binary, copy_file, web_root, engine):
    def node_eval(source):
        return subprocess.check_output(['node', '-e', source], text=True).strip()
    subprocess.run(['node', '--check', str(repo / 'tests/test-browser-reconnect.js')], check=True, timeout=10)
    package = Path(node_eval("console.log(require.resolve('playwright-core/package.json'))")).parent
    executable = Path(node_eval(f"console.log(require('playwright-core').{engine}.executablePath())"))
    if not executable.exists():
        raise RuntimeError('install the matching Playwright ' + engine + ' first')
    bundle = executable.parent
    shutil.copytree(package, root / 'test/node_modules/playwright-core')
    # Preserve soname aliases: copying each symlink as a separate regular file
    # can load the same WPE library twice, creating conflicting global state.
    shutil.copytree(bundle, root / 'test/browser', symlinks=True)
    (root / 'test/browser.json').write_text(json.dumps({'engine': engine, 'executable': '/test/browser/' + executable.name}))
    binary(shutil.which('node'), '/usr/bin/node')
    # Browser bundles resolve their private shared libraries via wrappers/RPATH.
    # Resolve ldd against the original bundle, but copy only external libraries;
    # the complete private bundle is already installed at /test/browser.
    dependency_env = dict(os.environ)
    if engine == 'webkit':
        dependency_env['LD_LIBRARY_PATH'] = ':'.join(str(bundle / p) for p in (
            'minibrowser-wpe/lib', 'minibrowser-wpe/sys/lib',
            'minibrowser-gtk/lib', 'minibrowser-gtk/sys/lib'))
        for command in ('env', 'bash', 'uname', 'dirname', 'readlink'):
            source = shutil.which(command)
            if source is None:
                raise RuntimeError(command + ' is required by the WebKit wrapper')
            binary(source, ('/usr/bin/' if command == 'env' else '/bin/') + command)
        # WPE's headless display uses Mesa through GLVND/dlopen, so these
        # software-rendering dependencies do not appear in MiniBrowser's ldd.
        vendors = Path('/usr/share/glvnd/egl_vendor.d')
        if not (vendors / '50_mesa.json').is_file():
            raise RuntimeError('WebKit fixture requires Mesa EGL software rendering')
        copy_file(vendors / '50_mesa.json')
        for pattern in ('*-linux-gnu/libEGL_mesa.so.0', '*-linux-gnu/libGLESv2.so.2',
                        '*-linux-gnu/dri/swrast_dri.so'):
            matches = list(Path('/usr/lib').glob(pattern))
            if not matches:
                raise RuntimeError('WebKit fixture requires ' + pattern)
            for lib in matches:
                binary(lib)
        # Recent Mesa splits the Gallium implementation out of swrast and
        # loads it dynamically through GBM; older installations have no split.
        for lib in Path('/usr/lib').glob('*-linux-gnu/libgallium-*.so'):
            binary(lib)
    elif engine == 'firefox':
        dependency_env['LD_LIBRARY_PATH'] = str(bundle)
    for elf in bundle.rglob('*'):
        if elf.is_file() and not elf.is_symlink():
            with elf.open('rb') as stream:
                is_elf = stream.read(4) == b'\x7fELF'
            if is_elf:
                binary(elf, '/test/browser/' + str(elf.relative_to(bundle)),
                       env=dependency_env, bundled_root=bundle)
    # Chromium loads NSS dynamically rather than listing it in ldd chrome.
    for lib in Path('/lib').glob('*-linux-gnu/libnss_*.so.2'):
        binary(lib)
    for directory in ('/usr/share/fonts/truetype/dejavu', '/etc/fonts'):
        if Path(directory).exists():
            shutil.copytree(directory, root / directory.lstrip('/'), symlinks=False, dirs_exist_ok=True)
    html = Path(web_root) / 'viewer/index.html'
    if not html.is_file():
        raise RuntimeError('build site first (npm ci && npm run build in site)')
    copy_file(html, '/test/web/viewer.html')
    copy_file(repo / 'web/viewer.js', '/test/web/viewer.js')
    # Network preparation happens here, before the --network none fixture.
    manifest = {}
    for url in sorted(set(re.findall(r'https://cdn\.jsdelivr\.net/npm/[^"\s<>]+', html.read_text()))):
        name = str(len(manifest)) + ('.css' if url.endswith('.css') else '.js')
        manifest[url] = name
        with urlopen(url, timeout=30) as response:
            payload = response.read(8 * 1024 * 1024 + 1)
        if len(payload) > 8 * 1024 * 1024:
            raise RuntimeError('browser asset exceeded fixture limit')
        (root / 'test/web' / name).write_bytes(payload)
    (root / 'test/web/assets.json').write_text(json.dumps(manifest))
    for name in ('relay_browser_fixture.py', 'test-browser-reconnect.js'):
        copy_file(repo / 'tests' / name, '/test/' + name)


def run_browser(rw, ro, transport, host, relay_request, wait_for, root):
    pane_tty = host('display-message', '-p', '#{pane_tty}')
    web = Path('/test/web')
    (root / 'browser').mkdir(exist_ok=True)
    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'
        def log_message(self, *args):
            pass
        def reply(self, status, body=b'', content_type='application/json'):
            self.send_response(status)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            if self.command != 'HEAD':
                self.wfile.write(body)
        def do_HEAD(self):
            self.do_GET()
        def do_GET(self):
            if self.path.startswith('/ws/'):
                path = self.path[len('/ws'):]
                if self.command == 'HEAD':
                    self.reply(relay_request('HEAD', path)[0])
                    return
                conn = http.client.HTTPConnection('127.0.0.1', 4002, timeout=45)
                capture_path = root / 'browser' / ('stream-' + str(time.monotonic_ns()) + '.sse')
                with capture_path.open('wb') as capture:
                    self.proxy_stream(conn, path, capture)
                return
            path = self.path.split('?')[0]
            if path.startswith('/s/'):
                self.reply(200, (web / 'viewer.html').read_bytes(), 'text/html')
            elif path in ('/viewer.js', '/assets.json') or re.fullmatch(r'/[0-9]+\.(js|css)', path):
                file = web / path[1:]
                self.reply(200, file.read_bytes(), 'text/css' if path.endswith('.css') else 'application/javascript')
            else:
                self.reply(404)
        def proxy_stream(self, conn, path, capture):
            try:
                conn.request('GET', path)
                response = conn.getresponse()
                self.send_response(response.status)
                self.send_header('Content-Type', response.getheader('Content-Type', 'text/event-stream'))
                self.send_header('Connection', 'close')
                self.end_headers()
                while data := response.read1(65536):
                    capture.write(data)
                    capture.flush()
                    self.wfile.write(data)
                    self.wfile.flush()
            except (OSError, http.client.HTTPException):
                pass
            finally:
                conn.close()
                self.close_connection = True
        def do_POST(self):
            data = self.rfile.read(int(self.headers.get('Content-Length', 0)))
            try:
                if self.path == '/_test/emit':
                    escaped = ''.join('\\%03o' % b for b in bytes.fromhex(json.loads(data)['hex']))
                    host('run-shell', "printf '" + escaped + "' > " + pane_tty)
                    self.reply(200)
                elif self.path == '/_test/reconnect':
                    sessions = Path('/tmp/tmtv/sessions')
                    target = os.readlink(sessions / rw)
                    generation = transport.generation
                    transport.cut()
                    transport.paused = False
                    wait_for(lambda: transport.generation > generation and (sessions / rw).is_symlink()
                             and os.readlink(sessions / rw) != target and relay_request('HEAD', '/' + rw)[0] == 200,
                             'browser host reconnect', seconds=45)
                    assert transport.error is None, transport.error
                    self.reply(200)
                elif self.path.startswith('/ws/'):
                    headers = {k: v for k, v in self.headers.items() if k.lower().startswith('x-tmtv-')}
                    status, body = relay_request('POST', self.path[len('/ws'):], data, headers)
                    self.reply(status, body)
                else:
                    self.reply(404)
            except Exception as error:
                self.reply(500, str(error).encode())
    server = ThreadingHTTPServer(('127.0.0.1', 8080), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with (root / 'browser/launch.log').open('w') as diagnostics:
            subprocess.run(['node', '/test/test-browser-reconnect.js', rw, ro], check=True, timeout=160,
                           env={**os.environ, 'DEBUG': 'pw:browser'}, stderr=diagnostics)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
