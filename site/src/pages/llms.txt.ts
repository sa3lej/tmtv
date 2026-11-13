// llms.txt (https://llmstxt.org/): a short Markdown index for language models.
import type { APIRoute } from "astro";
import { docs } from "../data/docs";
import { release } from "../data/release";

const site = "https://tmtv.se";

export const GET: APIRoute = () => {
  const body = `# tmtv

> tmtv is open-source terminal sharing built on tmux 3.8. Run \`tmtv\` and it prints an ssh command for guests who may type and one for guests who may only watch; guests need only an SSH client. An optional browser viewer and a self-hostable relay are included. Linux and macOS hosts, ISC license, by Lars-Erik Jonsson.

Facts:

- Current stable release: ${release.current} (${release.date}), tmux 3.8, sharing protocol 11. Hosts and relays must run matching major versions; SSH guests never need tmtv.
- Install: \`${release.installer}\` (verifies SHA-256 checksums) or \`${release.homebrew}\`.
- Start sharing: \`tmtv\`. Guests join with \`ssh TOKEN@tmtv.se\` (read-write) or \`ssh ro-TOKEN@tmtv.se\` (read-only).
- Browser sharing is opt-in: \`tmtv set -g tmtv-web-sharing on\` prints https://tmtv.se/s/TOKEN. Browser typing is a separate opt-in: \`tmtv set -g tmtv-web-input on\`.
- The relay can read shared terminal data. SSH encrypts each connection, but there is no end-to-end encryption between host and guests through the relay. Tokens are credentials; session names are not secret.
- Source: ${release.repository} · Releases: ${release.repository}/releases

## Docs

${docs.map((doc) => `- [${doc.nav}](${site}${doc.markdownUrl}): ${doc.description}`).join("\n")}

## Releases

- [tmtv 3.0.0 release notes and upgrade guide](${site}/releases/3-0/): tmux 3.8, sharing protocol 11, coordinated host and relay upgrade.
- [Release log](${site}/releases/)

## Optional

- [Full documentation in one file](${site}/llms-full.txt)
- [README](${release.repository}#readme)
- [Input socket protocol](${release.repository}/blob/develop/docs/input-socket.md)
- [Snapshot protocol and limits](${release.repository}/blob/develop/docs/snapshot-protocol.md)
`;
  return new Response(body, { headers: { "Content-Type": "text/plain; charset=utf-8" } });
};
