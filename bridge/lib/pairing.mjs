import { randomBytes, timingSafeEqual } from "node:crypto";
import { existsSync, readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname } from "node:path";

export function loadOrCreateToken(filePath) {
  if (existsSync(filePath)) {
    try { const t = JSON.parse(readFileSync(filePath, "utf8")).token; if (t) return t; } catch { /* regenerate */ }
  }
  const token = randomBytes(16).toString("hex");
  mkdirSync(dirname(filePath), { recursive: true });
  writeFileSync(filePath, JSON.stringify({ token }), { mode: 0o600 });
  return token;
}

export function tokensMatch(a, b) {
  if (typeof a !== "string" || typeof b !== "string") return false;
  const ba = Buffer.from(a), bb = Buffer.from(b);
  if (ba.length !== bb.length) return false;
  return timingSafeEqual(ba, bb);
}
