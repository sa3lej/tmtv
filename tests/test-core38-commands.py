#!/usr/bin/env python3
"""Production socket regressions for reconciled tmux 3.8 command paths."""
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(os.environ.get("TMTV", ROOT / "tmtv")).resolve()


class CoreCommandsTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="core38-")
        self.addCleanup(self.directory.cleanup)
        self.socket = Path(self.directory.name) / "socket"
        self.config = Path(self.directory.name) / "config"
        self.config.write_text('set -g tmtv-server-host ""\n')
        self.env = dict(os.environ, TERM="xterm-256color")
        for name in ("TMUX", "TMTV", "TMUX_PROGRAM"):
            self.env.pop(name, None)
        self.command("new-session", "-d", "-x", "80", "-y", "24", "-s", "core", "sleep 60")
        self.addCleanup(self.command, "kill-server", check=False)

    def command(self, *args, check=True):
        return subprocess.run([str(BINARY), "-S", str(self.socket), "-f", str(self.config), *args],
                              env=self.env, capture_output=True, text=True, timeout=10, check=check)

    def value(self, name):
        return self.command("show-options", "-gv", name).stdout.strip()

    def test_async_if_shell_preserves_queue_order_and_else(self):
        self.command("if-shell", "sleep .02; true", "set -g @branch true", "set -g @branch false",
                     ";", "set", "-gF", "@after", "#{@branch}")
        self.assertEqual(self.value("@branch"), "true")
        self.assertEqual(self.value("@after"), "true")
        self.command("if-shell", "false", "set -g @branch true", "set -g @branch false")
        self.assertEqual(self.value("@branch"), "false")
        self.command("if-shell", "-b", "sleep .02; true", "set -g @background done")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            result = self.command("show-options", "-gv", "@background", check=False)
            if result.returncode == 0:
                self.assertEqual(result.stdout.strip(), "done")
                break
            time.sleep(.02)
        else:
            self.fail("background branch did not execute")

    def test_display_panes_enters_new_mode_and_expires(self):
        self.command("split-window", "-d", "sleep 60")
        self.command("display-panes", "-d", "200", "-t", "core:0.0")
        result = self.command("display-message", "-p", "-t", "core:0.0", "#{pane_mode}")
        self.assertEqual(result.stdout.strip(), "panes-mode")
        time.sleep(.25)
        result = self.command("display-message", "-p", "-t", "core:0.0", "#{pane_mode}")
        self.assertEqual(result.stdout.strip(), "")

    def test_floating_geometry_and_tiled_layout_survive_commands(self):
        pane = self.command("new-pane", "-dP", "-F", "#{pane_id}", "-x", "20", "-y", "6",
                            "-X", "8", "-Y", "3", "sleep 60").stdout.strip()
        fields = "#{pane_floating_flag}:#{pane_width}:#{pane_height}:#{pane_left}:#{pane_top}"
        self.assertEqual(self.command("display-message", "-p", "-t", pane, fields).stdout.strip(),
                         "1:18:4:9:4")
        self.command("split-window", "-d", "-t", "core:0.0", "sleep 60")
        self.command("resize-pane", "-t", pane, "-x", "30")
        self.assertEqual(self.command("display-message", "-p", "-t", pane,
                                      "#{pane_width}").stdout.strip(), "28")
        self.command("kill-pane", "-t", pane)
        self.assertEqual(len(self.command("list-panes").stdout.splitlines()), 2)


if __name__ == "__main__":
    unittest.main()
