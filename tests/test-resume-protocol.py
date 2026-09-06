#!/usr/bin/env python3
"""Private-identity protocol tests; isolated localhost relay only, needs msgpack.

Usage: python3 tests/test-resume-protocol.py SSH_PORT SSE_PORT
"""
import base64
import hashlib
import hmac
import os
import select
import subprocess
import sys
import time
import msgpack

port, sse_port = map(int, sys.argv[1:])
secret = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
rw = hmac.new(bytes.fromhex(secret), b"tmtv:rw:v1", hashlib.sha256).hexdigest()[:32]
ro = "ro-" + hmac.new(bytes.fromhex(secret), b"tmtv:ro:v1", hashlib.sha256).hexdigest()[:32]
hosts = []


def host(identity):
    p = subprocess.Popen(["ssh", "-T", "-p", str(port),
                          "-o", "StrictHostKeyChecking=no",
                          "-o", "UserKnownHostsFile=/dev/null",
                          "-o", "BatchMode=yes", "-s", "tmtv@127.0.0.1", "tmate"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    hosts.append(p)
    # OpenSSH can send piped subsystem data before the relay installs its
    # daemon reader; the real libssh client waits for subsystem completion.
    time.sleep(.5)
    commands = [[0, 6, "2.0.4"],
                [12, "set-option", "tmtv-set", "session_identity=request-v1"],
                [12, "set-option", "tmtv-set", "web_sharing=true"],
                [1, 80, 24, [[0, "test", [[0, 80, 24, 0, 0]], 0]], 0], [9]]
    p.stdin.write(b"".join(msgpack.packb(c) for c in commands))
    p.stdin.flush()
    unpacker = msgpack.Unpacker(raw=False)
    messages = []
    until = time.monotonic() + 10
    sent = False
    while time.monotonic() < until:
        if not select.select([p.stdout], [], [], .2)[0]:
            continue
        data = os.read(p.stdout.fileno(), 65536)
        if not data:
            return p, messages
        unpacker.feed(data)
        for msg in unpacker:
            messages.append(msg)
            if msg == [4, "tmtv_reconnect_capability", "1"]:
                assert not sent
                sent = True
                p.stdin.write(msgpack.packb([18, identity]))
                p.stdin.flush()
            if msg == [5]:
                assert sent
                return p, messages
    raise AssertionError(f"identity handshake timed out: {messages!r}")


def stream(token):
    p = subprocess.run(["curl", "-sN", "--max-time", "1", "-D", "-",
                        f"http://127.0.0.1:{sse_port}/ws/{token}"], capture_output=True)
    assert p.stdout.startswith(b"HTTP/1.1 200"), p.stdout[:200]
    for line in p.stdout.splitlines():
        if line.startswith(b"data: "):
            data = base64.b64decode(line[6:])
            assert secret.encode() not in data, "private secret leaked to a viewer"
            msg = msgpack.unpackb(data, raw=False)
            assert not (msg[0] == 1 and msg[1][0] in (12, 18)), msg


try:
    first, messages = host(secret)
    assert any(m[0] == 4 and m[1] == "tmtv_ssh" and rw in m[2] for m in messages)
    stream(rw)
    stream(ro)
    print("PASS negotiated identity derives expected independent capabilities; no secret in RW/RO streams", flush=True)
    second, messages = host(secret)
    assert [5] in messages
    first.terminate()
    first.wait(timeout=5)
    time.sleep(.5)
    stream(rw)
    stream(ro)
    print("PASS same private identity reclaims a live unnamed session", flush=True)
    for invalid in (rw, ro, "g" * 64):
        rejected, messages = host(invalid)
        assert [5] not in messages, "viewer credential accepted as host identity"
        stream(rw)
    print("PASS RW token, RO token and malformed identity cannot claim a host", flush=True)
finally:
    for p in hosts:
        if p.poll() is None:
            p.terminate()
        p.wait(timeout=5)
