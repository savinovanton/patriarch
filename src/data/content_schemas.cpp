#include "pat/data/content_schemas.hpp"

#include "pat/data/demography_schemas.hpp"
#include "pat/data/economy_schemas.hpp"
#include "pat/data/example_schemas.hpp"

namespace pat::data {

SchemaSet content_schemas() {
    SchemaSet set;
    // E4-T05 replaces this example with the real kinds (province, dynasty, trait, skill, ...).
    set.add(make_example_trait_schema());
    // E9-T18: the economy kinds (data/economy/).
    add_economy_schemas(set);
    // E7-T07: the demography balance (data/balance/).
    add_demography_schemas(set);
    return set;
}

}  // namespace pat::data
