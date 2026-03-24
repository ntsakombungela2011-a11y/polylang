# PolyLang Changelog

## v6.6 — Zero-Trust Audit Round 2 (March 2026)
**12 new vulnerabilities found and fixed.**

### Critical
- **VLN-01** `pl_signal_bus`: Unbounded emission_queue_ → OOM DoS. Fixed: 4096 hard cap + dropped counter.
- **VLN-03** `pl_bridge`: Recursive cross-language calls → stack overflow. Fixed: thread-local depth guard (max 64).
- **VLN-06** `mod_loader`: ftell() -1 not checked before size_t cast. Fixed: explicit sz<0 error path.
- **VLN-09** `variant_bridge`: array.len/dict.len from untrusted adapters trusted → OOB read. Fixed: safe_len() clamp to 65536.

### High
- **VLN-02** `pl_signal_bus`: Duplicate Callable registration amplifies signals N×. Fixed: dedup scan before insert.
- **VLN-04** `pl_coroutine_scheduler`: pending_mutex_ nested inside active_mutex_ → ABBA deadlock. Fixed: separate scopes, no simultaneous hold.
- **VLN-05** `pl_coroutine_scheduler`: Signal listener registered before coroutine enters active_ → first signal dropped. Fixed: insert into active_ first, then register listener.
- **VLN-07** `pl_polyglot_parser`: Unbounded block count exhausts memory. Fixed: 256 block cap.
- **VLN-08** `pl_resource_bridge`: Worker threads block indefinitely on condition_variable. Fixed: 2s timed wait + shutdown() drain.

### Medium
- **VLN-10** `pl_profiler`: stats_ map grows unbounded from unlimited unique labels. Fixed: 1024 label cap + dropped_labels_ counter.
- **VLN-11** `pl_export_parser`: No cap on @export var count. Fixed: 512 var limit.
- **VLN-12** `pl_signal_bus`: dropped_emissions_ not observable. Fixed: get_dropped_emissions() GDScript API.

## v6.5 — Zero-Trust Audit Round 1 (March 2026)
18 vulnerabilities found and fixed (C-1 through H-6, M-1, M-2).

## v6.1–v6.4
15 new systems added: SignalBus, Typed Export Vars, Cross-Lang Calls, Cross-Lang Inheritance, REPL Dock, Sandbox Tiers, Hot Reload State, Coroutine Scheduler, Mod Loader, Profiler Hooks, Resource Bridge, Async Runtime, Engine API Bridge, Polyglot Parser, Variant Bridge.

## v1–v5
Initial framework through full production rewrite. See PolyLang_Master_Reference.docx.
