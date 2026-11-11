---
status: accepted
---

# `stream` is a recorded intent, not an enforcement boundary

SIPI's preflight vocabulary gains an eighth *Permission* type, `stream`. It means
"this representation is meant to be consumed in place, not handed over as a file
to keep" — the access shape that fits streamed audio, video and other material a
holder may play but may not give away.

## Context

Callers already express that intent, badly. A DSP-style deployment models
stream-only access by giving the asset a restricted view and letting SIPI serve
the whole file from `/file` anyway. The hook has no word for what it means, so
it says `restrict` (whose only mechanisms — a size cap and a watermark — apply to
still images and to nothing else) or `allow` (which says the opposite of what is
meant). Neither reaches SIPI as the thing the caller intends, and neither leaves
a trace: no counter, no log, nothing that says how much of the corpus is being
served under a stream-only intent.

Tightening streaming delivery — refusing a full-file `GET` without a `Range`
header, segmenting or transcoding on the fly, binding ranges to a short-lived
token, rate-limiting — is SIPI-local work. But none of it can begin while the
intent is indistinguishable from `allow` at the point the decision arrives.

## Decision

`stream` joins the *Permission* vocabulary as `SipiPermType::Stream` /
`SIPI_STREAM = 7`, appended to the hand-mirrored seam enum (never renumbered)
and pinned by the paired drift assertions on both sides.

**It changes no served byte.** On `/file` and on the IIIF image route a `stream`
decision is served exactly as `allow`: same bytes, same headers, no
`Content-Disposition` override (SIPI emits `inline` for every response already
and never sends `attachment`), native dimensions and the full tile grid in
`info.json` and `knora.json`. It carries no size cap and no watermark, so
`restricted_dims` leaves it alone.

The one addition is a counter, `sipi.preflight.decisions{permission}`,
incremented once per resolved decision under that decision's permission. The
attribute is what makes it useful: the population has to be measurable before
any enforcement is chosen for it, and `stream`'s share needs a denominator —
`decisions{permission="stream"} / sum(decisions)` — not a bare count that only
says `stream` is non-zero.

An e2e pins the parity — a `stream` decision's `/file` body is byte-identical to
`allow`'s, its `info.json` identical but for the request's own `id`, and its
rendered pixels identical. A later tightening therefore has to break those
assertions on purpose.

## What `stream` guarantees

**Nothing, against anyone who wants the bytes.** This is not the usual caveat
that no delivery mechanism survives a determined capture. It is narrower and
sharper: for every media kind that matters here, the derivative SIPI serves *is*
the original. Only still images are transcoded on ingest; moving images, audio,
vector images and other files are copied. So a decision that withholds the
original while streaming the derivative withholds no bytes at all — it closes
one route and records an intent while the same content remains available on
another.

Saying so plainly is the point of this record. `stream` is a place to put the
intent and a seam to enforce it from later. It is not an access control, and
nothing downstream should be built as if it were.

## What is reserved to SIPI, and what is not

SIPI-local, reachable without touching any contract: range-only delivery, a
forced inline disposition, token-bound ranges, rate limiting, per-client
budgets.

Not SIPI-local: pre-segmented HLS/DASH with key rotation, or anything else that
requires the *served artifact itself* to differ from the original. That needs a
different derivative, which only the ingest side can produce. The seam bought
here extends up to that line and no further — worth knowing before the question
is urgent rather than after.

## Considered options

**Model the intent as `restrict`.** Rejected: `restrict`'s mechanisms are a size
cap and a watermark, both meaningless for audio and video, and a `restrict`
decision that can apply neither is refused outright rather than served. Using it
would mean either weakening that refusal or denying the material.

**Model the intent as `allow` and revisit later.** Rejected: it preserves
today's behaviour identically, but every future tightening would then have to
begin by inventing the vocabulary and coordinating a release across the caller,
the hook and SIPI — the work this record does once, done later under pressure.

**Enforce something on day one** (range-only delivery, say). Rejected: the
population is unmeasured. Choosing an enforcement before knowing what it applies
to risks breaking working access for a corpus nobody has sized.

## Consequences

- The permission vocabulary is eight strings, not seven. `CONTEXT.md`,
  `CONTEXT.md` and the Lua reference all say so, and all three state that
  `stream` enforces nothing.
- A hook that returns `stream` against an older SIPI gets a **500**, not an auth
  response: `valid_permission` refuses the unknown string inside the Lua runtime,
  the preflight call fails, and both serve routes map that to
  `INTERNAL_SERVER_ERROR`. The shell's `permission_from_str` — and its `deny`
  fallback — is never reached. A version skew therefore presents as a server
  fault and pages, rather than showing up as a 401 nobody investigates. Emitting
  `stream` requires SIPI to be deployed first.
- `sipi.preflight.decisions{permission}` is the signal that sizes the population.
  Until it has data, no enforcement proposal is grounded.
