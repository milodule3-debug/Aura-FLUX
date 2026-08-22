#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <thread>
#include <cctype>
#include <cerrno>
#include "types.h"

namespace fs = std::filesystem;

// --- PLATFORM DETECTION ---
#if defined(_WIN32) || defined(_WIN64)
    #define PLATFORM_WINDOWS
#elif defined(__APPLE__)
    #define PLATFORM_MACOS
#elif defined(__linux__)
    #define PLATFORM_LINUX
#else
    #define PLATFORM_UNKNOWN
#endif

// --- PLATFORM-SPECIFIC HEADERS ---
#ifdef PLATFORM_LINUX
    #include <dirent.h>
    #include <sys/statvfs.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <signal.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

#ifdef PLATFORM_MACOS
    #include <sys/types.h>
    #include <sys/sysctl.h>
    #include <sys/mount.h>
#endif

// --- CROSS-PLATFORM STUBS (Windows/macOS) ---
#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_MACOS) || defined(PLATFORM_UNKNOWN)

inline std::string get_power_profile() { return "balanced"; }
inline void set_power_profile(const std::string& profile) { (void)profile; }
inline bool get_cpu_boost() { return false; }
inline bool set_cpu_boost_privileged(bool on) { (void)on; return false; }
inline int get_swappiness() { return 60; }
inline bool set_swappiness_privileged(int val) { (void)val; return false; }
inline std::string run_balance_cores_privileged() { return "Not supported on this platform"; }
inline bool run_drop_caches_privileged() { return false; }

#endif

// --- SYSTEM TUNING PRIVILEGE MODULES (Linux only) ---
#ifdef PLATFORM_LINUX
inline std::string get_power_profile() {
    int pipefd[2];
    if (pipe(pipefd) == -1) return "balanced";

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return "balanced";
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }

        execlp("powerprofilesctl", "powerprofilesctl", "get", nullptr);
        _exit(1);
    }

    close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0) {
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    }

    std::string res;
    char buf[64];
    auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(250);

    while (true) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > timeout) break;

        ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            res += buf;
            if (res.find('\n') != std::string::npos) break;
        } else if (n == 0) {
            break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        } else {
            break;
        }
    }

    close(pipefd[0]);

    // Reap the child with a bounded retry loop.  The old code called
    // waitpid(WNOHANG) then kill(SIGKILL) then waitpid(0) — if the child
    // was already reaped by the first call the second waitpid could block
    // indefinitely on certain kernels / under D-Bus stalls, starving the
    // render thread.  We now retry WNOHANG a bounded number of times.
    int status = 0;
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == 0) {
        kill(pid, SIGKILL);
        for (int retry = 0; retry < 50; ++retry) {
            w = waitpid(pid, &status, WNOHANG);
            if (w != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // Last resort: blocking reap (child is SIGKILL'd, so this is near-instant).
        if (w == 0) waitpid(pid, &status, 0);
    }

    while (!res.empty() && std::isspace(static_cast<unsigned char>(res.back()))) res.pop_back();
    if (res == "balanced" || res == "performance" || res == "power-saver") return res;
    return "balanced";
}

inline void set_power_profile(const std::string& profile) {
    std::string cmd = "powerprofilesctl set " + profile + " 2>/dev/null";
    int status = std::system(cmd.c_str());
    (void)status;
}

inline bool get_cpu_boost() {
    std::ifstream f("/sys/devices/system/cpu/cpufreq/boost");
    if (!f.is_open()) return false;
    char c; f >> c;
    return c == '1';
}

inline bool set_cpu_boost_privileged(bool on) {
    std::string val = on ? "1" : "0";
    std::string cmd = "pkexec sh -c 'echo " + val + " > /sys/devices/system/cpu/cpufreq/boost' 2>/dev/null";
    int status = std::system(cmd.c_str());
    return status == 0;
}

inline int get_swappiness() {
    std::ifstream f("/proc/sys/vm/swappiness");
    int val = 60;
    if (f >> val) return val;
    return 60;
}

inline bool set_swappiness_privileged(int val) {
    std::string cmd = "pkexec sh -c 'echo " + std::to_string(val) + " > /proc/sys/vm/swappiness' 2>/dev/null";
    int status = std::system(cmd.c_str());
    return status == 0;
}

inline std::string run_balance_cores_privileged() {
    const char* SCRIPT =
        "onlined=0;"
        "for f in /sys/devices/system/cpu/cpu*/online; do "
        "  [ -f \"$f\" ] || continue;"
        "  if [ \"$(cat \"$f\")\" = \"0\" ]; then echo 1 > \"$f\" && onlined=$((onlined+1)); fi;"
        "done;"
        "gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo none);"
        "if [ \"$gov\" != \"none\" ]; then "
        "  for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo \"$gov\" > \"$g\" 2>/dev/null || true; done;"
        "fi;"
        "autogroup=off;"
        "if [ -f /proc/sys/kernel/sched_autogroup_enabled ]; then "
        "  echo 1 > /proc/sys/kernel/sched_autogroup_enabled && autogroup=on;"
        "fi;"
        "irq=absent;"
        "if command -v irqbalance >/dev/null 2>&1; then "
        "  irqbalance --oneshot >/dev/null 2>&1 && irq=rebalanced || irq=failed;"
        "fi;"
        "echo \"cores onlined: $onlined | governor: $gov (all cores) | autogroup: $autogroup | IRQ spread: $irq\"";

    std::string cmd = "pkexec sh -c '" + std::string(SCRIPT) + "'";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "Authorization failed or command error";
    char buf[256];
    std::string res;
    while (fgets(buf, sizeof(buf), p)) res += buf;
    int status = pclose(p);
    if (status != 0) return "Authorization failed";
    return res;
}

inline bool run_drop_caches_privileged() {
    std::string cmd = "pkexec sh -c 'sync && echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null";
    int status = std::system(cmd.c_str());
    return status == 0;
}

#endif // PLATFORM_LINUX

// --- CROSS-PLATFORM STUB HARDWARE SCANNER (Windows/macOS) ---
#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_MACOS) || defined(PLATFORM_UNKNOWN)

class AdvancedHardwareScanner {
public:
    AdvancedHardwareScanner() {}

    void update_all_metrics(SystemMetrics& m, double dt_sec) {
        (void)dt_sec;
        // Return safe dummy values for non-Linux platforms
        m.cpu_load = 0.0;
        m.cpu_temp = 45.0;
        m.cpu_freq = 2.4;
        m.gpu_temp = 40.0;
        m.gpu_load = 0.0;
        m.current_watts = 15.0;
        m.cumulative_kwh = 0.0;
        m.accumulated_cost = 0.0;
        m.total_ram = 16384;
        m.free_ram = 8192;
        m.used_ram = 8192;
        m.total_swap = 2048;
        m.free_swap = 2048;
        m.used_swap = 0;
        m.net_download_kb = 0.0;
        m.net_upload_kb = 0.0;
        m.disk_read_kb = 0.0;
        m.disk_write_kb = 0.0;
        m.battery_percent = 100.0;
        m.uptime_str = "0h 0m";
        m.cpu_model = "Unknown CPU";
        m.loadavg_str = "0.00 / 0.00";
        m.proc_count = 0;
        m.core_loads.clear();
        m.drives.clear();
        m.processes.clear();
    }
};

#endif

// --- LINUX HARDWARE SCANNER ---
#ifdef PLATFORM_LINUX

// --- HARDWARE SCANNER ---
class AdvancedHardwareScanner {
private:
    unsigned long long last_user = 0, last_user_low = 0, last_sys = 0, last_idle = 0;
    unsigned long long last_net_rx = 0, last_net_tx = 0;
    unsigned long long last_disk_r = 0, last_disk_w = 0;
    std::string amdgpu_hwmon_path;
    std::string cpu_hwmon_path;
    std::string gpu_type;

    struct CoreStat {
        unsigned long long last_user = 0;
        unsigned long long last_user_low = 0;
        unsigned long long last_sys = 0;
        unsigned long long last_idle = 0;
    };
    std::vector<CoreStat> last_cores;

    bool read_sysfs_double(const std::string& path, double& out_val) {
        std::ifstream f(path);
        if (!f.is_open()) return false;
        double raw;
        if (f >> raw) {
            out_val = raw;
            return true;
        }
        return false;
    }

    void find_gpu_hwmon() {
        std::error_code ec;
        if (!fs::exists("/sys/class/hwmon", ec)) return;
        for (const auto& entry : fs::directory_iterator("/sys/class/hwmon", ec)) {
            if (ec) break;
            std::string path = entry.path().string();
            std::ifstream name_file(path + "/name");
            std::string name;
            if (name_file >> name) {
                if (name == "amdgpu" || name == "radeon") {
                    amdgpu_hwmon_path = path;
                    gpu_type = name;
                } else if (name == "k10temp" || name == "zenpower" || name == "coretemp") {
                    cpu_hwmon_path = path;
                }
            }
        }
    }

    DriveInfo get_drive_info(const std::string& mount_point, const std::string& label = "") {
        struct statvfs stat;
        DriveInfo d;
        d.mount_point = mount_point;
        d.label = label;
        if (statvfs(mount_point.c_str(), &stat) == 0) {
            double total = static_cast<double>(stat.f_blocks) * stat.f_frsize;
            double free = static_cast<double>(stat.f_bavail) * stat.f_frsize;
            double used = total - free;
            d.total_gb = total / (1024.0 * 1024.0 * 1024.0);
            d.used_gb = used / (1024.0 * 1024.0 * 1024.0);
            d.percent_used = d.total_gb > 0 ? (static_cast<double>(d.used_gb) / d.total_gb) * 100.0 : 0.0;
        }
        return d;
    }

    std::string cached_cpu_model;

    void read_cpu_model() {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("model name", 0) == 0) {
                size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string model = line.substr(colon + 1);
                    while (!model.empty() && std::isspace(static_cast<unsigned char>(model.front()))) model.erase(0, 1);
                    // Trim marketing noise for a compact sidebar label
                    for (const char* junk : {" with Radeon Graphics", "(R)", "(TM)", " CPU", " Processor"}) {
                        size_t p;
                        while ((p = model.find(junk)) != std::string::npos) model.erase(p, std::string(junk).size());
                    }
                    if (model.size() > 22) model = model.substr(0, 22);
                    cached_cpu_model = model;
                }
                break;
            }
        }
        if (cached_cpu_model.empty()) cached_cpu_model = "Unknown CPU";
    }

public:
    AdvancedHardwareScanner() {
        find_gpu_hwmon();
        read_cpu_model();
    }

    void update_all_metrics(SystemMetrics& m, double dt_sec) {
        if (dt_sec <= 0.0) return;

        // 1. CPU Load
        std::ifstream stat_file("/proc/stat");
        std::string label;
        if (stat_file >> label && label == "cpu") {
            unsigned long long u, ul, s, id;
            stat_file >> u >> ul >> s >> id;
            unsigned long long tot_diff = (u - last_user) + (ul - last_user_low) + (s - last_sys);
            unsigned long long id_diff = id - last_idle;
            unsigned long long total = tot_diff + id_diff;
            last_user = u; last_user_low = ul; last_sys = s; last_idle = id;
            m.cpu_load = total > 0 ? (static_cast<double>(tot_diff) / total) * 100.0 : 0.0;
        }
        stat_file.close();

        // 2. Individual Core Loads
        std::ifstream stat_cores("/proc/stat");
        std::string s_line;
        int core_idx = 0;
        m.core_loads.clear();
        while (std::getline(stat_cores, s_line)) {
            if (s_line.rfind("cpu", 0) == 0) {
                if (s_line.size() > 3 && s_line[3] >= '0' && s_line[3] <= '9') {
                    std::stringstream ss(s_line);
                    std::string core_label;
                    unsigned long long u, ul, s, id;
                    ss >> core_label >> u >> ul >> s >> id;

                    if (core_idx >= static_cast<int>(last_cores.size())) {
                        last_cores.push_back({0, 0, 0, 0});
                    }
                    auto& lc = last_cores[core_idx];
                    unsigned long long tot_diff = (u - lc.last_user) + (ul - lc.last_user_low) + (s - lc.last_sys);
                    unsigned long long id_diff = id - lc.last_idle;
                    unsigned long long total = tot_diff + id_diff;

                    lc.last_user = u; lc.last_user_low = ul; lc.last_sys = s; lc.last_idle = id;
                    double core_load = total > 0 ? (static_cast<double>(tot_diff) / total) * 100.0 : 0.0;
                    m.core_loads.push_back(core_load);
                    core_idx++;
                }
            }
        }
        stat_cores.close();

        // 3. CPU Frequency
        double freq_khz = 0.0;
        m.cpu_freq = 3.82;
        if (read_sysfs_double("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", freq_khz)) {
            m.cpu_freq = freq_khz / 1000000.0;
        }

        // 4. Uptime
        std::ifstream uptime_file("/proc/uptime");
        double seconds = 0.0;
        m.uptime_str = "1d 4h 47m";
        if (uptime_file >> seconds) {
            int days = static_cast<int>(seconds) / 86400;
            int hours = (static_cast<int>(seconds) % 86400) / 3600;
            int mins = (static_cast<int>(seconds) % 3600) / 60;
            if (days > 0) {
                m.uptime_str = std::to_string(days) + "d " + std::to_string(hours) + "h " + std::to_string(mins) + "m";
            } else {
                m.uptime_str = std::to_string(hours) + "h " + std::to_string(mins) + "m";
            }
        }
        uptime_file.close();

        // CPU Temperature
        double cpu_temp_val = 0.0;
        bool has_cpu_temp = false;
        if (!cpu_hwmon_path.empty()) {
            if (read_sysfs_double(cpu_hwmon_path + "/temp1_input", cpu_temp_val) ||
                read_sysfs_double(cpu_hwmon_path + "/temp2_input", cpu_temp_val)) {
                m.cpu_temp = cpu_temp_val / 1000.0;
                has_cpu_temp = true;
            }
        }
        if (!has_cpu_temp) {
            std::ifstream temp_file("/sys/class/thermal/thermal_zone0/temp");
            double t_raw;
            if (temp_file >> t_raw) {
                m.cpu_temp = t_raw / 1000.0;
                has_cpu_temp = true;
            }
            temp_file.close();
        }

        // GPU Status & Power Telemetry
        double g_temp = 0.0;
        bool has_g_temp = false;
        if (!amdgpu_hwmon_path.empty()) {
            double temp_val;
            if (read_sysfs_double(amdgpu_hwmon_path + "/temp1_input", temp_val)) {
                g_temp = temp_val / 1000.0;
                has_g_temp = true;
            }
        }
        m.gpu_temp = has_g_temp ? g_temp : m.cpu_temp * 0.9;

        double g_watts = 0.0;
        bool has_g_watts = false;
        if (!amdgpu_hwmon_path.empty()) {
            double gw_raw;
            if (read_sysfs_double(amdgpu_hwmon_path + "/power1_average", gw_raw) ||
                read_sysfs_double(amdgpu_hwmon_path + "/power1_input", gw_raw)) {
                g_watts = gw_raw / 1000000.0;
                has_g_watts = true;
            }
        }

        double cpu_package_watts = 0.0;
        bool has_cpu_package_watts = false;
        if (!cpu_hwmon_path.empty()) {
            double cp_raw;
            if (read_sysfs_double(cpu_hwmon_path + "/power1_input", cp_raw) ||
                read_sysfs_double(cpu_hwmon_path + "/power1_average", cp_raw)) {
                cpu_package_watts = cp_raw / 1000000.0;
                has_cpu_package_watts = true;
            }
        }

        if (!has_cpu_package_watts) {
            double energy_uj = 0.0;
            if (read_sysfs_double("/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj", energy_uj)) {
                static double last_energy = 0.0;
                static auto last_rapl_time = std::chrono::high_resolution_clock::now();
                auto now = std::chrono::high_resolution_clock::now();
                double dt = std::chrono::duration<double>(now - last_rapl_time).count();
                if (last_energy > 0.0 && dt > 0.0 && energy_uj >= last_energy) {
                    cpu_package_watts = (energy_uj - last_energy) / 1000000.0 / dt;
                    has_cpu_package_watts = true;
                }
                last_energy = energy_uj;
                last_rapl_time = now;
            }
        }

        if (has_g_watts && has_cpu_package_watts) {
            m.current_watts = g_watts + cpu_package_watts;
        } else if (has_g_watts) {
            m.current_watts = g_watts + (m.cpu_load * 0.25 + 10.0);
        } else if (has_cpu_package_watts) {
            m.current_watts = cpu_package_watts + 2.0;
        } else {
            m.current_watts = (m.cpu_load * 0.35) + 12.0;
        }

        // Memory & Swap space
        std::ifstream mem_file("/proc/meminfo");
        std::string key; unsigned long long val;
        unsigned long long mem_free = 0, mem_avail = 0;
        while (mem_file >> key >> val) {
            if (key == "MemTotal:") m.total_ram = val / 1024;
            else if (key == "MemFree:") mem_free = val / 1024;
            else if (key == "MemAvailable:") mem_avail = val / 1024;
            else if (key == "SwapTotal:") m.total_swap = val / 1024;
            else if (key == "SwapFree:") m.free_swap = val / 1024;
            std::string dummy; mem_file >> dummy;
        }
        m.free_ram = mem_avail > 0 ? mem_avail : mem_free;
        m.used_ram = m.total_ram - m.free_ram;
        m.used_swap = m.total_swap - m.free_swap;
        mem_file.close();

        // Network tracking
        std::ifstream net_file("/proc/net/dev");
        std::string line;
        unsigned long long total_rx = 0, total_tx = 0;
        std::getline(net_file, line); std::getline(net_file, line);
        while (std::getline(net_file, line)) {
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                unsigned long long rx = 0, tx = 0, dummy;
                std::stringstream ss(line.substr(colon + 1));
                ss >> rx >> dummy >> dummy >> dummy >> dummy >> dummy >> dummy >> dummy >> tx;
                total_rx += rx; total_tx += tx;
            }
        }
        if (last_net_rx > 0) {
            m.net_download_kb = ((total_rx - last_net_rx) / 1024.0) / dt_sec;
            m.net_upload_kb = ((total_tx - last_net_tx) / 1024.0) / dt_sec;
        }
        last_net_rx = total_rx; last_net_tx = total_tx;
        net_file.close();

        // Disk activity parsing
        std::ifstream disk_file("/proc/diskstats");
        unsigned long long r_sectors = 0, w_sectors = 0;
        while (disk_file >> label >> label >> label) {
            unsigned long long r_sec = 0, w_sec = 0, d;
            disk_file >> d >> d >> r_sec >> d >> d >> d >> w_sec;
            r_sectors += r_sec; w_sectors += w_sec;
            std::getline(disk_file, line);
        }
        if (last_disk_r > 0) {
            m.disk_read_kb = (((r_sectors - last_disk_r) * 512) / 1024.0) / dt_sec;
            m.disk_write_kb = (((w_sectors - last_disk_w) * 512) / 1024.0) / dt_sec;
        }
        last_disk_r = r_sectors; last_disk_w = w_sectors;
        disk_file.close();

        // Load average + CPU model label
        m.cpu_model = cached_cpu_model;
        std::ifstream loadavg_file("/proc/loadavg");
        double l1 = 0.0, l5 = 0.0;
        if (loadavg_file >> l1 >> l5) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.2f / %.2f", l1, l5);
            m.loadavg_str = buf;
        }

        // Battery Percent
        double bat_percent = 80.0;
        m.battery_percent = 80.0;
        if (read_sysfs_double("/sys/class/power_supply/BAT0/capacity", bat_percent) ||
            read_sysfs_double("/sys/class/power_supply/BAT1/capacity", bat_percent)) {
            m.battery_percent = bat_percent;
        }

        // Active partitions querying
        m.drives.clear();
        m.drives.push_back(get_drive_info("/", "root"));
        struct statvfs s_stat;
        struct MountSpec { const char* path; const char* label; };
        const MountSpec extra_mounts[] = {
            { "/boot/efi", "efi" },
            { "/mnt/bigdata", "bigdata" },
            { "/run/media/dusan/DATA1", "DATA1" },
            { "/mnt/usb-General_UDisk-0:0-part1", "USB 2T" },
        };
        for (const auto& mnt : extra_mounts) {
            if (statvfs(mnt.path, &s_stat) == 0 && s_stat.f_blocks > 0) {
                m.drives.push_back(get_drive_info(mnt.path, mnt.label));
            }
        }

        // Process mapping matrix
        m.processes.clear();
        m.proc_count = 0;
        DIR* dir = opendir("/proc");
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir))) {
                if (entry->d_type == DT_DIR && isdigit(entry->d_name[0])) {
                    int pid = atoi(entry->d_name);
                    m.proc_count++;
                    std::ifstream p_stat("/proc/" + std::to_string(pid) + "/stat");
                    std::string p_name; int p_pid;
                    if (p_stat >> p_pid >> p_name) {
                        if (!p_name.empty() && p_name.front() == '(') p_name.erase(0, 1);
                        if (!p_name.empty() && p_name.back() == ')') p_name.pop_back();

                        unsigned long long utime, stime;
                        for (int i = 0; i < 11; ++i) { std::string d; p_stat >> d; }
                        p_stat >> utime >> stime;

                        std::ifstream p_status("/proc/" + std::to_string(pid) + "/status");
                        std::string s_line_stat; unsigned long long rss = 0;
                        while (p_status >> s_line_stat) {
                            if (s_line_stat == "VmRSS:") { p_status >> rss; break; }
                        }
                        m.processes.push_back({pid, p_name, (static_cast<double>(utime + stime) * 0.01), rss});
                    }
                }
            }
            closedir(dir);
            std::sort(m.processes.begin(), m.processes.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
                return a.ram_kb > b.ram_kb;
            });
            if (m.processes.size() > 10) m.processes.resize(10);
        }
    }
};

#endif // PLATFORM_LINUX
