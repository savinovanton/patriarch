#include "pat/core/build_info.hpp"

namespace pat::core {

std::string_view version() noexcept {
    return PATRIARCH_VERSION_STRING;
}

}  // namespace pat::core
