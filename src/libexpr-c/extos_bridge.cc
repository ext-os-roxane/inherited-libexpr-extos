/* ExtOS ⇄ libexpr-extos C ABI bridge (PLAN 3.1.6).
 *
 * Compiled into the existing `nixexprc` shared library, so ExtOS gets one
 * `libnixexprc.so` to `dlopen()` — no extra build target, no extra runtime
 * dependency beyond the Nix libraries it already needs.
 *
 * The bridge evaluates an ExtOS `system.nix` (the grammar implemented by
 * `instanz::system`, mirrored here so both agree byte-for-byte) and renders a
 * JSON document containing:
 *
 *   - the effective store directory and where it came from (PLAN 3.1.2);
 *   - the ExtOS output layout `<store>/<origin>/<name>/<version>` for every
 *     package (PLAN 3.1.3);
 *   - a small DAG (nodes + `weave` edges) consumed by
 *     `libextos::nix_eval::EvalDag`.
 *
 * No exceptions escape: every entry point is wrapped in `try`/`catch` and
 * reports a numeric code plus an owned error string.
 */

#include "extos_bridge.h"

#include "nix/expr/eval.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/search-path.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-api.hh"
#include "nix/store/store-open.hh"
#include "nix/store/extos-store-layout.hh"
#include "nix/util/types.hh"
#include "nix/util/util.hh"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

using json = nlohmann::json;

namespace {

/** Mirrors `.version` plus the bridge revision; bump when the ABI changes. */
constexpr const char * EXTOS_BRIDGE_VERSION = "2.35.0-extos.1";

/* Package defaults — must match `instanz::system::{DEFAULT_ORIGIN,
   DEFAULT_VERSION}` so a config parsed by either side yields the same DAG. */
constexpr std::string_view DEFAULT_ORIGIN = "archlinux";
constexpr std::string_view DEFAULT_VERSION = "current";

constexpr int EXTOS_EVAL_OK = 0;
constexpr int EXTOS_EVAL_EINVAL = 1;
constexpr int EXTOS_EVAL_ENOENT = 2;
constexpr int EXTOS_EVAL_EVAL = 3;
constexpr int EXTOS_EVAL_EINTERNAL = 4;

/** Copy `s` into a malloc'd buffer the caller releases with free(). */
char * dupOut(const std::string & s)
{
    auto * buf = static_cast<char *>(std::malloc(s.size() + 1));
    if (!buf)
        return nullptr;
    std::memcpy(buf, s.c_str(), s.size() + 1);
    return buf;
}

void setErr(char ** out_err, const std::string & message)
{
    if (out_err)
        *out_err = dupOut(message);
}

std::string trim(std::string_view s)
{
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos)
        return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return std::string{s.substr(begin, end - begin + 1)};
}

bool validComponent(std::string_view s)
{
    return !s.empty() && s != "." && s != ".." && s.find('/') == std::string_view::npos
           && s.find('\0') == std::string_view::npos;
}

/**
 * One resolved package reference, mirroring `instanz::weaver::CasRef`.
 */
struct PackageRef
{
    std::string origin, name, version;
};

std::optional<PackageRef> parsePackageSpec(const std::string & raw, std::string & error)
{
    auto spec = trim(raw);
    if (spec.empty()) {
        error = "empty package spec";
        return std::nullopt;
    }

    std::string locator = spec, version{DEFAULT_VERSION};
    if (auto eq = spec.find('='); eq != std::string::npos) {
        auto v = trim(std::string_view{spec}.substr(eq + 1));
        if (v.empty()) {
            error = "`" + spec + "` has an empty version after `=`";
            return std::nullopt;
        }
        version = v;
        locator = trim(std::string_view{spec}.substr(0, eq));
    }

    std::string origin{DEFAULT_ORIGIN}, name = locator;
    if (auto slash = locator.find('/'); slash != std::string::npos) {
        origin = locator.substr(0, slash);
        name = locator.substr(slash + 1);
        if (origin.empty() || name.empty() || name.find('/') != std::string::npos) {
            error = "expected `[origin/]name[=version]`, got `" + spec + "`";
            return std::nullopt;
        }
    }

    for (const auto & [what, value] : {std::pair{"origin", &origin}, {"name", &name}, {"version", &version}})
        if (!validComponent(*value)) {
            error = std::string{what} + " `" + *value + "` is not a single path component";
            return std::nullopt;
        }

    return PackageRef{origin, name, version};
}

std::string normalizePm(std::string_view pm)
{
    auto lower = trim(pm);
    for (auto & c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "arch" || lower == "pacman")
        return "archlinux";
    if (lower == "dpkg" || lower == "apt")
        return "debian";
    if (lower == "apk")
        return "alpine";
    if (lower == "xbps")
        return "void";
    if (lower == "dnf" || lower == "yum" || lower == "zypper" || lower == "fedora" || lower == "rhel"
        || lower == "centos" || lower == "suse")
        return "rpm";
    return lower;
}

const json * getKey(const json & obj, const char * key)
{
    if (!obj.is_object())
        return nullptr;
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

std::optional<std::string> optString(const json & obj, const char * key)
{
    auto * v = getKey(obj, key);
    if (!v || !v->is_string())
        return std::nullopt;
    return v->get<std::string>();
}

std::optional<std::string> roofPm(const json & cfg)
{
    for (auto key : {"native_pm", "roof_native_pm", "roof", "primary_origin"})
        if (auto pm = optString(cfg, key); pm && !pm->empty())
            return normalizePm(*pm);

    /* Plural alias: a string, or a list whose first string wins. */
    if (auto * roofs = getKey(cfg, "roofs")) {
        if (roofs->is_string())
            return normalizePm(roofs->get<std::string>());
        if (roofs->is_array())
            for (const auto & item : *roofs)
                if (item.is_string())
                    return normalizePm(item.get<std::string>());
    }
    return std::nullopt;
}

/**
 * Nix runtime bootstrap (settings, GC, curl). Idempotent and thread-safe.
 */
void bootstrap()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        nix::initLibUtil();
        nix::initLibStore();
        nix::initGC();
    });
}

/**
 * Process-global serialisation of Nix evaluation (auditor G-L8 must-fix #1).
 *
 * `EvalState` and the Nix GC are not safe to drive concurrently from several
 * threads in one process: four parallel evaluations corrupt the allocator
 * (tcache) inside Nix's evaluator. Every evaluation critical section therefore
 * takes this mutex, so concurrent bridge calls are safe but sequential. It is
 * deliberately process-wide rather than per-call: two `EvalState`s in the same
 * process would still share the GC heap.
 */
std::mutex & evaluationMutex()
{
    static std::mutex mutex;
    return mutex;
}

/**
 * Evaluate `config_path` and normalise it into the ExtOS DAG document.
 *
 * Serialised against every other evaluation in this process; see
 * `evaluationMutex()`.
 *
 * @throws nix::Error, nlohmann::json::exception, std::exception
 */
json evaluateToDag(const std::string & config_path, const json & options)
{
    /* Held only around the Nix critical section — argument validation and
       result marshalling stay outside, and the `free` path never touches it. */
    std::lock_guard<std::mutex> evaluationGuard{evaluationMutex()};

    auto store = nix::openStore();

    bool readOnlyMode = true;
    nix::EvalSettings evalSettings{readOnlyMode};
    nix::fetchers::Settings fetchSettings;
    nix::LookupPath lookupPath;

    auto state = nix::make_ref<nix::EvalState>(lookupPath, store, fetchSettings, evalSettings, store);

    /* `rootPath(std::string_view)` accepts relative paths (resolved against the
       cwd); `CanonPath` would silently anchor them at `/`. */
    auto sourcePath = state->rootPath(std::string_view{config_path});
    auto * expr = state->parseExprFromFile(sourcePath);

    nix::Value value;
    state->eval(expr, value);
    state->forceValue(value, nix::noPos);

    nix::NixStringContext context;
    /* copyToStore = false: evaluation must never mutate the read-only CAS. */
    auto raw = nix::printValueAsJSON(*state, true, value, nix::noPos, context, false);

    if (!raw.is_object())
        throw std::runtime_error("the system config must be an attribute set");

    json out = json::object();
    out["version"] = EXTOS_BRIDGE_VERSION;
    out["config_path"] = config_path;
    out["store_dir"] = store->storeDir;
    out["store_dir_source"] = nix::extosRepositoryRoot() ? "extos_repository" : "upstream";
    out["store_uri"] = optString(options, "store_uri").value_or("auto");

    /* --- system block ------------------------------------------------- */
    std::string systemName;
    if (auto name = optString(raw, "name"); name && !name->empty())
        systemName = *name;
    else
        systemName = std::filesystem::path{config_path}.stem().string();

    json system = json::object();
    system["name"] = systemName;
    if (auto pm = roofPm(raw))
        system["native_pm"] = *pm;
    else
        system["native_pm"] = nullptr;
    for (auto key : {"kernel", "root", "linux"})
        if (auto value = optString(raw, key))
            system[key] = *value;
        else
            system[key] = nullptr;

    json services = json::array();
    if (auto * servicesValue = getKey(raw, "services")) {
        if (!servicesValue->is_array())
            throw std::runtime_error("`services` must be a list");
        for (const auto & item : *servicesValue) {
            if (item.is_string()) {
                services.push_back(item);
            } else if (item.is_object()) {
                auto name = optString(item, "name");
                if (!name)
                    throw std::runtime_error("service entry is missing `name`");
                services.push_back(*name);
            } else {
                throw std::runtime_error("service entry must be a string or an attribute set");
            }
        }
    }
    system["services"] = services;
    out["system"] = system;

    /* --- packages ----------------------------------------------------- */
    auto * packagesValue = getKey(raw, "packages");
    if (!packagesValue)
        throw std::runtime_error("missing required `packages = [ … ];` list");
    if (!packagesValue->is_array())
        throw std::runtime_error("`packages` must be a list of strings or attribute sets");

    json packages = json::array();
    json nodes = json::array();
    json edges = json::array();

    auto systemId = "system:" + systemName;
    nodes.push_back({{"id", systemId}, {"kind", "system"}, {"name", systemName}});

    for (const auto & entry : *packagesValue) {
        std::optional<PackageRef> ref;
        std::string error;
        if (entry.is_string()) {
            std::string spec = entry;
            ref = parsePackageSpec(spec, error);
        } else if (entry.is_object()) {
            auto name = optString(entry, "name");
            if (!name) {
                error = "attribute-set entry is missing `name`";
            } else {
                PackageRef r;
                r.name = *name;
                r.origin = optString(entry, "origin").value_or(std::string{DEFAULT_ORIGIN});
                r.version = optString(entry, "version").value_or(std::string{DEFAULT_VERSION});
                if (!validComponent(r.origin) || !validComponent(r.name) || !validComponent(r.version)) {
                    error = "package entry `" + r.name + "` has a component that is not a single path component";
                } else {
                    ref = std::move(r);
                }
            }
        } else {
            error = "expected a string or an attribute set";
        }

        if (!ref)
            throw std::runtime_error("invalid `packages` entry: " + error);

        auto id = ref->origin + "/" + ref->name + "/" + ref->version;
        nix::ExtosOutputIdentity identity{ref->origin, ref->name, ref->version};
        auto layout = nix::extosOutputLayoutPath(store->storeDir, identity, "out");

        json pkg = {
            {"origin", ref->origin},
            {"name", ref->name},
            {"version", ref->version},
            {"id", id},
        };
        /* `null` when the layout cannot be derived (unsafe metadata); callers
           then fall back to the legacy `<hash>-<name>` store path. */
        if (layout)
            pkg["layout"] = *layout;
        else
            pkg["layout"] = nullptr;
        packages.push_back(std::move(pkg));

        nodes.push_back({{"id", id}, {"kind", "package"}, {"origin", ref->origin}, {"name", ref->name}, {"version", ref->version}});
        edges.push_back({{"from", systemId}, {"to", id}, {"kind", "weave"}});
    }

    out["packages"] = std::move(packages);
    out["dag"] = json{{"nodes", std::move(nodes)}, {"edges", std::move(edges)}};

    return out;
}

} // namespace

extern "C" {

const char * extos_nix_eval_version(void)
{
    return EXTOS_BRIDGE_VERSION;
}

int extos_nix_eval_config(const char * config_path, const char * options_json, char ** out_json, char ** out_err)
{
    if (out_json)
        *out_json = nullptr;
    if (out_err)
        *out_err = nullptr;

    if (!config_path || !*config_path) {
        setErr(out_err, "config_path must not be null or empty");
        return EXTOS_EVAL_EINVAL;
    }

    json options = json::object();
    if (options_json && *options_json) {
        try {
            options = json::parse(options_json);
        } catch (const std::exception & e) {
            setErr(out_err, std::string{"invalid options_json: "} + e.what());
            return EXTOS_EVAL_EINVAL;
        }
        if (!options.is_object()) {
            setErr(out_err, "options_json must be a JSON object");
            return EXTOS_EVAL_EINVAL;
        }
    }

    std::string path{config_path};
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path{path}, ec) || ec) {
        setErr(out_err, "cannot read config file: " + path);
        return EXTOS_EVAL_ENOENT;
    }

    try {
        bootstrap();
    } catch (const std::exception & e) {
        setErr(out_err, std::string{"cannot initialise the Nix runtime: "} + e.what());
        return EXTOS_EVAL_EINTERNAL;
    }

    try {
        auto document = evaluateToDag(path, options);
        auto text = document.dump();
        if (!out_json) {
            setErr(out_err, "out_json must not be null");
            return EXTOS_EVAL_EINVAL;
        }
        *out_json = dupOut(text);
        if (!*out_json) {
            setErr(out_err, "out of memory while rendering the result");
            return EXTOS_EVAL_EINTERNAL;
        }
        return EXTOS_EVAL_OK;
    } catch (const nix::Error & e) {
        setErr(out_err, std::string{"evaluation failed: "} + e.what());
        return EXTOS_EVAL_EVAL;
    } catch (const std::exception & e) {
        /* Config-shape problems raised above are evaluation errors; anything
           else (store bootstrap, allocation) is internal. */
        setErr(out_err, e.what());
        return EXTOS_EVAL_EVAL;
    } catch (...) {
        setErr(out_err, "unknown error while evaluating the config");
        return EXTOS_EVAL_EINTERNAL;
    }
}

void extos_nix_eval_free(char * s)
{
    std::free(s);
}

} // extern "C"
