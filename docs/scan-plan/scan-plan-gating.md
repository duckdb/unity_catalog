# Scan-plan gating

How the Unity Catalog extension decides whether to use the IRC scan-plan (server-side scan
planning) read path vs. fall back to the Delta read path -- opt-in, capability-probed, and
resilient to a server that doesn't (yet) offer it.

## 1. Opt-in surface

- **`USE_IRC_SCAN_PLAN`** -- BOOLEAN ATTACH option, default `false`. When `false`, the Delta path
  is always used (today's behavior when `scan_plan_endpoint` was empty).
- **The URL is derived, not supplied.** The IRC base is predictable:

      IRC base = <credentials.endpoint>/api/2.1/unity-catalog/iceberg-rest
      plan URL = <IRC base>/v1/catalogs/{cat}/namespaces/{schema}/tables/{table}/plan

  This replaces the old `scan_plan_endpoint` ATTACH option (**removed**). **Base confirmed as
  `iceberg-rest`:** Databricks reports `.../iceberg/v1/` as *deprecated legacy* and directs clients
  to the new REST API at `.../iceberg-rest/v1/`. The scan-plan tests currently point at the legacy
  `.../iceberg` base, so deriving `iceberg-rest` also migrates off the deprecated endpoint -- confirm
  `/plan` works under `iceberg-rest` and re-point (or drop, for the derivation)
  `DATABRICKS_SCAN_PLAN_ENDPOINT`.
- **`API_IRC_ENDPOINT_OVERRIDE`** -- a hidden ATTACH option that overrides the derived base, kept
  only for testing against a mock / non-standard server. Not documented for users. (The offline
  `__internal_uc_plan_table_scan` table function already takes an explicit endpoint, so this is a
  convenience, not a requirement.)

## 2. Availability state (per-ATTACH)

A tri-state, stored atomically on the `UnityCatalog` instance, alongside a `steady_clock` stamp.
Only meaningful when `USE_IRC_SCAN_PLAN` is on.

    UNKNOWN      -- not yet probed (or a stale UNAVAILABLE that aged out)
    AVAILABLE    -- confirmed; sticky for the session
    UNAVAILABLE  -- confirmed absent/failed; re-probed after a wall-clock interval

**Lifetime:** `AVAILABLE` is sticky -- a working feature is not re-validated, and a later transient
failure is not treated as a capability change. `UNAVAILABLE` is **not** permanent: after a
hard-coded **15 minutes** it decays to `UNKNOWN`, allowing one re-probe (so a mid-session
enablement, or a transient blip that got treated as unavailable, recovers).

## 3. The probe = the `/plan` call itself

Databricks does **not** implement IRC `/config`: `GET .../iceberg/v1/config`, `.../iceberg/config`,
and `.../iceberg-rest/v1/config` all return 404 / `ENDPOINT_NOT_FOUND` -- yet `/plan` works
(live-verified). So `/config` is **not** a usable availability signal here: gating on its 404 would
disable a working feature. There is no cheap capability endpoint, so the **first real `/plan`
attempt IS the probe** (lazy, at first scan):

    success ......... AVAILABLE (sticky)
    any failure ..... UNAVAILABLE + WARNING, stamp, -> Delta; re-probe after 15 min

`/config` and its `endpoints` advertisement stay defined in the IRC spec but are **unused** -- no
`endpoints` parser is built. Revisit only if a target server is found that actually serves `/config`
(OSS UC is unconfirmed).

**Probe timing:** lazy, at first scan. The state is probe-source-agnostic, so an attach-time probe
can be added later without changing the gating logic.

## 4. Per-scan gating + fallback

At scan-bind time, when `USE_IRC_SCAN_PLAN` is on:

    UNAVAILABLE & within 15 min ....... Delta path, silent
    UNAVAILABLE & >= 15 min ........... decay to UNKNOWN, re-probe (below)
    UNKNOWN ........................... run scan-plan (the /plan call is the probe, §3)
    AVAILABLE ......................... run scan-plan

    scan-plan outcome:
      success ......................... AVAILABLE (sticky)
      any failure ..................... UNAVAILABLE + WARNING, stamp, -> Delta for this query

No hard errors: an unavailable-or-failed scan-plan always falls back to Delta. This is a behavior
change from today, where a scan-plan failure throws `IOException`.

## 5. Failure handling

**For now, every scan-plan failure is treated identically** -- 404 (route gone), 405 ("not
enabled", confirmed from a live endpoint), 5xx, and network/timeout all → log + mark `UNAVAILABLE` +
fall back to Delta. Simple, and the 15-minute re-probe recovers from transient blips.

**Required even so:** `UCAPI::PlanTableScan` must surface the HTTP status (a typed
`ScanPlanUnavailable` carrying the code, vs. a generic transient exception) so the WARNING can name
*why* it fell back, and so a later refinement can split the handling.

**Future refinement (not now):** distinguish transient (5xx / network) -- which should be a
per-query error with retry, not a capability flip -- from genuine unavailability (404 / 405). The
state machine already leaves room for this; only the failure classification changes.

## 6. What changes

- `UCCredentials`: drop `scan_plan_endpoint`; add nothing (derive on demand). Add
  `API_IRC_ENDPOINT_OVERRIDE` plumbing to the (hidden) override.
- ATTACH option parsing: replace the `scan_plan_endpoint` option with the `USE_IRC_SCAN_PLAN`
  boolean (+ the hidden override).
- `UnityCatalog`: add the atomic tri-state + stamp; replace `GetScanPlanEndpoint()` with a
  capability-aware accessor (e.g. `TryGetScanPlanEndpoint(context) -> optional<string>` that probes
  / consults state / honors the wall clock) and a `DeriveIRCEndpoint()` helper.
- `uc_table_entry.cpp`: the scan-bind branch (currently `if (!scan_ep.empty())`) becomes the state
  machine of §4; the `catch` (currently `throw IOException`) becomes mark-UNAVAILABLE + fall through
  to the Delta path.
- `UCAPI::PlanTableScan` / the config call: add `GetIRCConfig` and make failures status-typed.
- Logging: WARNINGs under the `api-irc` subsystem (e.g. `api-irc.Config`, `api-irc.PlanTableScan`).

## 7. Open / deferred

- **Base = `iceberg-rest`** (confirmed; `.../iceberg` is deprecated legacy per Databricks). Confirm
  `/plan` works under `iceberg-rest` and re-point the tests off the legacy base. `/config` is unused
  (Databricks doesn't serve it under either base).
- Attach-time (vs first-scan) probing -- structured for, not built.
- Transient-vs-unavailable split (§5) -- deferred; treat-as-404 for now.
- The re-probe interval is hard-coded at 15 min; promote to a `SET` only if a need appears.
