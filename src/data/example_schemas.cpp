#include "pat/data/example_schemas.hpp"

#include <utility>

namespace pat::data {

ObjectSchema make_example_trait_schema() {
    ObjectSchema chance("chance");
    chance.required("base", Type::fixed64(core::Fixed64::zero(), core::Fixed64::one()))
        .optional("max",
                  Type::fixed64(core::Fixed64::zero(), core::Fixed64::one()),
                  TypedValue::of(core::Fixed64::one()));

    ObjectSchema trait("trait");
    trait.required("name_key", Type::string())
        .required("cost", Type::integer(0, 10))
        .optional("group",
                  Type::enumeration({"vice", "virtue", "neutral"}),
                  TypedValue::of(EnumValue{"neutral", 2}))
        .optional("opposite", Type::reference("trait"))
        .optional("weight",
                  Type::fixed32(core::Fixed64::zero(), core::Fixed64::from_int(100)),
                  TypedValue::of(core::Fixed32::one()))
        .optional("inheritable", Type::boolean(), TypedValue::of(false))
        .optional("since", Type::date())
        .optional("tags", Type::list(Type::string()))
        .optional("skills", Type::map(Type::integer(-10, 10)))
        .repeatable("note", Type::string())
        .optional("chance", Type::block(std::move(chance)));
    return trait;
}

SchemaSet make_example_schema_set() {
    SchemaSet set;
    set.add(make_example_trait_schema());
    return set;
}

}  // namespace pat::data
