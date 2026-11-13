import type { APIRoute } from "astro";
import { docs } from "../data/docs";

const site = "https://tmtv.se";
// Dates are when each page's content last changed.
const pages = [
  { path: "/", lastmod: "2026-11-13" },
  ...docs.map((doc) => ({ path: doc.url, lastmod: doc.updated })),
  { path: "/releases/", lastmod: "2026-11-13" },
  { path: "/releases/3-0/", lastmod: "2026-11-13" },
];

export const GET: APIRoute = () => {
  const urls = pages.map((page) => `  <url>\n    <loc>${site}${page.path}</loc>\n    <lastmod>${page.lastmod}</lastmod>\n  </url>`).join("\n");
  const body = `<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n${urls}\n</urlset>\n`;
  return new Response(body, { headers: { "Content-Type": "application/xml; charset=utf-8" } });
};
