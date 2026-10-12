#include "cachesim/types.hpp"

#include <cctype>
#include <string>

namespace cachesim {

std::string_view to_string(Op op) noexcept {
    switch (op) {
        case Op::Read: return "R";
        case Op::Write: return "W";
        case Op::InstrFetch: return "I";
    }
    return "?";
}

std::string_view to_string(PolicyKind p) noexcept {
    switch (p) {
        case PolicyKind::LRU: return "LRU";
        case PolicyKind::FIFO: return "FIFO";
        case PolicyKind::Random: return "RANDOM";
        case PolicyKind::PLRU: return "PLRU";
        case PolicyKind::SRRIP: return "SRRIP";
        case PolicyKind::OPT: return "OPT";
    }
    return "?";
}

std::string_view to_string(WritePolicy w) noexcept {
    return w == WritePolicy::WriteBack ? "write-back" : "write-through";
}

std::string_view to_string(AllocPolicy a) noexcept {
    return a == AllocPolicy::WriteAllocate ? "write-allocate" : "no-write-allocate";
}

std::string_view to_string(AccessKind k) noexcept {
    switch (k) {
        case AccessKind::Demand: return "demand";
        case AccessKind::Fill: return "fill";
        case AccessKind::Writeback: return "writeback";
        case AccessKind::WriteThrough: return "write-through";
    }
    return "?";
}

std::string_view to_string(MissType m) noexcept {
    switch (m) {
        case MissType::None: return "-";
        case MissType::Compulsory: return "compulsory";
        case MissType::Capacity: return "capacity";
        case MissType::Conflict: return "conflict";
    }
    return "?";
}

std::optional<PolicyKind> parse_policy(std::string_view s) noexcept {
    std::string k;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (k == "lru") return PolicyKind::LRU;
    if (k == "fifo") return PolicyKind::FIFO;
    if (k == "random" || k == "rand" || k == "rnd") return PolicyKind::Random;
    if (k == "plru" || k == "treeplru" || k == "pseudolru") return PolicyKind::PLRU;
    if (k == "srrip" || k == "rrip") return PolicyKind::SRRIP;
    if (k == "opt" || k == "belady" || k == "min" || k == "optimal") return PolicyKind::OPT;
    return std::nullopt;
}

}  // namespace cachesim
