# ZepraScript API Surface Catalog

> **Engine:** ZepraScript 1.2.0
> **Last Updated:** 2026-04-22

---

## 1. Web Platform APIs (`api/`)

All implementations are inline in headers unless noted. Free function singletons
have `.cpp` definitions.

| API | Header | `.cpp` | Status | Lines |
|---|---|---|---|---|
| URL / URLSearchParams | `URLAPI.h` | — | ✅ Complete (inline) | 393 |
| TextEncoder / TextDecoder | `EncodingAPI.h` | — | ✅ Complete (inline) | 226 |
| Crypto / SubtleCrypto | `CryptoAPI.h` | `CryptoAPI.cpp` | ✅ SHA + AES-GCM real | ~760 |
| Blob / File / FileReader | `BlobAPI.h` | — | ✅ Complete (inline) | 259 |
| FormData | `FormDataAPI.h` | — | ✅ Complete (inline) | 193 |
| Streams (Readable/Writable/Transform) | `StreamsAPI.h` | — | ✅ Complete (inline) | 421 |
| Fetch (Request/Response) | `FetchAPI.h` | — | ✅ Complete (inline) | ~400 |
| Performance / PerformanceMark | `PerformanceAPI.h` | `PerformanceAPI.cpp` | ✅ Complete (inline) | ~380 |
| Timer (setTimeout/setInterval) | `TimerAPI.h` | `TimerAPI.cpp` | ✅ Complete (inline) | ~250 |
| Storage (localStorage/sessionStorage) | `StorageAPI.h` | — | ✅ Complete (inline) | ~200 |
| StructuredClone | `StructuredCloneAPI.h` | — | ✅ Complete (inline) | ~400 |
| Worker (Web Workers) | `WorkerAPI.h` | — | ✅ Complete (inline) | ~250 |
| Intl (DateTimeFormat, NumberFormat) | `IntlAPI.h` | — | ✅ Complete (inline) | ~400 |

### Embedder API (`api/` — `.cpp` files)

| File | Purpose | Lines |
|---|---|---|
| `isolate.cpp` | VM isolate lifecycle | ~100 |
| `local_handle.cpp` | GC-safe handle management | ~130 |
| `script.cpp` | Script compilation entry point | ~90 |
| `value_api.cpp` | Value boxing/unboxing for C++ | ~120 |
| `script_engine.cpp` | Engine initialization | ~80 |

---

## 2. Intl APIs (`runtime/intl/`)

All 14 implementations are inline in headers. No `.cpp` required.

| API | Header | Lines | Notes |
|---|---|---|---|
| Intl.Collator | `IntlCollatorAPI.h` | 180 | Locale-aware string comparison |
| Intl.NumberFormat | `IntlNumberFormatAPI.h` | ~250 | Decimal, currency, percent, unit |
| Intl.DateTimeFormat | `FormattingAPI.h` | ~200 | Date/time pattern formatting |
| Intl.ListFormat | `IntlListFormatAPI.h` | ~150 | Conjunction/disjunction lists |
| Intl.PluralRules | `IntlPluralRulesAPI.h` | ~150 | Cardinal/ordinal plural selection |
| Intl.RelativeTimeFormat | `IntlRelativeTimeFormatAPI.h` | ~220 | "3 days ago" formatting |
| Intl.Segmenter | `IntlSegmenterAPI.h` | ~200 | Grapheme/word/sentence segmentation |
| Intl.DisplayNames | `DisplayNamesAPI.h` | ~170 | Language/region/currency names |
| Intl.Locale | `LocaleAPI.h` | ~180 | BCP 47 locale parsing |
| Temporal.Instant | `InstantAPI.h` | ~180 | Epoch nanosecond timestamps |
| Temporal.PlainDateTime | `TemporalAPI.h` | ~350 | Calendar-aware date/time |
| Temporal.ZonedDateTime | `ZonedDateTimeAPI.h` | ~210 | Timezone-aware date/time |
| Temporal.Duration | `DurationFormatAPI.h` | ~180 | ISO 8601 durations |
| Temporal.TimeZone | `TimezoneAPI.h` | ~180 | IANA timezone database |

### Limitation

All Intl implementations use simplified ASCII-based algorithms. Full ICU/CLDR
integration is deferred. Current implementations are structurally correct but
produce approximate results for non-Latin scripts.

---

## 3. TC39 Proposals (`runtime/proposals/`)

All 9 implementations are inline in headers. No `.cpp` required.

| Proposal | Header | Stage | Lines | Notes |
|---|---|---|---|---|
| Decorators | `DecoratorsAPI.h` | 3 | 193 | Class/method/field/accessor decorators |
| Explicit Resource Mgmt | `DisposableAPI.h` | 3 | ~160 | `using` / `Symbol.dispose` |
| Resource Management | `ResourceManagementAPI.h` | 3 | ~170 | `DisposableStack` / `AsyncDisposableStack` |
| ShadowRealm | `ShadowRealmAPI.h` | 3 | ~160 | Isolated code evaluation |
| Pattern Matching | `PatternMatchAPI.h` | 1 | ~180 | `match` expression |
| Pipeline Operator | `PipelineAPI.h` | 1 | ~150 | `value |> fn` |
| Object.groupBy | `GroupingAPI.h` | 4 | ~130 | Array/Map grouping |
| Record & Tuple | `RecordTupleAPI.h` | 2 | ~220 | Immutable value types |
| ECMAScript Conformance | `ECMAScriptConformance.h` | — | ~450 | Spec compliance test harness |

---

## 4. Optimization Passes (`zopt/passes/`)

All 8 passes are inline in headers. Invoked by `ZOptPipeline.cpp`.

| Pass | Header | Lines | Description |
|---|---|---|---|
| Constant Folding | `ZOptConstFold.h` | 276 | Compile-time arithmetic evaluation |
| Dead Code Elimination | `ZOptDeadCodeElim.h` | ~70 | Remove unused values |
| Copy Propagation | `ZOptCopyProp.h` | ~130 | Replace copies with originals |
| Common Subexpression | `ZOptCommonSubexpr.h` | ~110 | CSE across basic blocks |
| Loop Invariant Code Motion | `ZOptLoopInvariant.h` | ~190 | Hoist invariants out of loops |
| Strength Reduction | `ZOptStrengthReduce.h` | ~320 | Replace expensive ops (mul→shl) |
| Escape Analysis | `EscapeAnalysis.h` | ~330 | Scalar replacement of aggregates |
| Speculative Inliner | `SpeculativeInliner.h` | ~260 | Profile-guided function inlining |

---

## 5. Browser Bindings (`browser/`)

| API | `.cpp` | In Build | Lines |
|---|---|---|---|
| Window | `window.cpp` | ✅ | — |
| Document | `document.cpp` | ✅ | — |
| Fetch | `fetch.cpp` | ✅ | — |
| URL | `url.cpp` | ✅ | — |
| Worker | `worker.cpp` | ✅ | — |
| EventSystem | `event_system.cpp` | ✅ | — |
| StorageAPI | `storage_api.cpp` | ✅ | — |
| SecureStorage | `secure_storage.cpp` | ✅ | — |
| PasswordVault | `password_vault.cpp` | ✅ | — |
| StructuredClone | `structured_clone.cpp` | ✅ | — |
| IndexedDB | `indexeddb.cpp` | ✅ | — |
| ServiceWorker | `service_worker.cpp` | ✅ | — |
| WebSocket | `websocket.cpp` | ✅ | — |
| Performance | `performance.cpp` | ✅ | — |
| ConsoleAPI | `console_api.cpp` | ✅ | — |
| DevTools | `devtools.cpp` | ✅ | — |
| VideoBindings | `video_bindings.cpp` | ✅ | — |

---

## 6. Known Gaps

| Area | Gap | Severity | Notes |
|---|---|---|---|
| Crypto | ~~SHA hashes zeroed~~ → FIPS 180-4 real impl | ~~High~~ ✅ Fixed | SHA-1/256/384/512 |
| Crypto | ~~Encrypt/decrypt XOR placeholder~~ → AES-GCM | ~~Critical~~ ✅ Fixed | FIPS 197 + GF(2^128) GHASH |
| Intl | No ICU/CLDR integration | Medium | ASCII-only locale support |
| ZOpt Passes | No `.cpp` separation | Low | Header-only by design |
| ZIR Backend | ~~No regalloc wired~~ → LinearScan in pipeline | ~~Medium~~ ✅ Fixed | Poletto-Sarkar via ZOptPipeline |
| Tier-up | ~~Profiler doesn't trigger JIT~~ → Bridge wired | ~~High~~ ✅ Fixed | jit_tier_bridge.cpp |

---
