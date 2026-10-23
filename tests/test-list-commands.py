#!/usr/bin/env python3
"""Exercise command discovery through the production socket and command queue."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(os.environ.get("TMTV", ROOT / "tmtv")).resolve()


class ListCommandsTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="list-commands-")
        self.addCleanup(self.directory.cleanup)
        self.socket = Path(self.directory.name) / "socket"
        self.config = Path(self.directory.name) / "config"
        self.config.write_text('set -g tmtv-server-host ""\n')
        self.env = dict(os.environ, TERM="xterm-256color")
        for name in ("TMUX", "TMTV", "TMUX_PROGRAM"):
            self.env.pop(name, None)
        self.run_command("new-session", "-d", "-s", "listing", "sleep 60")
        self.addCleanup(self.run_command, "kill-server", check=False)

    def run_command(self, *args, check=True):
        return subprocess.run(
            [str(BINARY), "-S", str(self.socket), "-f", str(self.config), *args],
            env=self.env, text=True, capture_output=True, timeout=10, check=check,
        )

    def test_default_and_custom_formats(self):
        self.assertEqual(self.run_command("list-commands", "list-commands").stdout,
                         "list-commands (lscm) [-F format] [command]\n")
        template = "#{command_list_name}|#{command_list_alias}|#{command_list_usage}"
        self.assertEqual(self.run_command("list-commands", "-F", template, "lscm").stdout,
                         "list-commands|lscm|[-F format] [command]\n")
        # Missing aliases and usage must clear values in the reused format tree.
        lines = self.run_command("list-commands", "-F", template).stdout.splitlines()
        self.assertIn("clock-mode||[-t target-pane]", lines)
        self.assertIn("start-server|start|", lines)
        self.assertIn("list-commands|lscm|[-F format] [command]", lines)

    def test_alias_and_unique_prefix_lookup(self):
        expected = self.run_command("list-commands", "list-commands").stdout
        for query in ("lscm", "list-command"):
            self.assertEqual(self.run_command("lscm", query).stdout, expected)

    def test_complete_listing_and_conditional_filter(self):
        names = self.run_command("list-commands", "-F", "#{command_list_name}").stdout.splitlines()
        self.assertEqual(len(names), len(set(names)))
        default_names = [line.split()[0] for line in self.run_command("list-commands").stdout.splitlines()]
        self.assertEqual(names, default_names)
        for name in ("list-commands", "list-keys", "new-session", "wait-for"):
            self.assertIn(name, names)
        template = "#{?#{==:#{command_list_name},list-commands},#{command_list_alias},}"
        self.assertEqual(self.run_command("list-commands", "-F", template).stdout, "lscm\n")
        self.assertEqual(self.run_command("list-commands", "-F", "").stdout, "")

    def test_unknown_and_ambiguous_errors_keep_server_usable(self):
        for query, message in (("no-such-command", "unknown command: no-such-command"),
                               ("list-", "ambiguous command: list-, could be:")):
            result = self.run_command("list-commands", query, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
            self.assertIn(message, result.stderr)
            if query == "list-":
                self.assertIn("list-commands", result.stderr)
                self.assertIn("list-keys", result.stderr)
            self.assertEqual(self.run_command("list-commands", "-F", "#{command_list_name}",
                                              "lscm").stdout, "list-commands\n")

    def test_listing_error_cancels_following_command_in_group(self):
        result = self.run_command("list-commands", "no-such-command", ";",
                                  "set-option", "-g", "@listing-should-not-run", "yes", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotEqual(self.run_command("show-options", "-g", "@listing-should-not-run",
                                             check=False).returncode, 0)
        self.assertEqual(self.run_command("list-keys", "-T", "root", check=False).returncode, 0)

    def test_invalid_arguments_rejected(self):
        for args in (("-F",), ("-Z",), ("list-keys", "wait-for")):
            result = self.run_command("list-commands", *args, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
            self.assertIn("list-commands", result.stderr)


if __name__ == "__main__":
    unittest.main()
