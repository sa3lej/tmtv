#!/usr/bin/env python3
"""Inside-container authenticated production host/relay transport regression.

SSH and web SSE terminal bytes are decoded with pyte, not searched as raw VT.
This exercises the web transport, not the browser DOM or full-fidelity SIXEL.
"""
import base64
import codecs
import fcntl
import http.client
import json
import os
from pathlib import Path
import pty
import re
import select
import shlex
import socket
import struct
import subprocess
import termios
import threading
import time
import unicodedata

import msgpack
import pyte
from wcwidth import wcwidth

ROOT = Path("/tmp/e2e")
SOCKET = str(ROOT / "host.sock")
PORT = 2222
WEB = 4002
HOST_PORT = 2223


class HostTransport:
    """Opaque TCP fault injector; viewers connect directly to the real relay."""
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", HOST_PORT))
        self.listener.listen(4)
        self.listener.settimeout(.1)
        self.lock = threading.Lock()
        self.connections = ()
        self.generation = 0
        self.paused = False
        self.stopping = False
        self.error = None
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            while not self.stopping:
                try:
                    client, _ = self.listener.accept()
                except socket.timeout:
                    continue
                with client:
                    if self.paused:
                        continue
                    with socket.create_connection(("127.0.0.1", PORT), 3) as upstream:
                        with self.lock:
                            self.connections = (client, upstream)
                            self.generation += 1
                        try:
                            # The relay must survive a delayed first host frame,
                            # including larger reconnect snapshots.
                            time.sleep(.15)
                            while not self.stopping and not self.paused:
                                ready, _, _ = select.select([client, upstream], [], [], .1)
                                for source in ready:
                                    data = source.recv(65536)
                                    if not data:
                                        raise EOFError
                                    (upstream if source is client else client).sendall(data)
                        except (EOFError, ConnectionError):
                            pass
                        finally:
                            with self.lock:
                                self.connections = ()
        except BaseException as error:
            if not self.stopping:
                self.error = repr(error)

    def cut(self):
        self.paused = True
        with self.lock:
            assert self.connections and self.error is None
            for connection in self.connections:
                connection.shutdown(socket.SHUT_RDWR)
        wait_for(lambda: not self.connections, "host TCP transport closes")

    def close(self):
        self.stopping = True
        self.thread.join(timeout=5)
        self.listener.close()
        assert not self.thread.is_alive(), "host TCP proxy did not stop"


def wait_for(predicate, label, seconds=15):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if predicate():
            return
        time.sleep(.05)
    raise AssertionError("timeout: " + label)


def command(args, **kw):
    return subprocess.run(args, check=True, capture_output=True, text=True, timeout=10, **kw).stdout.strip()


def host(*args):
    return command(["tmtv", "-S", SOCKET, *args])


def request(method, path, data=None, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", WEB, timeout=3)
    try:
        conn.request(method, path, body=data, headers=headers or {})
        response = conn.getresponse()
        return response.status, response.read()
    finally:
        conn.close()


class Screen(pyte.Screen):
    @property
    def display(self):
        # pyte leaves an empty wide-character stub when later narrow text
        # overwrites its leading cell. Real terminals clear that orphan to a
        # space; pyte's display property instead indexes char[0] and crashes.
        result = []
        for y in range(self.lines):
            chars, skip = [], False
            for x in range(self.columns):
                if skip:
                    skip = False
                    continue
                text = self.buffer[y][x].data
                chars.append(text or " ")
                skip = bool(text) and wcwidth(text[0]) == 2
            result.append("".join(chars))
        return result

    # tmux queries DEC private cursor status; this passive test decoder does
    # not emulate terminal replies. pyte otherwise rejects the private flag.
    def report_device_status(self, mode, **kwargs):
        if not kwargs.get("private"):
            super().report_device_status(mode)


class Viewer:
    def __init__(self, token, web=False, local=False):
        self.web = web
        self.raw = bytearray()
        self.text_pending = bytearray()
        # SSE's composed virtual PTY includes the host's 30 rows plus its
        # relay status row. SSH declares a 30-row physical terminal below.
        self.screen = Screen(100, 31 if web else 30)
        self.terminal = pyte.Stream(self.screen)
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self.proc = None
        self.conn = None
        self.viewer_id = None
        self.readonly = None
        self.input_enabled = None
        if web:
            self.conn = socket.create_connection(("127.0.0.1", WEB), 3)
            self.conn.sendall(f"GET /{token} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())
            self.fd = self.conn.fileno()
            self.pending = b""
            self.headers = False
            self.unpacker = msgpack.Unpacker(raw=False, strict_map_key=False)
        else:
            self.fd, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
            argv = ["tmtv", "-S", SOCKET, "attach-session", "-t", "main"] if local else ["ssh", "-F", "/dev/null", "-tt", "-p", str(PORT),
                "-o", "StrictHostKeyChecking=yes", "-o", "UserKnownHostsFile=" + str(ROOT / "known_hosts"),
                "-o", "BatchMode=yes", "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=3",
                token + "@127.0.0.1"]
            self.proc = subprocess.Popen(argv, stdin=slave, stdout=slave, stderr=slave)
            os.close(slave)

    def feed(self, data):
        self.raw.extend(data)
        if len(self.raw) > 4 * 1024 * 1024:
            del self.raw[:-4 * 1024 * 1024]
        # pyte implements text terminal state but not OSC 8 or SIXEL. Keep
        # complete control strings in raw for dedicated assertions, and remove
        # only these two unsupported controls from its text input. Buffer across
        # transport chunks so their contents never become spurious text.
        self.text_pending.extend(data)
        source, output, offset = self.text_pending, bytearray(), 0
        while offset < len(source):
            if source[offset] != 27:
                output.append(source[offset]); offset += 1; continue
            if offset + 1 == len(source):
                break
            kind = source[offset + 1]
            if kind in (ord("P"), ord("]")):
                end = source.find(b"\x1b\\", offset + 2)
                step = 2
                bel = source.find(b"\x07", offset + 2) if kind == ord("]") else -1
                if bel >= 0 and (end < 0 or bel < end):
                    end, step = bel, 1
                if end < 0:
                    break
                if kind == ord("]") and not source[offset + 2:end].startswith(b"8;"):
                    output.extend(source[offset:end + step])
                offset = end + step
                continue
            output.append(source[offset]); offset += 1
        del source[:offset]
        assert len(source) <= 4 * 1024 * 1024, "unterminated test control string"
        self.terminal.feed(self.decoder.decode(output))

    def pump(self):
        while select.select([self.fd], [], [], .01)[0]:
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                return
            if not data:
                return
            if not self.web:
                self.feed(data)
                continue
            self.pending += data
            if not self.headers:
                if b"\r\n\r\n" not in self.pending:
                    continue
                head, self.pending = self.pending.split(b"\r\n\r\n", 1)
                assert head.startswith(b"HTTP/1.1 200"), head
                self.headers = True
            while b"\n\n" in self.pending:
                event, self.pending = self.pending.split(b"\n\n", 1)
                for line in event.splitlines():
                    if not line.startswith(b"data: "):
                        continue
                    self.unpacker.feed(base64.b64decode(line[6:], validate=True))
                    for message in self.unpacker:
                        if message[0] == 1 and message[1][0] == 15:
                            self.readonly, self.input_enabled, self.viewer_id = message[1][1:4]
                        if message[0] == 1 and message[1][0] == 2 and message[1][1] == -1:
                            payload = message[1][2]
                            self.feed(payload.encode() if isinstance(payload, str) else payload)

    def text(self):
        self.pump()
        return "\n".join(self.screen.display)

    def sees(self, marker):
        wait_for(lambda: marker in self.text(), "decoded " + ("web" if self.web else "SSH") + " terminal: " + marker)

    def send(self, data):
        os.write(self.fd, data)

    def close(self):
        if self.conn:
            self.conn.close()
        else:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
            os.close(self.fd)


def main():
    ROOT.mkdir(mode=0o700)
    keys = ROOT / "keys"
    keys.mkdir()
    for kind in ("rsa", "ed25519"):
        command(["ssh-keygen", "-q", "-t", kind, "-N", "", "-f", str(keys / ("ssh_host_" + kind + "_key"))])
    pub = (keys / "ssh_host_ed25519_key.pub").read_text().split()
    (ROOT / "known_hosts").write_text(f"[127.0.0.1]:{PORT} {pub[0]} {pub[1]}\n")
    fp = command(["ssh-keygen", "-lf", str(keys / "ssh_host_ed25519_key.pub"), "-E", "sha256"]).split()[1]
    conf = ROOT / "tmtv.conf"
    conf.write_text(f'''set -g tmtv-server-host 127.0.0.1
set -g tmtv-server-port {HOST_PORT}
set -g tmtv-server-ed25519-fingerprint "{fp}"
set -g tmtv-session-name e2e
set -g tmtv-web-sharing on
set -g tmtv-web-input on
set -g status off
set -g default-shell /bin/sh
set -g invalid-option-E2E_VIEW yes
''')
    relay_log = open(ROOT / "relay.log", "w")
    relay_args = ["tmtv-server", "-vv", "-b", "127.0.0.1", "-h", "localhost", "-k", str(keys),
                  "-p", str(PORT), "-z", str(WEB), "-T", "120"]
    if os.environ.get("TMTV_E2E_TRACE") == "1":
        relay_args = ["strace", "-f", "-e", "trace=process,signal,write,openat", "-s", "160", "-o", str(ROOT / "trace.log"), *relay_args]
    if os.environ.get("TMTV_E2E_GDB") == "1":
        relay_args = ["gdb", "-nx", "-batch", "-ex", "set debuginfod enabled off", "-ex", "set follow-fork-mode child",
                      "-ex", "handle SIGPIPE nostop noprint pass", "-ex", "run", "-ex", "thread apply all bt", "--args", *relay_args]
    relay = subprocess.Popen(relay_args, cwd=ROOT, stdout=relay_log, stderr=relay_log)
    transport = HostTransport()
    viewers = []
    local_viewers = []
    try:
        def healthy():
            try:
                status, body = request("GET", "/healthz")
                return status == 200 and json.loads(body)["status"] == "ok"
            except (OSError, ValueError):
                return False
        wait_for(healthy, "relay health")
        master, slave = pty.openpty()
        process = subprocess.Popen(["tmtv", "-vv", "-S", SOCKET, "-f", str(conf), "new-session", "-d", "-s", "main", "-x", "100", "-y", "30", "/bin/sh -i"],
                                   cwd=ROOT, stdin=slave, stdout=slave, stderr=slave)
        os.close(slave)
        try:
            assert process.wait(timeout=10) == 0
        finally:
            os.close(master)
        sessions = Path("/tmp/tmtv/sessions")
        if os.environ.get("TMTV_E2E_EXPECT_LAYOUT_REJECTION") == "1":
            def relay_refused():
                notice = host("display-message", "-p", "#{tmtv_snapshot_error}|#{tmtv_layout_error}")
                # Older relays used "Client update required" for either
                # direction. Require their explicit protocol mismatch too.
                return ("Relay update required" in notice or
                        ("requires sharing protocol" in notice and "uses 11" in notice))
            wait_for(relay_refused, "unsupported relay reports required update")
            assert host("display-message", "-p", "#{tmtv_ssh}|#{tmtv_web}") == "|"
            assert "e2e" not in [p.name for p in sessions.iterdir()]
            host("send-keys", "-l", "printf 'LOCAL_STILL_WORKS\\n'")
            host("send-keys", "Enter")
            wait_for(lambda: "LOCAL_STILL_WORKS" in host("capture-pane", "-p"), "local terminal remains usable")
            print("PASS unsupported relay gets explicit update requirement, no sharing URL, and usable local terminal", flush=True)
            return
        if os.environ.get("TMTV_E2E_EXPECT_HOST_REJECTION") == "1":
            # Use the genuine previous host binary. Its normal NOTIFY handler
            # must see the notice while the SSH channel is still alive.
            wait_for(lambda: "Client update required" in host("display-message", "-p", "#{tmtv_snapshot_error}"), "old host receives update-required response")
            old_local = Viewer(None, local=True)
            local_viewers.append(old_local)
            old_local.sees("Client update required")
            old_local.sees("curl -fsSL")
            assert host("display-message", "-p", "#{tmtv_ssh}|#{tmtv_web}") == "|"
            assert not any(p.name.endswith("-e2e") for p in sessions.iterdir())
            old_local.send(b"q")
            old_local.pump()
            old_local.close()
            local_viewers.remove(old_local)
            host("send-keys", "-l", "printf 'OLD_LOCAL_WORKS\\n'")
            host("send-keys", "Enter")
            wait_for(lambda: "OLD_LOCAL_WORKS" in host("capture-pane", "-p"), "old host retains usable local terminal")
            print("PASS genuine old host receives update notice and install command before refusal; no URL, local terminal usable", flush=True)
            return
        wait_for(lambda: any(p.name.endswith("-e2e") and not p.name.startswith("ro-") for p in sessions.iterdir()), "authenticated host registration")
        rw = next(p.name for p in sessions.iterdir() if p.name.endswith("-e2e") and not p.name.startswith("ro-"))
        ro = "ro-e2e"
        print("PASS real host registers with pinned relay host key", flush=True)
        status, _ = request("HEAD", "/invalid-e2e-token")
        assert status in (403, 404), status
        wait_for(lambda: request("HEAD", "/" + rw)[0] == 200, "RW token authorization")
        wait_for(lambda: request("HEAD", "/" + ro)[0] == 200, "RO token authorization")
        print("PASS web token authorization accepts RW/RO and rejects an invalid token", flush=True)
        if os.environ.get('TMTV_E2E_BROWSER') == '1':
            from relay_browser_fixture import run_browser
            run_browser(rw, ro, transport, host, request, wait_for, ROOT)
            return
        rejected = Viewer("invalid-e2e-token")
        viewers.append(rejected)
        wait_for(lambda: rejected.proc.poll() is not None, "invalid SSH token is disconnected")
        assert rejected.proc.returncode != 0, rejected.text()
        rejected.close()
        viewers.remove(rejected)
        print("PASS invalid SSH token cannot open a viewer session", flush=True)
        host("link-window", "-s", "main:0", "-t", "main:9")
        host("select-window", "-t", "main:9")
        host("rename-window", "ALIAS_SHARED")
        ssh_rw = Viewer(rw)
        ssh_ro = Viewer(ro)
        web_rw = Viewer(rw, web=True)
        web_ro = Viewer(ro, web=True)
        viewers.extend([ssh_rw, ssh_ro, web_rw, web_ro])
        host("send-keys", "-l", "printf '\\033[2J\\033[HBASE_READY\\n'")
        host("send-keys", "Enter")
        for viewer in viewers:
            viewer.sees("BASE_READY")
            viewer.sees("9:ALIAS_SHARED*")
        assert host("list-windows", "-F", "#{window_id}").splitlines()[0] == host("list-windows", "-F", "#{window_id}").splitlines()[1]
        print("PASS linked window aliases share one graph and preserve the selected index in SSH/SSE", flush=True)
        assert web_rw.viewer_id and web_rw.readonly is False and web_rw.input_enabled is True
        assert web_ro.viewer_id and web_ro.readonly is True and web_ro.input_enabled is False
        print("PASS SSH and SSE RW/RO viewers render parsed terminal output", flush=True)
        ssh_rw.send(b"printf 'SSH_RW_RESULT\\n'; echo yes > /tmp/e2e/ssh-rw\r")
        wait_for(lambda: (ROOT / "ssh-rw").exists(), "SSH RW input reaches host")
        for viewer in viewers:
            viewer.sees("SSH_RW_RESULT")
        print("PASS SSH RW input reaches the real host and all decoded viewers", flush=True)
        # Check bytes written by the host shell, not terminal echo. Pace scalar
        # input so it exercises tty key decoding rather than assume-paste.
        unicode_text = "å界🦊"
        unicode_command = "printf '%s' '" + unicode_text + "' > /tmp/e2e/ssh-unicode\r"
        for char in unicode_command:
            ssh_rw.send(char.encode("utf-8"))
            time.sleep(0.02)
        wait_for(lambda: (ROOT / "ssh-unicode").exists(), "Unicode SSH input reaches host")
        assert (ROOT / "ssh-unicode").read_bytes() == unicode_text.encode("utf-8")
        # Explicit bracketed paste must preserve raw UTF-8 bytes as well.
        ssh_rw.send(("\x1b[200~printf '%s' '" + unicode_text +
                     "' > /tmp/e2e/ssh-unicode-paste\x1b[201~\r").encode("utf-8"))
        wait_for(lambda: (ROOT / "ssh-unicode-paste").exists(), "Unicode SSH paste reaches host")
        assert (ROOT / "ssh-unicode-paste").read_bytes() == unicode_text.encode("utf-8")
        print("PASS Unicode SSH keys and pasted UTF-8 preserve host-side bytes", flush=True)
        ssh_ro.send(b"echo BAD > /tmp/e2e/ssh-ro-forbidden\r")
        input_headers = {"X-Tmtv-Input": "1", "X-Tmtv-Viewer-Id": str(web_rw.viewer_id)}
        assert request("POST", "/" + ro + "/input", b"echo BAD > /tmp/e2e/web-ro-forbidden\r", input_headers)[0] == 403
        assert request("POST", "/" + rw + "/input", b"ignored", {})[0] == 400
        assert request("POST", "/" + rw + "/input", b"printf 'WEB_RW_RESULT\\n'; echo yes > /tmp/e2e/web-rw\r", input_headers)[0] == 200
        wait_for(lambda: (ROOT / "web-rw").exists(), "web RW input reaches host after RO attempts")
        for viewer in viewers:
            viewer.sees("WEB_RW_RESULT")
        assert not (ROOT / "ssh-ro-forbidden").exists()
        assert not (ROOT / "web-ro-forbidden").exists()
        print("PASS SSH/SSE RO input isolation and web CSRF gate; RW control remains usable", flush=True)

        # Exercise special keys through the actual SSH decoder and stable wire
        # numbers, rather than only ASCII or host-injected send-keys.
        sequences = [b"\x1b[A", b"\x1b[B", b"\x1b[D", b"\x1b[C", b"\x1b[3~", b"\x1bOP"]
        expected = b"".join(sequences)
        script = ("import os,termios,tty; original=termios.tcgetattr(0); tty.setraw(0); "
                  "print('SPECIAL_KEYS_READY',flush=True); data=b''; "
                  f"exec(\"while len(data)<{len(expected)}: data+=os.read(0,{len(expected)}-len(data))\"); "
                  "open('/tmp/e2e/special-keys','w').write(data.hex()); "
                  "termios.tcsetattr(0,termios.TCSANOW,original)")
        host("send-keys", "-l", "python3 -c " + shlex.quote(script))
        host("send-keys", "Enter")
        ssh_rw.sees("SPECIAL_KEYS_READY")
        for sequence in sequences:
            ssh_rw.send(sequence)
            time.sleep(.03)
        wait_for(lambda: (ROOT / "special-keys").exists(), "SSH special keys reach raw host input")
        assert (ROOT / "special-keys").read_text() == expected.hex()
        print("PASS SSH arrows, Delete and F1 retain legacy wire numbers and host key semantics", flush=True)

        # Observe real mode commands through decoded viewer terminals, not just
        # host state or echoed input. Enter without a selection, then move/select.
        host("copy-mode")
        host("send-keys", "-X", "history-top")
        host("send-keys", "-X", "cursor-down")
        host("send-keys", "-X", "cursor-right")
        def copy_cursor():
            return tuple(map(int, host("display-message", "-p", "#{copy_cursor_x},#{copy_cursor_y}").split(",")))
        def cursor_matches(viewer):
            viewer.pump()
            return (viewer.screen.cursor.x, viewer.screen.cursor.y) == copy_cursor()

        def reconnect_host(check, label):
            # Only the encrypted host TCP connection is cut. Existing viewers
            # observe that loss; fresh viewers then use the exact original links.
            aliases = {p.name: os.readlink(p) for p in sessions.iterdir() if p.is_symlink()}
            old_target = aliases[rw]
            old_generation = transport.generation
            before = host("display-message", "-p", "#{pane_mode}|#{copy_cursor_x},#{copy_cursor_y}|#{selection_present}|#{scroll_position}")
            transport.cut()
            transport.paused = False
            wait_for(lambda: transport.generation > old_generation and (sessions / rw).is_symlink()
                     and os.readlink(sessions / rw) != old_target and request("HEAD", "/" + rw)[0] == 200,
                     "host reconnect reclaims stable named token", seconds=45)
            assert transport.error is None, transport.error
            assert host("display-message", "-p", "#{pane_mode}|#{copy_cursor_x},#{copy_cursor_y}|#{selection_present}|#{scroll_position}") == before
            target = os.readlink(sessions / rw)
            for name in aliases:
                assert (sessions / name).is_symlink() and os.readlink(sessions / name) == target, "alias lost: " + name
            for viewer in viewers:
                viewer.close()
            viewers.clear()
            for token in (rw, ro):
                for web in (False, True):
                    viewer = Viewer(token, web=web)
                    viewers.append(viewer)
                    wait_for(lambda: check(viewer), label)
            assert viewers[1].readonly is False and viewers[1].input_enabled is True
            assert viewers[3].readonly is True and viewers[3].input_enabled is False

        for viewer in viewers:
            wait_for(lambda: cursor_matches(viewer), "copy cursor reaches viewer without selection")
        print("PASS copy-mode entry and cursor movement reach SSH/SSE viewers without a selection", flush=True)
        reconnect_host(cursor_matches, "reconnected viewer restores unselected copy cursor")
        print("PASS real host TCP reconnect restores unselected copy state through unchanged links", flush=True)
        host("send-keys", "-X", "begin-selection")
        host("send-keys", "-X", "cursor-right")
        host("send-keys", "-X", "cursor-right")
        expected_cursor = copy_cursor()
        def selection_matches(viewer):
            viewer.pump()
            x, y = expected_cursor
            selected = viewer.screen.buffer[y][x - 1]
            normal = viewer.screen.buffer[y][x + 1]
            return viewer.screen.display[y].startswith("BASE_READY") and (selected.fg, selected.bg, selected.reverse) != (normal.fg, normal.bg, normal.reverse)
        for viewer in viewers:
            wait_for(lambda: selection_matches(viewer), "copy selection reaches viewer")
        # Freeze copy backing, then change the live shell grid independently.
        # Reconnect must restore both grids, not clone the new live output.
        pane_tty = host("display-message", "-p", "#{pane_tty}")
        host("run-shell", "printf '\\033[2J\\033[HLIVE_WHILE_COPY\\n' > " + pane_tty)
        wait_for(lambda: "LIVE_WHILE_COPY" in host("capture-pane", "-p"), "live base changes underneath frozen copy mode")
        reconnect_host(selection_matches, "reconnected viewer restores copy selection")
        print("PASS real host TCP reconnect retains links and restores active copy selection/cursor", flush=True)
        host("send-keys", "-X", "cancel")
        host("send-keys", "-l", "printf 'COPY_EXIT_RESTORED\\n'")
        host("send-keys", "Enter")
        for viewer in viewers:
            viewer.sees("LIVE_WHILE_COPY")
            viewer.sees("COPY_EXIT_RESTORED")
        ssh_rw, web_rw = viewers[0], viewers[1]
        ssh_ro, web_ro = viewers[2], viewers[3]
        ssh_ro.send(b"echo BAD > /tmp/e2e/reconnect-ro-forbidden\r")
        assert request("POST", "/" + ro + "/input", b"echo BAD > /tmp/e2e/reconnect-web-ro-forbidden\r",
                       {"X-Tmtv-Input": "1", "X-Tmtv-Viewer-Id": str(web_rw.viewer_id)})[0] == 403
        ssh_rw.send(b"echo yes > /tmp/e2e/reconnect-rw\r")
        wait_for(lambda: (ROOT / "reconnect-rw").exists(), "RW input after reconnect and copy exit")
        assert not (ROOT / "reconnect-ro-forbidden").exists()
        assert not (ROOT / "reconnect-web-ro-forbidden").exists()
        print("PASS copy exit restores live output and reconnect preserves RO/RW isolation", flush=True)

        # Startup configuration errors use the production literal view-output
        # path when the first local client attaches. No synthetic wire frames.
        local_viewers.append(Viewer(None, local=True))
        wait_for(lambda: host("display-message", "-p", "#{pane_mode}") == "view-mode", "host enters literal view mode")
        for viewer in viewers:
            viewer.sees("invalid-option-E2E_VIEW")
        def view_matches(viewer):
            return "invalid-option-E2E_VIEW" in viewer.text()
        reconnect_host(view_matches, "view backing survives actual host reconnect")
        assert host("display-message", "-p", "#{pane_mode}") == "view-mode"
        print("PASS literal view backing and mode survive real reconnect for SSH/SSE RW/RO viewers", flush=True)
        host("send-keys", "-X", "cancel")
        host("send-keys", "-l", "printf 'VIEW_EXIT_RESTORED\\n'")
        host("send-keys", "Enter")
        for viewer in viewers:
            viewer.sees("VIEW_EXIT_RESTORED")
        print("PASS view exit returns all viewers to the preserved live shell", flush=True)

        # Inject through the actual pane PTY: the host parses these bytes,
        # snapshots its state over SSH, and both viewer transports render it.
        def emit_terminal(data):
            escaped = "".join("\\%03o" % b for b in data)
            host("run-shell", "printf '" + escaped + "' > " + pane_tty)

        emit_terminal(b"\x1b[2J\x1b[H\x1b[38;2;18;52;86;48;2;101;67;33;3;9mRGB_SNAPSHOT " + "e\u0301\u754c".encode())
        def rgb_matches(viewer):
            viewer.pump()
            for row in viewer.screen.buffer.values():
                for col, cell in row.items():
                    if cell.data == "R" and row[col + 1].data == "G" and row[col + 2].data == "B":
                        return cell.fg == "123456" and cell.bg == "654321" and cell.italics and cell.strikethrough
            return False
        for viewer in viewers:
            wait_for(lambda: rgb_matches(viewer) and "\u00e9\u754c" in viewer.text(), "styled Unicode before reconnect")
        reconnect_host(lambda v: rgb_matches(v) and "\u00e9\u754c" in v.text(), "full RGB/style/Unicode snapshot after reconnect")
        emit_terminal(b"_CONTINUED\x1b[0m\r\n")
        for viewer in viewers:
            viewer.sees("_CONTINUED")
        print("PASS truecolour foreground/background, italics, strike and combining/wide Unicode survive real SSH/SSE reconnect", flush=True)

        emit_terminal(b"\x1b[2J\x1b[HPARTIAL_CSI\x1b[38;2;17;34;")
        for viewer in viewers:
            viewer.sees("PARTIAL_CSI")
        reconnect_host(lambda v: "PARTIAL_CSI" in v.text(), "partial CSI snapshot")
        emit_terminal(b"51mCSI_CONTINUED")
        def csi_matches(viewer):
            viewer.pump()
            for row in viewer.screen.buffer.values():
                for col, cell in row.items():
                    if cell.data == "C" and "".join(row[col + j].data for j in range(13)) == "CSI_CONTINUED":
                        return cell.fg == "112233"
            return False
        for viewer in viewers:
            wait_for(lambda: csi_matches(viewer), "continued CSI retains RGB state")
        print("PASS partially received CSI resumes after reconnect with correct subsequent colour", flush=True)

        emit_terminal(b"\x1b[0m\x1b[2J\x1b[HUTF8_PREFIX_\xe7\x95")
        for viewer in viewers:
            viewer.sees("UTF8_PREFIX_")
        reconnect_host(lambda v: "UTF8_PREFIX_" in v.text(), "partial UTF-8 snapshot")
        emit_terminal(b"\x8c_UTF8_COMPLETE\r\n")
        for viewer in viewers:
            viewer.sees("UTF8_PREFIX_\u754c_UTF8_COMPLETE")
        print("PASS partial multibyte UTF-8 resumes after reconnect without replacement or duplicated bytes", flush=True)

        cluster = "\u00f8" + "\u0301" * 15
        assert len(cluster.encode()) == 32
        rendered = unicodedata.normalize("NFC", cluster)
        emit_terminal(b"\x1b[0m\x1b[2J\x1b[HFULL_CLUSTER_" + cluster.encode())
        for viewer in viewers:
            viewer.sees("FULL_CLUSTER_" + rendered)
            assert cluster.encode() in viewer.raw
        assert cluster in host("capture-pane", "-p")
        reconnect_host(lambda v: "FULL_CLUSTER_" + rendered in v.text() and cluster.encode() in v.raw,
                       "32-byte combined cluster survives reconnect")
        emit_terminal(b"_END")
        for viewer in viewers:
            viewer.sees("FULL_CLUSTER_" + rendered + "_END")
        assert cluster + "_END" in host("capture-pane", "-p")
        print("PASS full 32-byte Unicode cluster survives host storage, SSH/SSE reconnect and subsequent output", flush=True)

        # OSC 8 is a semantic screen attribute, not replayed application input.
        # The owned web terminal advertises support. SSH terminals without OSC 8
        # capability retain the text; their optional hyperlink capability is not
        # fabricated by this fixture.
        link = b"https://example.invalid/reconnect-link"
        emit_terminal(b"\x1b[0m\x1b[2J\x1b[H\x1b]8;id=fixture;" + link + b"\x1b\\LINK_SNAPSHOT")
        for viewer in viewers:
            viewer.sees("LINK_SNAPSHOT")
            if viewer.web:
                wait_for(lambda: link in viewer.raw, "web emits OSC 8 before reconnect")
        def link_matches(v):
            v.pump()
            return "LINK_SNAPSHOT" in v.text() and (not v.web or link in v.raw)
        reconnect_host(link_matches, "hyperlink restored from portable table")
        for viewer in viewers:
            viewer.raw.clear()
        emit_terminal(b"_LINK_CONTINUED\x1b]8;;\x1b\\")
        for viewer in viewers:
            viewer.sees("_LINK_CONTINUED")
            if viewer.web:
                wait_for(lambda: link in viewer.raw, "active parser hyperlink survives reconnect")
        print("PASS portable hyperlink tables and active link continuation survive SSH/SSE reconnect; web emits OSC 8", flush=True)

        red = b"\x1bP0;1q\"1;1;3;6#0;2;100;0;0~\x1b\\"
        green = b"\x1bP0;1q\"1;1;3;6#0;2;0;100;0~\x1b\\"
        def has_image(v, colour):
            v.pump()
            # Pixel rows, palette and transparency are checked by production
            # codec tests; here require genuine complete renderer DCS output.
            # Live output may be rescaled to the viewer's cell pixels, so a
            # six-pixel column ('~') can become a shorter nonempty column.
            return any(colour in image and re.search(rb"[@-~][-$]*$", image) for image in
                       re.findall(rb"\x1bP[0-9;]*q(.*?)\x1b\\", bytes(v.raw), re.S))
        emit_terminal(b"\x1b[2J\x1b[HBASE_IMAGE_STATE\x1b[3;5H" + red)
        for viewer in viewers:
            if viewer.web:
                wait_for(lambda: has_image(viewer, b"#0;2;100;0;0"), "SIXEL-capable web renderer sends red image")
        reconnect_host(lambda v: "BASE_IMAGE_STATE" in v.text() and has_image(v, b"#0;2;100;0;0"),
                       "SIXEL image restored from pixels")
        print("PASS current SIXEL bitmap survives actual SSH/SSE host reconnect", flush=True)
        emit_terminal(b"\x1b[?1049h\x1b[HALT_IMAGE_STATE\x1b[3;5H" + green)
        for viewer in viewers:
            if viewer.web:
                wait_for(lambda: has_image(viewer, b"#0;2;0;100;0"), "SIXEL-capable alternate screen green image")
        reconnect_host(lambda v: "ALT_IMAGE_STATE" in v.text() and has_image(v, b"#0;2;0;100;0"),
                       "alternate SIXEL and saved normal screen restored")
        for viewer in viewers:
            viewer.raw.clear()
        emit_terminal(b"\x1b[?1049l")
        for viewer in viewers:
            wait_for(lambda: "BASE_IMAGE_STATE" in viewer.text() and has_image(viewer, b"#0;2;100;0;0"),
                     "saved normal SIXEL returns after alternate exit")
        emit_terminal(b"\x1b[2J\x1b[H")
        print("PASS saved and alternate SIXEL owners survive reconnect and alternate-screen exit", flush=True)

        # Same rectangles and active pane, different stacking order. This was
        # invisible to the legacy layout hash. Observe composed terminal output.
        base = host("display-message", "-p", "#{pane_id}")
        def floating(marker, *flags):
            script = "import time; print('\\033[2J\\033[H" + marker + "',flush=True); time.sleep(300)"
            if "-O" in flags:
                script = ("import time; print('\\033[2J\\033[H" + marker + "',flush=True); "
                          "value=input(); open('/tmp/e2e/modal-input','w').write(value); time.sleep(300)")
            return host("new-pane", "-d", "-P", "-F", "#{pane_id}", "-x", "32", "-y", "8",
                        "-X", "4", "-Y", "3", *flags, "python3 -c " + shlex.quote(script))
        back = floating("FLOAT_BACK")
        front = floating("FLOAT_FRONT")
        def top_is(viewer, marker, hidden):
            text = viewer.text()
            return marker in text and hidden not in text
        for viewer in viewers:
            wait_for(lambda v=viewer: top_is(v, "FLOAT_FRONT", "FLOAT_BACK"), "front floating pane covers back")
        host("move-pane", "-t", back, "-P", "front")
        assert host("display-message", "-p", "#{pane_id}") == base
        for viewer in viewers:
            wait_for(lambda v=viewer: top_is(v, "FLOAT_BACK", "FLOAT_FRONT"), "layer-only change reaches viewer")
        print("PASS overlapping floats and layer-only changes reach SSH/SSE RW/RO viewers", flush=True)
        reconnect_host(lambda v: top_is(v, "FLOAT_BACK", "FLOAT_FRONT"), "reconnect restores floating layer order")
        print("PASS real reconnect restores exact floating geometry and layer order", flush=True)
        host("kill-pane", "-t", front)
        host("kill-pane", "-t", back)
        host("split-window", "-d", "-h", "-t", base, "/bin/sh -i")
        tiled = host("display-message", "-p", "-t", base, "#{window_panes}")
        assert tiled == "2", tiled
        host("resize-pane", "-Z", "-t", base)
        modal = floating("FLOAT_MODAL", "-O")
        for viewer in viewers:
            viewer.sees("FLOAT_MODAL")
        reconnect_host(lambda v: "FLOAT_MODAL" in v.text(), "modal float over zoom survives reconnect")
        viewers[2].send(b"RO_FORBIDDEN\r")
        viewers[0].send(b"MODAL_INPUT\r")
        wait_for(lambda: (ROOT / "modal-input").exists(), "RW input reaches modal pane after reconnect")
        assert (ROOT / "modal-input").read_text() == "MODAL_INPUT"
        host("kill-pane", "-t", modal)
        assert host("display-message", "-p", "-t", base, "#{window_zoomed_flag}") == "1"
        host("resize-pane", "-Z", "-t", base)
        assert host("display-message", "-p", "-t", base, "#{window_panes}") == "2"
        other = next(p for p in host("list-panes", "-F", "#{pane_id}").splitlines() if p != base)
        host("kill-pane", "-t", other)
        host("select-pane", "-t", base)
        print("PASS modal input/RO isolation, float over zoom, reconnect and saved tiled layout", flush=True)
        host("unlink-window", "-t", "main:0")
        host("move-window", "-s", "main:9", "-t", "main:3")
        host("new-window", "-d", "-t", "main:9", "-n", "REUSED_INDEX", "/bin/sh -i")
        host("select-window", "-t", "main:9")
        host("send-keys", "-l", "printf 'NEW_WINDOW_AT_REUSED_INDEX\\n'")
        host("send-keys", "Enter")
        for viewer in viewers:
            viewer.sees("NEW_WINDOW_AT_REUSED_INDEX")
        host("select-window", "-t", "main:3")
        assert host("display-message", "-p", "#{pane_id}") == base
        host("link-window", "-s", "main:3", "-t", "main:7")
        host("select-window", "-t", "main:7")
        host("send-keys", "-l", "printf 'ALIASED_WINDOW_RETAINED\\n'")
        host("send-keys", "Enter")
        def alias_matches(viewer):
            text = viewer.text()
            return "ALIASED_WINDOW_RETAINED" in text and "7:ALIAS_SHARED*" in text and "NEW_WINDOW_AT_REUSED_INDEX" not in text
        for viewer in viewers:
            wait_for(lambda v=viewer: alias_matches(v), "canonical removal, renumber and index reuse preserve correct window")
        print("PASS canonical link removal, renumbering and index reuse retain the correct window", flush=True)
        reconnect_host(alias_matches, "reconnect restores aliases and selected index after index reuse")
        print("PASS reconnect restores shared aliases and selected index after link churn", flush=True)
        for viewer in viewers:
            viewer.close()
        for viewer in local_viewers:
            viewer.close()
        local_viewers.clear()
        viewers.clear()
        host("send-keys", "-l", "printf '\\033[2J\\033[HLATE_VIEWER_READY\\n'")
        host("send-keys", "Enter")
        wait_for(lambda: "LATE_VIEWER_READY" in host("capture-pane", "-p"), "host late-viewer marker")
        for web in (False, True):
            viewer = Viewer(ro, web=web)
            viewers.append(viewer)
            viewer.sees("LATE_VIEWER_READY")
        print("PASS fresh SSH/SSE RO viewers reconstruct the current screen", flush=True)
        print("27 end-to-end checks passed; browser DOM, terminal capability discovery, other attributes and platforms remain separate coverage.", flush=True)
    except BaseException:
        with open(ROOT / "diagnostics.log", "w") as out:
            out.write("session entries: " + repr([p.name for p in Path('/tmp/tmtv/sessions').iterdir()]) + "\n")
            out.write("snapshot error: " + host("display-message", "-p", "#{tmtv_snapshot_error}|#{tmtv_layout_error}") + "\n")
            out.write("host mode: " + host("display-message", "-p", "#{pane_mode}|#{copy_cursor_x},#{copy_cursor_y}|#{selection_present}|#{scroll_position}") + "\n")
            out.write("host screen:\n" + host("capture-pane", "-p") + "\n")
            for viewer in viewers:
                out.write("viewer cursor: " + repr((viewer.screen.cursor.x, viewer.screen.cursor.y)) + "\n")
                out.write("viewer screen:\n" + viewer.text() + "\n")
                out.write("viewer raw tail: " + repr(bytes(viewer.raw[-8192:])) + "\n")
            out.write(subprocess.run(["tmtv", "-S", SOCKET, "show-messages"], capture_output=True, text=True, timeout=3).stdout)
            for path in ROOT.glob("*.log"):
                if path.name != 'diagnostics.log':
                    out.write("\n" + path.name + "\n" + path.read_text(errors='replace')[-20000:])
        raise
    finally:
        for viewer in viewers:
            viewer.close()
        for viewer in local_viewers:
            viewer.close()
        subprocess.run(["tmtv", "-S", SOCKET, "kill-server"], capture_output=True, timeout=5)
        relay.terminate()
        try:
            relay.wait(timeout=5)
        except subprocess.TimeoutExpired:
            relay.kill()
            relay.wait()
        relay_log.close()
        transport.close()


if __name__ == "__main__":
    main()
