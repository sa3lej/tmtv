#!/usr/bin/env python3
"""Inspect environments from real configuration jobs and spawned panes."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(os.environ.get("TMTV", ROOT / "tmtv")).resolve()
TERMINAL = {
    "TERM": "outside-terminal",
    "TERM_PROGRAM": "outside-program",
    "TERM_PROGRAM_VERSION": "outside-version",
    "COLORTERM": "outside-colour",
}


class ChildEnvironmentTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="environment-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.socket = self.root / "socket"
        self.config = self.root / "config"
        self.environment = dict(os.environ, **TERMINAL)
        for name in ("TMUX", "TMTV", "TMUX_PROGRAM"):
            self.environment.pop(name, None)

    def command(self, *args, check=True):
        result = subprocess.run(
            [str(BINARY), "-S", str(self.socket), "-f", str(self.config), *args],
            env=self.environment, capture_output=True, text=True, timeout=10,
        )
        if check and result.returncode != 0:
            raise AssertionError(f"command {args} failed: {result.stderr}")
        return result

    def snapshot_command(self, name):
        path = shlex.quote(str(self.root / name))
        temporary = shlex.quote(str(self.root / (name + ".tmp")))
        return f"env > {temporary} && mv -f {temporary} {path}"

    def start(self, config):
        self.config.write_text('set -g tmtv-server-host ""\n' + config)
        self.addCleanup(self.command, "kill-server", check=False)
        self.command("new-session", "-d", "-s", "environment", "sleep 60")

    def snapshot(self, name):
        path = self.root / name
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if path.exists():
                entries = dict(line.split("=", 1) for line in path.read_text().splitlines()
                               if "=" in line)
                if entries.get("TMUX_PROGRAM") == "tmtv":
                    return entries
            time.sleep(0.01)
        self.fail(f"child environment not written: {name}")

    def assert_identity(self, environment):
        socket, pid, session = environment["TMTV"].rsplit(",", 2)
        self.assertEqual(socket, str(self.socket))
        self.assertGreater(int(pid), 0)
        self.assertEqual(environment["TMUX"], f"tmtv,{pid},{session}")
        self.assertEqual(environment["TMUX_PROGRAM"], "tmtv")

    def test_config_job_preserves_inherited_terminal_and_identity(self):
        self.start(f'run-shell "{self.snapshot_command("config-job")}"\n')
        environment = self.snapshot("config-job")
        for name, value in TERMINAL.items():
            self.assertEqual(environment.get(name), value, name)
        self.assert_identity(environment)

    def test_config_job_preserves_explicit_unsets_and_hidden_values(self):
        self.start('set-environment -gu COLORTERM\n'
                   'set-environment -gh PRIVATE_TOKEN hidden-value\n'
                   f'run-shell "{self.snapshot_command("config-job")}"\n')
        environment = self.snapshot("config-job")
        self.assertNotIn("COLORTERM", environment)
        self.assertNotIn("PRIVATE_TOKEN", environment)
        self.assertEqual(environment.get("TERM"), TERMINAL["TERM"])
        self.assert_identity(environment)

    def test_spawned_pane_sets_terminal_and_session_identity(self):
        self.start('set -g default-terminal tmux-256color\n')
        self.command("new-window", "-t", "environment", "-n", "probe",
                     self.snapshot_command("pane") + "; sleep 60")
        environment = self.snapshot("pane")
        self.assertEqual(environment.get("TERM"), "tmux-256color")
        self.assertEqual(environment.get("TERM_PROGRAM"), "tmux")
        self.assertEqual(environment.get("TERM_PROGRAM_VERSION"),
                         self.command("display-message", "-p", "#{version}").stdout.strip())
        self.assertEqual(environment.get("COLORTERM"), "truecolor")
        self.assert_identity(environment)
        session = self.command("display-message", "-p", "-t", "environment", "#{session_id}")
        self.assertEqual(environment["TMTV"].rsplit(",", 1)[1], session.stdout.strip()[1:])


if __name__ == "__main__":
    unittest.main()
