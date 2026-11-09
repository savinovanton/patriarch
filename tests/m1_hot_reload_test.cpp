#include "pat/content/m1_hot_reload.hpp"
#include "pat/content/m1_scenario.hpp"
#include "pat/data/content_source.hpp"
#include "pat/mapdata/map_gen.hpp"

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::string read_file(const pat::data::ContentSource& source, std::string_view path) {
    auto text = source.read_file(path);
    INFO(path);
    REQUIRE(text.has_value());
    return std::move(text).value();
}

std::string replace_first(std::string text, std::string_view before, std::string_view after) {
    const auto pos = text.find(before);
    REQUIRE(pos != std::string::npos);
    text.replace(pos, before.size(), after);
    return text;
}

void require_publish(pat::content::M1HotReload& reload) {
    const auto result = reload.poll();
    if (!result) {
        INFO(result.error().message);
        REQUIRE(result.has_value());
    }
    REQUIRE(result.value());
}

std::string_view first_province_kind(const pat::data::ContentRegistry& registry) {
    const auto* province = registry.find("province", "p0001");
    REQUIRE(province != nullptr);
    const auto* field = province->object.body.find("kind");
    REQUIRE(field != nullptr);
    const auto* kind = field->get_if<pat::data::RefValue>();
    REQUIRE(kind != nullptr);
    return kind->id;
}

}  // namespace

TEST_CASE("M1 hot reload publishes valid edits and keeps the previous snapshot on failure") {
    const pat::data::DirectorySource shipped{std::string(PATRIARCH_DATA_DIR)};
    pat::data::MemorySource source("m1-hot-reload");
    const auto files = shipped.list_files();
    REQUIRE(files.has_value());
    for (const auto& path : files.value()) {
        source.add(path, read_file(shipped, path));
    }

    const auto map = pat::mapdata::generate_map(pat::content::m1_map_params());
    pat::content::M1HotReload reload(source, map);
    std::vector<decltype(reload.current())> notifications;
    reload.subscribe([&](auto snapshot) {
        CHECK(reload.current() == snapshot);
        notifications.push_back(std::move(snapshot));
    });

    require_publish(reload);
    const auto initial = reload.current();
    REQUIRE(initial != nullptr);
    REQUIRE(notifications.size() == 1);
    CHECK(notifications.back() == initial);
    CHECK(initial->bindings.size() == 20);
    CHECK(initial->province_names.at(1) == "Тестовая провинция 1");

    const auto unchanged = reload.poll();
    REQUIRE(unchanged.has_value());
    CHECK_FALSE(unchanged.value());
    CHECK(reload.current() == initial);
    CHECK(notifications.size() == 1);

    const std::string labels_path = "loc/ru/m1.pd";
    source.add(labels_path,
               replace_first(read_file(source, labels_path),
                             "\"Тестовая провинция 1\"",
                             "\"Обновлённая провинция 1\""));
    require_publish(reload);
    const auto renamed = reload.current();
    REQUIRE(renamed != nullptr);
    REQUIRE(notifications.size() == 2);
    CHECK(notifications.back() == renamed);
    CHECK(renamed != initial);
    CHECK(renamed->province_names.at(1) == "Обновлённая провинция 1");
    CHECK(renamed->localizer.format("province.p0001.name") == "Обновлённая провинция 1");
    CHECK(renamed->registry.content_hash() == initial->registry.content_hash());

    const std::string provinces_path = "provinces/m1.pd";
    const std::string provinces = read_file(source, provinces_path);
    source.add(provinces_path, "format_version = 1\nprovince \"p0001\" {\n");
    const auto malformed = reload.poll();
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error().message.find("PD014") != std::string::npos);
    const auto still_malformed = reload.poll();
    REQUIRE_FALSE(still_malformed.has_value());
    CHECK(still_malformed.error().message == malformed.error().message);
    CHECK(reload.current() == renamed);
    CHECK(notifications.size() == 2);

    source.add(provinces_path, replace_first(provinces, "province \"p0020\"", "province \"p0021\""));
    const auto mismatch = reload.poll();
    REQUIRE_FALSE(mismatch.has_value());
    CHECK(mismatch.error().message.find("map province 'p0020' has no content definition") !=
          std::string::npos);
    CHECK(mismatch.error().message.find("province definition 'p0021' has no map province") !=
          std::string::npos);
    CHECK(reload.current() == renamed);
    CHECK(notifications.size() == 2);

    source.add(provinces_path,
               replace_first(provinces, "kind = province_kind:rural", "kind = province_kind:city"));
    require_publish(reload);
    const auto recovered = reload.current();
    REQUIRE(recovered != nullptr);
    REQUIRE(notifications.size() == 3);
    CHECK(notifications.back() == recovered);
    CHECK(recovered != renamed);
    CHECK(recovered->registry.content_hash() != renamed->registry.content_hash());
    CHECK(recovered->province_names.at(1) == "Обновлённая провинция 1");
    CHECK(first_province_kind(recovered->registry) == "city");
    CHECK(first_province_kind(renamed->registry) == "rural");
    CHECK(initial->province_names.at(1) == "Тестовая провинция 1");
}
