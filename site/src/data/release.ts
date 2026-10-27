// Change only after release publication AND the public relay cutover are verified.
export const release = {
  current: "2.0.4",
  upcoming: "3.0.0",
  stage: "preview",
  repository: "https://github.com/sa3lej/tmtv",
  installer: "curl -fsSL https://tmtv.se/install.sh | sh",
  homebrew: "brew install sa3lej/tap/tmtv",
} as const;
