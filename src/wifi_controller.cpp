#include "wifi_controller.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>

// environ 必须在全局作用域声明；放在匿名 namespace 里会变成内部链接符号（链接报 undefined reference）。
extern char **environ;

namespace
{

constexpr const char kConnmanConf[] = "/storage/.config/connman_main.conf";
constexpr const char kConnmanCacheRoot[] = "/storage/.cache/connman";
constexpr const char kWfbConf[] = "/storage/digitalfpv/wfb.conf";
constexpr const char kSysNet[] = "/sys/class/net";

// ---------------- 基础工具 ----------------

std::string Trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> Split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (c == sep)
        {
            out.push_back(cur);
            cur.clear();
        }
        else
            cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

// 执行命令并返回 stdout
std::string Run(const std::string &cmd)
{
    std::string out;
    FILE *p = popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!p)
        return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), p))
        out += buf;
    pclose(p);
    return out;
}

void RunVoid(const std::string &cmd)
{
    std::system((cmd + " >/dev/null 2>&1").c_str());
}

std::string ReadFileStr(const std::string &path)
{
    std::ifstream f(path);
    if (!f)
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string ReadFirstLine(const std::string &path)
{
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);
    return Trim(line);
}

// 读 key=value 形式的配置
std::string ReadKeyValue(const std::string &path, const std::string &key)
{
    std::ifstream f(path);
    if (!f)
        return {};
    std::string line;
    while (std::getline(f, line))
    {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        if (Trim(line.substr(0, eq)) == key)
            return Trim(line.substr(eq + 1));
    }
    return {};
}

// ---------------- 网卡信息 ----------------

bool IsWifiIface(const std::string &iface)
{
    // WEXT 驱动建 wireless 软链；rtl88x2cs/cu 这类 nl80211 驱动只建 phy80211，
    // 只认 wireless 会把它们全部漏掉，导致选网卡列表为空。
    if (access((std::string(kSysNet) + "/" + iface + "/wireless").c_str(), F_OK) == 0)
        return true;
    return access((std::string(kSysNet) + "/" + iface + "/phy80211").c_str(), F_OK) == 0;
}

std::string DriverOf(const std::string &iface)
{
    char buf[512];
    std::string link = std::string(kSysNet) + "/" + iface + "/device/driver";
    ssize_t n = readlink(link.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0)
        return {};
    buf[n] = '\0';
    std::string s(buf);
    size_t slash = s.find_last_of('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

std::string MacOf(const std::string &iface)
{
    return ReadFirstLine(std::string(kSysNet) + "/" + iface + "/address");
}

bool IsUp(const std::string &iface)
{
    return ReadFirstLine(std::string(kSysNet) + "/" + iface + "/operstate") == "up";
}

// WFB 虚拟卡不参与 CA 连接
bool IsReservedIface(const std::string &iface)
{
    return iface.rfind("wlan_wfb", 0) == 0;
}

std::vector<std::string> AllWifiIfaces()
{
    std::vector<std::string> out;
    DIR *d = opendir(kSysNet);
    if (!d)
        return out;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr)
    {
        std::string n = e->d_name;
        if (n == "." || n == "..")
            continue;
        if (IsWifiIface(n))
            out.push_back(n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// 按 phy 支持的频率判断频段能力
std::string BandsOf(const std::string &iface)
{
    std::string phy = ReadFirstLine(std::string(kSysNet) + "/" + iface + "/phy80211/name");
    if (phy.empty())
        return "?";
    std::string info = Run("iw phy " + phy + " info");
    bool b24 = info.find("24") != std::string::npos && info.find(" MHz") != std::string::npos &&
               (info.find("2412 MHz") != std::string::npos || info.find("2437 MHz") != std::string::npos ||
                info.find("2462 MHz") != std::string::npos);
    bool b5 = info.find("5180 MHz") != std::string::npos || info.find("5745 MHz") != std::string::npos ||
              info.find("5825 MHz") != std::string::npos;
    if (b24 && b5)
        return "2.4G+5G";
    if (b5)
        return "5G";
    if (b24)
        return "2.4G";
    return "?";
}

// 从 `iw dev <if> link` 里取 SSID / 信号
std::string SsidOf(const std::string &iface)
{
    std::string link = Run("iw dev " + iface + " link");
    std::istringstream iss(link);
    std::string line;
    while (std::getline(iss, line))
    {
        size_t pos = line.find("SSID:");
        if (pos != std::string::npos)
            return Trim(line.substr(pos + 5));
    }
    return {};
}

// 拿某个接口的 IPv4 地址（getifaddrs，避免再起进程）
std::string Ipv4Of(const std::string &iface)
{
    struct ifaddrs *ifa = nullptr;
    if (getifaddrs(&ifa) != 0)
        return {};
    std::string out;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next)
    {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        if (iface != p->ifa_name)
            continue;
        char buf[INET_ADDRSTRLEN] = {0};
        auto *sin = reinterpret_cast<struct sockaddr_in *>(p->ifa_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)))
        {
            out = buf;
            break;
        }
    }
    freeifaddrs(ifa);
    return out;
}

// ---------------- ConnMan 交互 ----------------

struct ServiceProps
{
    std::string name;
    std::string state;
    int strength = 0;
    bool secure = false;
    bool found = false;
};

ServiceProps QueryService(const std::string &path)
{
    ServiceProps sp;
    std::string out = Run("connmanctl services " + path);
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line))
    {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        if (key == "Name")
            sp.name = val;
        else if (key == "State")
            sp.state = val;
        else if (key == "Strength")
            sp.strength = std::atoi(val.c_str());
        else if (key == "Security")
            sp.secure = (val.find("none") == std::string::npos && !val.empty());
        sp.found = true;
    }
    return sp;
}

// 列出 ConnMan 里所有 wifi service 的 path
std::vector<std::string> ListWifiServices()
{
    std::vector<std::string> out;
    std::string raw = Run("connmanctl services");
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line))
    {
        // 每行末段是 service 路径，形如 wifi_xxx_managed_psk
        size_t p = line.find_last_of(" \t");
        std::string last = (p == std::string::npos) ? Trim(line) : Trim(line.substr(p + 1));
        if (last.rfind("wifi_", 0) == 0)
            out.push_back(last);
    }
    return out;
}

// 在 ConnMan 的 service 列表里按 SSID 找路径
std::string FindServiceBySsid(const std::string &ssid)
{
    for (const auto &p : ListWifiServices())
    {
        ServiceProps sp = QueryService(p);
        if (sp.name == ssid)
            return p;
    }
    return {};
}

// 用原生 D-Bus 读 Service 的属性（State / Error / Favorite）。
// 不用 connmanctl：1.43 的 `services <svc>` 文本输出拿不到 Error，
// 而它的 connect 子命令在 service 路径过期时会报
// "Method Connect ... doesn't exist"（ConnMan 用自带 gdbus，没人认领时
// libdbus 兜底生成这句），既看不出原因也没法重试。
// dbus-send 的 GetProperties 输出形如：
//   dict entry(
//      string "State"
//      variant             string "association"
//   )
// 键名行之后紧跟值行。只需要顶层这三个键，嵌套字典里没有同名键。
struct ServiceLive
{
    std::string state;      // idle / association / configuration / ready / online / disconnect / failure
    std::string error;      // invalid-key / connect-failed / ...（没有错误时为空）
    bool favorite = false;  // ConnMan 已保存这条连接（密码在它自己的 settings 里）
    bool found = false;     // 对象存在并返回了属性
};

static ServiceLive QueryServiceLive(const std::string &svc)
{
    ServiceLive r;
    if (svc.empty())
        return r;
    const std::string out =
        Run("dbus-send --system --print-reply --dest=net.connman /net/connman/service/" + svc +
            " net.connman.Service.GetProperties");
    std::istringstream iss(out);
    std::string line;
    std::string want;
    while (std::getline(iss, line))
    {
        if (!want.empty())
        {
            // 值行：variant   string "ready" / boolean true / array [
            const size_t v = line.find("variant");
            std::string val = Trim(v == std::string::npos ? line : line.substr(v + 7));
            const size_t sp = val.find(' ');
            if (val.rfind("string \"", 0) == 0)
            {
                val = val.substr(8);
                const size_t q = val.find('"');
                val = q == std::string::npos ? val : val.substr(0, q);
            }
            else if (sp != std::string::npos)
            {
                // boolean true / byte 0x01 ...：空格前是类型词，值在后面。
                // （曾经在这里按第一个空格截断，取到的是 "boolean"，
                //   已保存密码的热点就被误判成 "需要密码"。）
                val = Trim(val.substr(sp + 1));
            }
            if (want == "State")
                r.state = val;
            else if (want == "Error")
                r.error = val;
            else
                r.favorite = (val == "true");
            r.found = true;
            want.clear();
            continue;
        }
        const size_t q1 = line.find('"');
        if (q1 == std::string::npos)
            continue;
        const size_t q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos)
            continue;
        const std::string key = line.substr(q1 + 1, q2 - q1 - 1);
        if (key == "State" || key == "Error" || key == "Favorite")
            want = key;
    }
    return r;
}

static bool IsServiceUp(const ServiceLive &live)
{
    // online 是 ready 之后的联网探测通过态；两者都算已连接，
    // 否则 ready->online 转得比轮询间隔快时会漏判。
    return live.state == "ready" || live.state == "online";
}

static bool IsServiceConnecting(const std::string &state)
{
    return state == "association" || state == "configuration";
}

// 把 ConnMan 的 Error 码翻成用户能看懂的话。
static std::string DescribeConnmanError(const std::string &err)
{
    if (err == "invalid-key")
        return "密码被热点拒绝（invalid-key）";
    if (err == "connect-failed")
        return "关联失败，热点没有接受本机（connect-failed）";
    if (err == "no-carrier")
        return "链路没有建立（no-carrier）";
    if (err == "timeout")
        return "热点长时间没有响应（timeout）";
    if (err.empty())
        return "ConnMan 没有给出原因";
    return err;
}

// 向 ConnMan 的某个 service 发一次无参数的 D-Bus 方法调用。
//
// !! dbus-send 不带 --print-reply 时发的是 signal，不是 method call !!
// ConnMan 对信号不处理，State 纹丝不动（真机 dbus-monitor 抓到的是
// "signal ... interface=net.connman.Service; member=Connect"）。
// 之前的 KickConnect 就是这么写的，等于从来没发出过连接。
// 必须显式 --type=method_call；不加 --print-reply 就不等回复，进程立刻退出。
// Connect/Disconnect 都是异步方法，真实结果看 State。
static void KickServiceMethod(const std::string &svc, const char *method)
{
    if (svc.empty() || svc.rfind("wifi_", 0) != 0)
        return;
    RunVoid(std::string("dbus-send --system --type=method_call --dest=net.connman "
                       "/net/connman/service/") + svc + " " + method);
}

static void KickConnect(const std::string &svc)
{
    KickServiceMethod(svc, "net.connman.Service.Connect");
}

static void KickDisconnect(const std::string &svc)
{
    KickServiceMethod(svc, "net.connman.Service.Disconnect");
}

// 网卡 MAC 去冒号小写，对应 service 路径前缀 wifi_<mac>_<ssid>...
static std::string MacHex(const std::string &iface)
{
    const std::string mac = MacOf(iface);
    std::string out;
    for (char c : mac)
        if (c != ':')
            out.push_back(static_cast<char>(::tolower(c)));
    return out;
}

// 连接流程日志：stdout 进 journal，排障时和 ConnMan/wpa_supplicant 日志对时间线。
static void WLog(const std::string &s)
{
    std::fprintf(stdout, "[wifi] %s\n", s.c_str());
    std::fflush(stdout);
}

// ConnMan 是否已经接管 Wi-Fi 网卡：看 wpa_supplicant 那边有没有建出接口。
// 没有（真机偶发：wlan1 刚断开就 restart connman，新 ConnMan 没再建接口）时
// scan wifi 报 "Not supported"、热点列表一直是空的，开关 Wi-Fi 电源也不会补建，
// 只有再重启一次 ConnMan 才恢复。
static bool ConnmanHasWifiIface()
{
    const std::string out = Run(
        "dbus-send --system --print-reply --dest=fi.w1.wpa_supplicant1 "
        "/fi/w1/wpa_supplicant1 org.freedesktop.DBus.Properties.Get "
        "string:fi.w1.wpa_supplicant1 string:Interfaces");
    return out.find("object path") != std::string::npos;
}

// 确保 ConnMan 接管了网卡，没有就重启一次 ConnMan 恢复。
// ConnMan 刚重启过（切换网络模式）时接口要 1 秒左右才建好，先等一等，不急着重启。
static bool EnsureConnmanWifi(std::string *why)
{
    for (int i = 0; i < 10; ++i)
    {
        if (ConnmanHasWifiIface())
            return true;
        usleep(300 * 1000);
    }
    static bool restarted = false;
    static std::chrono::steady_clock::time_point restarted_at;
    if (restarted &&
        std::chrono::steady_clock::now() - restarted_at < std::chrono::seconds(60))
    {
        if (why)
            *why = "刚重启过 ConnMan 仍未恢复";
        return false;
    }
    restarted = true;
    restarted_at = std::chrono::steady_clock::now();
    WLog("ConnMan has no interface on the Wi-Fi card, restarting connman to recover");
    RunVoid("systemctl restart connman.service");
    for (int i = 0; i < 40; ++i)
    {
        usleep(500 * 1000);
        if (ConnmanHasWifiIface())
        {
            sleep(1); // 接口刚建好，wpa_supplicant 还在初始化
            WLog("Wi-Fi card is managed by ConnMan again");
            return true;
        }
    }
    if (why)
        *why = "重启 ConnMan 后仍未恢复";
    return false;
}

// 当前黑名单
std::string ReadBlacklist()
{
    std::ifstream f(kConnmanConf);
    if (!f)
        return {};
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("NetworkInterfaceBlacklist=", 0) == 0)
            return Trim(line.substr(25));
    }
    return {};
}

// 把 iface 从黑名单里去掉（不写则已存在）
bool UnblacklistIface(const std::string &iface)
{
    std::string cur = ReadBlacklist();
    if (cur.empty())
        return false;
    auto items = Split(cur, ',');
    bool hit = false;
    std::vector<std::string> keep;
    for (auto &it : items)
    {
        std::string t = Trim(it);
        if (t.empty())
            continue;
        if (t == iface)
            hit = true;
        else
            keep.push_back(t);
    }
    if (!hit)
        return false;

    std::string joined;
    for (size_t i = 0; i < keep.size(); ++i)
    {
        if (i)
            joined += ",";
        joined += keep[i];
    }

    // 原地替换该行，保留其它配置
    std::ifstream in(kConnmanConf);
    if (!in)
        return false;
    std::vector<std::string> lines;
    std::string line;
    bool replaced = false;
    while (std::getline(in, line))
    {
        if (!replaced && line.rfind("NetworkInterfaceBlacklist=", 0) == 0)
        {
            lines.push_back("NetworkInterfaceBlacklist=" + joined);
            replaced = true;
        }
        else
            lines.push_back(line);
    }
    in.close();

    std::ofstream out(kConnmanConf, std::ios::trunc);
    if (!out)
        return false;
    for (auto &l : lines)
        out << l << "\n";
    return true;
}

// SSID -> 大写 hex（用于定位旧版 provisioning 配置文件）
static std::string SsidToHex(const std::string &ssid)
{
    static const char *digits = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : ssid)
    {
        out.push_back(digits[(c >> 4) & 0xF]);
        out.push_back(digits[c & 0xF]);
    }
    return out;
}

// 删掉旧版本（方案 A）留下的 provisioning 配置。
//
// 必须删，否则连不上：
//   provision_service_wifi() 会把服务置 immutable=true，
//   而 __connman_service_set_passphrase() 在 immutable 时直接返回 -EINVAL
//   —— 也就是 Agent 应答的密码会被静默丢弃，State 永远到不了 ready。
//   删文件会触发 config_notify_handler 的 IN_DELETE -> unregister_service()，
//   它先 set_immutable(false) 再 __connman_service_remove()，服务随后由扫描重建，
//   回到"可写"状态。
// 只删本程序自己写过的 amlgs_*.config，绝不碰用户其它配置。
static void RemoveStaleProvisionConfigs(const std::string &ssid)
{
    const std::string hex = SsidToHex(ssid);
    const std::string base = std::string(kConnmanCacheRoot) + "/amlgs_" + hex + ".config";
    unlink(base.c_str());
    unlink((base + ".tmp").c_str());
}

// 记住上次成功连接的 SSID，供开机自动连接使用。
// 不再写 ConnMan 的 main.conf（那本来就不是放 service 段的地方）。
constexpr const char kAmLgsWifiState[] = "/storage/.config/amlgs_wifi.conf";

static void RememberSsid(const std::string &ssid)
{
    std::ofstream out(kAmLgsWifiState, std::ios::trunc);
    if (!out)
        return;
    out << "SSID=" << ssid << "\n";
}

static std::string RememberedSsid()
{
    return ReadKeyValue(kAmLgsWifiState, "SSID");
}

// 把密码交给 ConnMan 的 provisioning 通道。
//
// !! 已废弃，仅为清理历史残留保留 SsidToHex / RemoveStaleProvisionConfigs !!
// 真机验证结论：只写配置不会触发连接（ConnMan 日志有 "Adding service
// configuration"、属性变 Immutable=true，但 State 恒 idle，连 association
// 都没有）。而且 immutable 会让 Agent 应答的密码被 __connman_service_set_passphrase()
// 以 -EINVAL 拒掉。现在的密码投递走 net.connman.Agent（scripts/wifi_agent.py）。
// 唯一入口是 Agent、provisioning 配置、settings 文件（仅加载时）。

// ---------------- Python Agent 会话 ----------------

// scripts/wifi_agent.py 在设备上的位置。部署在 /storage/digitalfpv 下，
// 由更新包随 AMLgsMenu 一起推送。
constexpr const char kAgentScriptDeployed[] = "/storage/digitalfpv/wifi_agent.py";
// 与 ../scripts/wifi_agent.py 对应的源码相对路径（本地调试用）
constexpr const char kAgentScriptRelative[] = "scripts/wifi_agent.py";
constexpr const char kAgentPidFile[] = "/run/amlgs_wifi_agent.pid";
constexpr const char kAgentLogFile[] = "/run/amlgs_wifi_agent.log";
// Agent 把这次连接的结论写在这里（ok / error=<码> / failed=<D-Bus 错误名>）。
constexpr const char kAgentResultFile[] = "/run/amlgs_wifi_agent.result";

std::string AgentScriptPath()
{
    // 1) 部署路径
    if (access(kAgentScriptDeployed, R_OK) == 0)
        return kAgentScriptDeployed;

    // 2) 可执行文件同级 / 源码树（本地或未打包场景）
    char exe[512] = {0};
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0)
    {
        exe[n] = '\0';
        std::string dir(exe);
        const size_t slash = dir.find_last_of('/');
        if (slash != std::string::npos)
        {
            dir = dir.substr(0, slash);
            const std::string cand = dir + "/" + kAgentScriptRelative;
            if (access(cand.c_str(), R_OK) == 0)
                return cand;
            const std::string cand2 = dir + "/wifi_agent.py";
            if (access(cand2.c_str(), R_OK) == 0)
                return cand2;
        }
    }
    return {};
}

// 结束上一次遗留的 agent（进程崩溃/被 kill -9 时可能残留）。
// 只认 pid 文件里、且 cmdline 确实是本脚本的进程，避免 pid 复用误杀。
void KillStaleAgent()
{
    std::ifstream f(kAgentPidFile);
    int pid = 0;
    if (!(f >> pid) || pid <= 1)
        return;

    char cmdline[512] = {0};
    const std::string proc = "/proc/" + std::to_string(pid) + "/cmdline";
    FILE *p = fopen(proc.c_str(), "rb");
    if (p)
    {
        const size_t got = fread(cmdline, 1, sizeof(cmdline) - 1, p);
        cmdline[got] = '\0';
        fclose(p);
    }
    if (std::string(cmdline).find("wifi_agent.py") == std::string::npos)
        return; // pid 已被别的进程复用，不动它

    kill(pid, SIGTERM);
    for (int i = 0; i < 20; ++i) // 最多等 1s
    {
        if (kill(pid, 0) != 0)
            return;
        usleep(50 * 1000);
    }
    kill(pid, SIGKILL);
}

// 一次连接尝试对应的 agent 生命周期。
// 构造后 Start()，析构时自动 kill —— 保证无论成功/失败/异常返回都不留进程。
class AgentSession
{
public:
    AgentSession() = default;
    ~AgentSession() { Stop(); }

    AgentSession(const AgentSession &) = delete;
    AgentSession &operator=(const AgentSession &) = delete;

    // 启动 python3 scripts/wifi_agent.py，密码只经环境变量传递。
    bool Start(const std::string &psk, const std::string &svc, std::string *why)
    {
        auto fail = [&](const std::string &m) {
            if (why)
                *why = m;
            return false;
        };

        if (psk.empty())
            return fail("密码为空，无法启动 Agent");

        const std::string script = AgentScriptPath();
        if (script.empty())
            return fail("找不到 wifi_agent.py（应部署到 " +
                        std::string(kAgentScriptDeployed) + "）");

        KillStaleAgent();
        unlink(kAgentResultFile); // 上一次的结论不能混进这一次

        // fork+execve 而不是 system()：密码要经环境变量传，不能进 argv/shell。
        // 同时把 stdout 落到日志文件，真机上可直接查看 RequestInput 应答过程。
        const pid_t pid = fork();
        if (pid < 0)
            return fail("fork 失败，无法启动 Wi-Fi Agent");
        if (pid == 0)
        {
            // 子进程：脱离会话，输出重定向到日志
            setsid();

            const int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0)
            {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
            const int logfd = open(kAgentLogFile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (logfd >= 0)
            {
                dup2(logfd, STDOUT_FILENO);
                dup2(logfd, STDERR_FILENO);
                close(logfd);
            }

            // 环境变量：在父环境基础上追加 AMLGS_WIFI_PSK
            std::vector<std::string> env_store;
            for (char **e = environ; e && *e; ++e)
                env_store.emplace_back(*e);
            env_store.emplace_back("AMLGS_WIFI_PSK=" + psk);
            // service 路径也走环境变量：Agent 要用自己那条
            // D-Bus 连接发 Service.Connect，否则会落到 default_agent。
            if (!svc.empty())
                env_store.emplace_back("AMLGS_WIFI_SVC=" + svc);
            std::vector<char *> envp;
            envp.reserve(env_store.size() + 1);
            for (auto &s : env_store)
                envp.push_back(const_cast<char *>(s.c_str()));
            envp.push_back(nullptr);

            char *const argv[] = {
                const_cast<char *>("python3"),
                const_cast<char *>(script.c_str()),
                nullptr,
            };
            execve("/usr/bin/python3", argv, envp.data());

            // execve 失败：直接退出，父进程会看到进程已消失
            _exit(127);
        }

        pid_ = pid;

        // 记 pid 文件，便于异常重启后清理
        if (std::ofstream pf{kAgentPidFile, std::ios::trunc})
            pf << pid_ << "\n";

        // 等 RegisterAgent 完成（脚本要连 bus、注册对象、调 RegisterAgent）。
        // 1s 足够；同时检查子进程是否已经退出（密码缺失/脚本报错会立刻退）。
        for (int i = 0; i < 20; ++i)
        {
            usleep(50 * 1000);
            int status = 0;
            const pid_t r = waitpid(pid_, &status, WNOHANG);
            if (r == pid_)
            {
                pid_ = 0;
                return fail("Wi-Fi Agent 启动即退出：" + AgentLogTail());
            }
            if (i >= 19)
                break;
        }
        return true;
    }

    void Stop()
    {
        if (pid_ <= 0)
        {
            unlink(kAgentPidFile);
            return;
        }
        // 先 SIGTERM 让脚本走 UnregisterAgent，再兜底 SIGKILL
        kill(pid_, SIGTERM);
        for (int i = 0; i < 40; ++i) // 最多等 2s
        {
            int status = 0;
            if (waitpid(pid_, &status, WNOHANG) == pid_)
            {
                pid_ = 0;
                unlink(kAgentPidFile);
                return;
            }
            usleep(50 * 1000);
        }
        kill(pid_, SIGKILL);
        int status = 0;
        waitpid(pid_, &status, 0);
        pid_ = 0;
        unlink(kAgentPidFile);
    }

private:
    // 取日志最后两行，用于把启动失败原因带回给 UI
    std::string AgentLogTail() const
    {
        std::ifstream f(kAgentLogFile);
        if (!f)
            return "（无日志）";
        std::string last;
        std::string line;
        while (std::getline(f, line))
        {
            if (!Trim(line).empty())
                last = Trim(line);
        }
        return last.empty() ? "（无日志）" : last;
    }

    pid_t pid_ = 0;
};

// 取 Agent 写下的结论：ok / error=<码> / failed=<D-Bus 错误名>；还没有就是空串。
// ConnMan 在连接失败时只把原因告诉 Agent（ReportError），然后把 service 置回
// idle、不进 failure，所以轮询 State 看不到原因，要靠这个文件。
static std::string AgentResult()
{
    return ReadFirstLine(kAgentResultFile);
}

// ---------------- 连接时间预算 ----------------

// 正常连接 12 秒左右；密码错约 15 秒就有结论。ConnMan 自己的上限是 120 秒
// （service.c CONNECT_TIMEOUT），这里取中间值：慢扫描和慢 DHCP 都够用，
// 又不至于让用户对着一个必然失败的连接等两分钟。
constexpr double kConnectBudgetSec = 60.0;
// Connect 发出后 State 停留在 idle 的最长时间。正常情况下毫秒级就进 association。
constexpr double kIdleLimitSec = 12.0;
// 进入过 association/configuration 之后又掉回 idle，并且持续这么久：这次尝试已经结束。
// 给 ConnMan 内部的握手重试（状态会短暂回到 idle）留出间隔。
constexpr double kIdleAfterConnectingSec = 3.0;
// 非密码类失败自动重试的次数和间隔，同 CoreELEC 网络插件。
constexpr int kConnectAttempts = 2;
constexpr int kRetryDelaySec = 5;

// ---------------- 后台任务的共享状态 ----------------

std::mutex g_scan_mu;
std::vector<WifiAp> g_scan_result;
std::atomic<bool> g_scanning{false};
std::atomic<bool> g_scan_fresh{false};
// 上一次扫描没能做的原因（空串表示正常），给向导的空列表换一句准确的提示。
std::string g_scan_note;

std::mutex g_conn_mu;
std::atomic<bool> g_conn_running{false};
std::atomic<bool> g_conn_done{false};
std::atomic<bool> g_conn_ok{false};
std::string g_conn_msg;

// 最新一次连接请求。连接线程正在跑的时候界面又点了"连接"（取消后重开向导、
// 换个热点再连），不能把新请求丢掉：记在这里，线程做完手上这次就接着处理它。
// 每次点击都生效，与 Kodi 网络列表里每次回车都会发起连接一致。
struct ConnectRequest
{
    std::string ssid;
    std::string psk;
    std::string iface;
    bool pending = false;
};
ConnectRequest g_conn_req;

// 连接进行到哪一步了，UI 每帧取一次显示在"正在连接"下面。
std::mutex g_prog_mu;
std::string g_conn_progress;

enum ConnmanPhase
{
    kPhaseLocate,
    kPhaseSubmit,
    kPhaseAssoc,
    kPhaseDhcp,
};

std::string PhaseText(ConnmanPhase phase, double elapsed_s)
{
    char tail[32];
    std::snprintf(tail, sizeof(tail), "（已 %.0f 秒）", elapsed_s);
    switch (phase)
    {
    case kPhaseLocate:
        return "正在定位热点" + std::string(tail);
    case kPhaseSubmit:
        return "正在向 ConnMan 提交连接请求" + std::string(tail);
    case kPhaseAssoc:
        return "正在关联热点（网卡扫描加关联约 10 秒）" + std::string(tail);
    case kPhaseDhcp:
        return "已关联，正在获取 IP 地址" + std::string(tail);
    }
    return {};
}

void SetConnectProgress(const std::string &text)
{
    std::lock_guard<std::mutex> lk(g_prog_mu);
    g_conn_progress = text;
}

} // namespace

// ================= 公共接口 =================

std::vector<WifiIface> WifiController::ListIfaces()
{
    std::vector<WifiIface> out;
    for (const auto &n : AllWifiIfaces())
    {
        if (IsReservedIface(n))
            continue;
        WifiIface it;
        it.name = n;
        it.driver = DriverOf(n);
        it.mac = MacOf(n);
        it.bands = BandsOf(n);
        it.up = IsUp(n);
        it.ssid = SsidOf(n);
        out.push_back(it);
    }
    return out;
}

std::string WifiController::ApModeValue()
{
    std::string v = ReadKeyValue(kWfbConf, "ap_mode");
    if (v.empty())
        v = ReadKeyValue("/flash/wfb.conf", "ap_mode");
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return v;
}

bool WifiController::IsWfbMode()
{
    // ap_mode 是唯一业务模式来源（与 /opt/bin/wifibroadcast 的 read_ap_mode 一致）：
    //   ap_mode=1 -> 连接天空端热点（managed/STA），不是 WFB
    //   ap_mode=0 / 缺失 / 非法 -> WFB 图传（保守回退）
    // 旧的 mode 字段已废弃，不再读取。
    const std::string v = ApModeValue();
    if (v == "1" || v == "true" || v == "yes" || v == "on")
        return false; // 连热点模式，允许扫描与连接
    return true;      // WFB 模式（含 ap_mode=0/缺失/非法值）
}

std::string WifiController::ConfiguredSsid()
{
    // 1) 本程序上次成功连接时记下的 SSID（正常路径）
    {
        const std::string s = RememberedSsid();
        if (!s.empty())
            return s;
    }
    // 2) 兼容历史：早期版本往 main.conf 写过 [service_AMLgsMenu] 段（无效用法，
    //    但用户机器上可能还有残留），能读到就用它。
    std::ifstream f(kConnmanConf);
    if (f)
    {
        std::string line;
        bool in_section = false;
        while (std::getline(f, line))
        {
            if (line.rfind("[service_AMLgsMenu]", 0) == 0)
            {
                in_section = true;
                continue;
            }
            if (in_section && line.rfind("[", 0) == 0)
                break;
            if (in_section)
            {
                size_t eq = line.find('=');
                if (eq != std::string::npos && Trim(line.substr(0, eq)) == "Name")
                    return Trim(line.substr(eq + 1));
            }
        }
    }
    return {};
}

void WifiController::StartScan(const std::string &iface)
{
    if (iface.empty() || g_scanning.exchange(true))
        return;

    std::thread([iface]() {
        std::vector<WifiAp> list;

        // 与 CoreELEC 官方一致：列表来自 ConnMan 的 service 集合。
        // 不自己跑 iw scan —— 已关联状态下 iw scan 会超时并让接口短暂离开信道。
        // connmanctl scan 在 1.43 上返回 NotImplemented（wifi driver 无 scan 字段），
        // 属正常，忽略即可；ConnMan 后台扫描一直在积累结果。
        if (WifiController::IsWfbMode())
        {
            // WFB 模式下网卡被 wifibroadcast 占用，不触发任何扫描动作
            std::lock_guard<std::mutex> lk(g_scan_mu);
            g_scan_result.clear();
            g_scan_fresh.store(true);
            g_scanning.store(false);
            return;
        }

        // 网卡没被 ConnMan 接管时扫描会直接报错、列表为空；这里顺带把它恢复过来。
        {
            std::lock_guard<std::mutex> lk(g_scan_mu);
            g_scan_note.clear();
        }
        std::string why;
        const bool ready = EnsureConnmanWifi(&why);
        if (!ready)
        {
            std::lock_guard<std::mutex> lk(g_scan_mu);
            g_scan_note = why;
        }
        if (ready)
            RunVoid("connmanctl scan wifi");

        for (const auto &path : ListWifiServices())
        {
            ServiceProps sp = QueryService(path);
            if (!sp.found || sp.name.empty())
                continue;
            WifiAp ap;
            ap.ssid = sp.name;
            ap.service_path = path;
            ap.signal_dbm = sp.strength;
            ap.secure = sp.secure;
            ap.state = sp.state;
            // 同一 SSID 只留信号最强的一条
            auto it = std::find_if(list.begin(), list.end(),
                                   [&](const WifiAp &a) { return a.ssid == ap.ssid; });
            if (it == list.end())
                list.push_back(ap);
            else if (ap.signal_dbm > it->signal_dbm)
                *it = ap;
        }

        std::sort(list.begin(), list.end(),
                  [](const WifiAp &a, const WifiAp &b) { return a.signal_dbm > b.signal_dbm; });

        {
            std::lock_guard<std::mutex> lk(g_scan_mu);
            g_scan_result = std::move(list);
        }
        g_scan_fresh.store(true);
        g_scanning.store(false);
    }).detach();
}

bool WifiController::PollScan(std::vector<WifiAp> *out, bool *scanning)
{
    if (scanning)
        *scanning = g_scanning.load();
    if (!g_scan_fresh.exchange(false))
        return false;
    std::lock_guard<std::mutex> lk(g_scan_mu);
    if (out)
        *out = g_scan_result;
    return true;
}

std::string WifiController::ScanNote()
{
    std::lock_guard<std::mutex> lk(g_scan_mu);
    return g_scan_note;
}

bool WifiController::ConnectSync(const std::string &ssid, const std::string &psk,
                                 const std::string &iface, std::string *message)
{
    auto fail = [&](const std::string &why) {
        SetConnectProgress({});
        WLog("connect " + ssid + " failed: " + why);
        if (message)
            *message = why;
        return false;
    };
    auto succeed = [&](const std::string &msg) {
        SetConnectProgress({});
        RememberSsid(ssid);
        WLog("connect " + ssid + " ok: " + msg);
        if (message)
            *message = msg;
        return true;
    };

    if (ssid.empty())
        return fail("SSID 为空");

    // WFB 模式下绝不连 AP（会抢走网卡、破坏 WFB 链路）
    if (IsWfbMode())
        return fail("当前是 WFB 模式，不连接热点（避免抢占网卡）");

    // 目标网卡
    std::string target = iface;
    if (target.empty() || !IsWifiIface(target))
    {
        // 优先 88x2cs/88x2cu（双频），否则第一张非 WFB 卡
        for (const auto &n : AllWifiIfaces())
        {
            if (IsReservedIface(n))
                continue;
            std::string drv = DriverOf(n);
            if (drv == "rtl88x2cs" || drv == "88x2cs" || drv == "rtl88x2cu" || drv == "88x2cu")
            {
                target = n;
                break;
            }
        }
        if (target.empty())
        {
            for (const auto &n : AllWifiIfaces())
            {
                if (IsReservedIface(n))
                    continue;
                target = n;
                break;
            }
        }
    }
    if (target.empty())
        return fail("没有可用的无线网卡");

    // 死接口先拉起来
    if (!IsUp(target))
        RunVoid("ip link set " + target + " up");
    // 若该网卡曾被写进 main.conf 的 NetworkInterfaceBlacklist，顺手去掉
    // （只删不加，幂等；不重启 connmand）
    UnblacklistIface(target);

    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    auto phase = [&](ConnmanPhase p) { SetConnectProgress(PhaseText(p, elapsed())); };
    WLog("connect " + ssid + " on " + target +
         (psk.empty() ? " (no password supplied)" : " (password supplied)"));

    // ---- 0. 清掉旧版本留下的 provisioning 配置 ----
    // 留着它会让服务变成 immutable，Agent 答的密码会被 ConnMan 直接丢掉。
    RemoveStaleProvisionConfigs(ssid);

    // ---- 1. 定位 service ----
    // 与 CoreELEC 网络插件一致：连接前不扫描，只在 service 还没出现在
    // ConnMan 列表里时才扫。connmanctl scan wifi 在 1.43 上要同步等到
    // 扫描结束（这块网卡 2.5~10 秒），用户刚从扫描列表里选过这个热点，
    // 再扫一遍只是白等。
    phase(kPhaseLocate);
    // 网卡没被 ConnMan 接管时扫什么都是空的，先把这一层修好再定位热点。
    std::string wifi_why;
    if (!EnsureConnmanWifi(&wifi_why))
        return fail("Wi-Fi 网卡没有被 ConnMan 接管（" + wifi_why + "），请重启设备后再试");
    std::string svc = FindServiceBySsid(ssid);
    if (svc.empty())
    {
        WLog("service for " + ssid + " not listed, scanning wifi");
        RunVoid("connmanctl scan wifi");
        for (int i = 0; i < 15 && svc.empty(); ++i)
        {
            svc = FindServiceBySsid(ssid);
            if (svc.empty())
                sleep(1);
        }
        if (svc.empty())
            return fail("未扫描到该热点（AP 不在线？该网卡频段不覆盖？）");
    }
    WLog("service " + svc);

    // ---- 2. 已经连上就不折腾，避免打断现有链路 ----
    if (IsServiceUp(QueryServiceLive(svc)))
        return succeed("已连接 " + ssid + "  " + Ipv4Of(target));

    // ---- 3. 清场：同一张网卡上别的连接还没结束就先停掉 ----
    // ConnMan 的 connect_service() 发现同网卡同类型的另一个 service 处于
    // association/configuration 时，会先对它做 Disconnect；断开是异步的，
    // 这一轮它会直接给本次 Connect 回错误（真机表现为 InProgress/超时）。
    // CoreELEC 插件是收到 InProgress 后再 disconnect + connect，这里提前做。
    {
        const std::string dev = "wifi_" + MacHex(target);
        std::vector<std::string> busy;
        for (const auto &p : ListWifiServices())
        {
            if (p == svc || p.rfind(dev, 0) != 0)
                continue;
            if (IsServiceConnecting(QueryServiceLive(p).state))
                busy.push_back(p);
        }
        if (!busy.empty())
        {
            WLog("stopping " + std::to_string(busy.size()) +
                 " other connection attempt(s) on " + target);
            for (const auto &p : busy)
                KickDisconnect(p);
            for (int i = 0; i < 25; ++i) // 最多等 5s
            {
                bool still = false;
                for (const auto &p : busy)
                    still = still || IsServiceConnecting(QueryServiceLive(p).state);
                if (!still)
                    break;
                usleep(200 * 1000);
            }
        }
    }

    // ---- 4. 发起连接并等结果 ----
    // 一次连接尝试的结论。retry 表示这种失败值得再试一次（与 CoreELEC 插件
    // 对 Input/output error 等"非用户输入导致"的失败等 5 秒重试一次的做法一致）。
    struct Outcome
    {
        bool ok = false;
        bool retry = false;
        std::string code;    // ConnMan 的 Error 码（invalid-key / connect-failed / ...）
        std::string reason;  // 给用户看的话
    };

    auto run_attempt = [&]() -> Outcome {
        AgentSession agent;

        // 目标自己已经在连接中（比如 ConnMan 开机自动连接正在跑）：直接沿用，
        // 重新发 Connect 只会得到 InProgress，还会把一次快成功的关联打断。
        const bool adopt = IsServiceConnecting(QueryServiceLive(svc).state);
        if (adopt)
        {
            WLog("adopting connection attempt already in progress on " + svc);
        }
        else if (psk.empty())
        {
            // 没给密码：开放网络，或者 ConnMan 自己保存过这条连接的密码。
            // 加密热点且没有保存过密码时 ConnMan 只会去问 agent，这里没人能答，
            // 等下去必然超时，直接说明原因。
            const ServiceProps sp = QueryService(svc);
            if (sp.secure && !QueryServiceLive(svc).favorite)
            {
                Outcome o;
                o.reason = "该热点还没有保存密码，请先在 Wi-Fi 连接向导里输入密码连接一次";
                return o;
            }
            phase(kPhaseSubmit);
            KickConnect(svc);
            WLog("Connect sent (open network or saved credentials)");
        }
        else
        {
            // 加密热点唯一的密码通道：ConnMan 1.43 的 service_connect() 在
            // passphrase 为空时直接返回 -ENOKEY，随后只会向 Agent 发 RequestInput。
            // 密码经环境变量交给 scripts/wifi_agent.py，不出现在 argv / ps 里。
            phase(kPhaseSubmit);
            if (!agent.Start(psk, svc, message))
            {
                Outcome o;
                o.reason = message ? *message : std::string("Wi-Fi Agent 启动失败");
                return o;
            }
            WLog("agent started, Connect sent by agent");
        }

        // 正常链路：Connect 后立即 association，wpa_supplicant 扫描加关联约 10 秒，
        // 然后 configuration 取地址，ready。
        std::string last_state;
        std::string seen_states;
        double last_locate = elapsed();
        double idle_since = -1.0;
        bool saw_connecting = false;
        while (elapsed() < kConnectBudgetSec)
        {
            // CoreELEC 默认 BackgroundScanning=true，期间 service 对象可能被删/重建，
            // 每 5s 重新解析一次路径，避免一直查一个不存在的对象空转到超时。
            if (elapsed() - last_locate >= 5.0)
            {
                last_locate = elapsed();
                const std::string fresh = FindServiceBySsid(ssid);
                if (!fresh.empty())
                    svc = fresh;
            }
            const ServiceLive live = QueryServiceLive(svc);
            if (live.state != last_state)
            {
                char at[24];
                std::snprintf(at, sizeof(at), "%.1fs", elapsed());
                WLog(std::string("state ") + (last_state.empty() ? "-" : last_state.c_str()) +
                     " -> " + (live.state.empty() ? "(none)" : live.state.c_str()) + " at " + at);
                if (!live.state.empty() && seen_states.find(live.state) == std::string::npos)
                    seen_states += (seen_states.empty() ? "" : ",") + live.state;
                last_state = live.state;
            }

            Outcome o;
            if (IsServiceUp(live))
            {
                o.ok = true;
                return o;
            }
            // 无 Agent 的路径（开放网络 / 已存密码）失败时 ConnMan 会进 failure
            if (live.state == "failure")
            {
                o.code = live.error;
                o.retry = !live.error.empty() && live.error != "invalid-key";
                o.reason = "连接失败：" + DescribeConnmanError(live.error);
                return o;
            }
            // 有 Agent 的路径：原因只在 Agent 的 ReportError 里
            const std::string res = AgentResult();
            if (res.rfind("error=", 0) == 0)
            {
                o.code = res.substr(6);
                o.retry = o.code != "invalid-key";
                o.reason = "连接失败：" + DescribeConnmanError(o.code);
                return o;
            }
            if (res.rfind("failed=", 0) == 0)
            {
                o.code = res.substr(7);
                o.reason = "ConnMan 没有受理连接请求（" + o.code + "）";
                return o;
            }
            // 连接进行到一半掉回 idle：先记下从何时开始，再看它是否持续不恢复
            const bool back_to_idle =
                live.state.empty() || live.state == "idle" || live.state == "disconnect";
            if (!back_to_idle)
                idle_since = -1.0;
            else if (idle_since < 0.0)
                idle_since = elapsed();

            if (live.state == "association")
                phase(kPhaseAssoc);
            else if (live.state == "configuration")
                phase(kPhaseDhcp);
            else
                phase(kPhaseSubmit);

            // 进过 association/configuration 之后又掉回 idle 并持续 3 秒：
            // 这次尝试已经结束了（ConnMan 内部握手重试只会短暂回到 idle）。
            if (saw_connecting && back_to_idle && elapsed() - idle_since > kIdleAfterConnectingSec)
            {
                o.code = live.error;
                o.retry = !live.error.empty() && live.error != "invalid-key";
                o.reason = live.error.empty()
                    ? std::string("连接被中断，热点没有完成这次关联")
                    : "连接失败：" + DescribeConnmanError(live.error);
                return o;
            }

            if (IsServiceConnecting(live.state))
            {
                saw_connecting = true;
            }
            else if (!saw_connecting && back_to_idle && !adopt &&
                     elapsed() - idle_since > kIdleLimitSec)
            {
                // Connect 已经发出却一直不进 association：ConnMan 多半在等密码，
                // 而密码请求没落到我们的 Agent（被别的网络管理界面的 agent 接走）。
                KickDisconnect(svc);
                o.reason = "ConnMan 没有开始关联（密码请求可能被其它网络界面接走）";
                return o;
            }
            usleep(500 * 1000);
        }

        // 超时收尾：ConnMan 自己的连接上限是 120 秒，我们放弃时它那边可能还在跑，
        // 不停掉的话下一次连接会被它挡住。
        KickDisconnect(svc);
        Outcome o;
        if (last_state == "association")
            o.reason = "连接超时：热点没有响应关联，请确认热点在线、信号强度足够";
        else if (last_state == "configuration")
            o.reason = "连接超时：已经关联但没有拿到 IP，请检查热点的 DHCP";
        else
            o.reason = "连接超时：ConnMan 没有发起关联";
        o.reason += "（等待 " + std::to_string(static_cast<long long>(kConnectBudgetSec)) +
                    " 秒，状态经过 " + (seen_states.empty() ? "无" : seen_states.c_str()) + "）";
        return o;
    };

    // 第一次失败若不是密码问题，等 5 秒再试一次（CoreELEC 插件同款）。
    // "已关联后被中断"这类瞬时失败，第二次往往就过了。
    for (int attempt = 1;; ++attempt)
    {
        const Outcome o = run_attempt();
        if (o.ok)
            return succeed("已连接 " + ssid + "  " + Ipv4Of(target));
        if (!o.retry || attempt >= kConnectAttempts)
            return fail(o.reason);
        WLog("attempt " + std::to_string(attempt) + " failed (" + o.code +
             "), retrying in 5s: " + o.reason);
        SetConnectProgress("第一次连接没有成功，5 秒后自动重试…");
        sleep(kRetryDelaySec);
    }
}

void WifiController::StartConnect(const std::string &ssid, const std::string &psk,
                                  const std::string &iface)
{
    if (ssid.empty())
        return;

    {
        std::lock_guard<std::mutex> lk(g_conn_mu);
        g_conn_req.ssid = ssid;
        g_conn_req.psk = psk;
        g_conn_req.iface = iface;
        g_conn_req.pending = true;
    }
    // 已经有线程在连：它收尾时会看到上面的新请求，这里不用再起一个。
    if (g_conn_running.exchange(true))
        return;
    g_conn_done.store(false);
    g_conn_ok.store(false);
    std::thread([]() {
        for (;;)
        {
            std::string ssid, psk, iface;
            {
                std::lock_guard<std::mutex> lk(g_conn_mu);
                if (!g_conn_req.pending)
                    break;
                ssid = g_conn_req.ssid;
                psk = g_conn_req.psk;
                iface = g_conn_req.iface;
                g_conn_req.pending = false;
            }
            std::string msg;
            const bool ok = WifiController::ConnectSync(ssid, psk, iface, &msg);
            {
                std::lock_guard<std::mutex> lk(g_conn_mu);
                g_conn_msg = msg;
            }
            g_conn_ok.store(ok);
            g_conn_done.store(true);
        }
        g_conn_running.store(false);
    }).detach();
}

std::string WifiController::ConnectProgress()
{
    std::lock_guard<std::mutex> lk(g_prog_mu);
    return g_conn_progress;
}

bool WifiController::PollConnect(bool *ok, std::string *message, bool *running)
{
    if (running)
        *running = g_conn_running.load();
    if (!g_conn_done.exchange(false))
        return false;
    if (ok)
        *ok = g_conn_ok.load();
    if (message)
    {
        std::lock_guard<std::mutex> lk(g_conn_mu);
        *message = g_conn_msg;
    }
    return true;
}

WifiStatus WifiController::QueryStatus(const std::string &iface)
{
    WifiStatus st;
    if (iface.empty() || !IsWifiIface(iface))
        return st;

    std::string link = Run("iw dev " + iface + " link");
    if (link.find("Connected to") == std::string::npos)
        return st;

    st.state = WifiState::Connected;
    std::istringstream iss(link);
    std::string line;
    while (std::getline(iss, line))
    {
        size_t p = line.find("SSID:");
        if (p != std::string::npos)
            st.ssid = Trim(line.substr(p + 5));
        p = line.find("signal:");
        if (p != std::string::npos)
        {
            std::string v = Trim(line.substr(p + 7));
            st.signal_dbm = std::atoi(v.c_str());
        }
    }
    st.ip = Ipv4Of(iface);
    return st;
}

void WifiController::AutoConnectOnBoot(int delay_s)
{
    std::thread([delay_s]() {
        if (delay_s > 0)
            sleep(static_cast<unsigned>(delay_s));

        if (IsWfbMode())
        {
            std::fprintf(stdout, "[wifi] WFB mode, skip boot auto-connect\n");
            return;
        }
        // 已经连上就不动（避免打断正常链路）
        // 不再写死 192.168.144.x：只要任意 wlan 拿到了有效 IPv4
        // （非链路本地 169.254.x），就认为已连接。
        for (const auto &n : AllWifiIfaces())
        {
            std::string ip = Ipv4Of(n);
            if (!ip.empty() && ip.rfind("169.254.", 0) != 0)
            {
                std::fprintf(stdout, "[wifi] already connected on %s (%s)\n",
                             n.c_str(), ip.c_str());
                return;
            }
        }
        const std::string ssid = ConfiguredSsid();
        if (ssid.empty())
        {
            std::fprintf(stdout, "[wifi] no configured SSID, skip boot auto-connect\n");
            return;
        }
        std::string msg;
        const bool ok = ConnectSync(ssid, "", "", &msg);
        std::fprintf(stdout, "[wifi] boot auto-connect %s: %s (%s)\n",
                     ok ? "ok" : "failed", ssid.c_str(), msg.c_str());
    }).detach();
}
void WifiController::Disconnect(const std::string &iface)
{
    if (iface.empty() || !IsWifiIface(iface))
        return;
    std::string ssid = SsidOf(iface);
    if (ssid.empty())
        return;
    std::string svc = FindServiceBySsid(ssid);
    if (!svc.empty())
        RunVoid("connmanctl disconnect " + svc);
}
