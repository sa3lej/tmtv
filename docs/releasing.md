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
workflow can also deploy a previous published stable release with the same
12-binary/checksum manifest, but rebuilds that tag's web assets and therefore
requires working historical dependencies. A saved bundle avoids that dependency.

Restore the Homebrew formula and GitHub `latest` selection to the intended
previous release as part of an authorized rollback; deploying an old tag does
not change either. Restart hosts using the matching old client and issue new
links. Neither forward deployment nor rollback promises uninterrupted sessions
or preserved links across host restarts. Verify RW/RO access and web rendering
again before announcing recovery.
