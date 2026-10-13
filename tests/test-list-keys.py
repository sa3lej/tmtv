#!/usr/bin/env python3
"""Roundtrip grouped key bindings through production listing and source-file."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(os.environ.get("TMTV", ROOT / "tmtv")).resolve()


class ListKeysTests(unittest.TestCase):
    def test_grouped_listing_and_notes_rebind_without_executing_commands(self):
        env = dict(os.environ, TERM="xterm-256color")
        for name in ("TMUX", "TMTV", "TMUX_PROGRAM"):
            env.pop(name, None)
        with tempfile.TemporaryDirectory(prefix="list-keys-", dir=ROOT / "tests") as directory:
            path = Path(directory)
            socket = path / "socket"
            config = path / "config"
            # File parsing creates a command group per line inside the block.
            config.write_text("set-option -g exit-empty off\n"
                              "bind-key -T serialization-live x {\n"
                              " set-option -g @live-first 'quote\"; dollar$ back\\slash'\n"
                              " set-option -g @live-second two\n}\n")

            def run(*args, check=True):
                return subprocess.run(
                    [str(BINARY), "-S", str(socket), "-f", str(config), *args],
                    env=env, text=True, capture_output=True, timeout=10, check=check,
                )

            try:
                run("new-session", "-d", "-s", "list-keys", "sleep 60")
                listing = run("list-keys", "-T", "serialization-live").stdout.strip()
                notes = run("list-keys", "-aN", "-T", "serialization-live").stdout.strip()
                for output in (listing, notes):
                    self.assertNotIn("\\;\\;", output)
                    self.assertIn("\\;", output)
                exported = path / "exported.conf"
                exported.write_text(listing + "\n")
                run("unbind-key", "-a", "-T", "serialization-live")
                run("source-file", str(exported))
                rebound = run("list-keys", "-T", "serialization-live").stdout.strip()
                self.assertEqual(rebound, listing)
                for option in ("@live-first", "@live-second"):
                    result = run("show-options", "-g", option, check=False)
                    self.assertNotEqual(result.returncode, 0,
                                        "Rebinding executed a command outside the binding")
            finally:
                run("kill-server", check=False)


if __name__ == "__main__":
    unittest.main()
