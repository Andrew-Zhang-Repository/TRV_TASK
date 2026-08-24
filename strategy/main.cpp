#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <random>
#include <mutex>
#include <atomic>
#include <chrono>
#include <map>
#include <nats/nats.h>
#include <cstdlib>
#include <cstring>
#include <iomanip>


std::string NATS_URL;
std::string SENDER;

// All 3 sources for trading no magic strings
std::vector<std::string> FEEDS = {"AAH6", "AAM6", "AAU6"};

// Quoter Params
const int SPREAD_TICKS = 3;       // How far from mid to quote
const int QUOTE_VOL = 5;          // Size of our quotes
const int MAX_POS = 100;          // Position limit per instrument
const double SKEW_FACTOR = 0.1;   // How much to skew quotes based on position
const int REQUOTE_MS = 100;       // Rate Limit: Min milliseconds between requotes

// --- State Management ---
struct InstrumentState {
    int position = 0;
    int best_bid = 0;
    int best_ask = 0;
    std::string active_bid_oid = "";
    std::string active_ask_oid = "";
    int active_bid_px = 0;
    int active_ask_px = 0;
    std::chrono::time_point<std::chrono::steady_clock> last_requote_time = std::chrono::steady_clock::now() - std::chrono::hours(1);
};

std::map<std::string, InstrumentState> instruments;
std::mutex state_mutex;
std::atomic<int> current_oid(0); // Thread-safe order ID counter

// --- Helpers ---
std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

// Generates pseudo random 8 len char id
std::string generate_oid() {
    char buf[9];
    snprintf(buf, sizeof(buf), "%08d", ++current_oid);
    return std::string(buf);
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

    // 2. Get mid point between bid and ask
    double mid = (inst.best_bid + inst.best_ask) / 2.0;
    
    // Skew based on our position in THIS specific instrument
    double skew = inst.position * SKEW_FACTOR; 
    
    int target_bid = static_cast<int>(mid - SPREAD_TICKS - skew);
    int target_ask = static_cast<int>(mid + SPREAD_TICKS - skew);

    bool need_requote = false;
    
    // Check if we need to update bid/ask prices
    if (inst.active_bid_px != target_bid && inst.position < MAX_POS) need_requote = true;
    if (inst.active_ask_px != target_ask && inst.position > -MAX_POS) need_requote = true;
    
    // Requote
    if (inst.active_bid_oid.empty() && inst.position < MAX_POS) need_requote = true;
    if (inst.active_ask_oid.empty() && inst.position > -MAX_POS) need_requote = true;

    if (!need_requote) return;

    std::string req_subject = "ex.req." + SENDER;

    // Cancel old quotes (if they are fully filled, exchange simply replies with Y 0 or N 203, which is safe)
    if (!inst.active_bid_oid.empty()) {
        std::string cancel_msg = SENDER + " C " + feed + " " + inst.active_bid_oid;
        natsConnection_PublishString(nc, req_subject.c_str(), cancel_msg.c_str());
    }
    if (!inst.active_ask_oid.empty()) {
        std::string cancel_msg = SENDER + " C " + feed + " " + inst.active_ask_oid;
        natsConnection_PublishString(nc, req_subject.c_str(), cancel_msg.c_str());
    }

    // Place new quotes
    if (inst.position < MAX_POS) {
        inst.active_bid_oid = generate_oid();
        inst.active_bid_px = target_bid;
        std::string add_msg = SENDER + " A " + feed + " " + inst.active_bid_oid + " B " + std::to_string(QUOTE_VOL) + " " + std::to_string(inst.active_bid_px) + " L";
        natsConnection_PublishString(nc, req_subject.c_str(), add_msg.c_str());
    } else {
        inst.active_bid_oid = ""; inst.active_bid_px = 0;
    }

    if (inst.position > -MAX_POS) {
        inst.active_ask_oid = generate_oid();
        inst.active_ask_px = target_ask;
        std::string add_msg = SENDER + " A " + feed + " " + inst.active_ask_oid + " S " + std::to_string(QUOTE_VOL) + " " + std::to_string(inst.active_ask_px) + " L";
        natsConnection_PublishString(nc, req_subject.c_str(), add_msg.c_str());
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

    auto parts = split(data, ' ');
    if (parts.size() < 6) return;
    
    std::string feed = parts[1]; // Get feed from payload: ts FEED bid ...

    {
        std::lock_guard<std::mutex> lock(state_mutex);
        instruments[feed].best_bid = (parts[2] == "-") ? 0 : std::stoi(parts[2]);
        instruments[feed].best_ask = (parts[4] == "-") ? 0 : std::stoi(parts[4]);
    }
    
    requote(nc, feed);
}

// Handle Market Data (Fills) on our specific feed and for logging purposes 
void on_md(natsConnection *nc, natsSubscription *sub, natsMsg *msg, void *closure) {
    std::string data(natsMsg_GetData(msg), natsMsg_GetDataLength(msg));
    std::string subj(natsMsg_GetSubject(msg));
    natsMsg_Destroy(msg);

    // Extract feed from subject: ex.md.AAH6.QUOTE001 and from other bbos with other instruments
    auto subj_parts = split(subj, '.');
    if (subj_parts.size() < 4) return;
    std::string feed = subj_parts[2];

    auto parts = split(data, ' ');
    if (parts.size() < 2) return;

    if (parts[1] == "E") {
        // Format: <ts> E <incoming:17> <resting:17> <volume> <price> <matchid> <B|S>
        if (parts.size() < 8) return;
        
        int fill_vol = std::stoi(parts[4]);
        std::string aggressor_side = parts[7];

        std::lock_guard<std::mutex> lock(state_mutex);
        // We only send 'L' resting orders.
        // If aggressor is 'B', someone bought from us, meaning we sold.
        // If aggressor is 'S', someone sold to us, meaning we bought.
        if (aggressor_side == "B") {
            instruments[feed].position -= fill_vol;
            std::cout << "[Quoter] " << feed << " Filled SELL " << fill_vol << " @ " << parts[5] << " | Pos: " << instruments[feed].position << std::endl;
        } else if (aggressor_side == "S") {
            instruments[feed].position += fill_vol;
            std::cout << "[Quoter] " << feed << " Filled BUY " << fill_vol << " @ " << parts[5] << " | Pos: " << instruments[feed].position << std::endl;
        }
        
        // Force an immediate requote upon fill by resetting the rate limit timer
        instruments[feed].last_requote_time = std::chrono::steady_clock::now() - std::chrono::hours(1);
    }
    
    // Re-evaluate quotes for this feed
    requote(nc, feed);
}

int main(int argc, char** argv) {
    // 1. Read Constraints from Env showing ip and ids because it was appropriate to do so in taker instead of init a local env file
    NATS_URL = get_env("NATS_URL", "nats://127.0.0.1:4222");
    SENDER = get_env("SENDER", "QUOTE001");
    if (SENDER.length() > 8) SENDER = SENDER.substr(0, 8);
    while (SENDER.length() < 8) SENDER += "X";

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
        instruments[feed] = InstrumentState();

        // Enable STP
        std::string req_subject = "ex.req." + SENDER;
        std::string stp_msg = SENDER + " Q " + feed;
        natsConnection_PublishString(nc, req_subject.c_str(), stp_msg.c_str());
        
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
