#include "pat/data/content_loader.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pat::data;

namespace fs = std::filesystem;

namespace {

SchemaSet make_schemas() {
    ObjectSchema trait("trait");
    trait.required("name_key", Type::string())
        .required("cost", Type::integer(0, 10))
        .optional("opposite", Type::reference("trait"))
        .optional("weight", Type::integer(0, 100), TypedValue::of(std::int64_t{1}))
        .optional("tags", Type::list(Type::string()));

    ObjectSchema dynasty("dynasty");
    dynasty.required("name_key", Type::string())
        .optional("favourite", Type::reference("trait"))
        .optional("traits", Type::list(Type::reference("trait")))
        .optional("home", Type::reference("province"));

    ObjectSchema province("province");
    province.required("name_key", Type::string());

    SchemaSet set;
    set.add(std::move(trait)).add(std::move(dynasty)).add(std::move(province));
    return set;
}

std::string file(const std::string& body, int version = 1) {
    return "format_version = " + std::to_string(version) + "\n" + body;
}

std::string trait(const std::string& id, int cost = 1, const std::string& extra = {}) {
    return "trait \"" + id + "\" {\n    name_key = \"trait." + id + "\"\n    cost = " + std::to_string(cost) +
           "\n" + extra + "}\n";
}

std::vector<std::string> codes(const LoadResult& r) {
    std::vector<std::string> out;
    for (const LoadError& e : r.errors)
        out.push_back(error_code_name(e.diagnostic.code));
    return out;
}

std::vector<std::string> ids_of(const ContentRegistry& registry, const char* kind) {
    std::vector<std::string> out;
    const ContentTable* table = registry.table(kind);
    REQUIRE(table != nullptr);
    for (const ContentObject& o : table->objects())
        out.push_back(o.object.id);
    return out;
}

std::string all_messages(const LoadResult& r) {
    std::string out;
    for (const LoadError& e : r.errors)
        out += e.format() + "\n";
    return out;
}

// A small two-root content set used by several tests.
MemorySource base_root() {
    MemorySource base("base");
    base.add("traits/core.pd",
             file(trait("honest", 2) + trait("greedy", 3, "    opposite = trait:honest\n")));
    base.add("dynasties/romanov.pd",
             file("dynasty \"romanov\" {\n    name_key = \"d.romanov\"\n    favourite = trait:greedy\n"
                  "    traits = [ trait:honest, trait:greedy ]\n}\n"));
    return base;
}

}  // namespace

TEST_CASE("content loader: one root loads into sorted typed tables") {
    const SchemaSet schemas = make_schemas();
    const MemorySource base = base_root();
    const LoadResult r = load_content(base, schemas);
    INFO(all_messages(r));
    REQUIRE(r.ok());

    CHECK(r.registry.object_count() == 3);
    CHECK(ids_of(r.registry, "trait") == std::vector<std::string>{"greedy", "honest"});  // sorted by id
    CHECK(ids_of(r.registry, "dynasty") == std::vector<std::string>{"romanov"});
    REQUIRE(r.registry.table("province") != nullptr);  // schema kinds always have a table
    CHECK(r.registry.table("province")->empty());
    CHECK(r.registry.table("nope") == nullptr);

    // Tables are sorted by kind name.
    std::vector<std::string> kinds;
    for (const ContentTable& t : r.registry.tables())
        kinds.push_back(t.kind());
    CHECK(kinds == std::vector<std::string>{"dynasty", "province", "trait"});

    const ContentObject* greedy = r.registry.find("trait", "greedy");
    REQUIRE(greedy != nullptr);
    CHECK(greedy->root == 0);
    CHECK(greedy->file == "traits/core.pd");
    CHECK(greedy->object.pos.line == 6);  // after the header and the 4 lines of "honest"
    const TypedValue* cost = greedy->object.body.find("cost");
    REQUIRE(cost != nullptr);
    CHECK(*cost->get_if<std::int64_t>() == 3);
    CHECK(r.registry.find("trait", "missing") == nullptr);
    CHECK(r.registry.find("nope", "greedy") == nullptr);
}

TEST_CASE("content loader: ids are interned in (kind, id) order and are findable") {
    const SchemaSet schemas = make_schemas();
    const MemorySource base = base_root();
    const LoadResult r = load_content(base, schemas);
    REQUIRE(r.ok());
    const pat::core::StringInterner& ids = r.registry.ids();
    // Kinds sort dynasty < province < trait; ids inside a kind sort bytewise.
    CHECK(ids.find("romanov")->value() == 0);
    CHECK(ids.find("greedy")->value() == 1);
    CHECK(ids.find("honest")->value() == 2);
    const ContentTable* traits = r.registry.table("trait");
    const ContentObject* honest = traits->find("honest");
    REQUIRE(honest != nullptr);
    CHECK(ids.view(honest->id) == "honest");
    CHECK(traits->find(honest->id, ids) == honest);
    CHECK(traits->find(pat::core::StringId{}, ids) == nullptr);
}

TEST_CASE("content loader: a later root overrides an object completely") {
    const SchemaSet schemas = make_schemas();
    const MemorySource base = base_root();
    MemorySource mod("mod");
    mod.add("anything/x.pd", file(trait("greedy", 9) + trait("lazy", 4)));  // greedy has no 'opposite' here

    const ContentSource* roots[] = {&base, &mod};
    const LoadResult r = load_content(roots, schemas);
    INFO(all_messages(r));
    REQUIRE(r.ok());

    CHECK(ids_of(r.registry, "trait") == std::vector<std::string>{"greedy", "honest", "lazy"});
    const ContentObject* greedy = r.registry.find("trait", "greedy");
    REQUIRE(greedy != nullptr);
    CHECK(greedy->root == 1);
    CHECK(greedy->file == "anything/x.pd");
    CHECK(*greedy->object.body.find("cost")->get_if<std::int64_t>() == 9);
    CHECK(greedy->object.body.find("opposite") == nullptr);  // replaced as a whole, not merged
    CHECK(r.registry.find("trait", "honest")->root == 0);
    CHECK(r.registry.find("trait", "lazy")->root == 1);
    CHECK(r.registry.root_count() == 2);
    CHECK(r.registry.root_name(1) == "mod");
}

TEST_CASE("content loader: reversing the roots changes which definition wins") {
    const SchemaSet schemas = make_schemas();
    MemorySource a("a");
    a.add("t.pd", file(trait("x", 1)));
    MemorySource b("b");
    b.add("t.pd", file(trait("x", 2)));
    const ContentSource* ab[] = {&a, &b};
    const ContentSource* ba[] = {&b, &a};
    const LoadResult r1 = load_content(ab, schemas);
    const LoadResult r2 = load_content(ba, schemas);
    REQUIRE(r1.ok());
    REQUIRE(r2.ok());
    CHECK(*r1.registry.find("trait", "x")->object.body.find("cost")->get_if<std::int64_t>() == 2);
    CHECK(*r2.registry.find("trait", "x")->object.body.find("cost")->get_if<std::int64_t>() == 1);
    CHECK(r1.registry.content_hash() != r2.registry.content_hash());
}

TEST_CASE("content loader: the same id twice in one root is an error with both positions") {
    const SchemaSet schemas = make_schemas();
    MemorySource base("base");
    base.add("a.pd", file(trait("dup", 1)));
    base.add("sub/b.pd", file("\n" + trait("dup", 2)));
    const LoadResult r = load_content(base, schemas);
    REQUIRE(r.errors.size() == 1);
    const LoadError& e = r.errors[0];
    CHECK(e.diagnostic.code == ErrorCode::DuplicateObject);
    CHECK(e.root == 0);
    CHECK(e.file == "sub/b.pd");  // "a.pd" < "sub/b.pd" bytewise, so a.pd is the first definition
    CHECK(e.diagnostic.pos == SourcePos{3, 1});
    CHECK(e.diagnostic.message.find("a.pd:2:1") != std::string::npos);
    CHECK(e.format().starts_with("[root 0 'base'] sub/b.pd:3:1: error[PD112]: "));
    // The first definition is kept.
    CHECK(*r.registry.find("trait", "dup")->object.body.find("cost")->get_if<std::int64_t>() == 1);

    // The same id in two different roots is an override, not a conflict.
    MemorySource mod("mod");
    mod.add("a.pd", file(trait("dup", 5)));
    MemorySource clean("clean");
    clean.add("a.pd", file(trait("dup", 1)));
    const ContentSource* roots[] = {&clean, &mod};
    CHECK(load_content(roots, schemas).ok());
}

TEST_CASE("content loader: same id in different kinds is not a duplicate") {
    const SchemaSet schemas = make_schemas();
    MemorySource base("base");
    base.add("a.pd", file(trait("same", 1) + "province \"same\" {\n    name_key = \"p.same\"\n}\n"));
    const LoadResult r = load_content(base, schemas);
    CHECK(r.ok());
    CHECK(r.registry.object_count() == 2);
}

TEST_CASE("content loader: unresolved references carry the position and a suggestion") {
    const SchemaSet schemas = make_schemas();
    MemorySource base("base");
    base.add("traits.pd", file(trait("honest", 1) + trait("greedy", 2, "    opposite = trait:honset\n")));
    base.add(
        "d.pd",
        file("dynasty \"d\" {\n    name_key = \"d\"\n    traits = [ trait:honest, trait:nothing_like_it ]\n"
             "    home = province:moscow\n}\n"));
    const LoadResult r = load_content(base, schemas);
    REQUIRE(r.errors.size() == 3);

    // Sorted by file: d.pd before traits.pd.
    CHECK(r.errors[0].file == "d.pd");
    CHECK(r.errors[0].diagnostic.code == ErrorCode::UnresolvedReference);
    CHECK(r.errors[0].diagnostic.pos == SourcePos{4, 30});  // second list element
    CHECK(r.errors[0].diagnostic.message.find("trait 'nothing_like_it'") != std::string::npos);
    CHECK(r.errors[0].diagnostic.message.find("did you mean") == std::string::npos);
    CHECK(r.errors[1].file == "d.pd");
    CHECK(r.errors[1].diagnostic.pos == SourcePos{5, 12});
    CHECK(r.errors[1].diagnostic.message.find("no 'province' objects are defined") != std::string::npos);

    CHECK(r.errors[2].file == "traits.pd");
    CHECK(r.errors[2].diagnostic.code == ErrorCode::UnresolvedReference);
    CHECK(r.errors[2].diagnostic.pos == SourcePos{9, 16});
    CHECK(r.errors[2].diagnostic.message.find("(did you mean 'honest'?)") != std::string::npos);
}

TEST_CASE("content loader: references resolve across roots and against overrides") {
    const SchemaSet schemas = make_schemas();
    MemorySource base("base");
    base.add("a.pd", file(trait("greedy", 1, "    opposite = trait:honest\n")));  // honest comes from the mod
    MemorySource mod("mod");
    mod.add("b.pd", file(trait("honest", 1)));
    const ContentSource* roots[] = {&base, &mod};
    CHECK(load_content(roots, schemas).ok());

    // Only the base: the reference is broken.
    const LoadResult broken = load_content(base, schemas);
    REQUIRE(broken.errors.size() == 1);
    CHECK(broken.errors[0].diagnostic.code == ErrorCode::UnresolvedReference);

    // A reference inside an object that a later root replaces is no longer checked.
    MemorySource fixer("fixer");
    fixer.add("c.pd", file(trait("greedy", 1)));
    const ContentSource* fixed_roots[] = {&base, &fixer};
    CHECK(load_content(fixed_roots, schemas).ok());
}

TEST_CASE("content loader: an object with a syntax error is skipped without cascading errors") {
    const SchemaSet schemas = make_schemas();
    MemorySource base("base");
    base.add("a.pd",
             file("trait \"broken\" {\n    name_key = \n    cost = 1\n}\n" + trait("fine", 1) +
                  trait("user", 1, "    opposite = trait:broken\n")));
    const LoadResult r = load_content(base, schemas);
    INFO(all_messages(r));
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].diagnostic.code == ErrorCode::ExpectedKey);  // `name_key = cost` swallows the next line
    CHECK(r.registry.find("trait", "broken") == nullptr);
    CHECK(r.registry.find("trait", "fine") != nullptr);  // later objects of the same file are kept
    CHECK(r.registry.find("trait", "user") != nullptr);
}

TEST_CASE("content loader: a reference to an invalid object does not cascade") {
    const SchemaSet schemas = make_schemas();
    MemorySource base("base");
    base.add("a.pd",
             file("trait \"broken\" {\n    name_key = \"x\"\n    cost = 99\n}\n" +
                  trait("user", 1, "    opposite = trait:broken\n")));
    const LoadResult r = load_content(base, schemas);
    CHECK(codes(r) == std::vector<std::string>{"PD104"});  // only the range error, no PD111
}

TEST_CASE("content loader: all errors are aggregated and sorted") {
    const SchemaSet schemas = make_schemas();
    MemorySource base("base");
    base.add("z_ok.pd", file(trait("fine", 1, "    opposite = trait:ghost\n")));               // PD111
    base.add("a_syntax.pd", file("trait \"s\" {\n    name_key = \n}\n"));                      // parse error
    base.add("m_schema.pd", file("trait \"m\" {\n    name_key = \"m\"\n    cost = 50\n}\n"));  // PD104
    base.add("zz_dup.pd", file(trait("fine", 1)));                                             // PD112
    base.add("q_unreadable.pd", file(""));
    base.add_unreadable("q_unreadable.pd");              // PD113
    base.add("w_kind.pd", file("traitt \"t\" {\n}\n"));  // PD108
    base.add("v_header.pd", "trait \"h\" {\n}\n");       // PD020
    MemorySource mod("mod");
    mod.fail_listing("no such directory");  // PD113 (root)

    const ContentSource* roots[] = {&base, &mod};
    const LoadResult r = load_content(roots, schemas);
    INFO(all_messages(r));
    std::vector<std::string> got;
    for (const LoadError& e : r.errors)
        got.push_back(std::to_string(e.root) + ":" + e.file + ":" + error_code_name(e.diagnostic.code));
    CHECK(got == std::vector<std::string>{"0:a_syntax.pd:PD011",
                                          "0:m_schema.pd:PD104",
                                          "0:q_unreadable.pd:PD113",
                                          "0:v_header.pd:PD020",
                                          "0:w_kind.pd:PD108",
                                          "0:z_ok.pd:PD111",
                                          "0:zz_dup.pd:PD112",
                                          "1::PD113"});
    CHECK(r.errors.back().format() ==
          "[root 1 'mod'] error[PD113]: cannot read the content root: no such directory");
    CHECK(r.errors[2].format().find("q_unreadable.pd: error[PD113]: cannot read the file: access denied") !=
          std::string::npos);
    // Valid objects are still registered.
    CHECK(r.registry.find("trait", "fine") != nullptr);
}

TEST_CASE("content loader: format_version is checked and migrated") {
    const SchemaSet schemas = make_schemas();

    SUBCASE("a newer file is PD021 at the version value") {
        MemorySource base("base");
        base.add("a.pd", file(trait("x", 1), 2));
        const LoadResult r = load_content(base, schemas);
        REQUIRE(r.errors.size() == 1);
        CHECK(r.errors[0].diagnostic.code == ErrorCode::UnsupportedVersion);
        CHECK(r.errors[0].diagnostic.pos == SourcePos{1, 18});
        CHECK(r.errors[0].diagnostic.message.find("up to 1") != std::string::npos);
        CHECK(r.registry.object_count() == 0);  // the file is not loaded at all
    }

    SUBCASE("an older file without a migration is PD021") {
        MemorySource base("base");
        base.add("a.pd", file(trait("x", 1), 1));
        LoadOptions options;
        options.current_version = 2;
        const LoadResult r = load_content(base, schemas, options);
        REQUIRE(r.errors.size() == 1);
        CHECK(r.errors[0].diagnostic.code == ErrorCode::UnsupportedVersion);
        CHECK(r.errors[0].diagnostic.message.find("no migration from version 1 to 2") != std::string::npos);
    }

    SUBCASE("registered steps upgrade old files one version at a time") {
        MemorySource base("base");
        base.add("old.pd", file(trait("x", 1), 1));
        base.add("new.pd", file(trait("y", 1), 3));
        LoadOptions options;
        options.current_version = 3;
        int calls = 0;
        options.migrations.add(1, [&calls](ParsedFile& f) -> std::optional<std::string> {
            ++calls;
            for (Entry& e : f.entries) {
                if (e.key == "trait" && e.label)
                    e.label = *e.label + "_v2";
            }
            return std::nullopt;
        });
        options.migrations.add(2, [&calls](ParsedFile&) -> std::optional<std::string> {
            ++calls;
            return std::nullopt;
        });
        const LoadResult r = load_content(base, schemas, options);
        INFO(all_messages(r));
        REQUIRE(r.ok());
        CHECK(calls == 2);
        CHECK(ids_of(r.registry, "trait") == std::vector<std::string>{"x_v2", "y"});
    }

    SUBCASE("a failing step is PD114") {
        MemorySource base("base");
        base.add("old.pd", file(trait("x", 1), 1));
        LoadOptions options;
        options.current_version = 2;
        options.migrations.add(1, [](ParsedFile&) -> std::optional<std::string> { return "cannot do it"; });
        const LoadResult r = load_content(base, schemas, options);
        REQUIRE(r.errors.size() == 1);
        CHECK(r.errors[0].diagnostic.code == ErrorCode::MigrationFailed);
        CHECK(r.errors[0].diagnostic.message.find("cannot do it") != std::string::npos);
    }

    SUBCASE("migration registry basics") {
        MigrationRegistry m;
        CHECK_FALSE(m.has(1));
        m.add(1, [](ParsedFile&) { return std::optional<std::string>{}; });
        CHECK(m.has(1));
        CHECK(m.find(1) != nullptr);
        CHECK(m.find(2) == nullptr);
    }
}

TEST_CASE("content loader: only the .pd extension is read") {
    const SchemaSet schemas = make_schemas();
    MemorySource base("base");
    base.add("a.pd", file(trait("real", 1)));
    base.add("notes.txt", "this is not data {{{");
    base.add("b.PD", "garbage");
    base.add("c.pd.bak", "garbage");
    base.add("sub/.pd", "garbage");
    const LoadResult r = load_content(base, schemas);
    CHECK(r.ok());
    CHECK(r.registry.object_count() == 1);
}

TEST_CASE("content loader: result does not depend on the insertion order of the source") {
    const SchemaSet schemas = make_schemas();
    struct F {
        const char* path;
        std::string text;
    };
    const std::vector<F> files = {
        {"traits/b.pd", file(trait("beta", 2, "    opposite = trait:alpha\n"))},
        {"traits/a.pd", file(trait("alpha", 1))},
        {"traits/sub/c.pd", file(trait("gamma", 3) + trait("alpha", 4))},  // PD112 against traits/a.pd
        {"d/e.pd", file("dynasty \"x\" {\n    name_key = \"x\"\n    favourite = trait:omega\n}\n")},
        {"d/f.pd", file("trait \"bad\" {\n}\n")},
    };

    auto load_in_order = [&](std::vector<std::size_t> order) {
        MemorySource base("base");
        for (const std::size_t i : order)
            base.add(files[i].path, files[i].text);
        MemorySource mod("mod");
        mod.add("m.pd", file(trait("beta", 7)));
        const ContentSource* roots[] = {&base, &mod};
        return load_content(roots, schemas);
    };

    std::vector<std::size_t> order = {0, 1, 2, 3, 4};
    const LoadResult reference = load_in_order(order);
    REQUIRE_FALSE(reference.ok());
    std::vector<std::string> reference_errors;
    for (const LoadError& e : reference.errors)
        reference_errors.push_back(e.format());

    int permutations = 0;
    do {
        const LoadResult r = load_in_order(order);
        std::vector<std::string> errors;
        for (const LoadError& e : r.errors)
            errors.push_back(e.format());
        CHECK(errors == reference_errors);
        CHECK(r.registry.content_hash() == reference.registry.content_hash());
        CHECK(ids_of(r.registry, "trait") == ids_of(reference.registry, "trait"));
        for (const ContentObject& o : reference.registry.table("trait")->objects()) {
            const ContentObject* same = r.registry.find("trait", o.object.id);
            REQUIRE(same != nullptr);
            CHECK(same->id == o.id);  // identical interned ids
            CHECK(same->file == o.file);
            CHECK(same->root == o.root);
        }
        ++permutations;
    } while (std::next_permutation(order.begin(), order.end()));
    CHECK(permutations == 120);
}

TEST_CASE("content loader: loading twice gives the same hash and tables") {
    const SchemaSet schemas = make_schemas();
    const MemorySource base = base_root();
    const LoadResult a = load_content(base, schemas);
    const LoadResult b = load_content(base, schemas);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a.registry.content_hash() == b.registry.content_hash());
    CHECK(a.registry.content_hash() != 0);
    CHECK(ids_of(a.registry, "trait") == ids_of(b.registry, "trait"));
}

TEST_CASE("content hash: ignores comments, whitespace, file layout and the providing root") {
    const SchemaSet schemas = make_schemas();
    MemorySource plain("p");
    plain.add("traits.pd", file(trait("a", 1) + trait("b", 2, "    tags = [ \"x\", \"y\" ]\n")));
    const LoadResult reference = load_content(plain, schemas);
    REQUIRE(reference.ok());
    const std::uint64_t h = reference.registry.content_hash();

    SUBCASE("comments and formatting") {
        MemorySource other("o");
        other.add("traits.pd",
                  "# a comment first\nformat_version = 1   # trailing\n\n\n"
                  "trait   \"a\"   {   name_key = \"trait.a\"  # note\n   cost=1 }\n"
                  "# between\n"
                  "trait \"b\" { cost = 2\n name_key = \"trait.b\"\n tags = [\"x\",\n   \"y\"] }\n");
        const LoadResult r = load_content(other, schemas);
        INFO(all_messages(r));
        REQUIRE(r.ok());
        CHECK(r.registry.content_hash() == h);  // also field order inside an object does not matter
    }

    SUBCASE("file names, directories and order of objects") {
        MemorySource other("different name");
        other.add("zzz/b.pd", file(trait("b", 2, "    tags = [ \"x\", \"y\" ]\n")));
        other.add("aaa/a.pd", file(trait("a", 1)));
        const LoadResult r = load_content(other, schemas);
        REQUIRE(r.ok());
        CHECK(r.registry.content_hash() == h);
    }

    SUBCASE("an explicit value equal to the schema default equals an omitted one") {
        MemorySource other("o");
        other.add("t.pd",
                  file(trait("a", 1, "    weight = 1\n") + trait("b", 2, "    tags = [ \"x\", \"y\" ]\n")));
        const LoadResult r = load_content(other, schemas);
        REQUIRE(r.ok());
        CHECK(r.registry.content_hash() == h);
    }

    SUBCASE("the same content split across roots") {
        MemorySource base("base");
        base.add("t.pd", file(trait("a", 1)));
        MemorySource mod("mod");
        mod.add("t.pd", file(trait("b", 2, "    tags = [ \"x\", \"y\" ]\n")));
        const ContentSource* roots[] = {&base, &mod};
        const LoadResult r = load_content(roots, schemas);
        REQUIRE(r.ok());
        CHECK(r.registry.content_hash() == h);
    }

    SUBCASE("a shadowed definition does not matter") {
        MemorySource base("base");
        base.add("t.pd", file(trait("a", 9) + trait("b", 9)));
        MemorySource mod("mod");
        mod.add("t.pd", file(trait("a", 1) + trait("b", 2, "    tags = [ \"x\", \"y\" ]\n")));
        const ContentSource* roots[] = {&base, &mod};
        const LoadResult r = load_content(roots, schemas);
        REQUIRE(r.ok());
        CHECK(r.registry.content_hash() == h);
    }
}

TEST_CASE("content hash: changes with any effective change") {
    const SchemaSet schemas = make_schemas();
    auto hash_of = [&](const std::string& a, const std::string& b) {
        MemorySource src("s");
        src.add("t.pd", file(a + b));
        const LoadResult r = load_content(src, schemas);
        REQUIRE(r.ok());
        return r.registry.content_hash();
    };
    const std::uint64_t reference = hash_of(trait("a", 1), trait("b", 2));
    CHECK(hash_of(trait("a", 1), trait("b", 2)) == reference);
    CHECK(hash_of(trait("a", 1), trait("b", 3)) != reference);                      // a value
    CHECK(hash_of(trait("a", 1), trait("c", 2)) != reference);                      // an id
    CHECK(hash_of(trait("a", 1), "") != reference);                                 // removed object
    CHECK(hash_of(trait("a", 1), trait("b", 2) + trait("c", 2)) != reference);      // added object
    CHECK(hash_of(trait("a", 1), trait("b", 2, "    weight = 2\n")) != reference);  // a non-default value
    CHECK(hash_of(trait("a", 1), trait("b", 2, "    tags = [ \"x\" ]\n")) !=
          reference);  // an added optional field
    CHECK(hash_of(trait("a", 1, "    tags = [ \"x\", \"y\" ]\n"), "") !=
          hash_of(trait("a", 1, "    tags = [ \"y\", \"x\" ]\n"), ""));  // list order is data

    // A string that moves between fields must not collide with the original.
    CHECK(hash_of(trait("ab", 1), "") != hash_of(trait("a", 1), trait("b", 1)));
}

TEST_CASE("content hash: depends on the root order when it changes the result") {
    const SchemaSet schemas = make_schemas();
    MemorySource a("a");
    a.add("t.pd", file(trait("only_a", 1) + trait("shared", 1)));
    MemorySource b("b");
    b.add("t.pd", file(trait("only_b", 1) + trait("shared", 2)));
    const ContentSource* ab[] = {&a, &b};
    const ContentSource* ba[] = {&b, &a};
    const ContentSource* aab[] = {&a, &a, &b};
    CHECK(load_content(ab, schemas).registry.content_hash() !=
          load_content(ba, schemas).registry.content_hash());
    // Repeating a root changes nothing: the effective result is identical.
    CHECK(load_content(ab, schemas).registry.content_hash() ==
          load_content(aab, schemas).registry.content_hash());
}

TEST_CASE("content hash: the empty content has a stable value") {
    const SchemaSet schemas = make_schemas();
    const MemorySource empty("e");
    const LoadResult r = load_content(empty, schemas);
    REQUIRE(r.ok());
    MemorySource only_header("h");
    only_header.add("a.pd", file(""));
    CHECK(load_content(only_header, schemas).registry.content_hash() == r.registry.content_hash());
    CHECK(r.registry.object_count() == 0);
}

// ---- directory source --------------------------------------------------------------------------

namespace {

class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / ("patriarch_e4t04_" + tag + "_" + std::to_string(ticks));
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

    void write(const std::string& relative, const std::string& text) const {
        const fs::path target = path_ / relative;
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary);
        out << text;
    }

private:
    fs::path path_;
};

}  // namespace

TEST_CASE("directory source: nested directories, sorted traversal, non-.pd files ignored") {
    const TempDir dir("nested");
    dir.write("traits/b.pd", file(trait("b", 1)));
    dir.write("traits/a.pd", file(trait("a", 1)));
    dir.write("traits/deep/er/c.pd", file(trait("c", 1)));
    dir.write("z.pd", file(trait("z", 1)));
    dir.write("README.txt", "not data {{{");
    dir.write("traits/old.pd.bak", "not data {{{");
    dir.write("data.json", "{}");

    const DirectorySource source(dir.path());
    auto listed = source.list_files();
    REQUIRE(listed.has_value());
    std::vector<std::string> paths = listed.value();
    std::sort(paths.begin(), paths.end());
    CHECK(paths == std::vector<std::string>{"README.txt",
                                            "data.json",
                                            "traits/a.pd",
                                            "traits/b.pd",
                                            "traits/deep/er/c.pd",
                                            "traits/old.pd.bak",
                                            "z.pd"});  // '/' separators, relative

    const SchemaSet schemas = make_schemas();
    const LoadResult r = load_content(source, schemas);
    INFO(all_messages(r));
    REQUIRE(r.ok());
    CHECK(ids_of(r.registry, "trait") == std::vector<std::string>{"a", "b", "c", "z"});
    CHECK(r.registry.find("trait", "c")->file == "traits/deep/er/c.pd");

    // Same tree in memory gives the same hash.
    MemorySource mem("mem");
    mem.add("whatever.pd", file(trait("a", 1) + trait("b", 1) + trait("c", 1) + trait("z", 1)));
    CHECK(load_content(mem, schemas).registry.content_hash() == r.registry.content_hash());
}

TEST_CASE("directory source: errors name the file relative to the root, in sorted order") {
    const TempDir dir("errors");
    dir.write("b/two.pd", file("trait \"t\" {\n}\n"));
    dir.write("a/one.pd", file("trait \"u\" {\n}\n"));
    const SchemaSet schemas = make_schemas();
    const DirectorySource source(dir.path());
    const LoadResult r = load_content(source, schemas);
    // Each trait lacks name_key and cost: two PD102 per file.
    REQUIRE(r.errors.size() == 4);
    CHECK(r.errors[0].file == "a/one.pd");
    CHECK(r.errors[2].file == "b/two.pd");
    CHECK(r.errors[0].diagnostic.code == ErrorCode::MissingField);
    CHECK(r.errors[0].format().find("a/one.pd:2:1: error[PD102]") != std::string::npos);
}

TEST_CASE("directory source: a missing root or a file instead of a directory is PD113") {
    const SchemaSet schemas = make_schemas();
    const TempDir dir("missing");
    {
        const DirectorySource source(dir.path() / "does_not_exist");
        CHECK_FALSE(source.list_files().has_value());
        const LoadResult r = load_content(source, schemas);
        REQUIRE(r.errors.size() == 1);
        CHECK(r.errors[0].diagnostic.code == ErrorCode::UnreadableSource);
        CHECK(r.errors[0].file.empty());
        CHECK(r.errors[0].format().find("cannot read the content root") != std::string::npos);
    }
    {
        dir.write("plain.txt", "x");
        const DirectorySource source(dir.path() / "plain.txt");
        const LoadResult r = load_content(source, schemas);
        REQUIRE(r.errors.size() == 1);
        CHECK(r.errors[0].diagnostic.code == ErrorCode::UnreadableSource);
    }
    {
        const DirectorySource source(dir.path());
        CHECK_FALSE(source.read_file("nope.pd").has_value());
        auto text = source.read_file("plain.txt");
        REQUIRE(text.has_value());
        CHECK(text.value() == "x");
    }
}

TEST_CASE("directory source: base and mod directories override by id") {
    const TempDir base_dir("base");
    const TempDir mod_dir("mod");
    base_dir.write("traits.pd", file(trait("a", 1) + trait("b", 1)));
    mod_dir.write("deeper/patch.pd", file(trait("b", 5)));
    const DirectorySource base(base_dir.path());
    const DirectorySource mod(mod_dir.path());
    const ContentSource* roots[] = {&base, &mod};
    const SchemaSet schemas = make_schemas();
    const LoadResult r = load_content(roots, schemas);
    REQUIRE(r.ok());
    CHECK(r.registry.find("trait", "b")->root == 1);
    CHECK(*r.registry.find("trait", "b")->object.body.find("cost")->get_if<std::int64_t>() == 5);
}
