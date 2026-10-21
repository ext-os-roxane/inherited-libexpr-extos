/**
 * ExtOS ⇄ libexpr-extos C ABI bridge (PLAN 3.1.6).
 *
 * Loaded at runtime by `libextos::nix_eval` through `libloading::Library`, so
 * nothing in ExtOS links against this library at build time. Every symbol is
 * plain C with a stable layout; C++ exceptions never cross the boundary.
 *
 * Ownership rules:
 * - strings returned through `out_json` / `out_err` are heap-allocated with
 *   `std::malloc` and MUST be released with `extos_nix_eval_free()`;
 * - on a non-zero return code `*out_json` is left `NULL`, and `*out_err` holds
 *   a human-readable message (also `NULL` when allocation failed).
 *
 * Thread safety: evaluation calls are internally serialized by a
 * process-global mutex; concurrent calls are safe but sequentialized. (Nix's
 * `EvalState`/GC corrupt the heap when driven concurrently, so the bridge
 * enforces this rather than relying on callers.) Nix's runtime bootstrap runs
 * once behind a `std::once_flag`, and `extos_nix_eval_free()` may be called
 * from any thread, including while another evaluation is running.
 *
 * Store relocation is process-global: `EXTOS_REPOSITORY` is re-read per call
 * (see `store-api.cc`), so changing it mid-process works for the plain-path
 * flow but is not concurrent-safe with an evaluation in progress.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Semantic version of this bridge, e.g. `"2.35.0-extos.1"`.
 *
 * The pointer is static and must not be freed.
 */
const char * extos_nix_eval_version(void);

/**
 * Evaluate an ExtOS `system.nix` and render the result as a JSON DAG.
 *
 * @param config_path   absolute or relative path to the `.nix` system config
 * @param options_json  optional JSON object; may be `NULL` or `""`.
 *                      Recognised keys (both accepted for forward
 *                      compatibility only — they are currently inert and do
 *                      not change behaviour):
 *                        - `"strict"` (bool, default `true`): accepted, inert
 *                        - `"store_uri"` (string): accepted, inert; the store
 *                          is resolved from `EXTOS_REPOSITORY` or the upstream
 *                          environment. The response reports the effective
 *                          `store_dir` plus `store_dir_source` so callers can
 *                          see what was actually used.
 * @param out_json      receives the JSON document (caller frees)
 * @param out_err       receives an error message on failure (caller frees)
 *
 * @return
 *   0  success
 *   1  invalid argument (null path, malformed options JSON)
 *   2  the config file could not be read
 *   3  Nix evaluation failed (parse/eval error inside the config)
 *   4  internal failure (store bootstrap, JSON allocation)
 */
int extos_nix_eval_config(const char * config_path, const char * options_json, char ** out_json, char ** out_err);

/** Release a string returned by this bridge. `NULL` is accepted. */
void extos_nix_eval_free(char * s);

#ifdef __cplusplus
}
#endif
