#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/types.h>

struct GroundSignalSnapshot {
    float signal_a = 0.0f;
    float signal_b = 0.0f;
    bool valid = false;
    std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::time_point::min();
};

struct PacketRateSnapshot {
    float primary_mbps = 0.0f;
    float secondary_mbps = 0.0f;
    bool valid = false;
    std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::time_point::min();

    // ---- STA 模式附加信息（wfb 模式下保持默认值）----
    // STA 模式下主码率由 rx_bytes 增量算出，是真实应用层吞吐；
    // phy_mbps 是 iw 报的协商速率（含空口开销，通常远高于码率）。
    bool sta_mode = false;
    float phy_mbps = 0.0f;
    int freq_mhz = 0;
    int signal_dbm = 0;
};

class SignalMonitor {
public:
    SignalMonitor();
    ~SignalMonitor() = default;

    bool Poll();
    GroundSignalSnapshot Latest() const;
    PacketRateSnapshot LatestRate() const;

private:
    struct RateState {
        uint64_t bytes = 0;
        std::chrono::steady_clock::time_point ts{};
        bool valid = false;
    };

    bool UpdateSnapshot();
    bool UpdateSnapshotFromStaLink();
    void ProcessEntry(pid_t pid, const std::string &message,
                      std::map<uint64_t, float> &antenna_data,
                      bool &saw_pkt,
                      uint64_t &video_bytes,
                      uint64_t &first_ts,
                      uint64_t &last_ts);
    static std::vector<std::string> SplitString(const std::string &line, char delim);
    static std::string TrimCopy(const std::string &text);
    static bool ReadLinkConfig(bool &sta_mode, std::string &wlan_name);
    static std::optional<float> ParseSignalDbm(const std::string &line);
    static std::optional<float> ParseBitrateMbps(const std::string &line);

    mutable std::mutex mutex_;
    GroundSignalSnapshot latest_;
    PacketRateSnapshot latest_rate_;
    std::unordered_map<std::string, RateState> rate_states_;
};
