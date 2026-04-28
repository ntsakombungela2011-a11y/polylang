# PolyLang Threat Model

## Scope

PolyLang is a Godot GDExtension that loads and executes scripts from multiple foreign runtimes through a frozen C ABI. The highest-risk surfaces are adapter loading, script compilation and execution, runtime service injection, and any data that crosses the Godot or foreign-runtime boundary.

## Threat Actors

- Malicious script author
- Malicious Godot project consuming the extension
- Compromised or tampered vendored dependency
- Compromised CI runner or build cache
- Malicious hot-reload file replacement

## Trust Boundaries

- Untrusted: all script source text, script paths, sidecar config values, and hot-reload inputs
- Untrusted: all values crossing the adapter ABI in either direction
- Partially trusted: Godot engine APIs and object lifetimes
- Trusted: PolyLang core C++ code after validation and invariant checks

## Security Objectives

- Prevent memory corruption, use-after-free, and double-free across the ABI
- Prevent sandbox escapes through language runtime standard libraries and module loading
- Prevent path traversal and unsafe adapter loading
- Prevent deadlocks and races in shared runtime subsystems
- Preserve deterministic builds and auditable CI inputs

## Primary Mitigations

- Frozen ABI validation before adapter use, including required function-pointer checks
- Per-instance runtime ownership for Lua and Wren to avoid shared mutable interpreter state
- Explicit GIL-managed CPython access with isolated module globals per instance
- Sandboxed runtime startup that strips blocked globals and blocks import or foreign binding paths
- Recursive PLValue cleanup with bounded conversion depth and collection size caps
- Runtime service injection only after singleton initialization
- CI builds pinned to vendored dependencies and explicit platform configuration

## Residual Risks

- Python sandboxing remains defense-in-depth rather than a perfect security boundary because CPython was not designed as a secure sandbox
- Wren compile validation still evaluates top-level code because the embed API does not expose a separate compile-only entry point
- Any adapter outside the rewritten Lua, Python, and Wren set still needs the same audit standard before being treated as production-complete
