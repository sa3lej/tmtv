#!/usr/bin/env python3
"""Exercise the production history loader through an isolated host server."""

import os
from pathlib import Path
import select
import shutil
import subprocess
import tempfile
import threading
import time
import unittest


ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(os.environ.get("TMTV", ROOT / "tmtv")).resolve()
TYPES = ("command", "search", "target", "window-target")


class PromptHistoryTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="history-", dir=ROOT / "tests"))
        self.addCleanup(self.cleanup_server)
        self.socket = self.root / "socket"
        self.history = self.root / "history"
        self.config = self.root / "config"
        self.config.write_text(f'set -g tmtv-server-host ""\nset -g history-file "{self.history}"\n')

    def command(self, *args, check=True):
        return subprocess.run(
            [str(BINARY), "-S", str(self.socket), "-f", str(self.config), *args],
            capture_output=True, text=True, timeout=10, check=check,
        )

    def cleanup_server(self):
        if self.socket.exists():
            self.command("kill-server", check=False)
            deadline = time.monotonic() + 10
            # server_start unlinks this socket only after saving history. A
            # disconnected kill-server client does not establish that fence.
            while self.socket.exists():
                if time.monotonic() >= deadline:
                    self.fail(f"history shutdown timed out; preserved {self.root}")
                time.sleep(0.01)
        # Keep the directory for diagnosis if shutdown fails or times out.
        if self.root.exists():
            shutil.rmtree(self.root)

    def load(self, contents):
        if contents is not None:
            self.history.write_bytes(contents)
        self.command("new-session", "-d", "-s", "history", "sleep 60")
        # This query runs after cfg_done has loaded history on the real server.
        self.command("show-options", "-g", "history-file")

    def entries(self, kind):
        output = self.command("show-prompt-history", "-T", kind).stdout
        self.assertTrue(output.startswith(f"History for {kind}:\n"), output)
        return [line.split(": ", 1)[1] for line in output.splitlines()
                if line.partition(": ")[0].isdigit()]

    def test_typed_legacy_and_unterminated_lines(self):
        self.load(b"command:display-message one\nsearch:needle\ntarget:session:2\n"
                  b"window-target:3\nlegacy without type\nunknown:keep:colons\n"
                  b"\ncommand:final without newline")
        self.assertEqual(self.entries("command"), ["display-message one",
                         "legacy without type", "unknown:keep:colons", "",
                         "final without newline"])
        self.assertEqual(self.entries("search"), ["needle"])
        self.assertEqual(self.entries("target"), ["session:2"])
        self.assertEqual(self.entries("window-target"), ["3"])

    def test_large_line_and_buffer_reuse(self):
        long_line = "x" * 65536
        self.load((f"command:{long_line}\nsearch:short\ncommand:last").encode())
        self.assertEqual(self.entries("command"), [long_line, "last"])
        self.assertEqual(self.entries("search"), ["short"])

    def test_cleanup_waits_for_history_write(self):
        # A FIFO deliberately blocks the real server's shutdown save after its
        # kill-server client has disconnected. Drain it only after checking the
        # fixture has kept the directory alive while shutdown is incomplete.
        contents = b"command:" + b"x" * 262144 + b"\n"
        self.load(contents)
        self.history.unlink()
        os.mkfifo(self.history)
        reader = os.open(self.history, os.O_RDONLY | os.O_NONBLOCK)
        returned = threading.Event()
        errors = []
        command = self.command

        def observed_command(*args, **kwargs):
            result = command(*args, **kwargs)
            if args == ("kill-server",):
                returned.set()
            return result

        def cleanup():
            try:
                self.cleanup_server()
            except Exception as error:
                errors.append(error)

        self.command = observed_command
        worker = threading.Thread(target=cleanup)
        worker.start()
        try:
            self.assertTrue(returned.wait(10), "kill-server client did not return")
            self.assertTrue(select.select([reader], [], [], 10)[0],
                            "server did not begin saving history")
            # Keep the writer blocked while giving teardown time to attempt
            # removal; draining immediately could conceal the original race.
            worker.join(0.1)
            self.assertTrue(worker.is_alive(), "cleanup raced ahead of history save")
            self.assertTrue(self.root.is_dir())
            self.assertTrue(self.socket.exists())
        finally:
            # Even an assertion failure must unblock this test's server.
            saved = bytearray()
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if not select.select([reader], [], [], 0.1)[0]:
                    continue
                chunk = os.read(reader, 65536)
                if not chunk:
                    break
                saved.extend(chunk)
            os.close(reader)
            worker.join(10)
            self.command = command
        self.assertFalse(worker.is_alive(), "cleanup did not finish")
        self.assertEqual(errors, [])
        self.assertEqual(saved, contents)
        self.assertFalse(self.root.exists())

    def test_empty_file(self):
        self.load(b"")
        for kind in TYPES:
            self.assertEqual(self.entries(kind), [])

    def test_missing_file(self):
        self.load(None)
        for kind in TYPES:
            self.assertEqual(self.entries(kind), [])


if __name__ == "__main__":
    unittest.main()
