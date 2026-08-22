#pragma once
#include <vector>
#include <string>
#include <utility>

struct ProcessInfo {
    int pid;
    std::string name;
    double cpu_usage = 0.0;
    unsigned long long ram_kb = 0;
};

struct DriveInfo {
    std::string mount_point;
    std::string label;  // short display name; falls back to mount_point when empty
    unsigned long long total_gb = 0;
    unsigned long long used_gb = 0;
    double percent_used = 0.0;
};

struct SystemMetrics {
    double cpu_load = 0.0;
    double cpu_temp = 0.0;
    double cpu_freq = 0.0;
    double gpu_temp = 0.0;
    double gpu_load = 0.0;
    double current_watts = 0.0;
    double cumulative_kwh = 0.0;
    double accumulated_cost = 0.0;

    unsigned long long total_ram = 0, free_ram = 0, used_ram = 0;
    unsigned long long total_swap = 0, free_swap = 0, used_swap = 0;

    double net_download_kb = 0.0;
    double net_upload_kb = 0.0;
    double disk_read_kb = 0.0;
    double disk_write_kb = 0.0;

    double battery_percent = 100.0;
    std::string uptime_str = "0h 0m";
    std::string cpu_model = "Unknown CPU";
    std::string loadavg_str = "0.00 / 0.00";
    int proc_count = 0;

    std::vector<ProcessInfo> processes;
    std::vector<double> core_loads;
    std::vector<DriveInfo> drives;
};

struct GraphData {
    std::vector<double> watt_history;
    std::vector<double> cpu_load_history;
    std::vector<double> cpu_temp_history;
    std::vector<double> gpu_temp_history;
    std::vector<double> ram_history;
    std::vector<double> net_download_history;
    std::vector<double> net_upload_history;
    std::vector<double> gpu_load_history;
    const size_t max_size = 120;
};

struct Point3D {
    double x, y, z;
};

struct ClipItem {
    int id;
    std::string content;
    long long created_at;
};

struct AIConfig {
    std::string provider = "Ollama";
    std::string api_key = "";
    std::string model = "llama3";
    std::string api_url = "http://localhost:11434/api/chat";
};

struct BenchmarkResults {
    bool cpu_running = false;
    double cpu_single_mops = 0.0;
    double cpu_multi_mops = 0.0;
    double cpu_scaling = 0.0;
    int cpu_threads = 0;

    bool mem_running = false;
    double mem_read_gbps = 0.0;
    double mem_write_gbps = 0.0;
    double mem_copy_gbps = 0.0;

    bool disk_running = false;
    double disk_write_mbps = 0.0;
    double disk_read_mbps = 0.0;

    bool llm_running = false;
    double llm_bw = 0.0;
    std::vector<std::pair<std::string, double>> llm_estimates;

    std::string ollama_status = "Not checked";
    std::string lmstudio_status = "Not checked";

    int progress_pct = 0;
    std::string progress_label = "Idle";
};

enum Theme {
    NEON,
    WIREFRAME,
    DRACULA,
    CARBON,
    SRBIJA
};

constexpr double PRICE_PER_KWH = 0.15;

struct GpuModelPreset {
    std::string name;
    int tdp_watts;
};

inline const std::vector<GpuModelPreset> GPU_PRESETS = {
    {"NVIDIA A100 (400W)", 400},
    {"NVIDIA H100 (700W)", 700},
    {"NVIDIA H200 (700W)", 700},
    {"NVIDIA B200 (1000W)", 1000},
    {"NVIDIA RTX 4090 (450W)", 450},
    {"NVIDIA RTX 3090 (350W)", 350},
    {"Custom TDP", 300}
};

struct GpuCalcState {
    int model_idx = 0;              // 0: A100 (400W)
    int gpu_count = 20000;          // 20,000 GPUs
    int custom_tdp = 400;           // TDP in Watts
    double load_pct = 70.0;         // 70% average load
    double pue = 1.30;              // 1.30 PUE
    double price_kwh = 0.10;        // $0.10 / kWh
    int selected_field = 0;         // 0: Model, 1: Count, 2: Load, 3: PUE, 4: Rate, 5: Presets, 6: Export

    // Direct text box edit buffers
    std::string count_str = "20000";
    std::string custom_tdp_str = "400";
    std::string load_str = "70";
    std::string pue_str = "1.30";
    std::string price_str = "0.10";

    void sync_buffers() {
        count_str = std::to_string(gpu_count);
        custom_tdp_str = std::to_string(custom_tdp);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", load_pct);
        load_str = buf;
        std::snprintf(buf, sizeof(buf), "%.2f", pue);
        pue_str = buf;
        std::snprintf(buf, sizeof(buf), "%.2f", price_kwh);
        price_str = buf;
    }

    int get_tdp() const {
        if (model_idx >= 0 && model_idx < static_cast<int>(GPU_PRESETS.size()) - 1) {
            return GPU_PRESETS[model_idx].tdp_watts;
        }
        return custom_tdp;
    }

    std::string get_model_name() const {
        if (model_idx >= 0 && model_idx < static_cast<int>(GPU_PRESETS.size()) - 1) {
            return GPU_PRESETS[model_idx].name;
        }
        return "Custom (" + std::to_string(custom_tdp) + "W)";
    }

    double get_avg_watts_per_gpu() const {
        return get_tdp() * (load_pct / 100.0);
    }

    double get_total_it_watts() const {
        return gpu_count * get_avg_watts_per_gpu();
    }

    double get_facility_watts() const {
        return get_total_it_watts() * pue;
    }

    double get_hourly_kwh() const {
        return get_facility_watts() / 1000.0;
    }

    double get_daily_kwh() const {
        return get_hourly_kwh() * 24.0;
    }

    double get_yearly_kwh() const {
        return get_hourly_kwh() * 8760.0;
    }

    double get_yearly_cost() const {
        return get_yearly_kwh() * price_kwh;
    }

    double get_monthly_cost() const {
        return get_yearly_cost() / 12.0;
    }

    double get_daily_cost() const {
        return get_yearly_cost() / 365.0;
    }

    double get_hourly_cost() const {
        return get_hourly_kwh() * price_kwh;
    }

    void apply_preset(int p) {
        if (p == 1) {
            // 20K A100 Datacenter Default
            model_idx = 0; // A100
            gpu_count = 20000;
            load_pct = 70.0;
            pue = 1.30;
            price_kwh = 0.10;
        } else if (p == 2) {
            // Home Inference Workstation
            model_idx = 4; // RTX 4090 (450W)
            gpu_count = 2;
            load_pct = 60.0;
            pue = 1.00;
            price_kwh = 0.15;
        } else if (p == 3) {
            // 8x H100 HGX Server
            model_idx = 1; // H100 (700W)
            gpu_count = 8;
            load_pct = 80.0;
            pue = 1.20;
            price_kwh = 0.10;
        } else if (p == 4) {
            // 1K H100 AI Cluster
            model_idx = 1; // H100 (700W)
            gpu_count = 1000;
            load_pct = 75.0;
            pue = 1.25;
            price_kwh = 0.08;
        }
        sync_buffers();
    }
};
