#include "pat/content/load_errors.hpp"

namespace pat::content {

std::string format_errors(const std::vector<data::LoadError>& errors) {
    std::string out;
    for (const data::LoadError& error : errors) {
        out += error.format();
        out += '\n';
    }
    return out;
}

}  // namespace pat::content
