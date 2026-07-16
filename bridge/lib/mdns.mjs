import { spawn } from "node:child_process";

export function dnsSdArgs(port, name = "AI Usage Bridge") {
  return ["-R", name, "_aiusage._tcp", "local", String(port)];
}

// Best-effort Bonjour advertisement via Apple's dns-sd. Returns the child so
// the caller can keep it alive; failures are non-fatal (Wi-Fi still works with
// a typed IP). The process must stay running for the record to persist.
export function advertise(port, name = "AI Usage Bridge") {
  try {
    const child = spawn("dns-sd", dnsSdArgs(port, name), { stdio: "ignore" });
    child.on("error", () => {});
    return child;
  } catch { return null; }
}
