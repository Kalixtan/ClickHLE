#include "clickhle/core/cpu.h"

#include "clickhle/common/log.h"
#include "clickhle/core/address_space.h"

namespace clickhle {

std::unique_ptr<ICpuCore> make_unicorn_cpu_core(AddressSpace& memory);
bool unicorn_cpu_available();

namespace {
constexpr std::string_view kCategory = "cpu";
}  // namespace

std::string_view to_string(RunStatus status) noexcept {
    switch (status) {
        case RunStatus::Svc:             return "svc";
        case RunStatus::Stopped:         return "stopped";
        case RunStatus::BudgetExhausted: return "budget-exhausted";
        case RunStatus::Fault:           return "fault";
        case RunStatus::Unsupported:     return "unsupported";
    }
    return "unknown";
}

std::unique_ptr<ICpuCore> make_cpu_core(std::string_view backend,
                                        AddressSpace& memory) {
    if (backend.empty()) {
        backend = "unicorn";
    }
    if (backend != "unicorn") {
        CLICKHLE_ERROR(kCategory) << "unsupported CPU backend '" << backend
                                  << "' (this build supports Unicorn only)";
        return nullptr;
    }
    if (!unicorn_cpu_available()) {
        CLICKHLE_ERROR(kCategory)
            << "Unicorn Engine is unavailable; install its runtime library to "
               "run games";
        return nullptr;
    }
    return make_unicorn_cpu_core(memory);
}

std::vector<std::string_view> available_cpu_backends() {
    return {"unicorn"};
}

}  // namespace clickhle
