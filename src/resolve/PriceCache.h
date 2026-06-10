#pragma once
#include "DecoderRingApi.h"
#include "resolve/Http.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <chrono>

namespace Decoder {
// Volatile trading-post prices: in-memory only, short TTL, never disk. A warm
// entry within TTL returns immediately; expired/missing kicks a background fetch.
class PriceCache {
public:
    static void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
    void Initialize(HttpFetch fetch);
    void Shutdown();
    bool Get(uint32_t itemId, DecoderPrice& out);            // warm within TTL -> true
    void DrainCompleted(std::vector<uint32_t>& out);          // ids that finished a fetch
private:
    struct Entry { DecoderPrice price; std::chrono::steady_clock::time_point at; };
    void Run();
    HttpFetch m_Fetch;
    std::unordered_map<uint32_t, Entry> m_Items;
    std::unordered_set<uint32_t> m_Pending;
    std::vector<uint32_t> m_Queue, m_Completed;
    std::mutex m_Mtx; std::condition_variable m_CV;
    std::atomic<bool> m_Stop{false}; std::thread m_Worker;
    int m_TtlSec = 300;
};
}
