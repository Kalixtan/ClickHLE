#include "clickhle/hle/registry.h"

#include "clickhle/common/log.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "hle";

std::string ordinal_key(std::string_view module, u32 ordinal) {
    return std::string(module) + "#" + std::to_string(ordinal);
}

std::string name_key(std::string_view module, std::string_view name) {
    return std::string(module) + "!" + std::string(name);
}

}  // namespace

std::string HleFunction::key() const {
    return has_ordinal ? ordinal_key(module, ordinal) : name_key(module, name);
}

void HleRegistry::add(std::string module, u32 ordinal, std::string name,
                      u8 arity, HleHandler handler) {
    auto function = std::make_unique<HleFunction>();
    function->module = std::move(module);
    function->name = std::move(name);
    function->ordinal = ordinal;
    function->has_ordinal = true;
    function->arity = arity;
    function->handler = std::move(handler);

    const HleFunction* raw = function.get();
    // Both keys are registered where both exist: games import by ordinal, but
    // the name form is what tests, traces and the coverage report use.
    //
    // Ordinal 0 is a real ordinal here -- the OpenGLES import table is indexed
    // from zero -- so membership is carried by has_ordinal rather than by
    // treating 0 as "none", which is what an import-by-name library needs and
    // what add_named() provides instead.
    const std::string key = ordinal_key(raw->module, ordinal);
    // A second registration of the same ordinal is a bug, not a re-definition.
    // It happened: InputEvents#0 was registered from two files with two
    // different arities, and the later one silently won. Nothing failed, the
    // coverage count was quietly inflated, and the dead copy sat there looking
    // authoritative.
    if (by_ordinal_.find(key) != by_ordinal_.end()) {
        CLICKHLE_ERROR(kCategory)
            << key << " registered twice; keeping the first";
        functions_.push_back(std::move(function));
        return;
    }
    by_ordinal_[key] = raw;
    if (!raw->name.empty()) {
        by_name_[name_key(raw->module, raw->name)] = raw;
    }
    functions_.push_back(std::move(function));
}

const HleFunction* HleRegistry::add_shadow(std::string module, u32 ordinal,
                                           std::string name, u8 arity,
                                           HleHandler handler) {
    auto function = std::make_unique<HleFunction>();
    function->module = std::move(module);
    function->name = std::move(name);
    function->ordinal = ordinal;
    function->has_ordinal = true;
    function->arity = arity;
    function->handler = std::move(handler);

    const HleFunction* raw = function.get();
    // Storage only. No key registration, so lookups and the coverage report do
    // not see it.
    functions_.push_back(std::move(function));
    return raw;
}

void HleRegistry::add_named(std::string module, std::string name, u8 arity,
                            HleHandler handler) {
    auto function = std::make_unique<HleFunction>();
    function->module = std::move(module);
    function->name = std::move(name);
    function->arity = arity;
    function->handler = std::move(handler);

    const HleFunction* raw = function.get();
    by_name_[name_key(raw->module, raw->name)] = raw;
    functions_.push_back(std::move(function));
}

const HleFunction* HleRegistry::find(std::string_view module,
                                     u32 ordinal) const {
    const auto it = by_ordinal_.find(ordinal_key(module, ordinal));
    return it != by_ordinal_.end() ? it->second : nullptr;
}

const HleFunction* HleRegistry::find(std::string_view module,
                                     std::string_view name) const {
    const auto it = by_name_.find(name_key(module, name));
    return it != by_name_.end() ? it->second : nullptr;
}

const HleFunction* HleRegistry::resolve(const GuestImport& import) const {
    return import.by_ordinal ? find(import.module, import.ordinal)
                             : find(import.module, import.name);
}

std::vector<const HleFunction*> HleRegistry::all() const {
    std::vector<const HleFunction*> out;
    out.reserve(functions_.size());
    for (const auto& function : functions_) {
        out.push_back(function.get());
    }
    return out;
}

// --- ThunkTable --------------------------------------------------------------

bool ThunkTable::initialise(AddressSpace& memory) {
    if (initialised_) {
        return true;
    }
    if (!memory.map(kBase, kSize, AddressSpace::kRX, "hle-thunks")) {
        return false;
    }
    // The sentinel that terminates a host-to-guest call. Host code sets lr here
    // before entering guest code; when the guest returns, it lands on this SVC
    // and the dispatcher knows the call is finished.
    if (!memory.write<u32>(kReturnSentinel, encode_svc(kReturnSvc))) {
        return false;
    }
    initialised_ = true;
    CLICKHLE_DEBUG(kCategory) << "thunk page mapped at 0x" << std::hex << kBase;
    return true;
}

std::optional<GuestAddr> ThunkTable::bind(const std::string& import_key,
                                          const HleFunction* function,
                                          AddressSpace& memory) {
    if (!initialised_) {
        CLICKHLE_ERROR(kCategory) << "bind() before initialise()";
        return std::nullopt;
    }

    if (const auto it = index_by_key_.find(import_key);
        it != index_by_key_.end()) {
        return entries_[it->second].address;
    }

    // The tail holds the return sentinel plus pipeline-prefetch padding.
    const u32 capacity = (kSize - kTailGuard) / kSlotSize;
    const auto index = static_cast<u32>(entries_.size());
    if (index >= capacity) {
        CLICKHLE_ERROR(kCategory) << "thunk page exhausted at " << capacity
                                << " entries";
        return std::nullopt;
    }

    const GuestAddr address = kBase + index * kSlotSize;
    if (!memory.write<u32>(address, encode_svc(index))) {
        CLICKHLE_ERROR(kCategory) << "failed to write thunk for " << import_key;
        return std::nullopt;
    }

    entries_.push_back(Entry{import_key, function, address});
    index_by_key_.emplace(import_key, index);
    return address;
}

bool ThunkTable::rebind(const std::string& import_key,
                        const HleFunction* function) {
    const auto it = index_by_key_.find(import_key);
    if (it == index_by_key_.end()) {
        return false;
    }
    entries_[it->second].function = function;
    return true;
}

const ThunkTable::Entry* ThunkTable::lookup(u32 svc_immediate) const {
    if (svc_immediate >= entries_.size()) {
        return nullptr;
    }
    return &entries_[svc_immediate];
}

}  // namespace clickhle
