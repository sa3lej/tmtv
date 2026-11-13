// All documentation pages as one plain Markdown file, for tools that prefer a single fetch.
import type { APIRoute } from "astro";
import { docs } from "../data/docs";

export const GET: APIRoute = () => {
  const parts = docs.map((doc) => `<!-- Source: https://tmtv.se${doc.url} (updated ${doc.updated}) -->\n\n${doc.markdown.trim()}\n`);
  const body = `# tmtv documentation\n\nAll pages from https://tmtv.se/docs/ in one file.\n\n${parts.join("\n---\n\n")}`;
  return new Response(body, { headers: { "Content-Type": "text/plain; charset=utf-8" } });
};
