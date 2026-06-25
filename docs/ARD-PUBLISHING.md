# Publishing LCI for Agentic Resource Discovery (ARD)

[ARD](https://agenticresourcediscovery.org/how_to_publish/) lets AI agents and
chatbots discover MCP servers, skills, and tools by crawling a well-known JSON
catalog. This repo ships a ready-to-host catalog for the LCI MCP server.

## Files

| File | Hosted as | Purpose |
|------|-----------|---------|
| [`.well-known/ai-catalog.json`](../.well-known/ai-catalog.json) | `https://<domain>/.well-known/ai-catalog.json` | ARD catalog: lists the LCI MCP server, its 14 tools, and example queries |
| [`.well-known/lci-mcp.json`](../.well-known/lci-mcp.json) | the catalog entry's `url` | stdio MCP config (`command: lci`, `args: [mcp]`) + install one-liners |

The catalog points at LCI's MCP server, which runs locally over stdio (`lci mcp`)
— there is no hosted HTTP endpoint to keep alive, only these two static files.

## Hosting (one of)

ARD requires **HTTPS**, `Content-Type: application/json`, and
`Access-Control-Allow-Origin: *` on the served files.

1. **Domain root** (`standardbeagle.com`) — copy `.well-known/` to the web root.
   Set the JSON content-type and the CORS header on that path.

2. **GitHub Pages user site** (`standardbeagle.github.io`) — publish these files
   from a `standardbeagle.github.io` repo. GitHub Pages serves `.well-known/`
   and already sends `Access-Control-Allow-Origin: *`, so no extra config.

3. **DNS TXT pointer** — if the catalog cannot live at the domain root, add a
   TXT record `_catalog._agents.standardbeagle.com` whose value is the full
   catalog URL (e.g. a GitHub Pages project path or raw URL).

## When the host domain changes

The identifiers are domain-anchored. If you serve from a domain other than
`standardbeagle.com`, update in `.well-known/ai-catalog.json`:

- `host.identifier` → `did:web:<domain>`
- each `entries[].identifier` → `urn:air:<domain>:server:lci`
- `entries[].url` → the hosted location of `lci-mcp.json`

## Verify

```sh
curl -fsSL https://<domain>/.well-known/ai-catalog.json | python3 -m json.tool
curl -fsSI https://<domain>/.well-known/ai-catalog.json | grep -i 'content-type\|access-control'
```
