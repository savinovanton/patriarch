#include "pat/content/load_errors.hpp"

#include <algorithm>
#include <tuple>
#include <utility>

namespace pat::content {

std::string format_errors(const std::vector<data::LoadError>& errors) {
    std::string out;
    for (const data::LoadError& error : errors) {
        out += error.format();
        out += '\n';
    }
    return out;
}

void ErrorSink::fail(const data::ContentObject& object, data::SourcePos pos, std::string message) {
    errors_.push_back(
        data::LoadError{object.root,
                        registry_->root_name(object.root),
                        object.file,
                        data::Diagnostic{pos, data::ErrorCode::ContentRule, std::move(message)}});
}

void ErrorSink::fail_general(std::string message) {
    errors_.push_back(data::LoadError{
        0,
        registry_->root_count() > 0 ? registry_->root_name(0) : std::string(),
        std::string(),
        data::Diagnostic{data::SourcePos{0, 0}, data::ErrorCode::ContentRule, std::move(message)}});
}

std::vector<data::LoadError> ErrorSink::take() {
    std::stable_sort(errors_.begin(), errors_.end(), [](const data::LoadError& a, const data::LoadError& b) {
        return std::tie(a.root, a.file, a.diagnostic.pos, a.diagnostic.code, a.diagnostic.message) <
               std::tie(b.root, b.file, b.diagnostic.pos, b.diagnostic.code, b.diagnostic.message);
    });
    return std::move(errors_);
}

}  // namespace pat::content
