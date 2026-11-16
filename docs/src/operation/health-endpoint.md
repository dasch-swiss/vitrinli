# Health Endpoint

## `/health` Endpoint

Returns JSON with server status, version, and uptime:

```json
{"status":"ok","version":"3.18.0","uptime_seconds":3600}
```

Once the wedged decode threads reach the cap (see
[Wedged Decode Threads](#wedged-decode-threads)) it answers `503` with
`"status":"wedged"`.

- **Response time**: < 5ms (no I/O, no Lua, no auth)
- **Method**: GET only
- **Route**: Registered before the catch-all IIIF handler

The `version` field is kept (not scrubbed as an information-disclosure
concern): UptimeRobot and release-train checks read it, and the Docker image
tag already discloses the running version, so removing it from the body
closes no attack surface.

## Healthcheck Ownership

The image does **not** embed a Docker `HEALTHCHECK`. `HEALTHCHECK` is a
Docker-specific extension, not part of the OCI image spec, so the Bazel
`rules_oci` image carries none. Liveness is owned by the orchestration layer —
the docker-compose / Swarm `healthcheck:` stanza (provisioned in ops-deploy,
INFRA-1226):

```yaml
healthcheck:
  test: ["CMD", "/sbin/sipi", "health", "--port", "1024"]
  interval: 30s
  timeout: 5s
  start_period: 10s
  retries: 3
```

The check uses the self-contained
[`sipi health`](../guide/running.md#health-check) subcommand, which probes
`http://127.0.0.1:1024/health` and exits `0` (healthy) or `1` (unhealthy).
Because the probe is a sipi subcommand, the image needs no `curl` binary for the
healthcheck.

**Swarm behavior**: Docker Swarm has a single HEALTHCHECK — no separate liveness/readiness probes. When HEALTHCHECK fails after `retries`, Swarm kills and replaces the container. The `start_period` gives 10s for initialization (failures don't count, but container *receives traffic* during this period).

**Design consequence**: `/health` returns healthy once initialization completes. Only return unhealthy for truly fatal conditions (deadlocks, corrupted state, wedged decode threads at the cap), not for transient load.

## Wedged Decode Threads

A JP2 decode that blows the seam deadline (DEV-7080) leaves its thread running —
Kakadu offers no clean abort, so the FFI seam detaches it rather than blocking
the request indefinitely. A spinning codec keeps a core busy for as long as the
thread runs. The leaked thread is counted by the `wedged_threads` gauge
(`Sipi::observability::Metrics`), exported over OTLP as `sipi.wedged_threads`
alongside the other decode/memory gauges. It is never decremented — a wedged
thread is not reclaimable short of a restart.

Once `wedged_threads` reaches `--max-wedged-threads` / `SIPI_MAX_WEDGED_THREADS`
(default `nthreads - 1`, at least 1), every admission sheds: IIIF requests
(images, `/file`, `info.json`, `knora.json`, cache hits included), Lua routes
and the docroot fileserver answer `503` with `Retry-After: 1`, and the server
logs one warning when it enters that state. The admission pool checks the cap
before a request waits and again when it is admitted, so a queued request is
shed too; decodes already running can still wedge, so the count can pass the
cap by at most `nthreads - 1`. `/health` answers `503`, so Swarm replaces the
container after its `retries`. Alert on `sipi.wedged_threads` as well, to see
how often this happens.

## Traefik Configuration

The `/health` endpoint is exposed externally via Traefik for UptimeRobot monitoring:

```yaml
- traefik.http.routers.{{ STACK }}-iiif-health.rule=Host(`{{ DSP_IIIF_HOST }}`) && Path(`/health`)
- traefik.http.routers.{{ STACK }}-iiif-health.service={{ STACK }}-iiif
- traefik.http.routers.{{ STACK }}-iiif-health.entrypoints=websecure
- traefik.http.routers.{{ STACK }}-iiif-health.tls=true
- traefik.http.routers.{{ STACK }}-iiif-health.tls.certresolver=leresolver
```

## UptimeRobot Configuration

Monitor `https://iiif.example.com/health`. Expected status: 200,
expected body contains: `"status":"ok"`. The Traefik label is
provisioned in ops-deploy.
