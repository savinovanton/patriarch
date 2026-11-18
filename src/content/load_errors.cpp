#include "pat/content/load_errors.hpp"

#include <algorithm>
#include <format>
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
    fail(object, pos, data::ErrorCode::ContentRule, std::move(message));
}

void ErrorSink::fail(const data::ContentObject& object,
                     data::SourcePos pos,
                     data::ErrorCode code,
                     std::string message) {
    errors_.push_back(data::LoadError{object.root,
                                      registry_->root_name(object.root),
                                      object.file,
                                      data::Diagnostic{pos, code, std::move(message)}});
}

void ErrorSink::fail_general(std::string message) {
    errors_.push_back(data::LoadError{
        0,
        registry_->root_count() > 0 ? registry_->root_name(0) : std::string(),
        std::string(),
        data::Diagnostic{data::SourcePos{0, 0}, data::ErrorCode::ContentRule, std::move(message)}});
}

const data::ContentObject* ErrorSink::single_object(std::string_view kind) {
    const auto* table = registry_->table(kind);
    if (table == nullptr || table->empty()) {
        fail_general(std::format("no {} object found (expected: {} \"main\" {{ ... }})", kind, kind));
        return nullptr;
    }
    const data::ContentObject& object = table->objects().front();
    for (std::size_t i = 1; i < table->size(); ++i) {
        const data::ContentObject& extra = table->objects()[i];
        fail(extra,
             extra.object.pos,
             std::format("only one {} object is allowed, '{}' is already defined in {}:{}:{}",
                         kind,
                         object.object.id,
                         object.file,
                         object.object.pos.line,
                         object.object.pos.column));
    }
    return &object;
}

std::vector<data::LoadError> ErrorSink::take() {
    std::stable_sort(errors_.begin(), errors_.end(), [](const data::LoadError& a, const data::LoadError& b) {
        return std::tie(a.root, a.file, a.diagnostic.pos, a.diagnostic.code, a.diagnostic.message) <
               std::tie(b.root, b.file, b.diagnostic.pos, b.diagnostic.code, b.diagnostic.message);
    });
    return std::move(errors_);
}

}  // namespace pat::content
