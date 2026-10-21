# libexpr-extos — ExtOS patches to the Nix evaluator (PLAN sub-phase 3.1)

This directory is a pristine Nix 2.35.0 source tree plus a small, clearly
marked set of ExtOS changes. Upstream behaviour is preserved when no ExtOS
environment variable is set.

## ExtOS changes at a glance

| File | PLAN task | Change |
| --- | --- | --- |
| `src/libstore/include/nix/store/extos-store-layout.hh` | 3.1.2–3.1.4 | New public header: runtime store relocation, `<origin>/<name>/<version>` output layout, weave sandbox opt-out contract |
| `src/libstore/extos-store-layout.cc` | 3.1.2–3.1.4 | New implementation (single source of truth) |
| `src/libstore/store-api.cc` | 3.1.2 | `EXTOS_REPOSITORY` takes precedence over `NIX_STORE_DIR`/`NIX_STORE` for the logical store dir; state *and* log dirs are re-read per call (no first-call freeze) |
| `src/libstore/store-registration.cc` | 3.1.2 | Suppresses the implicit `~/.local/share/nix/root` chroot store when `EXTOS_REPOSITORY` is set |
| `src/libstore/derivations.cc` | 3.1.3 | Emits the ExtOS output layout for derivations carrying `origin`/`name`/`version` metadata |
| `src/libstore/unix/build/derivation-builder.cc` | 3.1.4 | `EXTOS_WEAVE_NO_SANDBOX=1` opt-out, equivalent to `--no-sandbox` |
| `src/libexpr-c/extos_bridge.{h,cc}` | 3.1.6 | C ABI bridge: `extos_nix_eval_version/config/free` |

`src/libstore/unix/build/derivation-builder.cc` already honours the standard
`--no-sandbox` / `--option sandbox false` flags through
`BaseSetting<SandboxMode>::convertToArg`; the ExtOS opt-out adds an
environment-level equivalent for the weave driver so it does not have to
rewrite the argument vector of every build.

## Building

The fork is a Meson project whose top-level file is a *dev shell* that pulls in
every subproject. ExtOS only needs `libexpr-c` (which links `libexpr`,
`libstore`, `libfetchers`, `libutil`), so the leanest configuration disables the
parts that need optional tools (Perl bindings, gtest suites, `jv`, `lowdown`)
and picks GNU readline instead of editline:

```sh
meson setup build . \
  -Dbindings=false \
  -Dunit-tests=false \
  -Djson-schema-checks=false \
  -Ddoc-gen=false \
  -Dlibcmd:readline-flavor=readline
ninja -C build src/libexpr-c/libnixexprc.so.2.35.0
```

The bridge is compiled into `libnixexprc.so`, so there is no extra target:

```sh
ls build/src/libexpr-c/libnixexprc.so          # -> libnixexprc.so.2.35.0
```

System dependencies on CachyOS/Arch (all present on the reference host):

```sh
pacman -S --needed base-devel meson ninja boost boost-libs libarchive \
  libsodium curl sqlite nlohmann-json gc toml11 readline pkgconf
```

`editline` and `lowdown` are *not* required with the flags above (editline is
only used when `-Dlibcmd:readline-flavor=editline`, lowdown only for the manual
renderer, which `-Ddoc-gen=false` disables).

## Using the bridge from ExtOS

```sh
export EXTOS_REPOSITORY=/repository
export EXTOS_NIX_EVAL_LIB=/path/to/build/src/libexpr-c/libnixexprc.so
cd core/libextos && cargo test --test nix_eval_int
```

- `EXTOS_REPOSITORY` relocates the store directory to
  `<EXTOS_REPOSITORY>/store` (PLAN 3.1.2).
- The bridge reports, per package, the ExtOS artifact layout
  `<EXTOS_REPOSITORY>/store/<origin>/<name>/<version>` (PLAN 3.1.3).
- `EXTOS_WEAVE_NO_SANDBOX=1` is the explicit weave opt-out (PLAN 3.1.4); its
  safety contract is documented at the top of
  `src/libstore/include/nix/store/extos-store-layout.hh`.

### Bridge options: `store_uri` and `strict` are inert

`extos_nix_eval_config()`'s `options_json` object accepts `"store_uri"`
(string) and `"strict"` (bool) for forward compatibility, but **both are
currently inert**: the store is resolved from `EXTOS_REPOSITORY` (or the
upstream Nix environment) and `printValueAsJSON` always forces values. The
response reports the effective `store_dir` plus `store_dir_source` so a caller
can see what resolution actually happened. Honest documentation beats a
half-wired knob: neither option changes behaviour today, and passing them is
not an error.

### Thread safety

Evaluation calls are serialized internally by a process-global `std::mutex`
around the `EvalState` critical section: concurrent `extos_nix_eval_config()`
calls are safe but run sequentially. This is not a performance statement — Nix's
evaluator and GC corrupt the allocator (tcache) under four concurrent
evaluations in one process, so the bridge serializes instead of trusting
callers. `extos_nix_eval_free()` is lock-free and safe from any thread.

`EXTOS_REPOSITORY` is re-read per call (see `StoreConfig::getStateDir()` /
`getLogDir()` in `src/libstore/store-api.cc`), which is why a value set after
Nix's global settings were initialised still works. The corollary is a
single-config-per-process rule: changing it mid-process is supported only for
the plain-path flow and is not concurrent-safe with an evaluation in progress.

## Audit — hardcoded `/nix/store` references (PLAN 3.1.1)

`grep -Rni '/nix/store' src` over C/C++/Meson sources yields **116** matches
(plus ~1.1k in test fixtures, JSON schema samples, `.drv` files and shell
tests, which are data and deliberately untouched). Bucketed by role:

| Role | Files (code matches) |
| --- | --- |
| Prefix resolution / defaults | `libstore/store-api.cc` (1: doc), `libstore/include/nix/store/store-api.hh` (5: docs for `StoreDirSetting`, `realStoreDir`), `libstore/meson.build` (1: `NIX_STORE_DIR` define), `libstore/include/nix/store/local-settings.hh` (16: docs + `storeDir` example), `libutil/experimental-features.cc` (2: docs) |
| Store path construction | `libstore/store-dir-config.cc` (2: comments on the path fingerprint), `libstore/include/nix/store/store-dir-config.hh` (2: docs), `libstore/include/nix/store/path-with-outputs.hh`, `path-references.hh`, `store-reference.hh` (1 each: doc examples), `libexpr/include/nix/expr/print-options.hh` (1) |
| Sandbox / chroot paths | `libstore/unix/build/chroot.hh` (1), `libstore/unix/build/linux-derivation-builder.cc` (1), `libstore/unix/build/darwin-derivation-builder.cc` (1), `libstore/include/nix/store/restricted-store.hh` (2), `libstore/unix/meson.build` (1), `libstore/linux/meson.build` (1), `libstore/local-store.cc` (1), `libstore/gc.cc` (1) |
| Hardcoded constants in primops / settings docs | `libexpr/primops.cc` (8), `libexpr/primops/fetchClosure.cc` (8), `libexpr/primops/fetchTree.cc` (1), `libexpr/primops/context.cc` (1), `libexpr/eval.cc` (1), `libexpr/include/nix/expr/eval-settings.hh` (2), `libstore/include/nix/store/worker-settings.hh` (6), `libutil/include/nix/util/source-accessor.hh` (3), `libstore/include/nix/store/local-store.hh`, `local-overlay-store.hh`, `common-ssh-store-config.hh` (1 each), `nix/run.cc` (3), `nix/nix-env/user-env.cc` (1) |
| Public C API docs | `libstore-c/nix_api_store.h` (2), `libstore-c/nix_api_store/store_path.h` (2) |
| Tests / test support (out of scope) | `libstore-tests/*` (≈30), `libstore-test-support/*` (3), `libexpr-tests/*` (3), `libutil-tests/config.cc` (1) |
| ExtOS addition | `libstore/include/nix/store/extos-store-layout.hh` (1: contract doc) |

Only two of those are *behavioural*: the `NIX_STORE_DIR` definition in
`libstore/meson.build` (compile-time default) and its use in
`store-api.cc`'s `StoreDirSetting` (runtime default), both routed through the
ExtOS override. Everything else is documentation, test data, or a path used by
the sandbox after the store directory is already resolved, so it follows the
relocated prefix automatically.

## Notes / non-goals

- Store paths stay `<hash>-<name>` internally: the Nix path grammar and every
  protocol carrying a `StorePath` require exactly one component below the store
  directory. The `<origin>/<name>/<version>` layout is derived alongside and is
  what `instanz` weaves (see the comment in `writeDerivation`).
- No upstream file was re-formatted; every ExtOS block is marked with a
  `PLAN 3.1.x (ExtOS)` comment.
