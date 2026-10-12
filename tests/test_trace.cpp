#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cachesim/config.hpp"
#include "cachesim/policy.hpp"
#include "cachesim/trace.hpp"
#include "test_framework.hpp"

using namespace cachesim;

TEST(trace_original_bare_hex_format) {
    const Trace t = parse_trace("0x018ADB20\n0x018ADB28\n01A5DB58\n");
    REQUIRE(t.accesses.size() == 3);
    CHECK_EQ(t.accesses[0].addr, 0x018ADB20u);
    CHECK_EQ(t.accesses[1].addr, 0x018ADB28u);
    CHECK_EQ(t.accesses[2].addr, 0x01A5DB58u);
    CHECK(t.accesses[0].op == Op::Read);
    CHECK_EQ(t.accesses[0].size, 1u);
    CHECK_EQ(t.reads, 3u);
    CHECK_EQ(t.address_bits(), 32u);
}

TEST(trace_blank_lines_do_not_stop_reading) {
    // The original stopped at the first short line and silently dropped the rest.
    const Trace t = parse_trace("0x10\n\n   \n0x20\r\n\n0x30");
    REQUIRE(t.accesses.size() == 3);
    CHECK_EQ(t.accesses[2].addr, 0x30u);
    CHECK_EQ(t.text_lines, 6u);
}

TEST(trace_read_write_format) {
    const Trace t = parse_trace("R 0x1000 8\nW 0x2000,4\nr 3000\nw 0X4000 0x10\n");
    REQUIRE(t.accesses.size() == 4);
    CHECK(t.accesses[0].op == Op::Read);
    CHECK_EQ(t.accesses[0].size, 8u);
    CHECK(t.accesses[1].op == Op::Write);
    CHECK_EQ(t.accesses[1].size, 4u);
    CHECK_EQ(t.accesses[2].addr, 0x3000u);
    CHECK_EQ(t.accesses[3].addr, 0x4000u);
    CHECK_EQ(t.accesses[3].size, 16u);
    CHECK_EQ(t.writes, 2u);
}

TEST(trace_valgrind_lackey_format) {
    const char* text =
        "==12345== Lackey, an example Valgrind tool\n"
        "--12345-- some banner\n"
        "I  0400d7d4,8\n"
        " S 7ff000398,8\n"
        " L 04f6b868,8\n"
        " M 0421c7f0,4\n";
    const Trace t = parse_trace(text);
    REQUIRE(t.accesses.size() == 5);  // M expands to load + store
    CHECK(t.accesses[0].op == Op::InstrFetch);
    CHECK(t.accesses[1].op == Op::Write);
    CHECK_EQ(t.accesses[1].addr, 0x7ff000398u);
    CHECK(t.accesses[2].op == Op::Read);
    CHECK(t.accesses[3].op == Op::Read);
    CHECK(t.accesses[4].op == Op::Write);
    CHECK_EQ(t.accesses[4].addr, 0x421c7f0u);
    CHECK_EQ(t.accesses[4].size, 4u);
    CHECK_EQ(t.ifetches, 1u);
    CHECK_EQ(t.address_bits(), 64u);
}

TEST(trace_dinero_format) {
    const Trace t = parse_trace("0 1234\n1 5678\n2 9abc\n");
    REQUIRE(t.accesses.size() == 3);
    CHECK(t.accesses[0].op == Op::Read);
    CHECK_EQ(t.accesses[0].addr, 0x1234u);
    CHECK(t.accesses[1].op == Op::Write);
    CHECK(t.accesses[2].op == Op::InstrFetch);
}

TEST(trace_single_digit_bare_address) {
    const Trace t = parse_trace("0\n7 \nA\n");
    REQUIRE(t.accesses.size() == 3);
    CHECK_EQ(t.accesses[0].addr, 0u);
    CHECK_EQ(t.accesses[1].addr, 7u);
    CHECK_EQ(t.accesses[2].addr, 10u);
}

TEST(trace_comments_are_ignored) {
    const Trace t = parse_trace("# header\n0x10\n// note\n  # indented\n0x20\n");
    CHECK_EQ(t.accesses.size(), 2u);
}

TEST(trace_64_bit_addresses) {
    const Trace t = parse_trace("R 0xffffffffffffffc0 64\n");
    REQUIRE(t.accesses.size() == 1);
    CHECK_EQ(t.accesses[0].addr, 0xffffffffffffffc0ULL);
    CHECK_EQ(t.max_addr, UINT64_MAX);
}

TEST(trace_errors_report_the_line) {
    CHECK_THROWS_WITH(parse_trace("0x10\nX 0x20\n", "t.trc"), "t.trc:2: unknown operation 'X'");
    CHECK_THROWS_WITH(parse_trace("0x10\n0x2g0\n", "t.trc"), "t.trc:2: invalid hex address");
    CHECK_THROWS_WITH(parse_trace("R 0x10 8 9\n", "t.trc"), "too many fields");
    CHECK_THROWS_WITH(parse_trace("R 0x10 abc\n", "t.trc"), "invalid access size");
    CHECK_THROWS_WITH(parse_trace("R 0x10 99999999\n"), "access size too large");
    // Sizes that overflow 64 bits must not wrap around to a small value.
    CHECK_THROWS_WITH(parse_trace("R 0x10 18446744073709551617\n"), "access size too large");  // 2^64 + 1
    CHECK_THROWS_WITH(parse_trace("R 0x10 0x10000000000000001\n"), "access size too large");   // 2^64 + 1
    CHECK_THROWS_WITH(parse_trace("R 0x10 000000000000000000000000008 9\n"), "too many fields");
    CHECK_THROWS_WITH(parse_trace("0x11112222333344445\n"), "invalid hex address");  // 17 digits
    CHECK_THROWS_WITH(parse_trace("R\n"), "invalid hex address");
}

TEST(trace_round_trips_through_writer) {
    const Trace a = parse_trace("R 0x10 8\nW 0x7fff0000 4\nI 0x400 2\n");
    std::ostringstream os;
    write_trace(os, a.accesses);
    const Trace b = parse_trace(os.str());
    REQUIRE(a.accesses.size() == b.accesses.size());
    for (std::size_t i = 0; i < a.accesses.size(); ++i) {
        CHECK_EQ(a.accesses[i].addr, b.accesses[i].addr);
        CHECK_EQ(a.accesses[i].size, b.accesses[i].size);
        CHECK(a.accesses[i].op == b.accesses[i].op);
    }
}

TEST(trace_missing_file_is_an_error) {
    CHECK_THROWS_WITH(load_trace("/definitely/not/here.trc"), "cannot open");
}

namespace {
std::string write_temp(const std::string& name, const std::string& text) {
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    std::ofstream(path, std::ios::binary) << text;
    return path;
}
}  // namespace

TEST(stream_matches_batch_for_any_chunk_size) {
    std::string text = "# header\n==1== banner\n";
    XorShift64 rng(3);
    for (int i = 0; i < 3000; ++i) {
        static const char* ops[] = {"R", "W", " L", " S", " M", "I ", ""};
        const char* op = ops[rng.below(7)];
        char line[64];
        std::snprintf(line, sizeof line, "%s%s%llx%s%s\n", op, *op ? " " : "0x",
                      static_cast<unsigned long long>(rng.below(1u << 30)), *op ? "," : "",
                      *op ? std::to_string(1 + rng.below(16)).c_str() : "");
        text += line;
        if (rng.below(50) == 0) text += "\r\n\n";
    }
    text += "R 0x1234 8";  // no newline at the end
    const std::string path = write_temp("cachesim_stream_test.trc", text);
    const Trace batch = parse_trace(text, path);
    for (std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{64}, std::size_t{4096}, std::size_t{1} << 20}) {
        std::vector<Access> got;
        const Trace s = stream_trace(path, [&](const Access& a) { got.push_back(a); }, chunk);
        REQUIRE(got.size() == batch.accesses.size());
        bool same = true;
        for (std::size_t i = 0; i < got.size(); ++i) {
            same = same && got[i].addr == batch.accesses[i].addr && got[i].size == batch.accesses[i].size &&
                   got[i].op == batch.accesses[i].op;
        }
        CHECK(same);
        CHECK_EQ(s.records, batch.records);
        CHECK_EQ(s.reads, batch.reads);
        CHECK_EQ(s.writes, batch.writes);
        CHECK_EQ(s.max_addr, batch.max_addr);
        CHECK_EQ(s.text_lines, batch.text_lines);
        CHECK(s.accesses.empty());
    }
    std::remove(path.c_str());
}

TEST(stream_reports_errors_with_global_line_numbers) {
    std::string text;
    for (int i = 0; i < 500; ++i) text += "R 0x10 4\n";
    text += "R 0xZZ 4\n";
    const std::string path = write_temp("cachesim_stream_err.trc", text);
    CHECK_THROWS_WITH(stream_trace(path, [](const Access&) {}, 100), ":501: invalid hex address");
    std::remove(path.c_str());
}
