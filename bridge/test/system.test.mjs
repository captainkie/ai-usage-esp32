import { test } from "node:test";
import assert from "node:assert/strict";
import { parseCpu, parseMem, parseDf, parseNetstat, netRate, parseBattery, parseTop } from "../lib/system.mjs";

test("parseCpu takes the last sample and sums user+sys", () => {
  const s = `Processes: 1\nCPU usage: 5.0% user, 5.0% sys, 90.0% idle\nCPU usage: 12.5% user, 7.5% sys, 80.0% idle\n`;
  assert.equal(parseCpu(s), 20);           // last sample, 12.5+7.5
});

test("parseMem computes used% and GB from vm_stat + memsize", () => {
  const vm = [
    "Mach Virtual Memory Statistics: (page size of 16384 bytes)",
    "Pages free:                          100000.",
    "Pages active:                        300000.",
    "Pages wired down:                    150000.",
    "Pages occupied by compressor:         50000.",
  ].join("\n");
  const memBytes = 16 * 1024 ** 3;         // 16 GiB
  const m = parseMem(vm, memBytes);
  // used pages = 300000+150000+50000 = 500000 * 16384 = 8192000000 bytes = 7.63 GiB
  assert.equal(m.total_gb, 16);
  assert.equal(m.used_gb, 7.6);
  assert.equal(m.util, 48);                // round(7.63/16*100)
});

test("parseDf reads the / data line", () => {
  const df = [
    "Filesystem 1024-blocks      Used Available Capacity iused ifree %iused Mounted on",
    "/dev/disk3s5 971350180 380000000 400000000    74%  100 200   1%   /",
  ].join("\n");
  const d = parseDf(df);
  assert.equal(d.util, 74);
  assert.equal(d.mount, "/");
  assert.equal(d.total_gb, 926.4);         // 971350180 KiB / 1024^2 = 926.3517 -> 926.4
  assert.equal(d.used_gb, 362.4);          // 380000000 KiB / 1024^2
});

test("parseDf returns null on empty or header-only output (graceful)", () => {
  assert.equal(parseDf(""), null);
  assert.equal(parseDf("Filesystem 1024-blocks Used Available Capacity Mounted on"), null);
});

test("parseNetstat sums the first en0 row bytes", () => {
  const ns = [
    "Name  Mtu   Network       Address            Ipkts Ierrs     Ibytes    Opkts Oerrs     Obytes  Coll",
    "en0   1500  <Link#4>      a1:b2:c3:d4:e5:f6   1000     0    2000000     900     0     500000     0",
    "en0   1500  192.168.1     192.168.1.20        1000     0    2000000     900     0     500000     0",
  ].join("\n");
  assert.deepEqual(parseNetstat(ns, "en0"), { rx: 2000000, tx: 500000 });
});

test("netRate turns byte deltas into KB/s and clamps counter resets", () => {
  const r = netRate({ rx: 1000000, tx: 200000 }, { rx: 3048576, tx: 404800 }, 2000);
  assert.equal(r.down_kbps, 1000);         // (2048576 B / 2 s) / 1024
  assert.equal(r.up_kbps, 100);            // (204800 B / 2 s) / 1024
  assert.deepEqual(netRate({ rx: 5, tx: 5 }, { rx: 1, tx: 1 }, 1000), { down_kbps: 0, up_kbps: 0 });
});

test("parseBattery reads percent + charging, null when no battery", () => {
  const batt = "Now drawing from 'Battery Power'\n -InternalBattery-0 (id=123)\t82%; discharging; 3:12 remaining present: true";
  assert.deepEqual(parseBattery(batt), { percent: 82, charging: false });
  assert.equal(parseBattery("Now drawing from 'AC Power'"), null);
});

test("parseTop returns the top N by cpu, basename only", () => {
  const ps = " %CPU COMM\n 42.0 /Applications/Google Chrome.app/Contents/MacOS/Google Chrome\n 21.0 node\n 14.0 /usr/bin/Xcode\n 2.0 loginwindow";
  assert.deepEqual(parseTop(ps, 3), [
    { name: "Google Chrome", cpu: 42 },
    { name: "node", cpu: 21 },
    { name: "Xcode", cpu: 14 },
  ]);
});

test("parseTop keeps reverse-DNS names (no '.app' collapse to 'com')", () => {
  // real `ps -Aceo pcpu,comm` emits bare comm names like these
  const ps = " %CPU COMM\n 30.0 com.apple.WebKit.WebContent\n 12.0 com.apple.Virtualization.VirtualMachine";
  assert.deepEqual(parseTop(ps, 2), [
    { name: "com.apple.WebKit.WebContent", cpu: 30 },
    { name: "com.apple.Virtualization.VirtualMachine", cpu: 12 },
  ]);
});
