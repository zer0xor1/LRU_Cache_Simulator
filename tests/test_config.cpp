#include "cachesim/config.hpp"
#include "test_framework.hpp"

using namespace cachesim;

TEST(parse_size_accepts_units_and_hex) {
    CHECK_EQ(parse_size("32768").value(), 32768u);
    CHECK_EQ(parse_size("32K").value(), 32768u);
    CHECK_EQ(parse_size("32 KB").value(), 32768u);
    CHECK_EQ(parse_size("32kib").value(), 32768u);
    CHECK_EQ(parse_size("1M").value(), 1048576u);
    CHECK_EQ(parse_size("2GiB").value(), 2147483648u);
    CHECK_EQ(parse_size("0x8000").value(), 32768u);
    CHECK_EQ(parse_size(" 64 ").value(), 64u);
    CHECK_EQ(parse_size("010").value(), 10u);  // decimal, not octal (the original used base 0)
    CHECK(!parse_size(""));
    CHECK(!parse_size("K"));
    CHECK(!parse_size("12Q"));
    CHECK(!parse_size("-4"));
    CHECK(!parse_size("99999999999999999999999"));  // overflow
    CHECK(!parse_size("17179869184G"));             // overflow after shift
}

TEST(parse_assoc_handles_full) {
    CHECK_EQ(parse_assoc("8").value(), 8u);
    CHECK_EQ(parse_assoc("full").value(), 0u);
    CHECK_EQ(parse_assoc("Fully-Associative").value(), 0u);
    CHECK(!parse_assoc("0"));
    CHECK(!parse_assoc("many"));
}

TEST(format_size_is_readable) {
    CHECK_EQ(format_size(48), std::string("48 B"));
    CHECK_EQ(format_size(32768), std::string("32 KiB"));
    CHECK_EQ(format_size(163840), std::string("160 KiB"));
    CHECK_EQ(format_size(1536 * 1024), std::string("1.50 MiB"));
    CHECK_EQ(format_size(1ULL << 30), std::string("1 GiB"));
}

TEST(legacy_config_format_still_works) {
    const SimConfig c = parse_config("Cache Size=163840\nBlock Size=1024\nAssociativity=8\n");
    REQUIRE(c.levels.size() == 1);
    CHECK_EQ(c.levels[0].name, std::string("L1"));
    CHECK_EQ(c.levels[0].size_bytes, 163840u);
    CHECK_EQ(c.levels[0].block_bytes, 1024u);
    CHECK_EQ(c.levels[0].assoc, 8u);
    CHECK_EQ(c.levels[0].sets(), 20u);
    validate(c);  // 20 sets is allowed (modulo indexing)...
    const auto notes = config_notes(c);
    REQUIRE(notes.size() == 1);  // ...but reported
    CHECK(notes[0].find("not a power of two") != std::string::npos);
}

TEST(legacy_config_with_windows_line_endings) {
    const SimConfig c = parse_config("Cache Size=32768\r\nBlock Size=64\r\nAssociativity=4\r\n");
    CHECK_EQ(c.levels[0].size_bytes, 32768u);
    CHECK_EQ(c.levels[0].assoc, 4u);
}

TEST(multi_level_config) {
    const char* text = R"(
# a three-level hierarchy
[simulation]
seed = 7
ifetch = yes

[L1]
size = 32K        ; comment
block = 64
assoc = 8
policy = plru
write = write-through
allocate = no
latency = 3

[L2]
size = 1M
assoc = 16
policy = srrip

[LLC]
size = 8M
assoc = full

[memory]
latency = 250
)";
    const SimConfig c = parse_config(text);
    REQUIRE(c.levels.size() == 3);
    CHECK_EQ(c.seed, 7u);
    CHECK(c.include_ifetch);
    CHECK_EQ(c.memory_latency, 250u);
    CHECK_EQ(c.levels[0].policy, PolicyKind::PLRU);
    CHECK_EQ(c.levels[0].write_policy, WritePolicy::WriteThrough);
    CHECK_EQ(c.levels[0].alloc_policy, AllocPolicy::NoWriteAllocate);
    CHECK_EQ(c.levels[0].hit_latency, 3u);
    CHECK_EQ(c.levels[1].name, std::string("L2"));
    CHECK_EQ(c.levels[1].size_bytes, 1u << 20);
    CHECK_EQ(c.levels[1].policy, PolicyKind::SRRIP);
    CHECK_EQ(c.levels[1].hit_latency, default_latency(1));  // default per level
    CHECK_EQ(c.levels[2].name, std::string("LLC"));
    CHECK_EQ(c.levels[2].assoc, 0u);
    CHECK_EQ(c.levels[2].ways(), (8u << 20) / 64u);
    validate(c);
}

TEST(config_errors_carry_line_numbers) {
    CHECK_THROWS_WITH(parse_config("[L1]\nsize = 32K\nbogus = 3\n", "x.cfg"), "x.cfg:3: unknown key");
    CHECK_THROWS_WITH(parse_config("[L1]\nsize = lots\n", "x.cfg"), "x.cfg:2: invalid size");
    CHECK_THROWS_WITH(parse_config("[L1]\npolicy = clock\n"), "invalid policy");
    CHECK_THROWS_WITH(parse_config("[L1\nsize=1K\n"), "must end with ']'");
    CHECK_THROWS_WITH(parse_config("[L1]\n[l1]\n"), "duplicate level");
    CHECK_THROWS_WITH(parse_config("just text\n"), "expected 'key = value'");
    CHECK_THROWS_WITH(parse_config("# only a comment\n"), "no cache level");
    CHECK_THROWS_WITH(parse_config("[memory]\nsize = 4K\n"), "unknown key");
}

static SimConfig one_level(std::uint64_t size, std::uint64_t block, std::uint64_t assoc,
                           PolicyKind p = PolicyKind::LRU) {
    SimConfig c;
    c.levels[0].size_bytes = size;
    c.levels[0].block_bytes = block;
    c.levels[0].assoc = assoc;
    c.levels[0].policy = p;
    return c;
}

TEST(validation_rejects_impossible_geometry) {
    // This config crashed the original program (0 sets -> log(0)).
    CHECK_THROWS_WITH(validate(one_level(128, 64, 4)), "larger than the 2 lines");
    CHECK_THROWS_WITH(validate(one_level(4096, 48, 1)), "power of two");
    CHECK_THROWS_WITH(validate(one_level(1000, 64, 1)), "not a multiple of the block size");
    CHECK_THROWS_WITH(validate(one_level(64 * 12, 64, 8)), "multiple of block size x associativity");
    CHECK_THROWS_WITH(validate(one_level(64 * 12, 64, 3, PolicyKind::PLRU)), "PLRU needs a power-of-two");
    CHECK_THROWS(validate(one_level(0, 64, 1)));

    SimConfig two = one_level(32768, 64, 8);
    two.levels.push_back(two.levels[0]);
    two.levels[1].name = "L2";
    two.levels[1].policy = PolicyKind::OPT;
    CHECK_THROWS_WITH(validate(two), "OPT is only supported in the first level");
    two.levels[1].policy = PolicyKind::LRU;
    two.levels[1].block_bytes = 32;
    two.levels[1].size_bytes = 32768;
    CHECK_THROWS_WITH(validate(two), "block size must be >=");
}

TEST(validation_accepts_edge_shapes) {
    validate(one_level(64, 64, 1));                       // a single line
    validate(one_level(4096, 64, 0));                     // fully associative
    validate(one_level(4096, 1, 1));                      // byte-sized blocks
    validate(one_level(64 * 12, 64, 3));                  // 3-way, 4 sets
    validate(one_level(64 * 12, 64, 4, PolicyKind::OPT)); // 3 sets, modulo index
}
