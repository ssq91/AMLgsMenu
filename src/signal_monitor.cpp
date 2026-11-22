#include "signal_monitor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

namespace
{
const char *kJournalCmd = "journalctl -u wifibroadcast --since \"5 seconds ago\" --no-pager --output=export";
}

SignalMonitor::SignalMonitor() = default;
bool SignalMonitor::Poll()
{
    return UpdateSnapshot();
}

GroundSignalSnapshot SignalMonitor::Latest() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

PacketRateSnapshot SignalMonitor::LatestRate() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_rate_;
}

bool SignalMonitor::UpdateSnapshot()
{
    bool sta_mode = false;
    std::string wlan_name;
    // sta / ap 两种模式都直接问内核要 link 信息；只有 wfb 模式才去翻 journal。
    if (ReadLinkConfig(sta_mode, wlan_name) && sta_mode)
    {
        return UpdateSnapshotFromStaLink();
    }

    FILE *pipe = popen(kJournalCmd, "r");
    if (!pipe)
    {
        return false;
    }

    std::map<uint64_t, float> antenna_data;
    uint64_t video_bytes = 0;
    uint64_t first_ts = 0;
    uint64_t last_ts = 0;
    std::chrono::steady_clock::time_point log_ts = std::chrono::steady_clock::now();
    char buffer[1024];
    bool saw_pkt = false;

    pid_t current_pid = -1;
    std::string current_msg;
    auto flush_entry = [&]() {
        if (!current_msg.empty())
        {
            ProcessEntry(current_pid, current_msg, antenna_data, saw_pkt, video_bytes, first_ts, last_ts);
        }
        current_pid = -1;
        current_msg.clear();
    };

    while (fgets(buffer, sizeof(buffer), pipe))
    {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        {
            line.pop_back();
        }
        if (line.empty())
        {
            flush_entry();
            continue;
        }

        if (line.rfind("MESSAGE=", 0) == 0)
        {
            current_msg = line.substr(8);
        }
        else if (line.rfind("_PID=", 0) == 0)
        {
            try
            {
                current_pid = static_cast<pid_t>(std::stoi(line.substr(5)));
            }
            catch (const std::exception &)
            {
                current_pid = -1;
            }
        }
    }
    flush_entry();

    pclose(pipe);

    if (!antenna_data.empty())
    {
        GroundSignalSnapshot snapshot;
        size_t idx = 0;
        for (const auto &entry : antenna_data)
        {
            if (idx == 0)
            {
                snapshot.signal_a = entry.second;
            }
            else if (idx == 1)
            {
                snapshot.signal_b = entry.second;
                break;
            }
            ++idx;
        }

        snapshot.valid = true;
        snapshot.timestamp = log_ts;

        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = snapshot;
    }

    if (!saw_pkt)
        return true;

    auto now = std::chrono::steady_clock::now();
    PacketRateSnapshot rate{};
    rate.timestamp = now;
    auto &state = rate_states_["video"];

    double dt = 0.0;
    if (last_ts > first_ts)
    {
        dt = static_cast<double>(last_ts - first_ts) / 1000.0;
    }
    if (dt <= 0.0)
    {
        if (state.valid)
            dt = std::chrono::duration<double>(now - state.ts).count();
    }
    if (dt <= 0.0)
    {
        dt = 1.0;
    }

    rate.primary_mbps = static_cast<float>((static_cast<double>(video_bytes) * 8.0) /
                                           (1024.0 * 1024.0) / dt);
    state.bytes = video_bytes;
    state.ts = now;
    state.valid = true;
    rate.valid = (video_bytes > 0);

    std::lock_guard<std::mutex> lock(mutex_);
    latest_rate_ = rate;

    return true;
}

namespace
{
// 找出真正在承载业务的那个 wlan 接口。
// 配置里的 wlan= 不可信：实测设备 wfb.conf 写 wlan0，但系统里只有 wlan1。
// 优先试配置值，连不上再遍历 /sys/class/net/wlan*，取第一个 "Connected to" 的。
bool InterfaceConnected(const std::string &iface)
{
    FILE *pipe = popen(("iw dev " + iface + " link 2>/dev/null").c_str(), "r");
    if (!pipe)
    {
        return false;
    }
    char buf[512];
    bool connected = false;
    while (fgets(buf, sizeof(buf), pipe))
    {
        if (std::strncmp(buf, "Connected to", 12) == 0)
        {
            connected = true;
            break;
        }
    }
    pclose(pipe);
    return connected;
}

// 有些驱动（本例 rtl88x2cs）在 /proc/net/<driver>/<iface>/ 下暴露了很细的射频统计，
// 其中 rssi_a / rssi_b 是两根天线各自的 RSSI，单位是「dBm + 100」。
// 这比 iw dev link 给出的单一 signal 有用得多：地面站是双天线接收，
// 两个天线的差值直接反映分集效果与遮挡情况。
bool ReadProcInt(const std::string &path, int &out)
{
    std::ifstream in(path);
    if (!in.is_open())
    {
        return false;
    }
    in >> out;
    return !in.fail();
}

// 在 /proc/net/ 下找属于本接口的驱动目录（不同驱动名不同，不能硬编码）。
std::string FindDriverProcDir(const std::string &iface)
{
    const char *known[] = {"rtl88x2cs", "rtl88x2cu", "rtl88x2bs", "rtl88x2bu", "rtl8822cs"};
    for (const char *drv : known)
    {
        const std::string dir = std::string("/proc/net/") + drv + "/" + iface;
        if (std::ifstream(dir + "/rssi_a").good())
        {
            return dir;
        }
    }
    return std::string();
}
} // namespace

// 解析 wfb.conf 的 mode 键。历史上这里读的是 ap_mode 标志，但该标志在切到 STA 后
// 并不会被清掉（实测设备上 mode=sta 而 ap_mode 仍为 1），因此必须按 mode 判断。
bool SignalMonitor::ReadLinkConfig(bool &sta_mode, std::string &wlan_name)
{
    sta_mode = false;
    wlan_name = "wlan0";

    const char *paths[] = {"/storage/digitalfpv/wfb.conf", "/flash/wfb.conf"};
    for (const char *path : paths)
    {
        std::ifstream in(path);
        if (!in.is_open())
        {
            continue;
        }

        std::string mode_value;
        std::string line;
        while (std::getline(in, line))
        {
            line = TrimCopy(line);
            if (line.empty() || line[0] == '#')
            {
                continue;
            }
            auto eq = line.find('=');
            if (eq == std::string::npos)
            {
                continue;
            }
            const std::string key = TrimCopy(line.substr(0, eq));
            const std::string value = TrimCopy(line.substr(eq + 1));
            if (key == "mode")
            {
                mode_value = value;
            }
            else if (key == "wlan" && !value.empty())
            {
                wlan_name = value;
            }
        }

        // 决策：
        //   mode=wfb            -> 走 journal（原 wfb 行为不变）
        //   mode=sta / mode=ap -> 走 iw（AP 端 iw dev link 同样有效）
        //   mode 缺失（老配置）  -> 探测：有 wlan 处于 Connected 就按 STA 处理，
        //                          否则回落到 journal，保证纯 wfb 设备不受影响
        if (!mode_value.empty())
        {
            sta_mode = (mode_value != "wfb" && mode_value != "WFB");
        }
        else
        {
            sta_mode = false;
            if (InterfaceConnected(wlan_name))
            {
                sta_mode = true;
            }
            else
            {
                for (int i = 0; i < 8 && !sta_mode; ++i)
                {
                    const std::string name = "wlan" + std::to_string(i);
                    if (InterfaceConnected(name))
                    {
                        wlan_name = name;
                        sta_mode = true;
                    }
                }
            }
        }
        return true;
    }
    return false;
}


bool SignalMonitor::UpdateSnapshotFromStaLink()
{
    bool sta_mode = false;
    std::string configured;
    if (!ReadLinkConfig(sta_mode, configured) || !sta_mode)
    {
        return false;
    }

    // 1) 选接口
    std::string iface;
    if (InterfaceConnected(configured))
    {
        iface = configured;
    }
    else
    {
        for (int i = 0; i < 8 && iface.empty(); ++i)
        {
            const std::string name = "wlan" + std::to_string(i);
            if (std::ifstream("/sys/class/net/" + name + "/operstate").good() && InterfaceConnected(name))
            {
                iface = name;
            }
        }
    }
    if (iface.empty())
    {
        return false;
    }

    // 2) iw dev <if> link：信号、频率、协商速率
    FILE *pipe = popen(("iw dev " + iface + " link 2>/dev/null").c_str(), "r");
    if (!pipe)
    {
        return false;
    }

    bool connected = false;
    std::optional<float> signal_dbm;
    std::optional<float> phy_mbps;
    int freq_mhz = 0;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe))
    {
        const std::string line = TrimCopy(buffer);
        if (line.rfind("Connected to", 0) == 0)
        {
            connected = true;
            continue;
        }
        if (line.empty() || line == "Not connected.")
        {
            continue;
        }
        if (!signal_dbm.has_value())
        {
            signal_dbm = ParseSignalDbm(line);
        }
        if (line.rfind("freq:", 0) == 0)
        {
            try
            {
                freq_mhz = static_cast<int>(std::stof(TrimCopy(line.substr(5))));
            }
            catch (const std::exception &)
            {
            }
        }
        if (line.rfind("tx bitrate:", 0) == 0 || line.rfind("rx bitrate:", 0) == 0)
        {
            const auto mb = ParseBitrateMbps(line);
            // 取较大的那个作为空口协商速率（rx 通常更大，因为视频是下行）
            if (mb.has_value() && (!phy_mbps.has_value() || *mb > *phy_mbps))
            {
                phy_mbps = mb;
            }
        }
    }
    pclose(pipe);

    if (!connected)
    {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();

    // 3) 真实应用层吞吐：rx_bytes 增量 / 时间差
    // STA 模式下山头到地面的视频流全部走 rx，因此这个值就是视频码率。
    uint64_t rx_bytes = 0;
    bool have_rx = false;
    {
        std::ifstream in("/sys/class/net/" + iface + "/statistics/rx_bytes");
        if (in.is_open())
        {
            in >> rx_bytes;
            have_rx = in.good() || !in.fail();
        }
    }

    // 4) 信号快照：优先用驱动上报的双天线 RSSI（rssi_a / rssi_b，单位 dBm+100）
    {
        int rssi_a_raw = 0;
        int rssi_b_raw = 0;
        bool have_ab = false;

        const std::string proc_dir = FindDriverProcDir(iface);
        if (!proc_dir.empty())
        {
            const bool oka = ReadProcInt(proc_dir + "/rssi_a", rssi_a_raw);
            const bool okb = ReadProcInt(proc_dir + "/rssi_b", rssi_b_raw);
            if (oka && okb && (rssi_a_raw > 0 || rssi_b_raw > 0))
            {
                have_ab = true;
            }
        }

        if (have_ab)
        {
            GroundSignalSnapshot snapshot;
            snapshot.signal_a = static_cast<float>(rssi_a_raw - 100);
            snapshot.signal_b = static_cast<float>(rssi_b_raw - 100);
            snapshot.valid = true;
            snapshot.timestamp = now;
            std::lock_guard<std::mutex> lock(mutex_);
            latest_ = snapshot;
        }
        else if (signal_dbm.has_value())
        {
            GroundSignalSnapshot snapshot;
            snapshot.signal_a = *signal_dbm;
            snapshot.signal_b = *signal_dbm;
            snapshot.valid = true;
            snapshot.timestamp = now;
            std::lock_guard<std::mutex> lock(mutex_);
            latest_ = snapshot;
        }
    }

    // 5) 码率快照
    if (have_rx)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RateState &st = rate_states_["sta:" + iface];
        if (st.valid && rx_bytes >= st.bytes)
        {
            const double dt = std::chrono::duration<double>(now - st.ts).count();
            // 采样窗口太短时噪声很大，至少要 0.4s 才刷新
            if (dt >= 0.4)
            {
                const double mbps = static_cast<double>(rx_bytes - st.bytes) * 8.0 /
                                    (1024.0 * 1024.0) / dt;
                PacketRateSnapshot rate{};
                rate.primary_mbps = static_cast<float>(mbps);
                rate.secondary_mbps = phy_mbps.value_or(0.0f);
                rate.valid = true;
                rate.timestamp = now;
                rate.sta_mode = true;
                rate.phy_mbps = phy_mbps.value_or(0.0f);
                rate.freq_mhz = freq_mhz;
                rate.signal_dbm = static_cast<int>(signal_dbm.value_or(0.0f));
                latest_rate_ = rate;

                st.bytes = rx_bytes;
                st.ts = now;
            }
        }
        else
        {
            st.bytes = rx_bytes;
            st.ts = now;
            st.valid = true;
        }
    }

    return true;
}

void SignalMonitor::ProcessEntry(pid_t pid, const std::string &message,
                                 std::map<uint64_t, float> &antenna_data,
                                 bool &saw_pkt,
                                 uint64_t &video_bytes,
                                 uint64_t &first_ts,
                                 uint64_t &last_ts)
{
    (void)pid;
    if (message.empty())
        return;

    auto fields = SignalMonitor::SplitString(message, '\t');
    if (fields.size() < 2)
        return;

    if (fields[1] == "RX_ANT")
    {
        if (fields.size() < 5)
            return;
        try
        {
            uint64_t antenna_id = std::stoull(fields[3], nullptr, 16);
            auto stats = SignalMonitor::SplitString(fields[4], ':');
            if (stats.size() < 7)
                return;
            float rssi_val = static_cast<float>(std::stof(stats[2]));
            antenna_data[antenna_id] = rssi_val;
        }
        catch (const std::exception &)
        {
            return;
        }
    }
    else if (fields[1] == "PKT")
    {
        if (fields.size() < 3 || pid <= 0)
            return;
        uint64_t log_ts = 0;
        try
        {
            log_ts = static_cast<uint64_t>(std::stoull(fields[0]));
        }
        catch (const std::exception &)
        {
            log_ts = 0;
        }

        auto stats = SignalMonitor::SplitString(fields[2], ':');
        if (stats.size() < 11)
            return;
        uint64_t bytes_out = 0;
        try
        {
            bytes_out = static_cast<uint64_t>(std::stoull(stats.back()));
        }
        catch (const std::exception &)
        {
            return;
        }

        video_bytes += bytes_out;
        if (log_ts > 0)
        {
            if (first_ts == 0 || log_ts < first_ts)
                first_ts = log_ts;
            if (log_ts > last_ts)
                last_ts = log_ts;
        }
        saw_pkt = true;
    }
}

std::vector<std::string> SignalMonitor::SplitString(const std::string &line, char delim)
{
    std::vector<std::string> tokens;
    size_t start = 0;
    while (start <= line.size())
    {
        auto pos = line.find(delim, start);
        if (pos == std::string::npos)
        {
            tokens.emplace_back(line.substr(start));
            break;
        }
        tokens.emplace_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return tokens;
}

std::string SignalMonitor::TrimCopy(const std::string &text)
{
    size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t' ||
                                   text[start] == '\r' || text[start] == '\n'))
    {
        ++start;
    }
    size_t end = text.size();
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n'))
    {
        --end;
    }
    return text.substr(start, end - start);
}


std::optional<float> SignalMonitor::ParseSignalDbm(const std::string &line)
{
    if (line.rfind("signal:", 0) != 0)
        return std::nullopt;
    std::string value = TrimCopy(line.substr(std::strlen("signal:")));
    auto pos = value.find(" dBm");
    if (pos != std::string::npos)
    {
        value = value.substr(0, pos);
    }
    try
    {
        return std::stof(value);
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }
}

std::optional<float> SignalMonitor::ParseBitrateMbps(const std::string &line)
{
    auto colon = line.find(':');
    if (colon == std::string::npos)
        return std::nullopt;
    std::string value = TrimCopy(line.substr(colon + 1));
    auto unit_pos = value.find(" MBit/s");
    if (unit_pos == std::string::npos)
        return std::nullopt;
    value = value.substr(0, unit_pos);
    auto first_space = value.find(' ');
    if (first_space != std::string::npos)
    {
        value = value.substr(0, first_space);
    }
    try
    {
        return std::stof(value);
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }
}
