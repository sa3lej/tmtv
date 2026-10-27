#!/usr/bin/env python3
"""Compare deployed public assets with the bundle and probe both viewer aliases."""
import hashlib
import sys
import tarfile
import urllib.request

ROUTES = {'/': 'index.html', '/releases/': 'releases/index.html',
          '/releases/3-0/': 'releases/3-0/index.html', '/sitemap.xml': 'sitemap.xml',
          '/robots.txt': 'robots.txt', '/llms.txt': 'llms.txt',
          '/social-card.png': 'social-card.png', '/install.sh': 'install.sh',
          '/viewer.js': 'viewer.js'}


def verify(bundle, base):
    if not base.startswith(('https://', 'http://127.0.0.1:')):
        raise ValueError('Expected HTTPS public origin (or loopback HTTP fixture)')
    with tarfile.open(bundle, 'r:gz') as archive:
        members = {m.name.removeprefix('./'): m for m in archive if m.isfile()}
        for route, filename in ROUTES.items():
            expected = archive.extractfile(members[filename]).read()
            with urllib.request.urlopen(base.rstrip('/') + route, timeout=20) as response:
                actual = response.read(len(expected) + 1)
            if actual != expected:
                raise ValueError('Deployed asset differs from bundle: ' + route)
            print('Verified ' + route + ' sha256:' + hashlib.sha256(actual).hexdigest())
        for route in ('/s/deployment-check', '/j/deployment-check'):
            with urllib.request.urlopen(base.rstrip('/') + route, timeout=20) as response:
                body = response.read(1024 * 1024).decode('utf-8')
            if 'id="password-form"' not in body or '/viewer.js' not in body:
                raise ValueError('Missing production viewer at ' + route)
            print('Verified viewer shell ' + route)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('Usage: verify-web.py BUNDLE BASE_URL')
    verify(*sys.argv[1:])
