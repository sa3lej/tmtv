#!/usr/bin/env python3
"""Offline complete-tree deployment/rollback and transport regression fixtures."""
import hashlib
import http.server
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('web_installer', ROOT / 'deploy/tmtv-deploy-web.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def bundle(files, extras=()):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode='w:gz') as archive:
        for name, data in files.items():
            info = tarfile.TarInfo('./' + name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
        for info, data in extras:
            archive.addfile(info, io.BytesIO(data))
    return output.getvalue()


def tree(path):
    return {str(p.relative_to(path)): p.read_bytes() for p in path.rglob('*') if p.is_file()}


class Deployment(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='tmtv-web-test-')
        self.root = Path(self.temp.name)
        self.web = self.root / 'www/tmtv'
        self.stage = self.root / 'staging'
        self.web.mkdir(parents=True)
        self.stage.mkdir()
        for name in ('index.html', 'viewer.html', 'viewer.js', 'obsolete.html'):
            (self.web / name).write_bytes(b'old ' + name.encode())
        (self.web / 'old/deep').mkdir(parents=True)
        (self.web / 'old/deep/file.txt').write_bytes(b'old nested file')
        self.old = tree(self.web)
        self.files = {name: ('new ' + name).encode() for name in m.REQUIRED}
        self.files['viewer.html'] = b'<html><form id="password-form"></form><script src="/viewer.js"></script></html>'
        self.files['_astro/styles.css'] = b'body { color: lime; }'
        self.installer = m.Installer(self.web, self.stage)

    def tearDown(self):
        self.temp.cleanup()

    def stage_bundle(self, files=None, extras=()):
        data = bundle(self.files if files is None else files, extras)
        (self.stage / 'tmtv-web.tar.gz').write_bytes(data)
        return hashlib.sha256(data).hexdigest()

    def test_packager_includes_nested_assets_and_requires_discovery_files(self):
        import shutil
        repo = self.root / 'repo'
        (repo / 'scripts').mkdir(parents=True)
        (repo / 'web').mkdir()
        dist = repo / 'site/dist'; dist.mkdir(parents=True)
        shutil.copyfile(ROOT / 'scripts/package-site.sh', repo / 'scripts/package-site.sh')
        for name, data in self.files.items():
            path = dist / name; path.parent.mkdir(parents=True,exist_ok=True); path.write_bytes(data)
        (dist / 'viewer').mkdir(); (dist / 'viewer/index.html').write_bytes(self.files['viewer.html'])
        (repo / 'web/viewer.js').write_bytes(self.files['viewer.js'])
        output = self.root / 'packed.tar.gz'
        command = ['sh', str(repo / 'scripts/package-site.sh'), str(output)]
        result = subprocess.run(command,capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        (self.stage / 'tmtv-web.tar.gz').write_bytes(output.read_bytes())
        self.installer.run('bundle',hashlib.sha256(output.read_bytes()).hexdigest())
        self.assertEqual((self.web / '_astro/styles.css').read_bytes(),self.files['_astro/styles.css'])
        self.assertEqual((self.web / 'viewer.html').read_bytes(),self.files['viewer.html'])
        (dist / 'sitemap.xml').unlink()
        self.assertNotEqual(subprocess.run(command,capture_output=True).returncode,0)

    def test_complete_tree_and_exact_rollback(self):
        receipt = self.installer.run('bundle', self.stage_bundle())
        self.assertEqual(tree(self.web), self.files)
        self.assertEqual(tree(Path(receipt['backup'])), self.old)
        self.assertFalse((self.web / 'obsolete.html').exists())
        self.installer.run('rollback', receipt['deployment_id'])
        self.assertEqual(tree(self.web), self.old)
        self.assertEqual(tree(Path(receipt['backup'])), self.files)
        with self.assertRaises(ValueError):
            self.installer.run('rollback', receipt['deployment_id'])

    def test_superseded_rollback_cannot_undo_new_deployment(self):
        first = self.installer.run('bundle', self.stage_bundle())
        updated = {**self.files, 'index.html': b'newer site'}
        second = self.installer.run('bundle', self.stage_bundle(updated))
        with self.assertRaises(ValueError):
            self.installer.run('rollback', first['deployment_id'])
        self.assertEqual(tree(self.web), updated)
        self.installer.run('rollback', second['deployment_id'])
        self.assertEqual(tree(self.web), self.files)

    def test_invalid_bundles_preserve_current_and_receipt(self):
        self.installer.run('bundle', self.stage_bundle())
        before = self.installer.receipt.read_bytes()
        cases = []
        for path in ('../escape', '/tmp/escape', 'foo/../../escape', 'bad\\path'):
            item = tarfile.TarInfo(path); item.size = 1
            cases.append((self.files, [(item, b'x')]))
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.CHRTYPE, tarfile.FIFOTYPE):
            item = tarfile.TarInfo('link'); item.type = kind; item.linkname = '/etc/passwd'
            cases.append((self.files, [(item, b'')]))
        item = tarfile.TarInfo('index.html'); item.size = 1
        cases.append((self.files, [(item, b'x')]))
        for name in m.REQUIRED:
            cases.append(({k:v for k,v in self.files.items() if k != name}, []))
        cases.append(({**self.files, 'releases': b'file collides with directory'}, []))
        for files, extras in cases:
            with self.subTest(files=list(files), extras=[x.name for x,_ in extras]):
                sha = self.stage_bundle(files, extras)
                with self.assertRaises((ValueError, OSError, KeyError)):
                    self.installer.run('bundle', sha)
                self.assertEqual(tree(self.web), self.files)
                self.assertEqual(self.installer.receipt.read_bytes(), before)
        self.assertFalse((self.root / 'escape').exists())

    def test_checksum_corruption_and_symlink_input(self):
        self.stage_bundle()
        with self.assertRaises(ValueError): self.installer.run('bundle', '0' * 64)
        (self.stage / 'tmtv-web.tar.gz').write_bytes(b'not a tar')
        with self.assertRaises(tarfile.TarError):
            self.installer.run('bundle', hashlib.sha256(b'not a tar').hexdigest())
        (self.stage / 'tmtv-web.tar.gz').unlink()
        (self.stage / 'tmtv-web.tar.gz').symlink_to(self.web / 'index.html')
        with self.assertRaises(OSError): self.installer.run('bundle', '0' * 64)
        self.assertEqual(tree(self.web), self.old)

    def test_legacy_helper_call_preserves_nested_tree_and_rollback(self):
        (self.stage / 'index.html').write_bytes(b'legacy update')
        receipt = self.installer.run('legacy')
        self.assertEqual(tree(self.web), {**self.old, 'index.html':b'legacy update'})
        self.installer.run('rollback', receipt['deployment_id'])
        self.assertEqual(tree(self.web), self.old)

    def test_activation_failure_preserves_current(self):
        sha = self.stage_bundle()
        with patch.object(m, 'exchange', side_effect=OSError('filesystem refuses exchange')):
            with self.assertRaises(OSError): self.installer.run('bundle', sha)
        self.assertEqual(tree(self.web), self.old)
        self.assertFalse(self.installer.intent.exists())

    def test_receipt_failure_restores_current(self):
        sha = self.stage_bundle()
        original = m.save_json
        def save(path, value):
            if path == self.installer.receipt: raise OSError('disk full before receipt')
            original(path, value)
        with patch.object(m, 'save_json', side_effect=save):
            with self.assertRaises(OSError): self.installer.run('bundle', sha)
        self.assertEqual(tree(self.web), self.old)

    def test_interrupted_deployment_needs_inspection(self):
        self.installer.backups.mkdir()
        self.installer.intent.write_text('{}')
        with self.assertRaisesRegex(ValueError, 'inspection'):
            self.installer.run('bundle', self.stage_bundle())
        self.assertEqual(tree(self.web), self.old)

    def test_upload_driver_verifies_content_and_rolls_back_failed_probe(self):
        # Fake only SSH/SCP; run real driver, checksum checks, installer, HTTP and rollback.
        binpath = self.root / 'bin'; binpath.mkdir()
        state = {'bad':False}
        web = self.web
        class Handler(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *args, **kwargs): super().__init__(*args, directory=str(web), **kwargs)
            def log_message(self, *args): pass
            def do_GET(self):
                if state['bad'] and self.path == '/releases/':
                    self.send_error(500); return
                if self.path.startswith(('/s/', '/j/')): self.path = '/viewer.html'
                super().do_GET()
        server = http.server.ThreadingHTTPServer(('127.0.0.1',0), Handler)
        thread = threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        ssh = '''#!/usr/bin/env python3
import importlib.util,os,sys
from pathlib import Path
s=importlib.util.spec_from_file_location('helper',os.environ['HELPER']);m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
a=sys.argv[-1]
if a.endswith('--check'): print(m.VERSION)
elif '--bundle ' in a:
 import json
 sha=a.rsplit(' ',1)[1];st=Path(os.environ['STAGE']);(st/('.tmtv-web-'+sha+'.tar.gz')).replace(st/'tmtv-web.tar.gz')
 print(json.dumps(m.Installer(Path(os.environ['WEB']),st).run('bundle',sha)))
elif '--rollback ' in a:
 import json
 print(json.dumps(m.Installer(Path(os.environ['WEB']),Path(os.environ['STAGE'])).run('rollback',a.rsplit(' ',1)[1])))
elif a != 'mkdir -p staging': raise RuntimeError(a)
'''
        scp = '''#!/usr/bin/env python3
import os,sys,shutil
from pathlib import Path
shutil.copyfile(sys.argv[-2],Path(os.environ['STAGE'])/sys.argv[-1].rsplit('/',1)[1])
'''
        for name, body in (('ssh',ssh),('scp',scp)):
            (binpath/name).write_text(body);(binpath/name).chmod(0o755)
        env={**os.environ,'PATH':str(binpath)+os.pathsep+os.environ['PATH'], 'HELPER':str(ROOT/'deploy/tmtv-deploy-web.py'),'STAGE':str(self.stage),'WEB':str(self.web)}
        self.stage_bundle(); archive=self.root/'bundle.tar.gz';archive.write_bytes((self.stage/'tmtv-web.tar.gz').read_bytes())
        cmd=['sh',str(ROOT/'scripts/deploy-web.sh'),str(archive),'fixture@host','22','http://127.0.0.1:'+str(server.server_port)]
        try:
            result=subprocess.run(cmd,env=env,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(tree(self.web),self.files)
            self.files['index.html']=b'should roll back'
            self.stage_bundle();archive.write_bytes((self.stage/'tmtv-web.tar.gz').read_bytes());state['bad']=True
            result=subprocess.run(cmd,env=env,capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('rolling back deployment',result.stderr)
            self.assertEqual((self.web/'index.html').read_bytes(),b'new index.html')
        finally:
            server.shutdown();server.server_close();thread.join()


if __name__ == '__main__':
    unittest.main(verbosity=2)
