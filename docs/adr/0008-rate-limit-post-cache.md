---
status: superseded
---

# Rate-limit gate fires after the cache-hit short-circuit

> **Superseded (2026-08, sipi#777):** the per-client rate limiter this ADR concerns was removed (wrong axis against distributed crawler swarms). The post-cache gate placement it established still holds for the surviving engine-side policy — the decode memory budget.
>
> **Reading the body below (historical).** It is preserved as the decision record as-made and is *not* a description of the current tree. Two things it says no longer hold: (1) *Throttling* now comprises **Admission** (shell-side, pre-dispatch) and the **Decode memory budget** (engine-side, post-cache), not the three named below — the Rate limiter and the Output size guard are both gone; (2) the code paths it names (`route_handlers/serve_iiif.cpp`, `throttling/client_id.{h,cpp}`, a `throttling/` module) were the then-planned target layout and were never built — the one surviving engine-side gate fires inline in `src/ffi/cpp/serve_image.cpp`. See `CONTEXT.md` for the current *Throttling* terms.

The per-client rate limiter fires at the same post-cache gate point as the decode memory budget and the output-size guard, **not** before the cache lookup. Cache-hit responses therefore bypass rate-limit accounting entirely.

We accept this for three reasons.

**1. Threat-model honesty.** The rate limiter exists to mitigate harvest-bot impact. Harvest bots sweep unique IIIF URLs (one per image / per region / per size combination), which by construction is a cache-miss-dominant workload. Cache-hit rate-limiting was protecting against a workload that does not appear in production. The pre-cache placement was an over-fit to a hypothetical attack model.

**2. The Throttling umbrella becomes load-bearing in code.** Pre-Probe-7, "Backpressure" (the previous umbrella name) was a vocabulary fiction over two unrelated checkpoints separated by ~160 lines of cache logic in `SipiHttpServer.cpp`. After this decision, all three Throttling sub-policies (Decode memory budget, Rate limiter, Output size guard) fire at one call site in `route_handlers/serve_iiif.cpp` after the cache-hit short-circuit returns. The glossary umbrella maps 1:1 to a code seam, not just a docs seam.

**3. Cache hits become genuinely free.** A served cache hit no longer pays a rate-limiter mutex acquire + sliding-window cleanup + per-client deque mutation. Tiny win in absolute terms (microseconds), but it matches the user-facing intuition (cached = free) and it eliminates a noisy class of metric emissions on the hot path.

## Considered Options

- **Keep rate-limit pre-cache (status quo)** — rejected. The "rate-limit cache hits to defend against bots looping a single cached URL" attack model is not present in DSP's production threat picture; harvest bots are the actual concern. Pre-cache placement adds mental-model complexity (two gate sites separated by cache logic) without paying back against any real attack.

- **Pre-cache rate-limit + post-cache memory-budget** (today's split) — rejected. Two gate sites is exactly what Probe 7 set out to consolidate. Keeping them split would have left "Throttling" as a vocabulary umbrella over a code structure that doesn't actually share a seam.

- **Move all Throttling gates pre-cache** — rejected. Memory-budget pre-cache is incorrect: cache-hit responses do not decode and consume zero decode memory, so charging them against the budget would phantom-charge the gauge and could trigger spurious 503s under cache-hit-heavy load.

- **Add an explicit "cache-hit count" budget** to defend against cache-hit hammering — rejected. No production evidence such an attack exists at DSP. Premature defense; revisit if metrics later show pathological cache-hit traffic.

## Consequences

- **Cache-hit responses do not count against per-client pixel budget**. A bot that pounds a single cache-hittable URL is no longer rate-limited. Trade-off accepted per the threat-model rationale above.

- **Per-request work on rejected requests is slightly higher**. Image-shape lookup + canonical-URL build + cache lookup happens before rate-limit rejection. Microseconds; negligible on the rejection path.

- **All Throttling sub-policies share one gate site**. `route_handlers/serve_iiif.cpp` has one post-cache gate block with three sequential checks: output-size guard → rate limiter → memory budget. The order is "cheapest stateless check first; resource-acquiring check last" so a rejected request doesn't transiently allocate memory budget that another gate then refuses.

- **`resolve_client_id` moves into `throttling/`**. The XFF-rightmost / peer-IP helper at `SipiHttpServer.cpp:324` was sourced only by the (pre-cache) rate-limit call. Post-decision, it lives alongside the rate limiter in `throttling/client_id.{h,cpp}` as `Sipi::throttling::client_id_from(...)`.

- **Bot-mitigation contracts in tests update**. Any e2e/Hurl test that asserted rate-limit behaviour on a cache-hit response gets adjusted (or deleted, if the assertion was incidental). Tests asserting rate-limit behaviour on cache-miss responses are unaffected.

- **Metric outcomes unchanged**. The same decision outcomes are counted post-cache; only the distribution shifts (cache-miss requests dominate the counter; cache-hit requests are no longer counted). Production queries the flattened OTLP counters `sipi_rate_limit_{allowed,rejected,shadow_rejected}_total`; the engine registry's label-fanned `sipi_rate_limit_decisions_total{action=...}` is oracle-only. `rate_limit_clients_tracked` likewise shifts to count clients that have made at least one cache-miss request in the window.

- **Documented in glossary**. The `Rate limiter` glossary entry sharpens to note the post-cache placement; the `Cache` entry adds "cache-hit short-circuits all Throttling gates"; the `Throttling` umbrella entry references this ADR for the gate-site decision.
