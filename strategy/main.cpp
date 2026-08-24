#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <nats/nats.h>

#include "quote_logic.h"

std::string NATS_URL;
std::string SENDER;

// All 3 sources for trading no magic strings
std::vector<std::string> FEEDS = {"AAH6", "AAM6", "AAU6"};

quote::QuoteParams PARAMS;      // defaults: spread 3, skew 0.1, max pos 100, vol 5
const int REQUOTE_MS = 100;     // Rate Limit: Min milliseconds between requotes

// --- State Management ---
std::map<std::string, quote::InstrumentState> instruments;
std::mutex state_mutex;
std::atomic<int> current_oid(0); // Thread-safe order ID counter

// --- Helpers ---
std::string generate_oid() {
    return quote::format_oid(++current_oid);
}

// Get env var with fallback
std::string get_env(const char* name, const std::string& fallback) {
    const char* val = std::getenv(name);
    return val ? std::string(val) : fallback;
}

// --- Core Quoting Logic ---
void requote(natsConnection* nc, const std::string& feed) {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto& inst = instruments[feed];

    // Rate Limit Protection per instrument
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - inst.last_requote_time).count() < REQUOTE_MS) {
        return;
    }

    if (inst.best_bid == 0 || inst.best_ask == 0) return; // No market data yet

    quote::QuoteTargets targets =
        quote::compute_targets(inst.best_bid, inst.best_ask, inst.position, PARAMS);

    if (!quote::should_requote(inst, targets, PARAMS)) return;

    std::string req_subject = quote::request_subject(SENDER);

    // Cancel old quotes (if they are fully filled, exchange simply replies with Y 0 or N 203, which is safe)
    if (!inst.active_bid_oid.empty()) {
        natsConnection_PublishString(
            nc, req_subject.c_str(),
            quote::make_cancel_order(SENDER, feed, inst.active_bid_oid).c_str());
    }
    if (!inst.active_ask_oid.empty()) {
        natsConnection_PublishString(
            nc, req_subject.c_str(),
            quote::make_cancel_order(SENDER, feed, inst.active_ask_oid).c_str());
    }

    // Place new quotes
    if (inst.position < PARAMS.max_pos) {
        inst.active_bid_oid = generate_oid();
        inst.active_bid_px = targets.bid;
        natsConnection_PublishString(
            nc, req_subject.c_str(),
            quote::make_add_order(SENDER, feed, inst.active_bid_oid, 'B',
                                  PARAMS.quote_vol, inst.active_bid_px, 'L').c_str());
    } else {
        inst.active_bid_oid = ""; inst.active_bid_px = 0;
    }

    if (inst.position > -PARAMS.max_pos) {
        inst.active_ask_oid = generate_oid();
        inst.active_ask_px = targets.ask;
        natsConnection_PublishString(
            nc, req_subject.c_str(),
            quote::make_add_order(SENDER, feed, inst.active_ask_oid, 'S',
                                  PARAMS.quote_vol, inst.active_ask_px, 'L').c_str());
    } else {
        inst.active_ask_oid = ""; inst.active_ask_px = 0;
    }

    natsConnection_Flush(nc);
    inst.last_requote_time = std::chrono::steady_clock::now();
}

// --- NATS Callbacks ---

// Handle BBO updates
void on_bbo(natsConnection *nc, natsSubscription *sub, natsMsg *msg, void *closure) {
    std::string data(natsMsg_GetData(msg), natsMsg_GetDataLength(msg));
    natsMsg_Destroy(msg);

    auto update = quote::parse_bbo(quote::split(data, ' '));
    if (!update) return;

    {
        std::lock_guard<std::mutex> lock(state_mutex);
        instruments[update->feed].best_bid = update->bid;
        instruments[update->feed].best_ask = update->ask;
    }

    requote(nc, update->feed);
}

// Handle Market Data (Fills) on our specific feed and for logging purposes
void on_md(natsConnection *nc, natsSubscription *sub, natsMsg *msg, void *closure) {
    std::string data(natsMsg_GetData(msg), natsMsg_GetDataLength(msg));
    std::string subj(natsMsg_GetSubject(msg));
    natsMsg_Destroy(msg);

    // Extract feed from subject: ex.md.AAH6.QUOTE001 and from other bbos with other instruments
    auto feed_opt = quote::feed_from_md_subject(subj);
    if (!feed_opt) return;
    std::string feed = *feed_opt;

    auto parts = quote::split(data, ' ');
    if (parts.size() < 2) return;

    if (parts[1] == "E") {
        // Format: <ts> E <incoming:17> <resting:17> <volume> <price> <matchid> <B|S>
        auto exec = quote::parse_execution(parts);
        if (!exec) return;

        std::lock_guard<std::mutex> lock(state_mutex);
        auto& inst = instruments[feed];
        inst.position += quote::fill_delta(exec->aggressor, exec->vol);
        if (exec->aggressor == "B") {
            std::cout << "[Quoter] " << feed << " Filled SELL " << exec->vol << " @ " << parts[5] << " | Pos: " << inst.position << std::endl;
        } else if (exec->aggressor == "S") {
            std::cout << "[Quoter] " << feed << " Filled BUY " << exec->vol << " @ " << parts[5] << " | Pos: " << inst.position << std::endl;
        }

        // Force an immediate requote upon fill by resetting the rate limit timer
        inst.last_requote_time = std::chrono::steady_clock::now() - std::chrono::hours(1);
    }

    // Re-evaluate quotes for this feed
    requote(nc, feed);
}

int main(int argc, char** argv) {
    // 1. Read Constraints from Env showing ip and ids because it was appropriate to do so in taker instead of init a local env file
    NATS_URL = get_env("NATS_URL", "nats://127.0.0.1:4222");
    SENDER = quote::normalize_sender(get_env("SENDER", "QUOTE001"));

    // 2. Initialize Order IDs randomly
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(1, 10000000);
    current_oid = dist(gen);

    std::cout << "[Quoter] Starting. Sender: " << SENDER << " | OID Start: " << current_oid << std::endl;

    natsConnection *nc = nullptr;
    natsStatus s = natsConnection_ConnectTo(&nc, NATS_URL.c_str());
    if (s != NATS_OK) {
        std::cerr << "Failed to connect to NATS: " << natsStatus_GetText(s) << std::endl;
        return 1;
    }

    // 3. Setup logic for ALL 3 Feeds
    for (const auto& feed : FEEDS) {
        // Initialize state
        instruments[feed] = quote::InstrumentState();

        // Enable STP
        std::string req_subject = quote::request_subject(SENDER);
        natsConnection_PublishString(nc, req_subject.c_str(),
                                     quote::make_stp_request(SENDER, feed).c_str());

        // Subscribe to BBO
        natsSubscription *sub_bbo;
        std::string bbo_subj = "ex.bbo." + feed;
        natsConnection_Subscribe(&sub_bbo, nc, bbo_subj.c_str(), on_bbo, nullptr);

        // Subscribe to our Market Data Fills
        natsSubscription *sub_md;
        std::string md_subj = "ex.md." + feed + "." + SENDER;
        natsConnection_Subscribe(&sub_md, nc, md_subj.c_str(), on_md, nullptr);

        std::cout << "[Quoter] Connected and quoting on: " << feed << std::endl;
    }
    natsConnection_Flush(nc);

    // Keep alive
    while (true) {
        nats_Sleep(1000);
    }

    natsConnection_Destroy(nc);
    return 0;
}
