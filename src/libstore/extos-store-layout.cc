#include "nix/store/extos-store-layout.hh"

#include "nix/util/environment-variables.hh"
#include "nix/util/logging.hh"

#include <filesystem>

namespace nix {

/* PLAN 3.1.2 — runtime relocation of the store prefix.
 *
 * The prefix is only ever read here; every consumer (StoreDirSetting in
 * store-api.cc, the derivation output hook in derivations.cc and the C ABI
 * bridge in src/extos-bridge) calls through this single function so the
 * override cannot drift between call sites. */
std::optional<std::string> extosRepositoryRoot()
{
    auto env = getEnvOsNonEmpty(OS_STR("EXTOS_REPOSITORY"));
    if (!env)
        return std::nullopt;

    auto raw = os_string_to_string(std::move(*env));
    if (raw.empty())
        return std::nullopt;

    std::filesystem::path path{raw};
    if (!path.is_absolute()) {
        /* Defensive: a relative repository would make the store directory
           depend on the process' cwd. Keep upstream behaviour instead. */
        warn("EXTOS_REPOSITORY is not an absolute path, ignoring it: %s", raw);
        return std::nullopt;
    }

    auto normalised = path.lexically_normal().string();
    /* `lexically_normal()` keeps a trailing slash for "…/" on some
       implementations; store dirs are built by appending "/store". */
    while (normalised.size() > 1 && normalised.back() == '/')
        normalised.pop_back();

    if (normalised == "/")
        return std::string{"/"};

    return normalised;
}

std::optional<std::string> extosStoreDir()
{
    auto root = extosRepositoryRoot();
    if (!root)
        return std::nullopt;
    if (*root == "/")
        return std::string{"/store"};
    return *root + "/store";
}

std::optional<std::string> extosStateDir()
{
    auto root = extosRepositoryRoot();
    if (!root)
        return std::nullopt;
    if (*root == "/")
        return std::string{"/state"};
    return *root + "/state";
}

std::optional<std::string> extosLogDir()
{
    auto root = extosRepositoryRoot();
    if (!root)
        return std::nullopt;
    if (*root == "/")
        return std::string{"/log"};
    return *root + "/log";
}

bool extosIsSafePathComponent(std::string_view s)
{
    if (s.empty() || s == "." || s == "..")
        return false;
    return s.find('/') == std::string_view::npos && s.find('\\') == std::string_view::npos
           && s.find('\0') == std::string_view::npos;
}

std::optional<ExtosOutputIdentity> extosOutputIdentityFromAttrs(const StringPairs & attrs)
{
    auto lookup = [&](std::string_view key) -> std::optional<std::string> {
        auto it = attrs.find(std::string{key});
        if (it == attrs.end() || it->second.empty())
            return std::nullopt;
        return it->second;
    };

    auto name = lookup("name");
    if (!name)
        return std::nullopt;

    /* Defaults mirror `libextos::manifest::parse_identity` so the fork and the
       Rust manifest parser agree on the resulting layout. */
    auto origin = lookup("origin").value_or("scratch");
    auto version = lookup("version").value_or("0.0.0");

    return ExtosOutputIdentity{
        .origin = std::move(origin),
        .name = std::move(*name),
        .version = std::move(version),
    };
}

/* PLAN 3.1.3 — ExtOS output layout.
 *
 * This is deliberately a single, self-contained function: the entire
 * `<origin>/<name>/<version>` transformation lives here so the diff against
 * upstream is reviewable at a glance, and so the Rust bridge and the C++
 * build path cannot disagree.
 *
 * Regression checks (behavioural coverage in
 * `core/libextos/tests/nix_eval_int.rs`):
 *  - unset EXTOS_REPOSITORY  => callers never reach this (nullopt upstream);
 *  - unsafe component        => std::nullopt, legacy `<hash>-<name>` kept;
 *  - output "out"            => exactly three components, no trailing slash;
 *  - any other output name   => three components + output name. */
std::optional<std::string> extosOutputLayoutPath(
    std::string_view storeDir, const ExtosOutputIdentity & identity, std::string_view outputName)
{
    if (storeDir.empty() || !extosIsSafePathComponent(identity.origin) || !extosIsSafePathComponent(identity.name)
        || !extosIsSafePathComponent(identity.version))
        return std::nullopt;

    std::string path{storeDir};
    path += '/';
    path += identity.origin;
    path += '/';
    path += identity.name;
    path += '/';
    path += identity.version;

    if (outputName != "out") {
        if (!extosIsSafePathComponent(outputName))
            return std::nullopt;
        path += '/';
        path += outputName;
    }

    return path;
}

bool extosWeaveSandboxOptOut()
{
    /* PLAN 3.1.4 — explicit, per-process, EQ "1" only. Anything else (unset,
       "0", "true", "") keeps the upstream sandbox default. */
    static const bool enabled = [] {
        auto env = getEnvOsNonEmpty(OS_STR("EXTOS_WEAVE_NO_SANDBOX"));
        if (!env)
            return false;
        auto value = os_string_to_string(std::move(*env));
        if (value != "1")
            return false;
        warn(
            "PLAN 3.1.4: EXTOS_WEAVE_NO_SANDBOX=1 — weave builds run without "
            "sandboxing (equivalent to --no-sandbox); upstream default is unchanged");
        return true;
    }();
    return enabled;
}

} // namespace nix
