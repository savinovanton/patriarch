#include "pat/sim/date.hpp"

namespace pat::sim {

std::string to_string(Date date) {
    const YearMonthDay v = date.ymd();
    return std::format("{:04}-{:02}-{:02}", v.year, v.month, v.day);
}

}  // namespace pat::sim
