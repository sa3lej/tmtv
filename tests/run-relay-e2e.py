#!/usr/bin/env python3
"""Package local Linux binaries into a disposable, offline test container.

No existing relay, keys, sessions, Docker volume or published port is used.
Install requirements-relay-e2e.txt in a venv first. Docker must allow SYS_ADMIN
and unshare for the production relay's per-session jail; no test-only jail bypass.
"""
import argparse
import importlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import sysconfig
import tempfile
import uuid

from relay_browser_fixture import prepare_browser


def run(argv, **kw):
    return subprocess.run(argv, check=True, timeout=kw.pop("timeout", 120), **kw)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, help="retain fixture logs here")
    parser.add_argument("--trace", action="store_true", help="trace synthetic relay startup (requires strace)")
    parser.add_argument("--gdb", action="store_true", help="capture a synthetic relay crash (requires gdb)")
    parser.add_argument("--host-binary", type=Path, help="explicit local host binary")
    parser.add_argument("--relay-binary", type=Path, help="explicit local relay binary")
    parser.add_argument("--expect-layout-rejection", action="store_true", help="verify new host rejects an old relay")
    parser.add_argument("--expect-host-rejection", action="store_true", help="verify old host receives update notice before refusal")
    parser.add_argument('--browser', action='store_true', help='run actual browser rendering/reconnect tests (requires Playwright and built site)')
    parser.add_argument('--browser-engine', choices=('chromium', 'firefox', 'webkit'), default='chromium',
                        help='engine for --browser (default chromium)')
    parser.add_argument('--web-root', type=Path, help='built production Astro site (default site/dist)')
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("this native-binary fixture requires Linux and Docker")
    repo = Path(__file__).resolve().parent.parent
    modules = [importlib.import_module(n) for n in ("msgpack", "pyte", "wcwidth")]
    name = "tmtv-e2e-" + uuid.uuid4().hex[:12]
    with tempfile.TemporaryDirectory(prefix=name + "-") as tmp:
        context = Path(tmp)
        root = context / "rootfs"
        copied = set()

        def copy_file(source, dest=None):
            source = Path(source)
            dest = root / str(dest or source).lstrip("/")
            if dest in copied:
                return
            copied.add(dest)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, dest, follow_symlinks=True)

        def binary(source, dest=None, *, env=None, bundled_root=None):
            copy_file(source, dest)
            linked = run(["ldd", str(source)], capture_output=True, text=True, env=env).stdout
            for dep in re.findall(r"(?:=>\s*)?(/[^\s()]+)", linked):
                if bundled_root is None or not Path(dep).is_relative_to(bundled_root):
                    copy_file(dep)
            if "not found" in linked:
                raise RuntimeError("unresolved dependency: " + linked)

        for source, dest in [(args.host_binary or repo / "tmtv", "/usr/bin/tmtv"),
                             (args.relay_binary or repo / "tmtv-server", "/usr/bin/tmtv-server"),
                             (sys.executable, "/usr/bin/python3"),
                             ("/bin/sh", "/bin/sh")]:
            binary(source, dest)
        for command in ("ssh", "ssh-keygen"):
            source = shutil.which(command)
            if not source:
                parser.error(command + " is required")
            binary(source, "/usr/bin/" + command)
        if args.trace:
            source = shutil.which("strace")
            if not source:
                parser.error("--trace requires strace")
            binary(source, "/usr/bin/strace")
        if args.gdb:
            source = shutil.which("gdb")
            if not source:
                parser.error("--gdb requires gdb")
            binary(source, "/usr/bin/gdb")
        stdlib = Path(sysconfig.get_path("stdlib"))
        shutil.copytree(stdlib, root / str(stdlib).lstrip("/"),
                        ignore=shutil.ignore_patterns("site-packages", "dist-packages", "__pycache__", "test", "tests"))
        for lib in (stdlib / "lib-dynload").glob("*.so"):
            binary(lib)
        for module in modules:
            directory = Path(module.__file__).parent
            target = root / "test/lib" / directory.name
            shutil.copytree(directory, target, ignore=shutil.ignore_patterns("__pycache__"))
            for lib in directory.rglob("*.so"):
                binary(lib, "/test/lib/" + directory.name + "/" + str(lib.relative_to(directory)))
        locale = Path("/usr/lib/locale/C.utf8")
        if not locale.is_dir():
            parser.error("the native C.utf8 locale is required")
        shutil.copytree(locale, root / "usr/lib/locale/C.utf8")
        for name_in, content in {
            "/etc/passwd": "root:x:0:0:root:/tmp:/bin/sh\nnobody:x:65534:65534:nobody:/tmp:/bin/sh\n",
            "/etc/group": "root:x:0:\nnogroup:x:65534:\n",
            "/etc/nsswitch.conf": "passwd: files\ngroup: files\nhosts: files\n",
        }.items():
            p = root / name_in.lstrip("/")
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content)
        for term in ("x/xterm", "x/xterm-256color", "s/screen", "s/screen-256color"):
            source = next((p / term for p in (Path("/lib/terminfo"), Path("/usr/share/terminfo")) if (p / term).exists()), None)
            if source:
                copy_file(source, "/usr/share/terminfo/" + term)
        copy_file(repo / "tests/test-relay-e2e.py", "/test/test-relay-e2e.py")
        if args.browser:
            prepare_browser(repo, root, binary, copy_file, args.web_root or repo / 'site/dist', args.browser_engine)
        (root / "tmp").mkdir(mode=0o1777)
        (context / "Dockerfile").write_text(
            'FROM scratch\nCOPY rootfs /\nENV PATH=/usr/bin:/bin PYTHONPATH=/test/lib LC_ALL=C.UTF-8 TERM=xterm-256color HOME=/tmp\n'
            'ENTRYPOINT ["/usr/bin/python3", "-u", "/test/test-relay-e2e.py"]\n')
        try:
            run(["docker", "build", "--network", "none", "-q", "-t", name, str(context)], timeout=180)
            # The jail calls unshare; SYS_ADMIN is confined to this disposable
            # container. No host directories, Docker socket or network are mounted.
            run(["docker", "create", "--name", name, "--network", "none", "--read-only",
                 "--cap-add", "SYS_ADMIN", "--security-opt", "seccomp=unconfined",
                 *(["--cap-add", "SYS_PTRACE"] if args.trace or args.gdb else []),
                 *(["-e", "TMTV_E2E_TRACE=1"] if args.trace else []),
                 *(["-e", "TMTV_E2E_GDB=1"] if args.gdb else []),
                 *(["-e", "TMTV_E2E_EXPECT_LAYOUT_REJECTION=1"] if args.expect_layout_rejection else []),
                 *(["-e", "TMTV_E2E_EXPECT_HOST_REJECTION=1"] if args.expect_host_rejection else []),
                 *(['-e', 'TMTV_E2E_BROWSER=1'] if args.browser else []),
                 *(['-e', 'TMTV_E2E_BROWSER_ENGINE=' + args.browser_engine] if args.browser else []),
                 "-e", "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1", "-e", "UBSAN_OPTIONS=halt_on_error=1",
                 "--pids-limit", "256" if args.browser else "128",
                 "--memory", "768m" if args.browser else "512m", "--cpus", "2",
                 "--mount", "type=volume,src=" + name + ",dst=/tmp", name])
            result = subprocess.run(["docker", "start", "-a", name], timeout=240 if args.browser else 180)
            if result.returncode:
                raise RuntimeError("relay E2E fixture failed")
        finally:
            if args.artifacts and args.browser:
                args.artifacts.mkdir(parents=True, exist_ok=True, mode=0o700)
                subprocess.run(['docker', 'cp', name + ':/tmp/e2e/browser', str(args.artifacts)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
            if args.artifacts:
                args.artifacts.mkdir(parents=True, exist_ok=True, mode=0o700)
                subprocess.run(["docker", "cp", name + ":/tmp/e2e/relay.log", str(args.artifacts / "relay.log")],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
                subprocess.run(["docker", "cp", name + ":/tmp/e2e/diagnostics.log", str(args.artifacts / "diagnostics.log")],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
            subprocess.run(["docker", "rm", "-f", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
            subprocess.run(["docker", "volume", "rm", "-f", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
            subprocess.run(["docker", "image", "rm", "-f", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)


if __name__ == "__main__":
    main()
