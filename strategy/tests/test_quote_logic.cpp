// Unit tests for strategy/quote_logic.h — no NATS, no network.
// Build: g++ -std=c++17 -I.. tests/test_quote_logic.cpp -o quote_logic_tests
// Or via CMake: cmake -DQUOTE_BUILD_TESTS=ON .. && make quote_logic_tests

#include "../quote_logic.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failed = 0;
const char* g_suite = "";

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failed;                                                      \
            std::cout << "FAIL [" << g_suite << "] line " << __LINE__        \
                      << ": " << #cond << std::endl;                         \
        }                                                                    \
    } while (0)

void test_split() {
    g_suite = "split";
    auto parts = quote::split("1000 AAH6 598 20 602 15", ' ');
    CHECK(parts.size() == 6);
    CHECK(parts[0] == "1000");
    CHECK(parts[1] == "AAH6");
    CHECK(parts[5] == "15");
    CHECK(quote::split("", ' ').empty());
    CHECK(quote::split("ex.md.AAH6.QUOTE001", '.').size() == 4);
}

void test_normalize_sender() {
    g_suite = "normalize_sender";
    CHECK(quote::normalize_sender("QUOTE001") == "QUOTE001");
    CHECK(quote::normalize_sender("QUOTE00199") == "QUOTE001");
    CHECK(quote::normalize_sender("Q1") == "Q1XXXXXX");
    CHECK(quote::normalize_sender("") == "XXXXXXXX");
}

void test_format_oid() {
    g_suite = "format_oid";
    CHECK(quote::format_oid(0) == "00000000");
    CHECK(quote::format_oid(1) == "00000001");
    CHECK(quote::format_oid(42) == "00000042");
    CHECK(quote::format_oid(12345678) == "12345678");
    CHECK(quote::format_oid(99999999).size() == 8);
}

void test_compute_targets() {
    g_suite = "compute_targets";
    quote::QuoteParams p;

    auto t = quote::compute_targets(598, 602, 0, p);  // mid 600, no skew
    CHECK(t.bid == 597);
    CHECK(t.ask == 603);

    t = quote::compute_targets(599, 602, 0, p);       // mid 600.5, truncation
    CHECK(t.bid == 597);
    CHECK(t.ask == 603);

    t = quote::compute_targets(598, 602, 10, p);      // long -> skew shifts down
    CHECK(t.bid == 596);
    CHECK(t.ask == 602);

    t = quote::compute_targets(598, 602, -20, p);     // short -> skew shifts up
    CHECK(t.bid == 599);
    CHECK(t.ask == 605);

    t = quote::compute_targets(598, 602, 5, p);       // skew 0.5, truncation
    CHECK(t.bid == 596);
    CHECK(t.ask == 602);

    quote::QuoteParams custom;
    custom.spread_ticks = 1;
    custom.skew_factor = 0.0;
    t = quote::compute_targets(100, 104, 7, custom);  // mid 102, no skew
    CHECK(t.bid == 101);
    CHECK(t.ask == 103);
}

void test_should_requote() {
    g_suite = "should_requote";
    quote::QuoteParams p;
    quote::QuoteTargets targets{597, 603};

    quote::InstrumentState fresh;
    CHECK(quote::should_requote(fresh, targets, p));  // nothing quoted yet

    quote::InstrumentState matched;
    matched.active_bid_oid = "00000001";
    matched.active_ask_oid = "00000002";
    matched.active_bid_px = 597;
    matched.active_ask_px = 603;
    CHECK(!quote::should_requote(matched, targets, p));  // quotes on target

    CHECK(quote::should_requote(matched, quote::QuoteTargets{598, 603}, p));  // bid moved
    CHECK(quote::should_requote(matched, quote::QuoteTargets{597, 604}, p));  // ask moved

    auto at_max_long = matched;
    at_max_long.position = p.max_pos;
    CHECK(!quote::should_requote(at_max_long, quote::QuoteTargets{598, 603}, p));

    auto at_max_short = matched;
    at_max_short.position = -p.max_pos;
    CHECK(!quote::should_requote(at_max_short, quote::QuoteTargets{597, 604}, p));

    auto lost_bid = matched;
    lost_bid.active_bid_oid = "";
    CHECK(quote::should_requote(lost_bid, targets, p));  // replace lost quote

    auto lost_bid_at_max = lost_bid;
    lost_bid_at_max.position = p.max_pos;
    CHECK(!quote::should_requote(lost_bid_at_max, targets, p));  // but not beyond max

    auto lost_ask = matched;
    lost_ask.active_ask_oid = "";
    CHECK(quote::should_requote(lost_ask, targets, p));

    auto lost_ask_at_max = lost_ask;
    lost_ask_at_max.position = -p.max_pos;
    CHECK(!quote::should_requote(lost_ask_at_max, targets, p));
}

void test_fill_delta() {
    g_suite = "fill_delta";
    CHECK(quote::fill_delta("B", 5) == -5);   // aggressor bought -> we sold
    CHECK(quote::fill_delta("S", 5) == 5);    // aggressor sold -> we bought
    CHECK(quote::fill_delta("X", 5) == 0);    // unknown side -> no change
    CHECK(quote::fill_delta("B", 0) == 0);
}

void test_parse_bbo() {
    g_suite = "parse_bbo";

    auto u = quote::parse_bbo(quote::split("1000 AAH6 598 20 602 15", ' '));
    CHECK(u.has_value());
    CHECK(u->feed == "AAH6");
    CHECK(u->bid == 598);
    CHECK(u->ask == 602);

    auto empty_sides = quote::parse_bbo(quote::split("1000 AAH6 - 0 - 0", ' '));
    CHECK(empty_sides.has_value());
    CHECK(empty_sides->bid == 0);
    CHECK(empty_sides->ask == 0);

    CHECK(!quote::parse_bbo(quote::split("1000 AAH6 598", ' ')).has_value());
    CHECK(!quote::parse_bbo(std::vector<std::string>{}).has_value());
}

void test_feed_from_md_subject() {
    g_suite = "feed_from_md_subject";
    auto feed = quote::feed_from_md_subject("ex.md.AAH6.QUOTE001");
    CHECK(feed.has_value());
    CHECK(*feed == "AAH6");
    CHECK(!quote::feed_from_md_subject("ex.md.AAH6").has_value());
    CHECK(!quote::feed_from_md_subject("").has_value());
}

void test_parse_execution() {
    g_suite = "parse_execution";

    auto e = quote::parse_execution(
        quote::split("1000 E QUOTE001:00000001 MOVER001:00000002 5 600 0000000001 B", ' '));
    CHECK(e.has_value());
    CHECK(e->vol == 5);
    CHECK(e->aggressor == "B");

    auto sell = quote::parse_execution(
        quote::split("1000 E MOVER001:00000001 QUOTE001:00000002 3 601 0000000002 S", ' '));
    CHECK(sell.has_value());
    CHECK(sell->vol == 3);
    CHECK(sell->aggressor == "S");

    CHECK(!quote::parse_execution(
               quote::split("1000 E QUOTE001:00000001 MOVER001:00000002 5", ' '))
               .has_value());
    CHECK(!quote::parse_execution(std::vector<std::string>{}).has_value());
}

void test_order_messages() {
    g_suite = "order_messages";
    CHECK(quote::make_add_order("QUOTE001", "AAH6", "00000042", 'B', 5, 597, 'L') ==
          "QUOTE001 A AAH6 00000042 B 5 597 L");
    CHECK(quote::make_add_order("QUOTE001", "AAM6", "00000043", 'S', 5, 621, 'L') ==
          "QUOTE001 A AAM6 00000043 S 5 621 L");
    CHECK(quote::make_cancel_order("QUOTE001", "AAH6", "00000042") ==
          "QUOTE001 C AAH6 00000042");
    CHECK(quote::make_stp_request("QUOTE001", "AAH6") == "QUOTE001 Q AAH6");
    CHECK(quote::request_subject("QUOTE001") == "ex.req.QUOTE001");
}

}  // namespace

int main() {
    test_split();
    test_normalize_sender();
    test_format_oid();
    test_compute_targets();
    test_should_requote();
    test_fill_delta();
    test_parse_bbo();
    test_feed_from_md_subject();
    test_parse_execution();
    test_order_messages();

    std::cout << g_checks << " checks, " << g_failed << " failed" << std::endl;
    if (g_failed == 0) std::cout << "ALL QUOTER LOGIC TESTS PASSED" << std::endl;
    return g_failed == 0 ? 0 : 1;
}
