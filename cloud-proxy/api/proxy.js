/**
 * Claude.ai API Proxy - Vercel Edge Function
 *
 * Relays Claude.ai usage API requests from the ESP32.
 * The ESP32 can't connect directly to claude.ai because Cloudflare drops
 * its mbedTLS handshake. Vercel uses Fastly TLS which passes through.
 *
 * Deploy:
 *   npm i -g vercel
 *   cd cloud-proxy
 *   vercel --prod
 *
 * Then update CLAUDE_PROXY_BASE in claude_monitor.ino with the URL.
 *
 * Routes (configured in vercel.json rewrites):
 *   GET /ping                                    -> health check
 *   GET /api/organizations                       -> proxy
 *   GET /api/organizations/{uuid}/usage          -> proxy
 *   GET /api/organizations/{uuid}/overage_spend_limit -> proxy
 *
 * Auth: ESP32 sends the Claude sessionKey via X-Session-Key header.
 * The proxy never stores or logs it.
 */

export const config = { runtime: "edge" };

const CLAUDE_HOST = "https://claude.ai";

const BROWSER_HEADERS = {
  accept: "*/*",
  "accept-language": "en-US,en;q=0.9",
  "content-type": "application/json",
  "anthropic-client-platform": "web_claude_ai",
  "anthropic-client-version": "1.0.0",
  "user-agent":
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) " +
    "AppleWebKit/537.36 (KHTML, like Gecko) " +
    "Chrome/131.0.0.0 Safari/537.36",
  origin: "https://claude.ai",
  referer: "https://claude.ai/settings/usage",
  "sec-fetch-dest": "empty",
  "sec-fetch-mode": "cors",
  "sec-fetch-site": "same-origin",
};

export default async function handler(request) {
  const url = new URL(request.url);
  const path = url.searchParams.get("p");

  // Health check
  if (!path || path === "ping") {
    return Response.json({ ok: true, ts: Date.now() });
  }

  if (request.method !== "GET") {
    return new Response("Method not allowed", { status: 405 });
  }

  const sessionKey = request.headers.get("X-Session-Key");
  if (!sessionKey) {
    return Response.json(
      { error: "Missing X-Session-Key header" },
      { status: 401 }
    );
  }

  // Preserve any query params beyond ?p=
  url.searchParams.delete("p");
  const search = url.search;
  const upstreamUrl = `${CLAUDE_HOST}/api/${path}${search}`;

  try {
    const resp = await fetch(upstreamUrl, {
      method: "GET",
      headers: {
        ...BROWSER_HEADERS,
        Cookie: `sessionKey=${sessionKey}`,
      },
    });

    const body = await resp.arrayBuffer();
    return new Response(body, {
      status: resp.status,
      headers: {
        "Content-Type": "application/json",
        "Access-Control-Allow-Origin": "*",
      },
    });
  } catch (e) {
    return Response.json({ error: e.message }, { status: 502 });
  }
}
