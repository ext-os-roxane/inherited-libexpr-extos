/**
 * ExtOS store layout — runtime relocation of the Nix store (PLAN 3.1.2/3.1.3).
 *
 * ## Why
 *
 * Upstream Nix hardcodes `/nix/store` as the logical store directory at
 * configure time (`NIX_STORE_DIR`). ExtOS keeps every artifact in a single
 * read-only CAS rooted at `EXTOS_REPOSITORY` and weaves those artifacts into
 * `/runlevel/1/<system>`; the Nix store therefore has to be relocatable at
 * *runtime*, from the same environment variable the rest of ExtOS uses.
 *
 * ## Contract
 *
 * - When `EXTOS_REPOSITORY` is unset (or empty) every function in this header
 *   is a no-op returning `std::nullopt`; upstream behaviour is preserved
 *   bit-for-bit. This is a hard rule: the patch must never change defaults.
 * - When set, the logical store directory becomes `<EXTOS_REPOSITORY>/store`.
 *   The value must be an absolute, normalised path; relative paths are
 *   rejected (and treated as unset) so that a mis-configured environment can
 *   never silently scatter store paths into the process' cwd.
 * - Output layout (PLAN 3.1.3): for a derivation carrying explicit
 *   `origin` / `name` / `version` metadata, the ExtOS-visible output path is
 *
 *       <EXTOS_REPOSITORY>/store/<origin>/<name>/<version>[/<output>]
 *
 *   instead of `<store>/<hash>-<name>`. Only the `out` output omits the
 *   trailing output name, so the default output directory is exactly the
 *   three-component path named by PLAN 3.1.3.
 * - `EXTOS_REPOSITORY` is re-read on every call, so a value set after Nix's
 *   global settings were initialised is still honoured. One consequence is a
 *   single-config-per-process disclaimer: changing the variable mid-process is
 *   supported only for the plain-path flow (store/state/log directory
 *   resolution) and is *not* concurrent-safe with an evaluation in progress.
 *   Pick the repository once when the process starts.
 * - Missing or unsafe metadata (empty, containing a path separator, `.`/`..`)
 *   degrades gracefully to the legacy `<hash>-<name>` layout plus a warning
 *   trace. Nothing here ever throws: path generation runs on hot paths.
 *
 * ## Sandbox safety contract (PLAN 3.1.4)
 *
 * `EXTOS_WEAVE_NO_SANDBOX=1` is an *explicit*, per-process opt-in that makes
 * builds performed for a weave operation run without the sandbox, equivalent
 * to passing `--no-sandbox`. It is checked only from
 * `makeDerivationBuilder()`; the default sandbox path (`sandbox = true` on
 * Linux) is untouched, and the opt-in never weakens the sandbox implicitly
 * (setting any other value, including `"0"` or `""`, is ignored).
 *
 * Safety contract for callers enabling it:
 *
 * 1. only the `instanz` weave driver may set it, and only around the build of
 *    derivations whose inputs were already resolved inside the ExtOS CAS;
 * 2. the process must still drop privileges to a non-root build user — the
 *    opt-out removes namespace isolation, not the uid change;
 * 3. it must never be inherited by a long-lived daemon (`nix-daemon` clears
 *    the environment it passes on).
 *
 * The opt-out is recorded with a one-shot warning so an un-sandboxed weave is
 * always visible in logs.
 */

#pragma once
/// \file

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "nix/util/types.hh"

namespace nix {

/**
 * Explicit `origin` / `name` / `version` triple used to derive the ExtOS
 * output layout (PLAN 3.1.3). Fields are validated by
 * `extosOutputLayoutPath()`, not here, so callers can report precise errors.
 */
struct ExtosOutputIdentity
{
    std::string origin;
    std::string name;
    std::string version;
};

/**
 * `EXTOS_REPOSITORY`, normalised, or `std::nullopt` when unset/empty/relative.
 *
 * A relative or empty value is reported as unset with a warning; callers keep
 * the upstream hardcoded default in that case.
 */
std::optional<std::string> extosRepositoryRoot();

/**
 * Logical store directory under the ExtOS override: `<repo>/store`.
 *
 * Returns `std::nullopt` when `EXTOS_REPOSITORY` is unset, in which case the
 * caller must fall back to `NIX_STORE_DIR`.
 */
std::optional<std::string> extosStoreDir();

/**
 * State directory under the ExtOS override: `<repo>/state`.
 *
 * A relocated store needs a relocated metadata database; without this the
 * store root would be inside the repository while Nix still tried to create
 * `/nix/var/nix/profiles`, which fails for any non-root caller.
 *
 * Returns `std::nullopt` when `EXTOS_REPOSITORY` is unset.
 */
std::optional<std::string> extosStateDir();

/**
 * Log directory under the ExtOS override: `<repo>/log`.
 *
 * Returns `std::nullopt` when `EXTOS_REPOSITORY` is unset.
 */
std::optional<std::string> extosLogDir();

/**
 * Extract the ExtOS output identity from a derivation's environment attribute
 * map. Keys are the manifest keys already understood by
 * `libextos::manifest`: `origin`, `name`, `version`.
 *
 * Returns `std::nullopt` unless at least `name` is present and non-empty;
 * `origin`/`version` keep their historical manifest defaults (`scratch`,
 * `0.0.0`) so the transformation stays aligned with `manifest.rs`.
 */
std::optional<ExtosOutputIdentity> extosOutputIdentityFromAttrs(const StringPairs & attrs);

/**
 * Whether an ExtOS identity triples is safe to use as a path suffix: every
 * component non-empty, no `/`, no `\`, not `.` or `..`.
 */
bool extosIsSafePathComponent(std::string_view s);

/**
 * Compute the ExtOS-visible output path for one derivation output.
 *
 * @param storeDir   the logical store directory (already including `/store`)
 * @param identity   explicit origin/name/version metadata
 * @param outputName derivation output name (`out`, `dev`, …)
 *
 * @return `<storeDir>/<origin>/<name>/<version>` for `out`, and
 *         `<storeDir>/<origin>/<name>/<version>/<outputName>` otherwise;
 *         `std::nullopt` when any component is unsafe, in which case the
 *         caller keeps the legacy `<hash>-<name>` layout.
 */
std::optional<std::string>
extosOutputLayoutPath(std::string_view storeDir, const ExtosOutputIdentity & identity, std::string_view outputName);

/**
 * Whether the current process explicitly opted out of sandboxing for a weave
 * operation (`EXTOS_WEAVE_NO_SANDBOX=1`). See the safety contract above.
 */
bool extosWeaveSandboxOptOut();

} // namespace nix
