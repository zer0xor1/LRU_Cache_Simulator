// Drop-in embed for a Next.js (App Router or Pages Router) portfolio.
//
// 1. Copy web/index.html and web/cachesim.js into public/cachesim/
// 2. Use the component:  <CacheSimEmbed />  or  <CacheSimEmbed trace="matrix" policy="SRRIP" />
//
// The page is self-contained (no network requests), so the iframe needs no
// extra permissions. Height is fixed; the page scrolls inside on small screens.
type Props = {
  trace?: "seq" | "conflict" | "loop" | "hot" | "writes" | "matrix" | "custom";
  policy?: "LRU" | "FIFO" | "RANDOM" | "PLRU" | "SRRIP" | "OPT";
  assoc?: "1" | "2" | "4" | "8" | "full";
  height?: number;
  src?: string;
};

export default function CacheSimEmbed({ trace = "loop", policy, assoc, height = 1300, src = "/cachesim/index.html" }: Props) {
  const params = new URLSearchParams({ trace });
  if (policy) params.set("policy", policy);
  if (assoc) params.set("assoc", assoc);
  return (
    <iframe
      src={`${src}#${params.toString()}`}
      title="Interactive cache simulator"
      loading="lazy"
      style={{ width: "100%", height, border: 0, borderRadius: 12, background: "#0f1214" }}
    />
  );
}
