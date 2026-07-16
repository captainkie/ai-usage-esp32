// Pure parsers for macOS system stats. Each takes command output (a string)
// and returns plain data — so they are unit-testable with fixtures.
import { execFile } from "node:child_process";

const GiB = 1024 ** 3;
const r1 = (x) => Math.round(x * 10) / 10;

export function parseCpu(topOut) {
  const m = [...topOut.matchAll(/CPU usage:\s*([\d.]+)%\s*user,\s*([\d.]+)%\s*sys/g)];
  if (!m.length) return null;
  const last = m[m.length - 1];
  return Math.round(parseFloat(last[1]) + parseFloat(last[2]));
}

export function parseMem(vmStat, memBytes) {
  const page = Number((vmStat.match(/page size of (\d+) bytes/) || [])[1]) || 4096;
  const pages = (label) => Number((vmStat.match(new RegExp(label + ":\\s+(\\d+)\\.")) || [])[1]) || 0;
  const used = (pages("Pages active") + pages("Pages wired down") + pages("Pages occupied by compressor")) * page;
  return { util: Math.round((used / memBytes) * 100), used_gb: r1(used / GiB), total_gb: r1(memBytes / GiB) };
}

export function parseDf(dfOut) {
  const line = dfOut.trim().split("\n").filter(Boolean).at(-1).trim().split(/\s+/);
  const total = Number(line[1]) * 1024, used = Number(line[2]) * 1024;   // KiB -> bytes
  return { util: parseInt(line[4], 10), used_gb: r1(used / GiB), total_gb: r1(total / GiB), mount: line.at(-1) };
}

export function parseNetstat(nsOut, iface = "en0") {
  const row = nsOut.split("\n").find((l) => l.startsWith(iface + " ") || l.startsWith(iface + "\t"));
  if (!row) return { rx: 0, tx: 0 };
  const c = row.trim().split(/\s+/);
  return { rx: Number(c[6]) || 0, tx: Number(c[9]) || 0 };
}

export function netRate(prev, cur, dtMs) {
  const dt = dtMs / 1000;
  const rate = (a, b) => (b >= a && dt > 0 ? Math.round((b - a) / dt / 1024) : 0);
  return { down_kbps: rate(prev.rx, cur.rx), up_kbps: rate(prev.tx, cur.tx) };
}

export function parseBattery(pmset) {
  const pct = pmset.match(/(\d+)%/);
  if (!pct) return null;
  return { percent: Number(pct[1]), charging: /charging|charged|AC Power/i.test(pmset) && !/discharging/i.test(pmset) };
}

export function parseTop(psOut, n = 3) {
  return psOut.split("\n").map((l) => l.trim()).filter(Boolean)
    .map((l) => l.match(/^([\d.]+)\s+(.+)$/)).filter(Boolean)
    .filter((m) => !Number.isNaN(parseFloat(m[1])) && /^[\d.]/.test(m[1]))
    .map((m) => ({ cpu: Math.round(parseFloat(m[1])), name: m[2].split("/").at(-1).replace(/\.app.*$/, "").trim() }))
    .slice(0, n);
}

function run(file, args) {
  return new Promise((res) => execFile(file, args, { timeout: 4000 }, (e, out) => res(e ? "" : out)));
}

// Orchestrator: shells out, returns { system, net } where net is the raw
// counter sample to pass back next time for rate calculation.
export async function readSystem(prevNet = null, dtMs = 15000) {
  const [top, vm, mem, df, ns, batt, ps] = await Promise.all([
    run("top", ["-l", "2", "-n", "0"]), run("vm_stat", []),
    run("sysctl", ["-n", "hw.memsize"]), run("df", ["-k", "/"]),
    run("netstat", ["-ibn"]), run("pmset", ["-g", "batt"]),
    run("ps", ["-Aceo", "pcpu,comm", "-r"]),
  ]);
  const curNet = parseNetstat(ns);
  const memBytes = Number(mem.trim()) || 0;
  return {
    system: {
      cpu: parseCpu(top) != null ? { util: parseCpu(top) } : null,
      mem: memBytes ? parseMem(vm, memBytes) : null,
      disk: parseDf(df),
      net: prevNet ? netRate(prevNet, curNet, dtMs) : { down_kbps: 0, up_kbps: 0 },
      battery: parseBattery(batt),
      top: parseTop(ps, 3),
    },
    net: curNet,
  };
}
