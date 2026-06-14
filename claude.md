<div align="center">
  <h1>🚀 ZepraBrowser Global TODO & Goals</h1>
  <p><strong>Predictable &gt; Flashy | Stable &gt; Feature-Rich | Memory-Efficient &gt; Benchmark-Winning</strong></p>
</div>

---

## 🎯 The Ultimate Goal
**"Can it render this website 1000 times without degrading?"**
Transform ZepraBrowser into a robust, memory-efficient alternative to Electron for the Neolyx OS ecosystem. Target footprint: **50–150 MB** per app context. 
This is a real user experience win for constrained hardware and the rural India/Bihar context where lower-spec machines are common.

---

## 📊 The Metric That Actually Matters for Beta (NeolyxOS Targets)
If we hit these numbers, Zepra becomes the most memory-efficient full-featured browser in existence.

| Condition | Target RSS |
| :--- | :--- |
| **Idle, 1 tab** | `< 80MB` |
| **10 tabs, moderate sites** | `< 300MB` |
| **50 tabs** | `< 800MB` |
| **After closing all tabs (Z ≈ X)** | `< 100MB` |
| **24hr runtime, idle** | `< 5% memory growth` |

*Note: If memory after closing all tabs is the same as when 50 tabs were open (Z ≈ Y), we have leaks. Our Shenandoah-style GC should handle this well, but we must fix the JIT cache leak which currently acts as a floor.*

---

## 🛠️ High-Priority Bug Fixes (The "Unfreeze" Sprint)

### 1. ZepraScript Engine Stabilization
- [ ] **Parser Fix:** Implement comma operator `(0, _.E)` support in `parseExpression()` to stop the parser from hanging on minified JS.
- [ ] **JIT Memory Leak (B06):** Fix `CodeCache::compact()` in JIT so it actually decrements `usedSize_` and reclaims executable memory buffer space.
- [ ] **W^X Vulnerability:** Re-architect the `mprotect` calls in the JIT so compiling a new block doesn't alter permissions of existing active blocks on the same page.
- [ ] **VM Isolation (B08):** Eliminate global statics (`s_vm`, `s_vmContext`) in `script_context.cpp` to ensure true per-tab VM isolation and fix ObjectSecurity pointer reuse.

### 2. Rendering & UI Thread Unblocking
- [ ] **Asynchronous JS Execution:** Move `parseWithWebCore` and `executePageScripts()` off the main UI rendering loop, or implement yielding, so JS execution doesn't freeze the browser.

### 3. Network Reliability
- [ ] **Keep-Alive Delay:** Rewrite the `http_client` 1000ms `poll()` hack to parse HTTP headers (`Content-Length`) and break immediately when the payload finishes.
- [ ] **TLS Integration:** Implement the `nx_tls_connect` stub with OpenSSL/mbedTLS to enable HTTPS.
- [ ] **Chunked Decoding:** Fix the empty body and chunked decoding bugs in the new `nxhttp` C engine.

### 4. Storage Persistence (B13-B15)
- [ ] **Data Saving:** Replace the `TODO` stubs in `history_database.cpp`, `settings_store.cpp`, and `bookmark_manager.cpp` with actual JSON or SQLite writing so the browser saves user data.

---

## 🧪 The "Ironclad" Testing Suite & Priority Sequence

To ship a Stable Beta, run tests in this exact priority:

### 1. The "Immediate Bug Exposure" Tests (Run First)
- **Memory Stability:** `valgrind --leak-check=full --track-origins=yes ./zepra_browser`
  *Goal: Catch the JIT cache leak (B06) where 50+ tabs fill 16MB and silently die.*
- **Long Runtime:**
  *Goal: Catch ObjectSecurity pointer reuse (B08) that only manifests after enough alloc/free cycles.*

### 2. The "Low Resource" Test (Run Second - After engine fixes)
- **Action:** `systemd-run --scope -p MemoryMax=1G ./zepra_browser`
- **Goal:** Immediately expose whether tab discard (TabSuspender) actually works under pressure instead of just printing to logs.

### 3. The "Heavy Site" Test (Run Third - After TLS fix)
- **GitHub:** Tests HTTPS + complex CSS + moderate JS.
- **Reddit:** Tests infinite scroll + lazy loading.
- **Gmail:** Tests heavy JS + WebSockets.
- **Discord/Figma/Docs:** Save for last (WebGL/Canvas/heavy APIs).

### 4. The "Crash Recovery" Test (Run Before Beta)
- **Action:** Open 20 tabs, `kill -9 zepra_browser`, then restart.
- **Goal:** Verify what state survives. (Currently fails due to stubbed storage).

---
> *Generated based on the new Neolyx OS engineering philosophy.*
