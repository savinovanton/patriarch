#include "pat/data/content_schemas.hpp"

#include "pat/data/example_schemas.hpp"

namespace pat::data {

SchemaSet content_schemas() {
    SchemaSet set;
    // E4-T05 replaces this example with the real kinds (province, dynasty, trait, skill, ...).
    set.add(make_example_trait_schema());
    return set;
}

}  // namespace pat::data
