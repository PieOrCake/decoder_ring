#pragma once
#include "resolve/Http.h"
#include "nlohmann/json.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <chrono>
#include <fstream>
#include <string>
#include <utility>

namespace Decoder {

// Generic async id->Meta resolver: a worker thread fetches via an injected
// HttpFetch, parses with Traits::Parse, never persists empties/failures, applies
// a fail cooldown, and (optionally) disk-caches via Traits::ToJson/FromJson.
//
// Traits must provide:
//   using Meta;                                  // the cached value type
//   static std::string Url(uint32_t id);
//   static bool Parse(const std::vector<char>& body, Meta&);
//   static const char* FileName();               // "" disables disk
//   static nlohmann::json ToJson(const Meta&);
//   static void FromJson(const nlohmann::json&, Meta&);
template <typename Traits>
class AsyncResolver {
public:
    using Meta = typename Traits::Meta;

    static void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

    // dir: directory for the disk cache ("" disables disk). fetch: injected HTTP.
    void Initialize(const std::string& dir, HttpFetch fetch) {
        m_Fetch = std::move(fetch);
        m_Dir = dir;
        if (!m_Dir.empty() && Traits::FileName()[0]) LoadFromDisk();
        m_Stop = false;
        m_Worker = std::thread([this]{ Run(); });
    }

    void Shutdown() {
        m_Stop = true; m_CV.notify_all();
        if (m_Worker.joinable()) m_Worker.join();
        if (m_Dirty.exchange(false)) FlushToDisk();
    }

    void SetFailCooldownSec(int s) { m_FailCooldownSec = s; }

    // Warm -> copies into out, true. Cold/failed-in-cooldown -> kicks a fetch
    // (unless already pending or cooling down) and returns false.
    bool Get(uint32_t id, Meta& out) {
        if (id == 0) return false;
        std::lock_guard<std::mutex> lk(m_Mtx);
        auto it = m_Items.find(id);
        if (it != m_Items.end()) { out = it->second; return true; }
        if (m_Pending.count(id)) return false;
        auto fit = m_Fail.find(id);
        if (fit != m_Fail.end()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - fit->second).count();
            if (age < m_FailCooldownSec) return false;
        }
        m_Pending.insert(id);
        m_Queue.push_back(id);
        m_CV.notify_one();
        return false;
    }

    // Move completed (id, success) pairs out for the service to emit events from.
    void DrainCompleted(std::vector<std::pair<uint32_t,bool>>& out) {
        std::lock_guard<std::mutex> lk(m_Mtx);
        if (m_Completed.empty()) return;
        out.insert(out.end(), m_Completed.begin(), m_Completed.end());
        m_Completed.clear();
    }

    // Throttled disk flush; call from the main-thread pump.
    void Tick() {
        if (!m_Dirty.load() || m_Dir.empty() || !Traits::FileName()[0]) return;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastFlush).count() < 30000) return;
        m_LastFlush = now;
        if (m_Dirty.exchange(false)) FlushToDisk();
    }

private:
    std::string FilePath() const { return m_Dir + "/" + Traits::FileName(); }

    void LoadFromDisk() {
        std::ifstream f(FilePath());
        if (!f) return;
        try {
            nlohmann::json j; f >> j;
            if (j.is_object())
                for (auto it = j.begin(); it != j.end(); ++it) {
                    Meta m; Traits::FromJson(it.value(), m);
                    m_Items[(uint32_t)std::stoul(it.key())] = std::move(m);
                }
        } catch (...) {}
    }

    void FlushToDisk() {
        nlohmann::json j = nlohmann::json::object();
        { std::lock_guard<std::mutex> lk(m_Mtx);
          for (auto& kv : m_Items) j[std::to_string(kv.first)] = Traits::ToJson(kv.second); }
        std::ofstream f(FilePath(), std::ios::trunc);
        if (f) f << j.dump();
    }

    void Run() {
        for (;;) {
            uint32_t id = 0;
            { std::unique_lock<std::mutex> lk(m_Mtx);
              m_CV.wait(lk, [this]{ return m_Stop.load() || !m_Queue.empty(); });
              if (m_Stop.load()) return;
              id = m_Queue.back(); m_Queue.pop_back(); }
            std::vector<char> body; Meta meta;
            bool ok = m_Fetch && m_Fetch(Traits::Url(id), body) && Traits::Parse(body, meta);
            std::lock_guard<std::mutex> lk(m_Mtx);
            m_Pending.erase(id);
            if (ok) { m_Items[id] = std::move(meta); m_Fail.erase(id); m_Dirty = true; }   // never persist empties
            else    { m_Fail[id] = std::chrono::steady_clock::now(); }
            m_Completed.emplace_back(id, ok);
        }
    }

    HttpFetch m_Fetch;
    std::string m_Dir;
    std::unordered_map<uint32_t, Meta> m_Items;
    std::unordered_set<uint32_t> m_Pending;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> m_Fail;
    std::vector<uint32_t> m_Queue;
    std::vector<std::pair<uint32_t,bool>> m_Completed;
    std::mutex m_Mtx;
    std::condition_variable m_CV;
    std::atomic<bool> m_Stop{false};
    std::atomic<bool> m_Dirty{false};
    std::thread m_Worker;
    std::chrono::steady_clock::time_point m_LastFlush{std::chrono::steady_clock::now()};
    std::atomic<int> m_FailCooldownSec{60};   // atomic: SetFailCooldownSec may run off the worker thread
};
}
