// Release bundle metadata. Deploy only after this release is published and the relay is upgraded.
export const release = {
  current: "3.0.0",
  stage: "released",
  repository: "https://github.com/sa3lej/tmtv",
  installer: "curl -fsSL https://tmtv.se/install.sh | sh",
  homebrew: "brew install sa3lej/tap/tmtv",
} as const;
