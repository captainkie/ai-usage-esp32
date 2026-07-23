# Seamless Connectivity (WiFiMulti + TF-card config) — Design

> **Status:** design approved 2026-07-23 (home session). Refines and supersedes the
> WiFiMulti + TF-card pieces of `docs/superpowers/plans/2026-07-23-seamless-connectivity-and-usb-actions.md`.
> Build-then-verify-**with-user**; do NOT flash over the working display unattended.

## 1. Context — what already exists

The "seamless" idea was scoped earlier into five pieces. Three are already shipped:

| Piece | Status |
|-------|--------|
| USB actions (Remote over the cable, no Wi-Fi) | ✅ merged to main (PR #11) |
| mDNS auto-discover the Mac (`_aiusage._tcp`, `net_discover()`) | ✅ on `feat/seamless-firmware` (PR #13), unverified on-device |
| Tap the LIVE indicator → reopen setup portal | ✅ merged to main (PR #11) |
| **WiFiMulti — save multiple networks, auto-join** | ⬜ this spec |
| **TF-card `pixie.json` config** | ⬜ this spec |

This spec covers the two remaining pieces, plus a **security posture** and **user-facing
security docs** that the earlier plan did not address.

**Branch:** continue on `feat/seamless-firmware` (already carries mDNS) → the resulting PR
becomes the complete "seamless" PR and **supersedes draft PR #13**. Independent of the
voice work on `feat/voice-firmware` (they are separate features off `main`).

## 2. Goal

Carry Pixie between home ↔ office (and phone hotspot) with **no re-typing of Wi-Fi or
the Mac IP**, with the connection config **portable on a TF card** so it survives a
reflash and can be pre-loaded for a giveaway — **without leaving the Wi-Fi password on a
removable card longer than necessary**.

## 3. Architecture overview

Two responsibilities, kept as separate concerns:

- **Provisioning UI** stays with **WiFiManager** (tzapu) — its captive portal is the
  fallback for entering a network + token when there is no card and nothing saved.
- **Connection** moves from WiFiManager `autoConnect` (single network) to **WiFiMulti**
  (`addAP()` per network, `run()` joins the strongest reachable one).

The multiple networks come from a single **AP store in NVS** (our own small JSON blob).
That store is **seeded** from two sources and is the sole input to WiFiMulti:

```
  TF card /pixie.json  ──import──►  NVS AP store  ──build──►  WiFiMulti.run()
  Captive portal save  ──append──►      ▲
                                        │ (source of truth for connection)
```

**Import-and-forget:** the card is read **once at boot** to top up the NVS store, then the
card is not needed — the password lives in NVS (soldered flash), and the card can be
physically removed. Keeping the card inserted makes the config survive a reflash
(NVS-erasing) because the next boot re-imports. **The firmware never writes to the SD
card** — this avoids FAT corruption on power loss and keeps the removable card from
accumulating secrets the user did not put there.

## 4. Components (units + interfaces)

Small, independently-testable units. Each answers: what it does / how you use it / what it
depends on.

### 4.1 `sdconf.h` (new) — read the card config
- **Does:** mount the SD card (Waveshare BSP `04_SD_Card` driver, `sdcard_bsp.*`; note the
  `GPIO_NUM_8` power-enable), open `/pixie.json`, parse it (ArduinoJson v6) into a
  `PixieConfig` struct. Read-only; never writes.
- **Interface:**
  ```c
  typedef struct { char ssid[33]; char pass[64]; } WifiCred;
  typedef struct {
    WifiCred wifi[MAX_WIFI_APS]; int wifi_n;
    char token[41];
    char host[24];      // "" = discover via mDNS
    char port[6];       // "" = DEFAULT_BRIDGE_PORT
    bool present;       // a card was mounted AND /pixie.json parsed
  } PixieConfig;
  bool sdconf_load(PixieConfig *out);   // false if no card / no file / bad JSON
  ```
- **Depends on:** SD BSP, ArduinoJson, config.h.

### 4.2 `wifistore.h` (new) — the NVS AP store
- **Does:** persist and retrieve the list of `{ssid,pass}` in NVS (JSON string in the
  `aiusage` Preferences namespace, key `aps`). Dedup by SSID.
- **Interface:**
  ```c
  int  wifistore_load(WifiCred out[], int max);          // -> count
  void wifistore_merge(const WifiCred *e);               // add new SSID, or update pass of existing
  // (persist is internal to merge; the whole list is re-serialized on change)
  ```
- **Merge semantics:** new SSID → appended; beyond `MAX_WIFI_APS` → log + skip (no
  eviction); existing SSID → password overwritten (so **editing the card or
  re-provisioning updates the stored password**).
- **Depends on:** Preferences, ArduinoJson, config.h.

### 4.3 `net.h` (modify) — orchestration
- `net_begin()` rewritten to the boot flow in §6. Uses `wifistore_*` + `sdconf_load` +
  `WiFiMulti` + existing `net_discover()` (mDNS) + WiFiManager portal (fallback only).
- Portal save callback additionally captures the chosen creds (`wm.getWiFiSSID()` /
  `wm.getWiFiPass()`) into the AP store via `wifistore_merge`, so a portal-configured
  network joins on the next boot like any other.
- `net_fetch` / `net_discover` / `net_action` / USB paths **unchanged**.

### 4.4 `config.h` (modify)
- `#define MAX_WIFI_APS 6`
- `#define SD_CONFIG_PATH "/pixie.json"`

### 4.5 `ai-usage-esp32.ino` (modify)
- setup() order: draw the setup/boot screen → `sdconf_load` → `net_begin` (which imports +
  connects). Show a brief "importing config…" line on the setup screen when a card is
  found (serial log always).

### 4.6 BSP dependency
- Copy the Waveshare `Examples/Arduino/04_SD_Card` driver (`sdcard_bsp.h/.cpp`) into the
  sketch, alongside the existing display + codec `src/` dirs. Add this to the firmware
  assembly instructions (README) next to the `lv_demo_widgets` note.

### 4.7 `firmware/pixie.example.json` (new) — user template
- A valid example file matching the §5 schema (with placeholder values) that a user copies
  to the card root and renames to `pixie.json`. Because JSON has no comments, the field
  meanings live in the README security/setup section, not inline.

## 5. `pixie.json` schema + precedence

At the SD-card root. Field names align with the earlier plan (flat, easy to hand-edit).

```json
{
  "wifi": [
    { "ssid": "HomeNet",   "pass": "home-password" },
    { "ssid": "OfficeNet", "pass": "office-password" },
    { "ssid": "iPhone",    "pass": "hotspot-password" }
  ],
  "token": "pairing-token-from-the-bridge",
  "bridge_host": "",
  "bridge_port": 8787
}
```

- `wifi[]` — up to `MAX_WIFI_APS`. WiFiMulti joins the strongest **in range**; array order
  is documentation, not strict priority.
- `token` — the bridge pairing token (gates Remote + Voice). Imported into NVS.
- `bridge_host` — leave `""` to auto-discover via mDNS; set to override (office VLANs that
  block mDNS).
- `bridge_port` — optional; defaults to `DEFAULT_BRIDGE_PORT` (8787).

**Precedence (effective):** `card (when present) > NVS/portal-saved`. At boot the NVS store
is loaded first, then the card is merged **over** it (card wins for the fields it
specifies). Consequence to document: if you re-provision via the portal while a card is
inserted, update `pixie.json` (or remove the card) or the card value returns next boot.

Not to be confused with `firmware/pixie-wakeword/pixie.json` (the wake-word manifest) — a
different file in a different place.

## 6. Boot data flow (`net_begin`)

```
1. Load host/port/token + AP list from NVS. provisioned = prov-flag || host saved || AP list non-empty.
2. sdconf_load(&cfg):
     if cfg.present:
       for each cfg.wifi[i]: wifistore_merge(&cfg.wifi[i])   // seed / update NVS
       if cfg.token[0]: save token to NVS
       if cfg.host[0] : save host to NVS   (else leave blank -> mDNS)
       if cfg.port[0] : save port to NVS
       provisioned = true
3. Build WiFiMulti from the NVS AP list (addAP each). 
4. If the list is non-empty: WiFiMulti.run(timeout) -> joins strongest reachable.
5. If connected and host is blank: net_discover() (mDNS) fills host/port.
6. If NOT provisioned and nothing joined: open the captive portal (blocks first-run only),
   capture creds -> wifistore_merge, set prov, retry.
   If provisioned but nothing joined (carried to an unknown place): do NOT block — return so
   USB-serial still drives the display; tapping LIVE reopens the portal on demand.
```

## 7. Security model (new — the reason for import-and-forget)

Neither store is encrypted by default; the difference is **exposure**:

- **NVS (on-board flash):** requires physical possession **and** a flash dump (esptool) to
  read. Soldered — the higher bar. This is where the password ends up living.
- **TF card (`pixie.json`, FAT):** removable; readable on any PC. The lower bar.

Design choices that follow:
- Wi-Fi passwords are **imported into NVS and the card can be removed** ("import-and-forget").
  Plaintext on the card is transient by design.
- The firmware **never writes** secrets to the card (no SD write at all).
- Passwords are **never printed to serial** (log SSID + a redacted marker, never `pass`).
- The **pairing token** is treated like a password: it gates Remote + Voice; if exposed,
  rotate it at the bridge.

This is a hobby/giveaway device on a home LAN; the posture is "reasonable, documented, not
false-secure." **Flash encryption / secure-NVS (eFuse) is explicitly out of scope** — it
complicates giveaway flashing and risks bricking for little real gain here.

## 8. User-facing docs deliverable (README)

Add a **Security** subsection to the repo README (and/or firmware README) so a recipient
understands the trade-offs:
- Where the Wi-Fi password is stored (NVS, on-board, not encrypted) and what that means.
- That `pixie.json` on the card is **plaintext, readable on any PC** → **remove the card
  after the first boot** (import-and-forget), or accept the risk if left in.
- The **pairing token** is a secret; rotate at the bridge if leaked.
- Recommend a **guest / dedicated Wi-Fi** for a giveaway unit.
- How to create `pixie.json` (schema from §5) and the precedence rule (card > portal).

## 9. Error handling / edge cases

- **No card / no file / bad JSON:** `sdconf_load` returns false → behave exactly as today
  (NVS + portal). Never a hard failure.
- **Card present but `wifi` empty / malformed entry:** skip that entry, keep valid ones.
- **AP list full (`MAX_WIFI_APS`):** new SSID beyond the cap → log + skip (do not evict).
- **Nothing in range but provisioned:** non-blocking; USB drives the display (existing
  behavior preserved).
- **mDNS blocked (office VLAN):** `bridge_host` override in the card, or USB — both already
  exist.
- **SD ↔ display bus:** display is QSPI (AXS15231B), SD is a separate bus in the BSP;
  confirm no pin/power conflict on-device (the `GPIO_NUM_8` power-enable in particular).

## 10. Testing & verification

**Host-side (before flashing):**
- Unit-test the `pixie.json` → `PixieConfig` parse with a tiny host harness (feed sample
  JSON strings incl. malformed / empty / over-cap; assert parsed fields). Mirrors the
  existing `design/tools/mascot_ctest.c` pattern.
- `arduino-cli compile` green (reassemble per the "COMPILE-VERIFIED GREEN" recipe + the
  new `04_SD_Card` BSP dir).

**On-device (WITH the user, flat home LAN — flash once, keep known-good recoverable):**
- [ ] Card with home + a fake office SSID → boot → serial shows parsed networks → auto-joins home.
- [ ] mDNS: `bridge_host` blank → device finds the Mac by name (no IP typed) → dashboard live.
- [ ] Remove the card → reboot → still joins home (NVS persisted).
- [ ] Edit the card password → reboot → NVS updates (joins with the new password).
- [ ] No card + fresh NVS → captive portal opens; save → joins; reboot → auto-joins (no re-type).
- [ ] Regression: dashboard fine over USB + Wi-Fi; USB frames still drive the display; Remote unaffected.

## 11. Out of scope

- SD **write-back** (the earlier plan's "write merged config back to the card") — dropped
  in favor of read-only + import-and-forget.
- Flash encryption / secure-NVS.
- Office enterprise/5GHz Wi-Fi join (unchanged; USB is the office answer).
- Voice / wake-word work (separate branch).
```