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
import select
import socket
import struct
import subprocess
import termios
import time

import msgpack
import pyte

ROOT = Path("/tmp/e2e")
SOCKET = str(ROOT / "host.sock")
PORT = 2222
WEB = 4002


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
    # tmux queries DEC private cursor status; this passive test decoder does
    # not emulate terminal replies. pyte otherwise rejects the private flag.
    def report_device_status(self, mode, **kwargs):
        if not kwargs.get("private"):
            super().report_device_status(mode)


class Viewer:
    def __init__(self, token, web=False):
        self.web = web
        self.screen = Screen(100, 30)
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
            self.proc = subprocess.Popen(["ssh", "-F", "/dev/null", "-tt", "-p", str(PORT),
                "-o", "StrictHostKeyChecking=yes", "-o", "UserKnownHostsFile=" + str(ROOT / "known_hosts"),
                "-o", "BatchMode=yes", "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=3",
                token + "@127.0.0.1"], stdin=slave, stdout=slave, stderr=slave)
            os.close(slave)

    def feed(self, data):
        self.terminal.feed(self.decoder.decode(data))

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
set -g tmtv-server-port {PORT}
set -g tmtv-server-ed25519-fingerprint "{fp}"
set -g tmtv-session-name e2e
set -g tmtv-web-sharing on
set -g tmtv-web-input on
set -g status off
set -g default-shell /bin/sh
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
    viewers = []
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
        wait_for(lambda: any(p.name.endswith("-e2e") and not p.name.startswith("ro-") for p in sessions.iterdir()), "authenticated host registration")
        rw = next(p.name for p in sessions.iterdir() if p.name.endswith("-e2e") and not p.name.startswith("ro-"))
        ro = "ro-e2e"
        print("PASS real host registers with pinned relay host key", flush=True)
        status, _ = request("HEAD", "/invalid-e2e-token")
        assert status in (403, 404), status
        wait_for(lambda: request("HEAD", "/" + rw)[0] == 200, "RW token authorization")
        wait_for(lambda: request("HEAD", "/" + ro)[0] == 200, "RO token authorization")
        print("PASS web token authorization accepts RW/RO and rejects an invalid token", flush=True)
        rejected = Viewer("invalid-e2e-token")
        viewers.append(rejected)
        wait_for(lambda: rejected.proc.poll() is not None, "invalid SSH token is disconnected")
        assert rejected.proc.returncode != 0, rejected.text()
        rejected.close()
        viewers.remove(rejected)
        print("PASS invalid SSH token cannot open a viewer session", flush=True)
        ssh_rw = Viewer(rw)
        ssh_ro = Viewer(ro)
        web_rw = Viewer(rw, web=True)
        web_ro = Viewer(ro, web=True)
        viewers.extend([ssh_rw, ssh_ro, web_rw, web_ro])
        host("send-keys", "-l", "printf '\\033[2J\\033[HBASE_READY\\n'")
        host("send-keys", "Enter")
        for viewer in viewers:
            viewer.sees("BASE_READY")
        assert web_rw.viewer_id and web_rw.readonly is False and web_rw.input_enabled is True
        assert web_ro.viewer_id and web_ro.readonly is True and web_ro.input_enabled is False
        print("PASS SSH and SSE RW/RO viewers render parsed terminal output", flush=True)
        ssh_rw.send(b"printf 'SSH_RW_RESULT\\n'; echo yes > /tmp/e2e/ssh-rw\r")
        wait_for(lambda: (ROOT / "ssh-rw").exists(), "SSH RW input reaches host")
        for viewer in viewers:
            viewer.sees("SSH_RW_RESULT")
        print("PASS SSH RW input reaches the real host and all decoded viewers", flush=True)
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
        for viewer in viewers:
            viewer.close()
        viewers.clear()
        host("send-keys", "-l", "printf '\\033[2J\\033[HLATE_VIEWER_READY\\n'")
        host("send-keys", "Enter")
        wait_for(lambda: "LATE_VIEWER_READY" in host("capture-pane", "-p"), "host late-viewer marker")
        for web in (False, True):
            viewer = Viewer(ro, web=web)
            viewers.append(viewer)
            viewer.sees("LATE_VIEWER_READY")
        print("PASS fresh SSH/SSE RO viewers reconstruct the current screen", flush=True)
        print("7 end-to-end checks passed; browser DOM, host transport reconnect, copy/view mode and SIXEL remain separate coverage.", flush=True)
    except BaseException:
        with open(ROOT / "diagnostics.log", "w") as out:
            out.write("session entries: " + repr([p.name for p in Path('/tmp/tmtv/sessions').iterdir()]) + "\n")
            out.write("host screen:\n" + host("capture-pane", "-p") + "\n")
            for viewer in viewers:
                out.write("viewer screen:\n" + viewer.text() + "\n")
            out.write(subprocess.run(["tmtv", "-S", SOCKET, "show-messages"], capture_output=True, text=True, timeout=3).stdout)
            for path in ROOT.glob("*.log"):
                if path.name != 'diagnostics.log':
                    out.write("\n" + path.name + "\n" + path.read_text(errors='replace')[-20000:])
        raise
    finally:
        for viewer in viewers:
            viewer.close()
        subprocess.run(["tmtv", "-S", SOCKET, "kill-server"], capture_output=True, timeout=5)
        relay.terminate()
        try:
            relay.wait(timeout=5)
        except subprocess.TimeoutExpired:
            relay.kill()
            relay.wait()
        relay_log.close()


if __name__ == "__main__":
    main()
