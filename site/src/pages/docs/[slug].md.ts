import type { APIRoute } from "astro";
import { docs } from "../../data/docs";

export function getStaticPaths() {
  return docs.map((doc) => ({ params: { slug: doc.slug }, props: { markdown: doc.markdown } }));
}

export const GET: APIRoute = ({ props }) =>
  new Response(props.markdown as string, { headers: { "Content-Type": "text/markdown; charset=utf-8" } });
