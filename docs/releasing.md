# Releasing tmtv 3.0

## Candidate validation

Push the candidate to `develop`. Develop validation runs Linux and native macOS
host regressions. Build runs static packaging for five Linux architectures and
both native Macs, isolated relay/browser tests, and the exact twelve-binary
manifest check. These branches do not publish releases or deploy production.
Build also supports `staging` and manual dispatch for repeat validation.

Review all jobs for the exact candidate commit. Download retained fixture logs
and browser screenshots from `relay-e2e-*`, and the verified binaries and
`checksums.txt` from `release-assets`. Missing or failed jobs are not a pass.
Run each target binary on a representative system; QEMU compilation and version
checks are not a substitute for application smoke tests on supported hardware.

On a machine with authorized staging access, check out that same commit and run:

```sh
scripts/staging-deploy.sh --full-tests
```

This command deploys to staging. Confirm `STAGING_HOST`, `STAGING_USER` and
`STAGING_SSH_PORT` before invoking it. Exercise hosting, RW and RO SSH/web access,
Unicode typing, bracketed paste, pane resize/layout changes, disconnect/reconnect,
OSC 8 links and SIXEL. Test native Safari against staging following
[README-relay-e2e.md](../tests/README-relay-e2e.md); Linux WebKit is a separate
engine test. Record commit, OS/browser versions, results and logs in release
validation evidence. Use the project's bd tracker for unresolved failures.

## Website dependency audit

Run `npm audit --prefix site` when validating the candidate. The compatible
lockfile refresh on 2026-10-27 reduced npm's report from 22 to 6 affected packages
(1 critical, 3 high, 2 low); it did not make the dependency audit clean. Remaining
reports involve Astro, its Tailwind integration, sharp, esbuild, zod and zod-to-ts.
The automatic proposed fix upgrades Astro across major versions and needs its
own migration validation. The deployed site is static HTML/JS, not an Astro
server, so server-side advisories do not by themselves establish a remotely
exploitable production issue. Review build-time exposure and the actual use of
affected features before accepting the candidate; do not mark a successful
static build as a security-audit pass.

## Publication and cutover

1. Agree the maintenance window and announce the protocol-11 requirement. Save
   the currently deployed host, relay and web bundle, their checksums and service
   configuration. Confirm the existing server deployment helper's restart and
   backup behavior on the actual host; this repository does not contain it.
2. Merge the validated candidate into the default branch so the manual deployment
   workflow is available. Merging does not deploy. Ensure `configure.ac`, site
   metadata and `docs/releases/v3.0.0.md` describe the intended version. Configure
   required reviewers on the GitHub `production` environment if approval should
   be enforced by GitHub; naming the environment alone does not enforce review.
3. With release authorization, tag the candidate `v3.0.0` and push that tag.
   Build repeats the complete validation and publishes only the verified asset
   set. The tag must match `configure.ac`. Check the published checksums, downloads
   and Homebrew tap update (the tap step can fail without failing publication).
   Publishing updates `latest`; installation scripts may start selecting v3
   before the relay switches, so keep the cutover window short and announced.
4. With deployment authorization, manually run **Deploy published release** with
   tag `v3.0.0` from the trusted default branch. It requires a published stable
   release, checks its manifest and checksums, builds web assets from the resolved
   tag commit, then deploys the relay and web assets. Deployments are serialized.
   There is no deployment on a push to `main` or on publication alone.
5. Upgrade and restart hosting clients, then verify new sessions and both viewer
   access modes against production. Communicate fresh links. Check relay health
   and errors before ending the maintenance window.

Production server and web deployment are sequential, not transactional. If the
web step fails after the server succeeds, repair the web deployment or roll back
the whole bundle; a failed workflow does not automatically restore production.

## Rollback

Keep the previous binaries available before changing the service. If the new
relay or viewer fails, stop the rollout and restore the saved relay, host and web
bundle using the deployment host's documented service procedure. The manual
workflow can also deploy a previous published stable release that contains the
complete-site packaging contract and the same 12-binary/checksum manifest. Older
tags without that contract fail before deployment. Rebuilding historical web
assets requires working historical dependencies; a saved complete tree avoids
that dependency.

Restore the Homebrew formula and GitHub `latest` selection to the intended
previous release as part of an authorized rollback; deploying an old tag does
not change either. Restart hosts using the matching old client and issue new
links. Neither forward deployment nor rollback promises uninterrupted sessions
or preserved links across host restarts. Verify RW/RO access and web rendering
again before announcing recovery.

## Website preview and complete asset delivery

The prepared website labels 3.0 as upcoming and keeps the installer on published
stable 2.0.4. Its shared metadata is in `site/src/data/release.ts`; release prose,
`CHANGELOG.md`, the README and `site/public/llms.txt` must also reflect the actual
publication and relay-cutover state when those events occur. Publishing a preview
page does not authorize switching the public relay or announcing that 3.0 is live.

Build and create a complete local web bundle with:

```sh
npm ci --prefix site
npm run build --prefix site
sh scripts/package-site.sh /absolute/path/to/tmtv-web.tar.gz
```

The bundle includes the nested `/releases/` pages, all static assets, `sitemap.xml`,
and the top-level `viewer.html`/`viewer.js` used by Caddy's session routes. Caddy's
existing static-file fallback can serve those nested pages.

Production and both staging entry points now use this complete bundle through
`scripts/deploy-web.sh`. The driver verifies the installed helper contract before
uploading, supplies the exact bundle SHA-256, verifies public assets byte-for-byte,
and probes both `/s/` and `/j/` viewer shells. A failed probe rolls back that exact
deployment ID; it cannot undo a newer deployment. Viewer-shell probes do not
replace authenticated SSH/SSE/browser integration tests.

The root-owned server helper is versioned at `deploy/tmtv-deploy-web.py`. It needs
Linux `renameat2(RENAME_EXCHANGE)` and Python 3. For an existing site, install it
once through authorized server administration:

```sh
sudo install -o root -g root -m 0755 deploy/tmtv-deploy-web.py /usr/local/bin/tmtv-deploy-web.new
sudo mv -f /usr/local/bin/tmtv-deploy-web.new /usr/local/bin/tmtv-deploy-web
sudo /usr/local/bin/tmtv-deploy-web --check
```

The production workflow checks this contract before restarting the relay. The
admin-owned staging scripts provision the same helper during an authorized staging
deployment. The deployment account must already be permitted to invoke this helper
with sudo; no workflow changes sudoers. Bundles are read from the sudo caller's
`~/staging/tmtv-web.tar.gz`. No-argument legacy invocations still overlay the old
root-file upload set from `/home/deploy/staging`, preserving nested existing files.
New deployments always replace the full tree, so obsolete assets disappear.

The helper rejects invalid checksums, missing required pages, duplicate/traversal
paths, links and special files before activation. It atomically exchanges the
complete `/var/www/tmtv` directory with the prepared tree and retains the previous
site under `/var/www/tmtv-web-backups/<deployment_id>`. It needs no Caddy reload or
relay restart. Backups remain until an operator deliberately removes them.

For a reviewed web-only rollback, use the ID returned by the deployment:

```sh
sudo /usr/local/bin/tmtv-deploy-web --rollback DEPLOYMENT_ID
```

A web rollback does not roll back the relay, host binaries, GitHub release or
Homebrew formula. If the process is interrupted during activation, an
`in-progress.json` record blocks further mutations until an operator inspects the
current tree, receipt and preserved candidate/backup. Do not blindly retry or
remove that record. The normal driver rolls back only after a confirmed install
and failed HTTP verification; uncertain SSH replies require inspection.

Local regressions (`make -C tests web-deploy`) exercise the real installer and
upload driver against temporary trees and a local HTTP fixture, including complete
replacement, malicious/missing bundles, failure preservation and rollback. They
never contact a deployment host.

For a local preview without deployment, use `npm run preview --prefix site --
--host 127.0.0.1`. See `docs/release-positioning.md` for the researched positioning
and `docs/releases/v3.0.0-launch-copy.md` for unpublished announcement drafts.
