// Documentation pages live as Markdown in src/docs. Each page is rendered at
// /docs/<slug>/ and its source is published unchanged at /docs/<slug>.md.
import type { MarkdownHeading } from "astro";

export interface DocFrontmatter {
  title: string;
  nav: string;
  order: number;
  description: string;
  updated: string | Date;
}

interface DocModule {
  frontmatter: DocFrontmatter;
  Content: any;
  compiledContent: () => string | Promise<string>;
  getHeadings: () => MarkdownHeading[];
}

const modules = import.meta.glob<DocModule>("../docs/*.md", { eager: true });
const sources = import.meta.glob<string>("../docs/*.md", { eager: true, query: "?raw", import: "default" });

const slugOf = (path: string) => path.split("/").pop()!.replace(/\.md$/, "");

export const docs = Object.entries(modules)
  .map(([path, module]) => {
    const slug = slugOf(path);
    const source = sources[path].replace(/^---\n[\s\S]*?\n---\n+/, "");
    return {
      slug,
      url: slug === "index" ? "/docs/" : `/docs/${slug}/`,
      markdownUrl: `/docs/${slug}.md`,
      ...module.frontmatter,
      // YAML turns 2026-11-13 into a Date; keep the plain ISO date.
      updated: new Date(module.frontmatter.updated).toISOString().slice(0, 10),
      Content: module.Content,
      html: module.compiledContent,
      headings: module.getHeadings(),
      markdown: source,
    };
  })
  .sort((a, b) => a.order - b.order);

export type Doc = (typeof docs)[number];
