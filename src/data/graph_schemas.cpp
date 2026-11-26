#include "pat/data/graph_schemas.hpp"

#include <string>
#include <utility>

namespace pat::data {

namespace {

ObjectSchema node_schema() {
    ObjectSchema s;
    s.describe_kind("Узел графа: провинция (метка — ключ определения province) и её координаты в км.");
    s.required("x_km", Type::integer(-100'000, 100'000))
        .describe("Координата центра провинции по горизонтали (на восток), км; для эвристики A*.")
        .required("y_km", Type::integer(-100'000, 100'000))
        .describe("Координата центра провинции по вертикали (на юг), км.");
    return s;
}

ObjectSchema edge_schema() {
    ObjectSchema s;
    s.describe_kind(
        "Неориентированное ребро графа между двумя соседними провинциями; записывается один раз.");
    s.required("from", Type::reference("province"))
        .describe("Один конец ребра (узел графа).")
        .required("to", Type::reference("province"))
        .describe("Другой конец ребра (узел графа).")
        .required("length_km", Type::integer(10, 10'000))
        .describe("Длина ребра, км: не меньше 10 и не короче прямой между узлами (экспорт из карты).")
        .optional("road", Type::enumeration({"none", "tract"}), TypedValue::of(EnumValue{"none", 0}))
        .describe("Дорога по ребру: none — нет, tract — тракт (быстрее); правится вручную.");
    return s;
}

ObjectSchema province_graph_schema() {
    ObjectSchema s{std::string(kProvinceGraphKind)};
    s.intern_last();
    s.describe_kind(
        "Граф провинций для симуляции (army-model §5): соседи, длины рёбер и тракты. Генерируется "
        "`mapgen --export-graph` из карты; тест сверяет узлы, рёбра и длины с картой. Это "
        "правила, а не состояние мира.");
    s.repeatable_labeled("node", Type::block(node_schema()))
        .describe("Узел: `node \"p0001\" { x_km = … y_km = … }`, метка — ключ провинции.")
        .repeatable("edge", Type::block(edge_schema()))
        .describe("Ребро: `edge { from = province:p0001 to = province:p0002 length_km = … }`.");
    return s;
}

}  // namespace

void add_graph_schemas(SchemaSet& schemas) {
    schemas.add(province_graph_schema());
}

}  // namespace pat::data
