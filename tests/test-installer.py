#!/usr/bin/env python3
"""Offline installer regressions. No network, sudo, HOME changes or real installs.

Runs the production script, overriding only detect_install_dir to a disposable
location. Curated PATH fixtures select real SHA tools and simulate failed inputs.
Pass an older installer as argv[1] to verify that the regressions detect it.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

SOURCE = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / 'site/public/install.sh'
source = SOURCE.read_text()
assert source.endswith('\nmain\n'), 'Installer main entrypoint changed; review fixture boundary'
source = source[:-len('main\n')] + '''detect_install_dir() { printf '%s\\n' "$TEST_INSTALL_DIR"; }
main
'''
PAYLOAD = b'#!/bin/sh\nprintf "verified fixture\\n"\n'
DIGEST = hashlib.sha256(PAYLOAD).hexdigest()
OLD = b'previous installation must survive\n'
CURL = '''import os, pathlib, sys
args = sys.argv[1:]
url = args[-1]
case = os.environ['TEST_CASE']
if url.endswith('/releases/latest'):
    print('{"tag_name": "v3.0.0"}')
elif url.endswith('/checksums.txt'):
    pathlib.Path(args[args.index('-o') + 1]).write_bytes(pathlib.Path(os.environ['TEST_MANIFEST']).read_bytes())
    if case == 'manifest-download-failure': sys.exit(22)
elif '/releases/download/v3.0.0/tmtv-' in url:
    pathlib.Path(args[args.index('-o') + 1]).write_bytes(pathlib.Path(os.environ['TEST_BINARY']).read_bytes())
    if case == 'binary-download-failure': sys.exit(22)
else:
    raise SystemExit('Unexpected URL: ' + url)
'''


def fixture(case, tool, existing, platform='Linux'):
    with tempfile.TemporaryDirectory(prefix='tmtv-installer-') as temporary:
        root = Path(temporary)
        bindir = root / 'bin'
        install = root / 'install with spaces'
        scratch = root / 'downloads'
        for directory in (bindir, install, scratch):
            directory.mkdir()
        target = install / 'tmtv'
        if existing:
            target.write_bytes(OLD)
            target.chmod(0o751)
        script = root / 'installer.sh'
        script.write_text(source)
        binary = root / 'binary'
        binary.write_bytes(PAYLOAD)
        asset = 'tmtv-macos-amd64' if platform == 'Darwin' else 'tmtv-linux-amd64'
        row = f'{DIGEST}  {asset}\n'
        manifests = {
            'missing-entry': f'{DIGEST}  unrelated\n',
            'empty-manifest': '',
            'suffix-only': f'{DIGEST}  wrong-prefix-{asset}\n',
            'duplicate-entry': row + row,
            'conflicting-entry': row + f'{"0" * 64}  {asset}\n',
            'malformed-hash': f'{"z" * 64}  {asset}\n',
            'short-hash': f'{DIGEST[:-1]}  {asset}\n',
            'extra-column': row.rstrip() + ' unexpected\n',
            'mismatch': f'{"0" * 64}  {asset}\n',
            'uppercase': f'{DIGEST.upper()}  {asset}\n',
            'binary-marker': f'{DIGEST} *{asset}\n',
            'multi-asset': f'{"0" * 64}  other-asset\n' + row,
        }
        manifest = root / 'manifest'
        manifest.write_text(manifests.get(case, row))
        for name in ('awk', 'grep', 'head', 'sed', 'rm', 'chmod', 'mkdir', 'mv'):
            executable = shutil.which(name)
            assert executable, f'Missing test prerequisite: {name}'
            (bindir / name).symlink_to(executable)
        def executable(name, text):
            path = bindir / name
            path.write_text(text)
            path.chmod(0o755)
        executable('curl', f'#!{sys.executable}\n' + CURL)
        executable('uname', f'#!/bin/sh\ncase "$1" in -s) echo {platform};; -m) echo x86_64;; *) exit 1;; esac\n')
        mktemp = shutil.which('mktemp')
        executable('mktemp', f'#!/bin/sh\nexec "{mktemp}" -d "$TEST_SCRATCH/transfer.XXXXXX"\n')
        if tool:
            if case == 'hash-command-failure':
                executable(tool, f'#!/bin/sh\necho "{DIGEST}  ignored"\nexit 1\n')
            elif case == 'invalid-hash-output':
                executable(tool, '#!/bin/sh\necho invalid\n')
            else:
                actual_tool = shutil.which(tool)
                assert actual_tool, f'Missing test prerequisite: {tool}'
                (bindir / tool).symlink_to(actual_tool)
        env = dict(os.environ, PATH=str(bindir), TEST_INSTALL_DIR=str(install),
                   TEST_SCRATCH=str(scratch), TEST_MANIFEST=str(manifest),
                   TEST_BINARY=str(binary), TEST_CASE=case)
        result = subprocess.run(['/bin/sh', str(script)], env=env,
                                capture_output=True, text=True, timeout=15)
        success = case in ('valid', 'uppercase', 'binary-marker', 'multi-asset') and tool
        if success:
            assert result.returncode == 0, result.stdout + result.stderr
            assert target.read_bytes() == PAYLOAD, 'Wrong installed bytes'
            assert os.access(target, os.X_OK), 'Installed file is not executable'
            assert 'Checksum OK' in result.stdout, 'Verification success not reported'
        else:
            assert result.returncode != 0, 'Installer accepted unverifiable download'
            if existing:
                assert target.read_bytes() == OLD, 'Existing installation replaced'
                assert target.stat().st_mode & 0o777 == 0o751, 'Existing mode changed'
            else:
                assert not target.exists(), 'Unverified fresh installation created'
            assert 'Done!' not in result.stdout, 'Failure reported as success'
        assert not list(scratch.iterdir()), 'Temporary download was not cleaned up'


cases = []
for tool, platform in [('sha256sum', 'Linux'), ('shasum', 'Darwin')]:
    for case in ['valid', 'uppercase', 'binary-marker', 'multi-asset',
                 'manifest-download-failure', 'binary-download-failure',
                 'missing-entry', 'empty-manifest', 'suffix-only', 'duplicate-entry',
                 'conflicting-entry', 'malformed-hash', 'short-hash', 'extra-column',
                 'mismatch', 'hash-command-failure', 'invalid-hash-output']:
        for existing in (False, True):
            cases.append((case, tool, existing, platform))
for existing in (False, True):
    cases.append(('missing-tool', None, existing, 'Linux'))
failures = []
for case, tool, existing, platform in cases:
    label = f'{tool or "no hash tool"}/{platform}/{case}/{"upgrade" if existing else "fresh"}'
    try:
        fixture(case, tool, existing, platform)
    except (AssertionError, subprocess.TimeoutExpired) as error:
        failures.append(label)
        print(f'FAIL {label}: {error}')
print(f'{len(cases) - len(failures)}/{len(cases)} installer checks passed')
raise SystemExit(bool(failures))
