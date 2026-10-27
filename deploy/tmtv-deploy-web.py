#!/usr/bin/python3
"""Root-owned Linux installer for complete tmtv static sites; never restarts services."""
import ctypes
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import pwd
import shutil
import stat
import sys
import tarfile
import uuid

VERSION = 'tmtv-web-bundle-v1'
REQUIRED = ('index.html', 'viewer.html', 'viewer.js', 'install.sh', 'robots.txt',
            'llms.txt', 'sitemap.xml', 'social-card.png', 'releases/index.html',
            'releases/3-0/index.html')
MAX_BYTES = 256 * 1024 * 1024
MAX_ENTRIES = 10000


def read_file(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, 'rb') as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_BYTES:
            raise ValueError('Expected a bounded regular file')
        data = source.read(MAX_BYTES + 1)
        if len(data) > MAX_BYTES:
            raise ValueError('Input exceeds size limit')
        return data


def exchange(left, right):
    # Atomic directory exchange preserves the public path even on the first rollout.
    libc = ctypes.CDLL(None, use_errno=True)
    rename = libc.renameat2
    rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    if rename(-100, os.fsencode(left), -100, os.fsencode(right), 2):
        raise OSError(ctypes.get_errno(), 'Atomic web-tree exchange failed')


def sync_dir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def save_json(path, value):
    temp = path.with_name(path.name + '.new')
    with temp.open('w') as stream:
        json.dump(value, stream)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temp, path)
    sync_dir(path.parent)


def validate(tree, required=REQUIRED):
    for name in required:
        path = tree / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
            raise ValueError('Missing required site file: ' + name)
    for path in tree.rglob('*'):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise ValueError('Site contains a link or special file')


def unpack(data, target):
    seen, size = set(), 0
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
        for member in archive:
            name = PurePosixPath(member.name)
            if name.is_absolute() or '..' in name.parts or '\\' in member.name:
                raise ValueError('Unsafe archive path')
            if not (member.isdir() or member.isfile()):
                raise ValueError('Archive links and special files are forbidden')
            if str(name) == '.':
                if not member.isdir():
                    raise ValueError('Invalid archive root')
                continue
            if name in seen or len(seen) >= MAX_ENTRIES:
                raise ValueError('Duplicate or excessive archive entries')
            seen.add(name)
            size += member.size
            if member.size < 0 or size > MAX_BYTES:
                raise ValueError('Expanded archive exceeds size limit')
            dest = target.joinpath(*name.parts)
            if member.isdir():
                dest.mkdir(parents=True, exist_ok=True)
            else:
                dest.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, dest.open('xb') as output:
                    shutil.copyfileobj(source, output)
                    output.flush()
                    os.fsync(output.fileno())
                dest.chmod(0o644)
    validate(target)


class Installer:
    def __init__(self, webroot=Path('/var/www/tmtv'), staging=Path('/home/deploy/staging')):
        self.webroot, self.staging = Path(webroot), Path(staging)
        self.backups = self.webroot.parent / (self.webroot.name + '-web-backups')
        self.receipt = self.backups / 'current.json'
        self.intent = self.backups / 'in-progress.json'

    def run(self, action, digest=None):
        if self.webroot.is_symlink() or not self.webroot.is_dir():
            raise ValueError('Web root must be an existing real directory')
        validate(self.webroot, ('index.html', 'viewer.html', 'viewer.js'))
        if self.backups.is_symlink():
            raise ValueError('Backup path must not be a symlink')
        self.backups.mkdir(mode=0o700, exist_ok=True)
        with (self.backups / '.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            if self.intent.exists():
                raise ValueError('Interrupted deployment needs inspection: ' + str(self.intent))
            if action == 'rollback':
                return self.rollback(digest)
            ident = uuid.uuid4().hex
            candidate = self.backups / ident
            candidate.mkdir(mode=0o755)
            exchanged = False
            committed = False
            previous = json.loads(read_file(self.receipt)) if self.receipt.exists() else None
            try:
                if action == 'bundle':
                    if not re.fullmatch('[a-f0-9]{64}', digest or ''):
                        raise ValueError('Expected SHA-256 digest')
                    data = read_file(self.staging / 'tmtv-web.tar.gz')
                    if hashlib.sha256(data).hexdigest() != digest:
                        raise ValueError('Web bundle checksum mismatch')
                    unpack(data, candidate)
                elif action == 'legacy':
                    # Old workflows remain usable until their default branch is upgraded.
                    validate(self.webroot, ('index.html', 'viewer.html', 'viewer.js'))
                    shutil.copytree(self.webroot, candidate, dirs_exist_ok=True)
                    files = [p for p in self.staging.iterdir() if p.suffix in
                             ('.html', '.js', '.css', '.png', '.gif', '.sh', '.ico', '.svg', '.txt')]
                    if not files:
                        raise ValueError('No legacy web assets in staging')
                    for source in files:
                        (candidate / source.name).write_bytes(read_file(source))
                    validate(candidate, ('index.html', 'viewer.html', 'viewer.js'))
                else:
                    raise ValueError('Unknown action')
                for path in candidate.rglob('*'):
                    path.chmod(0o755 if path.is_dir() else 0o644)
                    if path.is_file():
                        with path.open('rb') as stream:
                            os.fsync(stream.fileno())
                for path in sorted((p for p in candidate.rglob('*') if p.is_dir()), reverse=True):
                    sync_dir(path)
                sync_dir(candidate)
                save_json(self.intent, {'deployment_id': ident, 'action': action, 'backup': str(candidate)})
                exchange(candidate, self.webroot)
                exchanged = True
                sync_dir(self.webroot.parent)
                sync_dir(self.backups)
                receipt = {'deployment_id': ident, 'mode': action, 'sha256': digest, 'backup': str(candidate)}
                save_json(self.receipt, receipt)
                committed = True
                self.intent.unlink()
                sync_dir(self.backups)
                return receipt
            except BaseException:
                if committed:
                    raise  # Keep the intent for inspection; never undo an accepted receipt.
                if exchanged:
                    exchange(candidate, self.webroot)
                    if previous is not None:
                        save_json(self.receipt, previous)
                    elif self.receipt.exists():
                        self.receipt.unlink()
                shutil.rmtree(candidate)
                if self.intent.exists():
                    self.intent.unlink()
                raise

    def rollback(self, ident):
        receipt = json.loads(read_file(self.receipt))
        if not re.fullmatch('[a-f0-9]{32}', ident or '') or receipt.get('deployment_id') != ident:
            raise ValueError('Rollback ID does not match the current deployment')
        backup = self.backups / ident
        if backup.is_symlink() or not backup.is_dir():
            raise ValueError('Invalid rollback tree')
        validate(backup, ('index.html', 'viewer.html', 'viewer.js'))
        save_json(self.intent, {'deployment_id': ident, 'action': 'rollback', 'backup': str(backup)})
        exchange(backup, self.webroot)
        sync_dir(self.webroot.parent)
        sync_dir(self.backups)
        # The failed/new version stays at the backup path for investigation.
        save_json(self.receipt, {**receipt, 'deployment_id': None, 'rolled_back': ident})
        self.intent.unlink()
        sync_dir(self.backups)
        return {'rolled_back': ident}


def main(args):
    if args == ['--check']:
        print(VERSION)
        return
    if not args:
        result = Installer().run('legacy')
    elif len(args) == 2 and args[0] in ('--bundle', '--rollback'):
        caller = os.environ.get('SUDO_USER', 'deploy')
        staging = Path(pwd.getpwnam(caller).pw_dir) / 'staging'
        result = Installer(staging=staging).run(args[0][2:], args[1])
    else:
        raise ValueError('Usage: tmtv-deploy-web [--check | --bundle SHA256 | --rollback DEPLOYMENT_ID]')
    print(json.dumps(result))


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except Exception as error:
        print('Web deployment refused: ' + str(error), file=sys.stderr)
        sys.exit(1)
