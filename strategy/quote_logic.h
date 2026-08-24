#pragma once

// Pure quoting logic for the market-making seat, free of NATS and global
// state so it can be unit tested in isolation. main.cpp wires these to the
// exchange; tests/test_quote_logic.cpp exercises them directly.

#include <chrono>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace quote {

struct QuoteParams {
    int spread_ticks = 3;
    double skew_factor = 0.1;
    int max_pos = 100;
    int quote_vol = 5;
};

struct InstrumentState {
    int position = 0;
    int best_bid = 0;
    int best_ask = 0;
    std::string active_bid_oid = "";
    std::string active_ask_oid = "";
    int active_bid_px = 0;
    int active_ask_px = 0;
    std::chrono::time_point<std::chrono::steady_clock> last_requote_time =
        std::chrono::steady_clock::now() - std::chrono::hours(1);
};

struct QuoteTargets {
    int bid = 0;
    int ask = 0;
};

struct BboUpdate {
    std::string feed;
    int bid = 0;
    int ask = 0;
};

struct Execution {
    int vol = 0;
    std::string aggressor;
};

inline std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

inline std::string normalize_sender(std::string sender) {
    if (sender.length() > 8) sender = sender.substr(0, 8);
    while (sender.length() < 8) sender += "X";
    return sender;
}

inline std::string format_oid(int oid) {
    char buf[9];
    snprintf(buf, sizeof(buf), "%08d", oid);
    return std::string(buf);
}

inline QuoteTargets compute_targets(int best_bid, int best_ask, int position,
                                    const QuoteParams& params) {
    double mid = (best_bid + best_ask) / 2.0;
    double skew = position * params.skew_factor;
    return {static_cast<int>(mid - params.spread_ticks - skew),
            static_cast<int>(mid + params.spread_ticks - skew)};
}

inline bool should_requote(const InstrumentState& inst, const QuoteTargets& targets,
                           const QuoteParams& params) {
    if (inst.active_bid_px != targets.bid && inst.position < params.max_pos) return true;
    if (inst.active_ask_px != targets.ask && inst.position > -params.max_pos) return true;
    if (inst.active_bid_oid.empty() && inst.position < params.max_pos) return true;
    if (inst.active_ask_oid.empty() && inst.position > -params.max_pos) return true;
    return false;
}

// We only rest passive orders: an incoming buyer lifts our ask (we sold),
// an incoming seller hits our bid (we bought).
inline int fill_delta(const std::string& aggressor_side, int vol) {
    if (aggressor_side == "B") return -vol;
    if (aggressor_side == "S") return vol;
    return 0;
}

inline std::optional<BboUpdate> parse_bbo(const std::vector<std::string>& parts) {
    if (parts.size() < 6) return std::nullopt;
    BboUpdate u;
    u.feed = parts[1];
    u.bid = (parts[2] == "-") ? 0 : std::stoi(parts[2]);
    u.ask = (parts[4] == "-") ? 0 : std::stoi(parts[4]);
    return u;
}

inline std::optional<std::string> feed_from_md_subject(const std::string& subject) {
    auto subj_parts = split(subject, '.');
    if (subj_parts.size() < 4) return std::nullopt;
    return subj_parts[2];
}

// Expects the payload of an E (execution) message; returns nullopt if it is
// too short to hold all execution fields.
inline std::optional<Execution> parse_execution(const std::vector<std::string>& parts) {
    if (parts.size() < 8) return std::nullopt;
    Execution e;
    e.vol = std::stoi(parts[4]);
    e.aggressor = parts[7];
    return e;
}

inline std::string request_subject(const std::string& sender) {
    return "ex.req." + sender;
}

inline std::string make_add_order(const std::string& sender, const std::string& feed,
                                  const std::string& oid, char side, int volume, int price,
                                  char type) {
    return sender + " A " + feed + " " + oid + " " + side + " " + std::to_string(volume) +
           " " + std::to_string(price) + " " + type;
}

inline std::string make_cancel_order(const std::string& sender, const std::string& feed,
                                     const std::string& oid) {
    return sender + " C " + feed + " " + oid;
}

inline std::string make_stp_request(const std::string& sender, const std::string& feed) {
    return sender + " Q " + feed;
}

}  // namespace quote
