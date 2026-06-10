#include "resolve/PriceCache.h"
#include "nlohmann/json.hpp"

namespace Decoder {
namespace {
bool ParsePrice(const std::vector<char>& body, DecoderPrice& out) {
    try {
        auto j = nlohmann::json::parse(body.begin(), body.end());
        out.buy = (j.contains("buys") && j["buys"].contains("unit_price")) ? j["buys"]["unit_price"].get<int>() : -1;
        out.sell = (j.contains("sells") && j["sells"].contains("unit_price")) ? j["sells"]["unit_price"].get<int>() : -1;
        return true;
    } catch (...) { return false; }
}
}

void PriceCache::Initialize(HttpFetch fetch) {
    m_Fetch = std::move(fetch); m_Stop = false;
    m_Worker = std::thread([this]{ Run(); });
}
void PriceCache::Shutdown() { m_Stop = true; m_CV.notify_all(); if (m_Worker.joinable()) m_Worker.join(); }

bool PriceCache::Get(uint32_t itemId, DecoderPrice& out) {
    if (itemId == 0) return false;
    std::lock_guard<std::mutex> lk(m_Mtx);
    auto it = m_Items.find(itemId);
    if (it != m_Items.end()) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - it->second.at).count();
        if (age < m_TtlSec) { out = it->second.price; return true; }   // fresh
    }
    if (m_Pending.count(itemId)) return false;
    m_Pending.insert(itemId); m_Queue.push_back(itemId); m_CV.notify_one();
    return false;
}

void PriceCache::DrainCompleted(std::vector<uint32_t>& out) {
    std::lock_guard<std::mutex> lk(m_Mtx);
    out.insert(out.end(), m_Completed.begin(), m_Completed.end());
    m_Completed.clear();
}

void PriceCache::Run() {
    for (;;) {
        uint32_t id = 0;
        { std::unique_lock<std::mutex> lk(m_Mtx);
          m_CV.wait(lk, [this]{ return m_Stop.load() || !m_Queue.empty(); });
          if (m_Stop.load()) return;
          id = m_Queue.back(); m_Queue.pop_back(); }
        std::vector<char> body; DecoderPrice p{-1,-1};
        bool ok = m_Fetch && m_Fetch("https://api.guildwars2.com/v2/commerce/prices/" + std::to_string(id), body)
                  && ParsePrice(body, p);
        std::lock_guard<std::mutex> lk(m_Mtx);
        m_Pending.erase(id);
        if (ok) m_Items[id] = Entry{ p, std::chrono::steady_clock::now() };   // memory only
        m_Completed.push_back(id);
    }
}
}
