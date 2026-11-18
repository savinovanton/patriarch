#pragma once

#include "pat/data/content_loader.hpp"
#include "pat/data/content_registry.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace pat::content {

/// The errors, one per line (LoadError::format(): `[root] file:line:column: error[CODE]: text`), for
/// logs and test output. Shared by every content loader of this module.
[[nodiscard]] std::string format_errors(const std::vector<data::LoadError>& errors);

/// Collects the cross-object errors (PD115, ErrorCode::ContentRule) of a builder that runs on an
/// error-free registry. New builders use it instead of repeating the same three helpers.
class ErrorSink {
public:
    explicit ErrorSink(const data::ContentRegistry& registry) : registry_(&registry) {}

    /// A problem at `pos` of `object` (position {0, 0} when unknown).
    void fail(const data::ContentObject& object, data::SourcePos pos, std::string message);
    /// A problem that belongs to no object (an object is missing).
    void fail_general(std::string message);

    /// The only object of `kind` (the first, by registry order). Reports a missing table or a second
    /// object (`kind "main" { ... }` is expected) and returns nullptr only when there is none.
    [[nodiscard]] const data::ContentObject* single_object(std::string_view kind);

    [[nodiscard]] bool empty() const noexcept { return errors_.empty(); }
    /// The errors sorted by (root, file, position, code, message).
    [[nodiscard]] std::vector<data::LoadError> take();

private:
    const data::ContentRegistry* registry_;
    std::vector<data::LoadError> errors_;
};

}  // namespace pat::content
