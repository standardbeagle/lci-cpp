// Web layer: small request/response helpers.

export interface Request {
  method: string;
  path: string;
}

export function makeRequest(method: string, path: string): Request {
  return { method, path };
}

export function formatStatus(code: number): string {
  if (code >= 500) {
    return "server error";
  }
  if (code >= 400) {
    return "client error";
  }
  if (code >= 300) {
    return "redirect";
  }
  return "ok";
}

export function summarize(reqs: Request[]): string {
  const counts: Record<string, number> = {};
  for (const r of reqs) {
    counts[r.method] = (counts[r.method] || 0) + 1;
  }
  return Object.keys(counts)
    .sort()
    .map((k) => `${k}=${counts[k]}`)
    .join(",");
}
