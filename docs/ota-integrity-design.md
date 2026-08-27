# OTA Firmware Integrity — Proposed Design (issue #10)

**Status:** Draft / proposal. No firmware behavior changes in this PR — this
document only captures the design so it can be reviewed and the cross-repo work
scheduled.

## Problem

The OTA path has no integrity guarantee at any hop:

| Location (`src/main.cpp`) | Issue |
|---|---|
| `client.setInsecure()` before the `/api/firmware/latest` request | server cert not validated |
| `client.setInsecure()` before the `/count` request | server cert not validated |
| `httpUpdate.update(client, updateUrl, CURRENT_VERSION)` | version compare only — no signature check on the binary |
| `webSocket.beginSSL(serverAddress, wsPort, wsPath, "", "")` | empty fingerprint — WSS cert not validated |

GamerBell serves whatever `.bin` is attached to the latest GitHub Release, with
no signature or checksum. Anyone able to MITM the device network, spoof DNS for
`gamerbell.fitznet.doomdns.org`, or push a GitHub release can flash arbitrary
firmware — RCE on hardware inside the home network.

`.github/agents.md` currently documents `setInsecure()` as intentional ("no cert
pinning by design ... don't add pinning without coordinating with GamerBell's
certificate rotation"). This proposal is the coordination step; adopting it means
updating that note.

## Proposed design (defense in depth)

### 1. Firmware signature verification (primary control)

- Generate an Ed25519 keypair. Private key lives only in GitHub Actions secrets
  for `Esp32FitznetBell`; public key is embedded in the firmware as a
  `const uint8_t OTA_PUBKEY[32]` in `src/main.cpp`.
- `release.yml` signs `firmware.bin` after `pio run`:
  - compute `SHA-256(firmware.bin)`
  - sign the digest with the Ed25519 key
  - publish `firmware.bin.sig` (64-byte raw signature) and `firmware.bin.sha256`
    as additional release assets.
- GamerBell `FirmwareService` exposes the signature alongside the binary, e.g.
  `GET /api/firmware/latest.sig` (or an `X-Firmware-Signature: <base64>` response
  header on the existing endpoint).
- On device: use `httpUpdate` in the mode where the stream is buffered and
  verified before commit. Two viable implementations:
  - **`Update.installSignature()` / signed-update support** built into the ESP32
    `Update` library: append the signature trailer to the `.bin` and provide a
    `UpdateClass` verifier seeded with `OTA_PUBKEY`. This is the least custom code.
  - **Manual verify:** download to the inactive OTA partition without committing,
    compute SHA-256 over the written bytes, verify the Ed25519 signature with
    `mbedtls`, and only then call `Update.end()` to set the boot partition.
- If verification fails: abort, do not switch partitions, `queueDeviceLog("error",
  "ota", "signature verify failed")`, keep running current firmware.

### 2. Re-enable TLS validation (secondary control)

- Embed the CA that issues `gamerbell.fitznet.doomdns.org` (Let's Encrypt ISRG
  Root X1 today) as `const char SERVER_ROOT_CA[]` in `src/main.cpp`.
- Replace both `client.setInsecure()` calls with `client.setCACert(SERVER_ROOT_CA)`.
- Replace `webSocket.beginSSL(serverAddress, wsPort, wsPath, "", "")` with the
  `beginSslWithCA` overload passing `SERVER_ROOT_CA`.
- Prefer pinning the **root CA** (survives leaf renewals) over a leaf SHA-1
  fingerprint (breaks every ~60–90 days on Let's Encrypt renewal). If the CA ever
  rotates, that is a coordinated firmware release.
- Keep a build flag (`-DOTA_TLS_INSECURE=1`) to fall back to `setInsecure()` for
  local development against a self-signed GamerBell, off by default.

### 3. Publish + pass through checksum (belt and suspenders)

- `firmware.bin.sha256` in the release lets GamerBell (and a human) sanity-check
  the artifact it caches independently of the device-side Ed25519 check.

## Cross-repo work required

| Repo | Change |
|---|---|
| `Esp32FitznetBell` | embed pubkey + root CA; verify signature before commit; `setCACert` / `beginSslWithCA`; sign step in `release.yml` |
| `GamerBell` | `FirmwareService` serves the signature (endpoint or header); passes through / stores `.sig` from the release |
| `Fitz-Net` | none (no OTA involvement) |

## Rollout ordering

1. Land TLS validation first (section 2) — device-only, no GamerBell change,
   immediately closes the MITM/DNS-spoof vector.
2. Add signing to `release.yml` + GamerBell passthrough (sections 1 & 3).
3. Enable on-device signature enforcement in a release **after** at least one
   signed release exists, so the first signed `.bin` can itself be delivered.

## Out of scope

- Rollback protection / anti-downgrade counter.
- Secure boot / flash encryption on the ESP32 (separate hardening effort).
