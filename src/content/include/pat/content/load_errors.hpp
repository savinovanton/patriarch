#pragma once

#include "pat/data/content_loader.hpp"

#include <string>
#include <vector>

namespace pat::content {

/// The errors, one per line (LoadError::format(): `[root] file:line:column: error[CODE]: text`), for
/// logs and test output. Shared by every content loader of this module.
[[nodiscard]] std::string format_errors(const std::vector<data::LoadError>& errors);

}  // namespace pat::content
