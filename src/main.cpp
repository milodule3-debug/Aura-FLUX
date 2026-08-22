#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <csignal>
#include <ctime>
#include <unistd.h>
#include <mutex>
#include <atomic>
#include <sstream>
#include <sqlite3.h>

#include <ftxui/screen/terminal.hpp>
#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/canvas.hpp"

#include "types.h"
#include "utils.h"
#include "datalogger.h"
#include "clipboard.h"
#include "ai.h"
#include "benchmarks.h"
#include "scanner.h"
#include "tokens.h"

using namespace ftxui;
using namespace std;
namespace fs = std::filesystem;

// --- SYSTEM MUTEXES FOR SAFE THREAD CONCURRENCY ---
std::mutex vault_mutex;
std::mutex bench_mutex;

// Collects all spawned threads for clean shutdown
static std::vector<std::thread> bg_threads;

int main() {
    // Force 24-bit color: launched from the app menu COLORTERM may be unset,
    // making FTXUI fall back to the washed-out 256-color palette.
    Terminal::SetColorSupport(Terminal::Color::TrueColor);
    auto screen = ScreenInteractive::TerminalOutput();
    SystemMetrics metrics;
    GraphData graph;
    AdvancedHardwareScanner scanner;
    DataLogger logger;

    // Persistently declared canvases to avoid stack-use-after-scope
    Canvas cpu_canvas(10, 10);
    Canvas memory_canvas(10, 10);
    Canvas power_canvas(10, 10);
    Canvas thermals_canvas(10, 10);
    Canvas network_canvas(10, 10);
    Canvas gpu_canvas(10, 10);
    Canvas globe_canvas(56, 56);

    // Dynamic core count detection
    unsigned int threads = thread::hardware_concurrency();
    metrics.core_loads.assign(threads > 0 ? threads : 16, 0.0);

    // Initial synchronous seed call
    scanner.update_all_metrics(metrics, 0.1);

    std::mutex metrics_mutex;
    int selected_proc_idx = 0;
    Theme current_theme = NEON;

    // --- NEW MULTI-TAB NAVIGATION AND AUXILIARY STATES ---
    std::atomic<int> current_tab{0};
    BenchmarkResults bench_res;
    AIConfig ai_config = load_ai_config();
    std::mutex ai_mutex;

    struct OptimizationStatus {
        std::string power_profile = "balanced";
        bool cpu_boost = false;
        int swappiness = 60;
    };
    OptimizationStatus opt_status;
    std::mutex opt_mutex;
    opt_status.power_profile = get_power_profile();
    opt_status.cpu_boost = get_cpu_boost();
    opt_status.swappiness = get_swappiness();

    // SQLite Database initialisation
    sqlite3* vault_db = nullptr;
    string last_seen_clip;
    {
        const char* home_c = getenv("HOME");
        string home_dir = home_c ? home_c : "/tmp";
        fs::create_directories(home_dir + "/.local/share/powerboard");
        string db_path = home_dir + "/.local/share/powerboard/vault.db";
        if (sqlite3_open(db_path.c_str(), &vault_db) == SQLITE_OK) {
            const char* sql = "CREATE TABLE IF NOT EXISTS clips (id INTEGER PRIMARY KEY AUTOINCREMENT, content TEXT UNIQUE, created_at INTEGER NOT NULL);";
            sqlite3_exec(vault_db, sql, nullptr, nullptr, nullptr);
            auto clips_init = get_clips(vault_db);
            if (clips_init.empty()) {
                add_clip(vault_db, "20K A100 GPU Cluster: 20,000 GPUs @ 280W (70% of 400W TDP) = 5.6MW IT draw. PUE 1.3 -> 7.28MW facility total. Yearly: 63.78M kWh @ $0.10/kWh = $6,378,000/year ($6.4M). Sources: NVIDIA A100 Spec, Uptime Institute PUE, U.S. EIA.");
            }
        }
    }

    int selected_bench_menu = 0;
    int selected_opt_menu = 0;
    int selected_vault_idx = 0;
    int selected_ai_menu = 0;
    int vault_focus_pane = 1; // 0: Vault (Top Left), 1: Live Calculator (Bottom Left), 2: AI Settings (Right)
    GpuCalcState gpu_calc;
    bool calc_first_digit = true;
    string custom_question;
    string ai_response_text = "Select a clipboard item on the left, configure your AI provider on the right, or use the live GPU datacenter calculator.";
    bool ai_thinking = false;

    // Pre-generate 180 points on a Fibonacci 3D sphere for the live spinning globe
    vector<Point3D> globe_points;
    for (int i = 0; i < 180; ++i) {
        double phi = acos(1.0 - 2.0 * (i + 0.5) / 180.0);
        double theta = M_PI * (1.0 + sqrt(5.0)) * i;
        globe_points.push_back({sin(phi) * cos(theta), sin(phi) * sin(theta), cos(phi)});
    }

    struct GlobeConnection {
        size_t i, j;
    };
    vector<GlobeConnection> globe_connections;
    for (size_t i = 0; i < globe_points.size(); ++i) {
        for (size_t j = i + 1; j < globe_points.size(); ++j) {
            double dx = globe_points[i].x - globe_points[j].x;
            double dy = globe_points[i].y - globe_points[j].y;
            double dz = globe_points[i].z - globe_points[j].z;
            double dist = sqrt(dx*dx + dy*dy + dz*dz);
            if (dist < 0.28) {
                globe_connections.push_back({i, j});
            }
        }
    }

    // Pink satellites orbiting the globe: radius in globe units (surface = 1.0),
    // speed as multiple of globe spin, phase offset, and orbital-plane tilt.
    struct SatelliteSpec { double radius, speed, phase, tilt; };
    const SatelliteSpec sat_specs[3] = {
        {1.30, 1.7, 0.0, 0.45},
        {1.18, 2.6, 2.1, 1.15},
        {1.42, 1.1, 4.4, 2.05},
    };

    double globe_angle_y = 0.0;
    double globe_angle_x = 0.0;

    auto component = Container::Vertical({});
    component = CatchEvent(component, [&](Event event) {
        if (event == Event::Custom) return true;
        string input = event.input();

        // Detect if user is actively typing in a text field (Tab 3 has its own dedicated input handlers)
        bool in_text_input = (current_tab.load() == 3);

        // Global Tab Navigation — use FTXUI event constants for function keys
        if (event == Event::F1) { current_tab.store(0); return true; }
        if (event == Event::F2) { current_tab.store(1); return true; }
        if (event == Event::F3) { current_tab.store(2); return true; }
        if (event == Event::F4) { current_tab.store(3); return true; }
        if ((input == "[" || input == "{") && !in_text_input) {
            int old_tab = current_tab.load();
            current_tab.store((old_tab + 3) % 4);
            return true;
        }
        if ((input == "]" || input == "}") && !in_text_input) {
            int old_tab = current_tab.load();
            current_tab.store((old_tab + 1) % 4);
            return true;
        }

        // Quit only when NOT actively typing in a text field
        if ((input == "q" || input == "\x1B") && !in_text_input) { screen.ExitLoopClosure()(); return true; }

        // --- TAB 0: DIAGNOSTICS CONTROL ---
        if (current_tab == 0) {
            if (event == Event::ArrowDown) {
                selected_proc_idx = min(9, selected_proc_idx + 1);
                return true;
            }
            if (event == Event::ArrowUp) {
                selected_proc_idx = max(0, selected_proc_idx - 1);
                return true;
            }
            if (input == "k" || input == "K") {
                int pid_to_kill = -1;
                {
                    std::lock_guard<std::mutex> lock(metrics_mutex);
                    if (selected_proc_idx < static_cast<int>(metrics.processes.size())) {
                        pid_to_kill = metrics.processes[selected_proc_idx].pid;
                    }
                }
                if (pid_to_kill != -1) {
                    kill(pid_to_kill, SIGKILL);
                }
                return true;
            }
        }
        // --- TAB 1: BENCHMARKS CONTROL ---
        else if (current_tab == 1) {
            if (event == Event::ArrowDown) {
                selected_bench_menu = min(4, selected_bench_menu + 1);
                return true;
            }
            if (event == Event::ArrowUp) {
                selected_bench_menu = max(0, selected_bench_menu - 1);
                return true;
            }
            if (event == Event::Return) {
                std::lock_guard<std::mutex> lk(bench_mutex);
                if (selected_bench_menu == 0 && !bench_res.cpu_running) {
                    bg_threads.emplace_back([&bench_res, &screen]() {
                        run_cpu_test(bench_res, screen, &bench_mutex);
                    });
                } else if (selected_bench_menu == 1 && !bench_res.mem_running) {
                    bg_threads.emplace_back([&bench_res, &screen]() {
                        run_mem_test(bench_res, screen, &bench_mutex);
                    });
                } else if (selected_bench_menu == 2 && !bench_res.disk_running) {
                    bg_threads.emplace_back([&bench_res, &screen]() {
                        run_disk_test(bench_res, screen, &bench_mutex);
                    });
                } else if (selected_bench_menu == 3 && !bench_res.llm_running) {
                    bg_threads.emplace_back([&bench_res, &screen]() {
                        run_llm_test(bench_res, screen, &bench_mutex);
                    });
                } else if (selected_bench_menu == 4) {
                    bg_threads.emplace_back([&bench_res, &screen]() {
                        probe_local_servers(bench_res, screen, &bench_mutex);
                    });
                }
                return true;
            }
        }
        // --- TAB 2: SYSTEM OPTIMIZATION CONTROL ---
        else if (current_tab == 2) {
            if (event == Event::ArrowDown) {
                selected_opt_menu = min(7, selected_opt_menu + 1);
                return true;
            }
            if (event == Event::ArrowUp) {
                selected_opt_menu = max(0, selected_opt_menu - 1);
                return true;
            }
            if (event == Event::Return) {
                if (selected_opt_menu == 0) {
                    bg_threads.emplace_back([]() { set_power_profile("balanced"); });
                } else if (selected_opt_menu == 1) {
                    bg_threads.emplace_back([]() { set_power_profile("performance"); });
                } else if (selected_opt_menu == 2) {
                    bg_threads.emplace_back([]() { set_power_profile("power-saver"); });
                } else if (selected_opt_menu == 3) {
                    bool current_boost = get_cpu_boost();
                    bg_threads.emplace_back([current_boost]() { set_cpu_boost_privileged(!current_boost); });
                } else if (selected_opt_menu == 4) {
                    int sw = get_swappiness();
                    bg_threads.emplace_back([sw]() { set_swappiness_privileged(min(100, sw + 10)); });
                } else if (selected_opt_menu == 5) {
                    int sw = get_swappiness();
                    bg_threads.emplace_back([sw]() { set_swappiness_privileged(max(0, sw - 10)); });
                } else if (selected_opt_menu == 6) {
                    bg_threads.emplace_back([&screen]() {
                        run_balance_cores_privileged();
                        safe_post_event(screen);
                    });
                } else if (selected_opt_menu == 7) {
                    bg_threads.emplace_back([&screen]() {
                        run_drop_caches_privileged();
                        safe_post_event(screen);
                    });
                }
                return true;
            }
        }
        // --- TAB 3: CLIPBOARD VAULT & AI ENRICHMENT & LIVE GPU CALCULATOR ---
        else if (current_tab == 3) {
            // Global pane navigation (Tab and Shift-Tab exclusively)
            if (event == Event::Tab || input == "\t") {
                vault_focus_pane = (vault_focus_pane + 1) % 3;
                calc_first_digit = true;
                return true;
            }
            if (event == Event::TabReverse) {
                vault_focus_pane = (vault_focus_pane + 2) % 3;
                calc_first_digit = true;
                return true;
            }

            // PANE 0: CLIPBOARD VAULT (Left Top)
            if (vault_focus_pane == 0) {
                if (event == Event::ArrowDown) {
                    if (selected_vault_idx == 14) {
                        vault_focus_pane = 1;
                        calc_first_digit = true;
                    } else {
                        selected_vault_idx = min(14, selected_vault_idx + 1);
                    }
                    return true;
                }
                if (event == Event::ArrowUp) {
                    selected_vault_idx = max(0, selected_vault_idx - 1);
                    return true;
                }
                if (event == Event::Return) {
                    std::lock_guard<std::mutex> lk(vault_mutex);
                    auto clips = get_clips(vault_db);
                    if (selected_vault_idx < static_cast<int>(clips.size())) {
                        write_clipboard(clips[selected_vault_idx].content);
                        last_seen_clip = clips[selected_vault_idx].content;
                    }
                    return true;
                }
                if (input == "d" || input == "D") {
                    std::lock_guard<std::mutex> lk(vault_mutex);
                    auto clips = get_clips(vault_db);
                    if (selected_vault_idx < static_cast<int>(clips.size())) {
                        delete_clip(vault_db, clips[selected_vault_idx].id);
                    }
                    return true;
                }
            }
            // PANE 1: LIVE GPU POWER & COST CALCULATOR (Left Bottom)
            else if (vault_focus_pane == 1) {
                if (event == Event::ArrowDown) {
                    gpu_calc.selected_field = min(6, gpu_calc.selected_field + 1);
                    calc_first_digit = true;
                    return true;
                }
                if (event == Event::ArrowUp) {
                    if (gpu_calc.selected_field == 0) {
                        vault_focus_pane = 0;
                    } else {
                        gpu_calc.selected_field = max(0, gpu_calc.selected_field - 1);
                    }
                    calc_first_digit = true;
                    return true;
                }

                // Preset direct hotkeys 1-4
                if (gpu_calc.selected_field == 0 || gpu_calc.selected_field == 5 || gpu_calc.selected_field == 6) {
                    if (input == "1" || input == "2" || input == "3" || input == "4") {
                        gpu_calc.apply_preset(input[0] - '0');
                        calc_first_digit = true;
                        return true;
                    }
                }

                bool is_inc = (event == Event::ArrowRight || input == "+" || input == "=");
                bool is_dec = (event == Event::ArrowLeft || input == "-" || input == "_");

                if (gpu_calc.selected_field == 0) {
                    // GPU Model selection
                    if (is_inc || event == Event::Return) {
                        gpu_calc.model_idx = (gpu_calc.model_idx + 1) % GPU_PRESETS.size();
                        gpu_calc.sync_buffers();
                        return true;
                    } else if (is_dec) {
                        gpu_calc.model_idx = (gpu_calc.model_idx + static_cast<int>(GPU_PRESETS.size()) - 1) % GPU_PRESETS.size();
                        gpu_calc.sync_buffers();
                        return true;
                    }
                }
                else if (gpu_calc.selected_field == 1) {
                    // GPU Count - direct typing / backspace / stepping
                    if (input.size() == 1 && std::isdigit(input[0])) {
                        if (calc_first_digit || gpu_calc.count_str == "0") {
                            gpu_calc.count_str.clear();
                            calc_first_digit = false;
                        }
                        gpu_calc.count_str += input[0];
                        if (gpu_calc.count_str.size() > 7) gpu_calc.count_str = gpu_calc.count_str.substr(0, 7);
                        gpu_calc.gpu_count = std::max(1, std::atoi(gpu_calc.count_str.c_str()));
                        return true;
                    } else if (event == Event::Backspace) {
                        calc_first_digit = false;
                        if (!gpu_calc.count_str.empty()) gpu_calc.count_str.pop_back();
                        gpu_calc.gpu_count = gpu_calc.count_str.empty() ? 0 : std::max(1, std::atoi(gpu_calc.count_str.c_str()));
                        return true;
                    } else if (is_inc) {
                        calc_first_digit = false;
                        int delta = (gpu_calc.gpu_count >= 10000) ? 1000 : (gpu_calc.gpu_count >= 1000) ? 500 : (gpu_calc.gpu_count >= 100) ? 100 : (gpu_calc.gpu_count >= 10) ? 5 : 1;
                        gpu_calc.gpu_count = min(10000000, gpu_calc.gpu_count + delta);
                        gpu_calc.count_str = to_string(gpu_calc.gpu_count);
                        return true;
                    } else if (is_dec) {
                        calc_first_digit = false;
                        int delta = (gpu_calc.gpu_count > 10000) ? 1000 : (gpu_calc.gpu_count > 1000) ? 500 : (gpu_calc.gpu_count > 100) ? 100 : (gpu_calc.gpu_count > 10) ? 5 : 1;
                        gpu_calc.gpu_count = max(1, gpu_calc.gpu_count - delta);
                        gpu_calc.count_str = to_string(gpu_calc.gpu_count);
                        return true;
                    } else if (event == Event::Return) {
                        if (gpu_calc.gpu_count < 1) { gpu_calc.gpu_count = 1; gpu_calc.count_str = "1"; }
                        gpu_calc.selected_field = 2;
                        calc_first_digit = true;
                        return true;
                    }
                }
                else if (gpu_calc.selected_field == 2) {
                    // Average Load %
                    if (input.size() == 1 && std::isdigit(input[0])) {
                        if (calc_first_digit || gpu_calc.load_str == "0") {
                            gpu_calc.load_str.clear();
                            calc_first_digit = false;
                        }
                        gpu_calc.load_str += input[0];
                        if (gpu_calc.load_str.size() > 3) gpu_calc.load_str = gpu_calc.load_str.substr(0, 3);
                        gpu_calc.load_pct = std::clamp(std::atof(gpu_calc.load_str.c_str()), 1.0, 100.0);
                        return true;
                    } else if (event == Event::Backspace) {
                        calc_first_digit = false;
                        if (!gpu_calc.load_str.empty()) gpu_calc.load_str.pop_back();
                        gpu_calc.load_pct = gpu_calc.load_str.empty() ? 10.0 : std::clamp(std::atof(gpu_calc.load_str.c_str()), 1.0, 100.0);
                        return true;
                    } else if (is_inc) {
                        calc_first_digit = false;
                        gpu_calc.load_pct = min(100.0, gpu_calc.load_pct + 5.0);
                        char b[16]; std::snprintf(b, sizeof(b), "%.0f", gpu_calc.load_pct); gpu_calc.load_str = b;
                        return true;
                    } else if (is_dec) {
                        calc_first_digit = false;
                        gpu_calc.load_pct = max(5.0, gpu_calc.load_pct - 5.0);
                        char b[16]; std::snprintf(b, sizeof(b), "%.0f", gpu_calc.load_pct); gpu_calc.load_str = b;
                        return true;
                    } else if (event == Event::Return) {
                        gpu_calc.selected_field = 3;
                        calc_first_digit = true;
                        return true;
                    }
                }
                else if (gpu_calc.selected_field == 3) {
                    // PUE Overhead
                    if (input.size() == 1 && (std::isdigit(input[0]) || input[0] == '.')) {
                        if (calc_first_digit) {
                            gpu_calc.pue_str.clear();
                            calc_first_digit = false;
                        }
                        if (input[0] == '.' && gpu_calc.pue_str.find('.') != string::npos) return true;
                        gpu_calc.pue_str += input[0];
                        if (gpu_calc.pue_str.size() > 4) gpu_calc.pue_str = gpu_calc.pue_str.substr(0, 4);
                        gpu_calc.pue = std::clamp(std::atof(gpu_calc.pue_str.c_str()), 1.0, 3.0);
                        return true;
                    } else if (event == Event::Backspace) {
                        calc_first_digit = false;
                        if (!gpu_calc.pue_str.empty()) gpu_calc.pue_str.pop_back();
                        gpu_calc.pue = gpu_calc.pue_str.empty() ? 1.0 : std::clamp(std::atof(gpu_calc.pue_str.c_str()), 1.0, 3.0);
                        return true;
                    } else if (is_inc) {
                        calc_first_digit = false;
                        gpu_calc.pue = min(3.00, gpu_calc.pue + 0.05);
                        char b[16]; std::snprintf(b, sizeof(b), "%.2f", gpu_calc.pue); gpu_calc.pue_str = b;
                        return true;
                    } else if (is_dec) {
                        calc_first_digit = false;
                        gpu_calc.pue = max(1.00, gpu_calc.pue - 0.05);
                        char b[16]; std::snprintf(b, sizeof(b), "%.2f", gpu_calc.pue); gpu_calc.pue_str = b;
                        return true;
                    } else if (event == Event::Return) {
                        gpu_calc.selected_field = 4;
                        calc_first_digit = true;
                        return true;
                    }
                }
                else if (gpu_calc.selected_field == 4) {
                    // Electricity Rate $/kWh
                    if (input.size() == 1 && (std::isdigit(input[0]) || input[0] == '.')) {
                        if (calc_first_digit) {
                            gpu_calc.price_str.clear();
                            calc_first_digit = false;
                        }
                        if (input[0] == '.' && gpu_calc.price_str.find('.') != string::npos) return true;
                        gpu_calc.price_str += input[0];
                        if (gpu_calc.price_str.size() > 5) gpu_calc.price_str = gpu_calc.price_str.substr(0, 5);
                        gpu_calc.price_kwh = std::clamp(std::atof(gpu_calc.price_str.c_str()), 0.001, 2.0);
                        return true;
                    } else if (event == Event::Backspace) {
                        calc_first_digit = false;
                        if (!gpu_calc.price_str.empty()) gpu_calc.price_str.pop_back();
                        gpu_calc.price_kwh = gpu_calc.price_str.empty() ? 0.01 : std::clamp(std::atof(gpu_calc.price_str.c_str()), 0.001, 2.0);
                        return true;
                    } else if (is_inc) {
                        calc_first_digit = false;
                        gpu_calc.price_kwh = min(2.00, gpu_calc.price_kwh + 0.01);
                        char b[16]; std::snprintf(b, sizeof(b), "%.2f", gpu_calc.price_kwh); gpu_calc.price_str = b;
                        return true;
                    } else if (is_dec) {
                        calc_first_digit = false;
                        gpu_calc.price_kwh = max(0.01, gpu_calc.price_kwh - 0.01);
                        char b[16]; std::snprintf(b, sizeof(b), "%.2f", gpu_calc.price_kwh); gpu_calc.price_str = b;
                        return true;
                    } else if (event == Event::Return) {
                        gpu_calc.selected_field = 5;
                        calc_first_digit = true;
                        return true;
                    }
                }
                else if (gpu_calc.selected_field == 5) {
                    // Cycle Presets
                    if (is_inc || is_dec || event == Event::Return) {
                        static int curr_p = 1;
                        curr_p = (curr_p % 4) + 1;
                        gpu_calc.apply_preset(curr_p);
                        calc_first_digit = true;
                        return true;
                    }
                }
                else if (gpu_calc.selected_field == 6 || input == "c" || input == "C") {
                    // Export & Save to Vault
                    if (event == Event::Return || input == "c" || input == "C") {
                        std::string summary = "GPU Model (" + gpu_calc.get_model_name() + " x " + to_string(gpu_calc.gpu_count) + "): "
                            + format_power(gpu_calc.get_total_it_watts()) + " IT, " + format_power(gpu_calc.get_facility_watts()) + " facility (PUE " + format_double(gpu_calc.pue, 2) + "). "
                            + "Yearly: " + format_energy(gpu_calc.get_yearly_kwh()) + " @ $" + format_double(gpu_calc.price_kwh, 2) + "/kWh = $" + format_money(gpu_calc.get_yearly_cost()) + "/yr. "
                            + "Sources: NVIDIA Spec, Uptime Institute PUE, U.S. EIA.";
                        write_clipboard(summary);
                        last_seen_clip = summary;
                        {
                            std::lock_guard<std::mutex> lk(vault_mutex);
                            add_clip(vault_db, summary);
                        }
                        {
                            std::lock_guard<std::mutex> lk(ai_mutex);
                            ai_response_text = "== LIVE GPU ENERGY & COST CALCULATION REPORT ==\n\n"
                                + string(" • Cluster Scale:   ") + to_string(gpu_calc.gpu_count) + "x " + gpu_calc.get_model_name() + " (" + to_string(gpu_calc.get_tdp()) + "W TDP)\n"
                                + string(" • Average Draw:    ") + format_double(gpu_calc.load_pct, 1) + "% of TDP -> " + format_double(gpu_calc.get_avg_watts_per_gpu(), 1) + " W/GPU\n"
                                + string(" • IT Total Power:  ") + format_power(gpu_calc.get_total_it_watts()) + "\n"
                                + string(" • Facility Total:  ") + format_power(gpu_calc.get_facility_watts()) + " (PUE " + format_double(gpu_calc.pue, 2) + "x)\n"
                                + string(" • Daily Energy:    ") + format_double(gpu_calc.get_daily_kwh(), 0) + " kWh/day\n"
                                + string(" • Yearly Energy:   ") + format_energy(gpu_calc.get_yearly_kwh()) + "\n"
                                + string(" • Electricity Rate:") + " $" + format_double(gpu_calc.price_kwh, 2) + " / kWh\n"
                                + string(" • Daily Cost:      ") + "$" + format_money(gpu_calc.get_daily_cost()) + " / day\n"
                                + string(" • Monthly Cost:    ") + "$" + format_money(gpu_calc.get_monthly_cost()) + " / month\n"
                                + string(" • Annual Total:    ") + "$" + format_money(gpu_calc.get_yearly_cost()) + " / year\n\n"
                                + string("Copied to system clipboard and saved to SQLite Vault history.\n")
                                + string("Citations: NVIDIA Hardware Specifications, Uptime Institute Global PUE Survey, U.S. EIA Electric Power Monthly.");
                        }
                        safe_post_event(screen);
                        return true;
                    }
                }
            }
            // PANE 2: AI COPILOT (Right)
            else if (vault_focus_pane == 2) {
                if (event == Event::ArrowDown) {
                    selected_ai_menu = min(8, selected_ai_menu + 1);
                    return true;
                }
                if (event == Event::ArrowUp) {
                    selected_ai_menu = max(0, selected_ai_menu - 1);
                    return true;
                }

                if (event == Event::Return && selected_ai_menu >= 3) {
                    string system_prompt;
                    string user_prompt;
                    AIConfig ai_config_snapshot;
                    {
                        std::lock_guard<std::mutex> lk(ai_mutex);
                        ai_config_snapshot = ai_config;
                    }

                    string clip_text;
                    {
                        std::lock_guard<std::mutex> lk(vault_mutex);
                        auto clips = get_clips(vault_db);
                        if (selected_vault_idx < static_cast<int>(clips.size())) {
                            clip_text = clips[selected_vault_idx].content;
                        }
                    }
                    // Cap context: giant clips (binary pastes, logs) make CPU inference
                    // take minutes and can blow the model's context window entirely.
                    if (clip_text.size() > 4000) {
                        clip_text = clip_text.substr(0, 4000) + "\n...[clip truncated at 4000 chars]";
                    }

                    if (selected_ai_menu == 3 || selected_ai_menu == 4) {
                        system_prompt = "You are a helpful AI Linux coding copilot named Aura AI. Answer the user's question directly, clearly, and concisely in a technical cyberpunk tone.";
                        user_prompt = custom_question;
                        if (!clip_text.empty()) {
                            user_prompt += "\n\nContext Clipboard Text:\n" + clip_text;
                        }
                    } else if (selected_ai_menu == 5) {
                        system_prompt = "Summarize the following text concisely. Provide a bulleted list of key takeaways, and a one-sentence overview.";
                        user_prompt = clip_text;
                    } else if (selected_ai_menu == 6) {
                        system_prompt = "You are an expert software security and performance auditor. Analyze the following source code clip. Identify potential performance improvements, security concerns, or logic errors, and output extremely clean improved code.";
                        user_prompt = clip_text;
                    } else if (selected_ai_menu == 7) {
                        system_prompt = "Translate the following text. If it is in Serbian (Cyrillic or Latin), translate it to English. If it is in English, translate it to Serbian Cyrillic. Maintain a high-quality, professional, contextually appropriate translation.";
                        user_prompt = clip_text;
                    } else if (selected_ai_menu == 8) {
                        system_prompt = "You are an expert AI infrastructure architect and data center economist. Provide a detailed technical and financial breakdown for operating a 20,000 NVIDIA A100 GPU cluster (TDP 400W, 70% load = 280W per GPU, PUE 1.3, 7.28 MW facility power, 63.78M kWh/year, at $0.10/kWh -> $6,378,000/year). Include energy efficiency, cooling, and operational considerations with citations (NVIDIA specs, Uptime Institute PUE, U.S. EIA).";
                        user_prompt = "Provide full financial, power, and infrastructure analysis for running 20,000 A100 GPUs continuously for 1 year at $0.10/kWh and PUE 1.3.";
                    }

                    if (!user_prompt.empty()) {
                        {
                            std::lock_guard<std::mutex> lk(ai_mutex);
                            ai_thinking = true;
                            ai_response_text = "Thinking...";
                        }
                        safe_post_event(screen);

                        bg_threads.emplace_back([ai_config_snapshot, system_prompt, user_prompt, &ai_response_text, &ai_thinking, &ai_mutex, &screen]() {
                            std::string response = run_ai_query(ai_config_snapshot, system_prompt, user_prompt);
                            {
                                std::lock_guard<std::mutex> lk(ai_mutex);
                                ai_response_text = std::move(response);
                                ai_thinking = false;
                            }
                            safe_post_event(screen);
                        });
                    }
                    return true;
                }

                if (selected_ai_menu == 0) {
                    if (event == Event::Return || input == " ") {
                        AIConfig snapshot;
                        {
                            std::lock_guard<std::mutex> lk(ai_mutex);
                            if (ai_config.provider == "Ollama") ai_config.provider = "OpenAI";
                            else if (ai_config.provider == "OpenAI") ai_config.provider = "DeepSeek";
                            else if (ai_config.provider == "DeepSeek") ai_config.provider = "Gemini";
                            else if (ai_config.provider == "Gemini") ai_config.provider = "LM Studio";
                            else ai_config.provider = "Ollama";
                            snapshot = ai_config;
                        }
                        save_ai_config(snapshot);
                        return true;
                    }
                }
                else if (selected_ai_menu == 1) {
                    if (event == Event::Backspace) {
                        AIConfig snapshot;
                        {
                            std::lock_guard<std::mutex> lk(ai_mutex);
                            if (!ai_config.model.empty()) ai_config.model.pop_back();
                            snapshot = ai_config;
                        }
                        save_ai_config(snapshot);
                        return true;
                    }
                    else if (input.size() == 1 && isprint(input[0])) {
                        AIConfig snapshot;
                        {
                            std::lock_guard<std::mutex> lk(ai_mutex);
                            ai_config.model += input;
                            snapshot = ai_config;
                        }
                        save_ai_config(snapshot);
                        return true;
                    }
                }
                else if (selected_ai_menu == 2) {
                    if (event == Event::Backspace) {
                        AIConfig snapshot;
                        {
                            std::lock_guard<std::mutex> lk(ai_mutex);
                            if (!ai_config.api_key.empty()) ai_config.api_key.pop_back();
                            snapshot = ai_config;
                        }
                        save_ai_config(snapshot);
                        return true;
                    }
                    else if (input.size() == 1 && isprint(input[0])) {
                        AIConfig snapshot;
                        {
                            std::lock_guard<std::mutex> lk(ai_mutex);
                            ai_config.api_key += input;
                            snapshot = ai_config;
                        }
                        save_ai_config(snapshot);
                        return true;
                    }
                }
                else if (selected_ai_menu == 3) {
                    if (event == Event::Backspace) {
                        if (!custom_question.empty()) custom_question.pop_back();
                        return true;
                    }
                    else if (input.size() == 1 && isprint(input[0])) {
                        custom_question += input;
                        return true;
                    }
                }
            }
        }

        // Direct theme hotkeys matching sidebar index
        if (input == "1") { current_theme = NEON; return true; }
        if (input == "2") { current_theme = WIREFRAME; return true; }
        if (input == "3") { current_theme = DRACULA; return true; }
        if (input == "4") { current_theme = CARBON; return true; }
        if (input == "5") { current_theme = SRBIJA; return true; }
        if (input == "t" || input == "T") {
            current_theme = static_cast<Theme>((static_cast<int>(current_theme) + 1) % 5);
            return true;
        }
        return false;
    });

    std::atomic<bool> refresh_ui_loop{true};
    thread data_thread([&]() {
        auto last_update_time = chrono::high_resolution_clock::now();
        auto last_log_time = chrono::high_resolution_clock::now();
        auto last_opt_refresh = chrono::high_resolution_clock::now() - chrono::seconds(2);
        while (refresh_ui_loop.load()) {
            auto current_time = chrono::high_resolution_clock::now();
            chrono::duration<double> elapsed = current_time - last_update_time;
            last_update_time = current_time;

            SystemMetrics temp_metrics;
            {
                std::lock_guard<std::mutex> lock(metrics_mutex);
                scanner.update_all_metrics(metrics, elapsed.count());

                metrics.cumulative_kwh += (metrics.current_watts * (elapsed.count() / 3600.0)) / 1000.0;
                metrics.accumulated_cost = metrics.cumulative_kwh * PRICE_PER_KWH;

                graph.watt_history.push_back(metrics.current_watts);
                if (graph.watt_history.size() > graph.max_size) graph.watt_history.erase(graph.watt_history.begin());

                graph.cpu_load_history.push_back(metrics.cpu_load);
                if (graph.cpu_load_history.size() > graph.max_size) graph.cpu_load_history.erase(graph.cpu_load_history.begin());

                graph.cpu_temp_history.push_back(metrics.cpu_temp);
                if (graph.cpu_temp_history.size() > graph.max_size) graph.cpu_temp_history.erase(graph.cpu_temp_history.begin());

                graph.gpu_temp_history.push_back(metrics.gpu_temp);
                if (graph.gpu_temp_history.size() > graph.max_size) graph.gpu_temp_history.erase(graph.gpu_temp_history.begin());

                graph.ram_history.push_back(metrics.used_ram);
                if (graph.ram_history.size() > graph.max_size) graph.ram_history.erase(graph.ram_history.begin());

                graph.net_download_history.push_back(metrics.net_download_kb);
                if (graph.net_download_history.size() > graph.max_size) graph.net_download_history.erase(graph.net_download_history.begin());

                graph.net_upload_history.push_back(metrics.net_upload_kb);
                if (graph.net_upload_history.size() > graph.max_size) graph.net_upload_history.erase(graph.net_upload_history.begin());

                double busy_pct = metrics.cpu_load * 0.85;
                metrics.gpu_load = busy_pct;
                graph.gpu_load_history.push_back(metrics.gpu_load);
                if (graph.gpu_load_history.size() > graph.max_size) graph.gpu_load_history.erase(graph.gpu_load_history.begin());

                globe_angle_y += 0.04;
                globe_angle_x += 0.02;

                temp_metrics = metrics;
            }

            // High-Performance Clipboard Polling: Only poll Wayland wl-paste when on Clipboard Vault Tab (Tab 3)
            // to avoid system-wide context-menu closes and display-server grab interrupts.
            static int clipboard_poll_counter = 0;
            if (current_tab.load() == 3) {
                if (++clipboard_poll_counter >= 20) { // Poll every 2.0 seconds when actively on the Vault Tab
                    clipboard_poll_counter = 0;
                    string clip = read_clipboard();
                    if (!clip.empty()) {
                        std::lock_guard<std::mutex> lock(vault_mutex);
                        if (clip != last_seen_clip) {
                            last_seen_clip = clip;
                            add_clip(vault_db, clip);
                        }
                    }
                }
            } else {
                clipboard_poll_counter = 0;
            }

            if (chrono::duration_cast<chrono::seconds>(current_time - last_log_time).count() >= 5) {
                log_to_csv(logger, temp_metrics.current_watts, temp_metrics.cumulative_kwh, temp_metrics.accumulated_cost);
                last_log_time = current_time;
            }

            // Never run potentially blocking shell commands in the renderer path.
            if (current_tab.load() == 2 &&
                chrono::duration_cast<chrono::milliseconds>(current_time - last_opt_refresh).count() >= 800) {
                OptimizationStatus latest_opt;
                latest_opt.power_profile = get_power_profile();
                latest_opt.cpu_boost = get_cpu_boost();
                latest_opt.swappiness = get_swappiness();
                {
                    std::lock_guard<std::mutex> lk(opt_mutex);
                    opt_status = std::move(latest_opt);
                }
                last_opt_refresh = current_time;
            }

            safe_post_event(screen);
            this_thread::sleep_for(chrono::milliseconds(100));
        }
    });

    auto renderer = Renderer(component, [&] {
        SystemMetrics local_metrics;
        vector<double> local_watt_history;
        vector<double> local_cpu_load_history;
        vector<double> local_cpu_temp_history;
        vector<double> local_gpu_temp_history;
        vector<double> local_ram_history;
        vector<double> local_net_download_history;
        vector<double> local_net_upload_history;
        vector<double> local_gpu_load_history;
        double local_globe_angle_y = 0.0, local_globe_angle_x = 0.0;
        AIConfig local_ai_config;
        std::string local_ai_response_text;
        bool local_ai_thinking = false;
        OptimizationStatus local_opt_status;
        {
            std::lock_guard<std::mutex> lock(metrics_mutex);
            local_metrics = metrics;
            local_watt_history = graph.watt_history;
            local_cpu_load_history = graph.cpu_load_history;
            local_cpu_temp_history = graph.cpu_temp_history;
            local_gpu_temp_history = graph.gpu_temp_history;
            local_ram_history = graph.ram_history;
            local_net_download_history = graph.net_download_history;
            local_net_upload_history = graph.net_upload_history;
            local_gpu_load_history = graph.gpu_load_history;
            local_globe_angle_y = globe_angle_y;
            local_globe_angle_x = globe_angle_x;
        }
        {
            std::lock_guard<std::mutex> lk(ai_mutex);
            local_ai_config = ai_config;
            local_ai_response_text = ai_response_text;
            local_ai_thinking = ai_thinking;
        }
        {
            std::lock_guard<std::mutex> lk(opt_mutex);
            local_opt_status = opt_status;
        }

        int64_t token_total = g_token_stats.get_total_tokens();
        int32_t token_queries = g_token_stats.get_query_count();
        double token_estimated_cost = g_token_stats.estimate_cost_usd(local_ai_config.provider);
        int64_t token_remaining = g_token_stats.get_remaining_context(local_ai_config.provider, local_ai_config.model);

        auto term_size = Terminal::Size();
        int term_w = term_size.dimx;

        // Dynamic theme styling tokens with enhanced palettes
        Color primary_color = Color::Cyan;
        Color secondary_color = Color::Magenta;
        Color accent_color = Color::Yellow;
        Color border_color = Color::Cyan;
        Color bg_selected = Color::Red;
        Color gradient_high = Color::Cyan;
        Color gradient_mid = Color::Blue;
        Color gradient_low = Color::RGB(0, 50, 100);
        Color glow_color = Color::RGB(0, 100, 150);
        Color warning_color = Color::Orange1;
        Color danger_color = Color::Red;
        Color success_color = Color::Green;
        // Deep-space backdrop behind every panel; overridden per theme.
        Color panel_bg = Color::RGB(3, 8, 18);
        // Gradient fill under flux graph lines; defaults to signature AURA palette.
        FluxPalette flux_pal = {
            Color::RGB(255, 45, 130), Color::RGB(255, 210, 0),
            Color::RGB(255, 70, 70), Color::RGB(15, 60, 150)
        };

        if (current_theme == NEON) {
            primary_color = Color::RGB(0, 255, 255);      // Electric cyan (AURA PRIME)
            secondary_color = Color::RGB(255, 55, 170);   // Hot pink
            accent_color = Color::RGB(255, 170, 0);       // Amber
            border_color = Color::RGB(0, 220, 240);
            bg_selected = Color::RGB(255, 45, 140);
            gradient_high = Color::RGB(0, 255, 255);
            gradient_mid = Color::RGB(0, 180, 255);
            gradient_low = Color::RGB(10, 55, 120);
            glow_color = Color::RGB(0, 210, 245);
            warning_color = Color::RGB(255, 170, 0);
            danger_color = Color::RGB(255, 50, 90);
            success_color = Color::RGB(0, 255, 150);
        } else if (current_theme == WIREFRAME) {
            primary_color = Color::RGB(200, 200, 200);
            secondary_color = Color::RGB(180, 180, 180);
            accent_color = Color::RGB(255, 255, 255);
            border_color = Color::RGB(150, 150, 150);
            bg_selected = Color::RGB(100, 100, 100);
            gradient_high = Color::RGB(200, 200, 200);
            gradient_mid = Color::RGB(150, 150, 150);
            gradient_low = Color::RGB(80, 80, 80);
            glow_color = Color::RGB(120, 120, 120);
            warning_color = Color::RGB(200, 150, 50);
            danger_color = Color::RGB(200, 50, 50);
            success_color = Color::RGB(50, 180, 50);
            panel_bg = Color::RGB(8, 8, 8);
            flux_pal = { Color::RGB(230, 230, 230), Color::RGB(150, 150, 150),
                         Color::RGB(90, 90, 90), Color::RGB(30, 30, 30) };
        } else if (current_theme == DRACULA) {
            primary_color = Color::RGB(189, 147, 249);    // Purple
            secondary_color = Color::RGB(255, 85, 85);    // Red
            accent_color = Color::RGB(255, 121, 198);     // Pink
            border_color = Color::RGB(98, 114, 164);
            bg_selected = Color::RGB(68, 71, 90);
            gradient_high = Color::RGB(189, 147, 249);
            gradient_mid = Color::RGB(139, 233, 253);
            gradient_low = Color::RGB(68, 71, 90);
            glow_color = Color::RGB(139, 233, 253);
            warning_color = Color::RGB(255, 184, 108);
            danger_color = Color::RGB(255, 85, 85);
            success_color = Color::RGB(80, 250, 123);
            panel_bg = Color::RGB(24, 25, 36);
            flux_pal = { Color::RGB(255, 121, 198), Color::RGB(189, 147, 249),
                         Color::RGB(255, 85, 85), Color::RGB(40, 42, 54) };
        } else if (current_theme == CARBON) {
            primary_color = Color::RGB(100, 200, 100);   // Matrix green
            secondary_color = Color::RGB(150, 150, 150);
            accent_color = Color::RGB(150, 255, 100);
            border_color = Color::RGB(60, 60, 60);
            bg_selected = Color::RGB(50, 100, 50);
            gradient_high = Color::RGB(100, 255, 100);
            gradient_mid = Color::RGB(50, 180, 50);
            gradient_low = Color::RGB(20, 60, 20);
            glow_color = Color::RGB(50, 150, 50);
            warning_color = Color::RGB(200, 180, 50);
            danger_color = Color::RGB(200, 80, 50);
            success_color = Color::RGB(80, 200, 80);
            panel_bg = Color::RGB(0, 8, 2);
            flux_pal = { Color::RGB(160, 255, 120), Color::RGB(80, 220, 80),
                         Color::RGB(30, 150, 60), Color::RGB(8, 40, 12) };
        } else if (current_theme == SRBIJA) {
            // Cyrillic labels + AURA FLUX palette (same as NEON)
            primary_color = Color::RGB(0, 255, 255);
            secondary_color = Color::RGB(255, 55, 170);
            accent_color = Color::RGB(255, 170, 0);
            border_color = Color::RGB(0, 220, 240);
            bg_selected = Color::RGB(255, 45, 140);
            gradient_high = Color::RGB(0, 255, 255);
            gradient_mid = Color::RGB(0, 180, 255);
            gradient_low = Color::RGB(10, 55, 120);
            glow_color = Color::RGB(0, 210, 245);
            warning_color = Color::RGB(255, 170, 0);
            danger_color = Color::RGB(255, 50, 90);
            success_color = Color::RGB(0, 255, 150);
        }

        // Ghost palette for background layers (e.g. CPU trace behind the GPU line).
        FluxPalette dim_pal = { gradient_low, gradient_low, gradient_low, panel_bg };

        // Inline Cyrillic Translation helper
        auto L = [&](const string& en, const string& sr) {
            return ::L(en, sr, current_theme);
        };

        // --- SIDEBAR WIDGETS (Fast Main-Thread Pre-Calculated Globe Projection) ---
        int g_size = 56;
        globe_canvas = Canvas(g_size, g_size);
        // Slightly smaller sphere leaves room for the satellite orbits around it.
        double r_globe = g_size / 2.9;
        double cx_g = g_size / 2.0;
        double cy_g = g_size / 2.0;
        double cos_y = cos(local_globe_angle_y);
        double sin_y = sin(local_globe_angle_y);
        double cos_x = cos(local_globe_angle_x);
        double sin_x = sin(local_globe_angle_x);

        // Parallax starfield backdrop: two layers drifting behind the globe.
        draw_starfield(globe_canvas, g_size, g_size, local_globe_angle_y,
                       gradient_low, glow_color);

        // Wireframe latitude/longitude mesh: adds 3D depth to the globe body.
        draw_globe_grid(globe_canvas, cx_g, cy_g, r_globe,
                        local_globe_angle_y, local_globe_angle_x,
                        Color::Interpolate(0.6f, panel_bg, glow_color),
                        Color::Interpolate(0.8f, panel_bg, gradient_low));

        // Draw spinning 3D network connections using pre-calculated static connections with depth-based coloring
        for (const auto& conn : globe_connections) {
            size_t i = conn.i;
            size_t j = conn.j;
            double xi = globe_points[i].x * cos_y - globe_points[i].z * sin_y;
            double zi = globe_points[i].x * sin_y + globe_points[i].z * cos_y;
            double yi = globe_points[i].y * cos_x - zi * sin_x;
            double zzi = globe_points[i].y * sin_x + zi * cos_x;

            double xj = globe_points[j].x * cos_y - globe_points[j].z * sin_y;
            double zj = globe_points[j].x * sin_y + globe_points[j].z * cos_y;
            double yj = globe_points[j].y * cos_x - zj * sin_x;
            double zzj = globe_points[j].y * sin_x + zj * cos_x;

            int sxi = static_cast<int>(cx_g + xi * r_globe);
            int syi = static_cast<int>(cy_g + yi * r_globe);
            int sxj = static_cast<int>(cx_g + xj * r_globe);
            int syj = static_cast<int>(cy_g + yj * r_globe);

            // Enhanced depth-based color blending
            double avg_z = (zzi + zzj) / 2.0;
            Color col_mesh;
            if (avg_z > 0.3) col_mesh = primary_color;
            else if (avg_z > 0) col_mesh = glow_color;
            else if (avg_z > -0.3) col_mesh = gradient_mid;
            else col_mesh = gradient_low;

            globe_canvas.DrawPointLine(sxi, syi, sxj, syj, col_mesh);
        }

        // Draw globe dots with glow effect
        for (const auto& pt : globe_points) {
            double x1 = pt.x * cos_y - pt.z * sin_y;
            double z1 = pt.x * sin_y + pt.z * cos_y;
            double y2 = pt.y * cos_x - z1 * sin_x;
            double z2 = pt.y * sin_x + z1 * cos_x;

            int sx = static_cast<int>(cx_g + x1 * r_globe);
            int sy = static_cast<int>(cy_g + y2 * r_globe);

            // Enhanced depth-based coloring for dots
            Color col_dot;
            if (z2 > 0.4) col_dot = accent_color;
            else if (z2 > 0.1) col_dot = primary_color;
            else if (z2 > -0.2) col_dot = glow_color;
            else if (z2 > -0.5) col_dot = gradient_mid;
            else col_dot = gradient_low;

            globe_canvas.DrawPoint(sx, sy, true, col_dot);

            // Add glow effect for front-facing points
            if (z2 > 0.3) {
                globe_canvas.DrawPoint(sx + 1, sy, true, glow_color);
                globe_canvas.DrawPoint(sx - 1, sy, true, glow_color);
                globe_canvas.DrawPoint(sx, sy + 1, true, glow_color);
                globe_canvas.DrawPoint(sx, sy - 1, true, glow_color);
            }
        }

        // --- PINK SATELLITES: orbiters with comet trails and earth uplink beams ---
        for (int s = 0; s < 3; ++s) {
            const auto& sp = sat_specs[s];
            double ct = cos(sp.tilt), st = sin(sp.tilt);
            // Orbit position in view space; the globe's view rotation is applied so
            // satellites both orbit and ride the scene's slow tumble.
            auto orbit_pos = [&](double ang) {
                double px = cos(ang) * sp.radius;
                double py = sin(ang) * sp.radius * ct;
                double pz = sin(ang) * sp.radius * st;
                double rx = px * cos_y - pz * sin_y;
                double rz = px * sin_y + pz * cos_y;
                double ry = py * cos_x - rz * sin_x;
                double rz2 = py * sin_x + rz * cos_x;
                return Point3D{rx, ry, rz2};
            };
            // Satellite hides when it passes behind the planet's disc.
            auto occluded = [](const Point3D& p) {
                return p.z < 0 && sqrt(p.x * p.x + p.y * p.y) < 1.0;
            };
            auto to_sx = [&](double v) { return static_cast<int>(cx_g + v * r_globe); };
            auto to_sy = [&](double v) { return static_cast<int>(cy_g + v * r_globe); };

            // Faint dotted orbit ring
            for (int k = 0; k < 44; k += 2) {
                Point3D op = orbit_pos(k * 2.0 * M_PI / 44.0);
                if (!occluded(op)) {
                    globe_canvas.DrawPoint(to_sx(op.x), to_sy(op.y), true, gradient_low);
                }
            }

            double ang = local_globe_angle_y * sp.speed + sp.phase;

            // Comet trail fading behind the satellite
            for (int k = 1; k <= 5; ++k) {
                Point3D tp = orbit_pos(ang - k * 0.09);
                if (occluded(tp)) continue;
                Color tcol = Color::Interpolate(static_cast<float>(k) / 6.0f,
                                                secondary_color, gradient_low);
                globe_canvas.DrawPoint(to_sx(tp.x), to_sy(tp.y), true, tcol);
            }

            Point3D sat = orbit_pos(ang);
            if (occluded(sat)) continue;
            int sx = to_sx(sat.x);
            int sy = to_sy(sat.y);

            // Uplink beam to the sub-satellite ground point, with a data pulse
            // traveling up the link. Only when the satellite faces the viewer.
            if (sat.z > 0) {
                double inv = 1.0 / sp.radius;
                int gx = to_sx(sat.x * inv);
                int gy = to_sy(sat.y * inv);
                Color beam = Color::Interpolate(0.55f, Color::Black, secondary_color);
                globe_canvas.DrawPointLine(gx, gy, sx, sy, beam);
                double pulse = fmod(local_globe_angle_y * (1.3 + 0.4 * s), 1.0);
                globe_canvas.DrawPoint(gx + static_cast<int>((sx - gx) * pulse),
                                       gy + static_cast<int>((sy - gy) * pulse),
                                       true, accent_color);
            }

            // Satellite body: hot white core wrapped in pink glow
            globe_canvas.DrawPoint(sx, sy, true, Color::RGB(255, 230, 245));
            globe_canvas.DrawPoint(sx + 1, sy, true, secondary_color);
            globe_canvas.DrawPoint(sx - 1, sy, true, secondary_color);
            globe_canvas.DrawPoint(sx, sy + 1, true, secondary_color);
            globe_canvas.DrawPoint(sx, sy - 1, true, secondary_color);
        }

        // Title breathes between cyan and pink in sync with the globe spin.
        double breathe = 0.5 + 0.5 * sin(local_globe_angle_y * 1.6);
        Color title_col = Color::Interpolate(static_cast<float>(breathe),
                                             primary_color, secondary_color);
        auto globe_widget = vbox({
            canvas(&globe_canvas) | size(HEIGHT, EQUAL, 14) | size(WIDTH, EQUAL, 28) | hcenter,
            text(spaced(L("AURA FLUX", "АУРА ФЛУКС"))) | bold | hcenter | color(title_col)
        });

        auto side_row = [&](const string& label, const string& value, Color val_col) {
            return hbox({ text("  " + label) | color(Color::GrayDark), filler(), text(value + "  ") | color(val_col) });
        };

        auto system_box = vbox({
            text("  " + spaced(L("SYSTEM", "СИСТЕМ"))) | bold | color(primary_color),
            separator() | color(glow_color),
            side_row(L("CPU", "ПРОЦ"), local_metrics.cpu_model, Color::GrayLight),
            side_row(L("Uptime", "Време рада"), local_metrics.uptime_str, Color::GrayLight),
            side_row(L("Load", "Оптер."), local_metrics.loadavg_str, Color::GrayLight),
            side_row(L("Processes", "Процеси"), to_string(local_metrics.proc_count), Color::GrayLight)
        });

        auto navigation_box = vbox({
            text("  " + spaced(L("NAVIGATION", "НАВИГАЦИЈА"))) | bold | color(primary_color),
            separator() | color(glow_color),
            text(current_tab == 0 ? "  [F1] * " + L("DIAGNOSTICS", "ДИЈАГНОСТИКА") : "  [F1]   " + L("DIAGNOSTICS", "ДИЈАГНОСТИКА")) | color(current_tab == 0 ? primary_color : Color::White),
            text(current_tab == 1 ? "  [F2] * " + L("BENCHMARKS", "ТЕСТИРАЊЕ") : "  [F2]   " + L("BENCHMARKS", "ТЕСТИРАЊЕ")) | color(current_tab == 1 ? primary_color : Color::White),
            text(current_tab == 2 ? "  [F3] * " + L("OPTIMIZATION", "ОПТИМИЗАЦИЈА") : "  [F3]   " + L("OPTIMIZATION", "ОПТИМИЗАЦИЈА")) | color(current_tab == 2 ? primary_color : Color::White),
            text(current_tab == 3 ? "  [F4] * " + L("VAULT & AI", "БЕЛЕЖНИК И АИ") : "  [F4]   " + L("VAULT & AI", "БЕЛЕЖНИК И АИ")) | color(current_tab == 3 ? primary_color : Color::White),
        });

        char clock_buf[16] = "--:--:--";
        {
            time_t tt = time(nullptr);
            tm lt{};
            if (localtime_r(&tt, &lt)) strftime(clock_buf, sizeof(clock_buf), "%H:%M:%S", &lt);
        }

        // Live heartbeat pulse: rotating glyph proves the render loop is alive
        // (if this stops spinning, the freeze is back).
        const char* pulse_chars[] = {"●", "◉", "○", "◌"};
        int pulse_idx = static_cast<int>(local_globe_angle_y * 2.0) & 3;
        std::string pulse_glyph = std::string(" ") + pulse_chars[pulse_idx] + " ";
        bool system_ok = local_metrics.cpu_temp < 85;
        auto status_resume = vbox({
            hbox({ text(pulse_glyph) | color(system_ok ? success_color : danger_color) | bold,
                   text(spaced(L("STATUS", "СТАТУС"))) | bold | color(primary_color) }),
            separator() | color(glow_color),
            side_row(L("Clock", "Сат"), clock_buf, primary_color),
            side_row(L("Draw", "Потрошња"), format_double(local_metrics.current_watts, 1) + " W", accent_color),
            side_row(L("Energy", "Енергија"), format_double(local_metrics.cumulative_kwh, 4) + " kWh", secondary_color),
            side_row(L("System", "Систем"),
                     system_ok ? L("Operational", "Оперативан") : L("⚠ Throttling", "⚠ Грејање"),
                     system_ok ? success_color : danger_color)
        });

        // Token Stats Section
        auto token_stats_box = vbox({
            text("  " + spaced(L("AI TOKENS", "АИ ТОКЕНИ"))) | bold | color(primary_color),
            separator() | color(glow_color),
            side_row(L("Total", "Укупно"), g_token_stats.format_token_count(token_total), primary_color),
            side_row(L("Queries", "Упити"), to_string(token_queries), secondary_color),
            side_row(L("Est Cost", "Процена"), "$" + format_double(token_estimated_cost, 4), success_color)
        });

        auto theme_selector = vbox({
            text("  " + spaced(L("THEME", "ТЕМА"))) | bold | color(primary_color),
            separator() | color(glow_color),
            text(current_theme == NEON ? "  [1] * NEON" : "  [1] NEON") | color(current_theme == NEON ? primary_color : Color::White),
            text(current_theme == WIREFRAME ? "  [2] * WIREFRAME" : "  [2] WIREFRAME") | color(current_theme == WIREFRAME ? primary_color : Color::White),
            text(current_theme == DRACULA ? "  [3] * DRACULA" : "  [3] DRACULA") | color(current_theme == DRACULA ? primary_color : Color::White),
            text(current_theme == CARBON ? "  [4] * CARBON" : "  [4] CARBON") | color(current_theme == CARBON ? primary_color : Color::White),
            text(current_theme == SRBIJA ? "  [5] * ЋИРИЛИЦА" : "  [5] ЋИРИЛИЦА") | color(current_theme == SRBIJA ? primary_color : Color::White),
        });

        auto sidebar = vbox({ globe_widget, separator() | color(glow_color), system_box, separator() | color(glow_color), navigation_box, separator() | color(glow_color), status_resume, separator() | color(glow_color), token_stats_box, separator() | color(glow_color), theme_selector }) | border;

        // --- DASHBOARD GRIDS (Right Side Panel) ---
        int right_w = std::max(80, term_w - 32);
        int curve_w_px = (right_w * 0.6) * 2;
        int curve_h_px = 32;

        // 1. CPU MATRIX — neon flux line graph + per-core gauge bars
        cpu_canvas = Canvas(curve_w_px, curve_h_px);
        {
            double now_load = local_cpu_load_history.empty() ? 0.0 : local_cpu_load_history.back();
            Color line_col = (now_load > 75) ? danger_color : (now_load > 50) ? warning_color : primary_color;
            draw_flux_graph(cpu_canvas, local_cpu_load_history, graph.max_size, 100.0,
                            curve_w_px, curve_h_px, line_col, gradient_mid, &flux_pal);
            draw_scanlines(cpu_canvas, curve_w_px, curve_h_px, local_globe_angle_y, line_col);
        }

        Elements cpu_grid_rows;
        {
            size_t n = local_metrics.core_loads.size();
            size_t rows = (n + 1) / 2;
            for (size_t r = 0; r < rows; ++r) {
                Elements row_elems;
                for (size_t c = 0; c < 2; ++c) {
                    size_t i = r + c * rows;
                    if (i >= n) { row_elems.push_back(filler()); continue; }
                    double load = local_metrics.core_loads[i];
                    Color col;
                    if (load > 85) col = danger_color;
                    else if (load > 60) col = warning_color;
                    else if (load > 35) col = accent_color;
                    else col = primary_color;
                    string num_str = to_string(i);
                    if (num_str.size() == 1) num_str = "0" + num_str;
                    row_elems.push_back(hbox({
                        text(" " + num_str + " ") | color(glow_color),
                        gauge(load / 100.0) | color(col) | size(WIDTH, EQUAL, 10),
                        text(" " + format_double(load, 0) + "% ") | color(load > 60 ? col : Color::GrayLight) | size(WIDTH, EQUAL, 6)
                    }));
                }
                cpu_grid_rows.push_back(hbox(move(row_elems)));
            }
        }

        auto stat_cell = [&](const string& label, const string& value, Color val_col) {
            return vbox({
                text(" " + label) | color(Color::GrayDark),
                text(" " + value) | bold | color(val_col)
            });
        };

        auto cpu_matrix_box = window(
            hbox({ text(" ⚙ " + spaced(L("CPU MATRIX", "ПРОЦЕСОР")) + " ") | bold | color(primary_color),
                   text(" " + format_double(local_metrics.cpu_load, 1) + "% ") | bold | color(accent_color) }),
            vbox({
            hbox({
                vbox({
                    stat_cell(L("FREQUENCY", "ФРЕКВЕНЦИЈА"), format_double(local_metrics.cpu_freq, 2) + " GHz", primary_color),
                    stat_cell(L("TEMP", "ТЕМП"), to_string(static_cast<int>(local_metrics.cpu_temp)) + " °C",
                              local_metrics.cpu_temp > 75 ? danger_color : accent_color),
                    stat_cell(L("LOAD 1M", "ОПТЕРЕЋЕЊЕ"), local_metrics.loadavg_str, secondary_color)
                }) | size(WIDTH, EQUAL, 16),
                separator() | color(glow_color),
                vbox(move(cpu_grid_rows)) | flex
            }),
            separator() | color(glow_color),
            canvas(&cpu_canvas) | size(HEIGHT, EQUAL, 8)
        })) | color(border_color) | flex;

        // 2. MEMORY BANKS — hot-pink flux line over RAM pressure
        int mem_w_px = (right_w * 0.35) * 2;
        memory_canvas = Canvas(mem_w_px, curve_h_px);
        {
            double total_ram_safe = local_metrics.total_ram > 0 ? static_cast<double>(local_metrics.total_ram) : 1.0;
            double pct_now = local_ram_history.empty() ? 0.0 : local_ram_history.back() / total_ram_safe;
            Color line_col = (pct_now > 0.85) ? danger_color : (pct_now > 0.65) ? warning_color : secondary_color;
            draw_flux_graph(memory_canvas, local_ram_history, graph.max_size, total_ram_safe,
                            mem_w_px, curve_h_px, line_col, gradient_mid, &flux_pal);
            draw_scanlines(memory_canvas, mem_w_px, curve_h_px, local_globe_angle_y * 1.3, line_col);
        }

        double ram_pct = local_metrics.total_ram > 0
            ? (static_cast<double>(local_metrics.used_ram) / local_metrics.total_ram) * 100.0 : 0.0;
        auto memory_banks_box = window(
            hbox({ text(" ▤ " + spaced(L("MEMORY BANKS", "МЕМОРИЈА")) + " ") | bold | color(primary_color),
                   text(" " + format_double(ram_pct, 1) + "% ") | bold | color(secondary_color) }),
            vbox({
            hbox({
                stat_cell(L("RAM USED", "РАМ"), format_double(local_metrics.used_ram / 1024.0, 1) + " GB", secondary_color) | flex,
                stat_cell(L("SWAP", "СВАП"), format_double(local_metrics.used_swap / 1024.0, 1) + " GB",
                          local_metrics.used_swap > local_metrics.total_swap / 2 ? warning_color : primary_color) | flex,
                stat_cell(L("AVAILABLE", "СЛОБОДНО"), format_double(local_metrics.free_ram / 1024.0, 1) + " GB", success_color) | flex
            }),
            separator() | color(glow_color),
            canvas(&memory_canvas) | size(HEIGHT, EQUAL, 8)
        })) | color(border_color);

        // 3. POWER DRAW — cyan flux line, stat cells like AURA PRIME
        power_canvas = Canvas(curve_w_px, curve_h_px);
        {
            double now_w = local_watt_history.empty() ? 0.0 : local_watt_history.back();
            Color line_col = (now_w > 45) ? danger_color : (now_w > 30) ? warning_color : primary_color;
            draw_flux_graph(power_canvas, local_watt_history, graph.max_size, 60.0,
                            curve_w_px, curve_h_px, line_col, gradient_mid, &flux_pal);
            draw_scanlines(power_canvas, curve_w_px, curve_h_px, local_globe_angle_y * 0.7, line_col);
        }

        auto power_draw_box = window(
            hbox({ text(" ⚡ " + spaced(L("POWER DRAW", "ПОТРОШЊА")) + " ") | bold | color(primary_color),
                   text(" " + format_double(local_metrics.current_watts, 1) + " W ") | bold | color(accent_color) }),
            vbox({
            hbox({
                stat_cell(L("APU PKG", "АПУ ПАКЕТ"), format_double(local_metrics.current_watts * 0.6, 1) + " W", primary_color) | flex,
                stat_cell(L("CHARGE", "БАТЕРИЈА"), to_string(static_cast<int>(local_metrics.battery_percent)) + " %",
                          local_metrics.battery_percent < 25 ? danger_color : success_color) | flex,
                stat_cell(L("ENERGY TOTAL", "УКУПНО"), format_double(local_metrics.cumulative_kwh, 4) + " kWh", secondary_color) | flex,
                stat_cell(L("EST. COST", "ТРОШАК"), "$" + format_double(local_metrics.accumulated_cost, 4) + " @ " + format_double(PRICE_PER_KWH, 2) + "/kWh", accent_color) | flex
            }),
            separator() | color(glow_color),
            canvas(&power_canvas) | size(HEIGHT, EQUAL, 8)
        })) | color(border_color) | flex;

        // 4. THERMALS — three ring gauges (CPU / GPU / battery), AURA PRIME style
        {
            int th_h = 36;
            thermals_canvas = Canvas(mem_w_px, th_h);
            struct GaugeSpec { double value; double pct; string unit; string label; Color col; };
            auto temp_col = [&](double t) {
                return t > 80 ? danger_color : t > 65 ? warning_color : t > 50 ? accent_color : primary_color;
            };
            GaugeSpec gauges[3] = {
                { local_metrics.cpu_temp, local_metrics.cpu_temp, "°C", L("CPU", "ПРОЦ"), temp_col(local_metrics.cpu_temp) },
                { local_metrics.gpu_temp, local_metrics.gpu_temp, "°C", L("GPU", "ГРАФ"), temp_col(local_metrics.gpu_temp) },
                { local_metrics.battery_percent, local_metrics.battery_percent, "%", L("BAT", "БАТ"),
                  local_metrics.battery_percent < 25 ? danger_color : success_color }
            };
            for (int g = 0; g < 3; ++g) {
                int cx = mem_w_px * (2 * g + 1) / 6;
                int cy = 14;
                draw_rev_gauge(thermals_canvas, cx, cy, 9, gauges[g].pct,
                               success_color, warning_color, danger_color,
                               Color::White, Color::RGB(40, 48, 70));
                string val_str = to_string(static_cast<int>(gauges[g].value)) + gauges[g].unit;
                thermals_canvas.DrawText(cx - static_cast<int>(val_str.size()), th_h - 8, val_str, gauges[g].col);
                thermals_canvas.DrawText(cx - static_cast<int>(gauges[g].label.size()), th_h - 4, gauges[g].label, glow_color);
            }
        }

        auto thermals_box = window(
            hbox({ text(" ♨ " + spaced(L("THERMALS", "ТЕМПЕРАТУРЕ")) + " ") | bold | color(primary_color) }),
            vbox({
                canvas(&thermals_canvas) | size(HEIGHT, EQUAL, 9) | hcenter
            })) | color(border_color);

        // 5. NETWORK LINK Box with enhanced activity-based coloring
        int col3_w_px = (right_w / 3) * 2;
        int net_h_px = 24;
        network_canvas = Canvas(col3_w_px, net_h_px);
        {
            // Auto-scale to recent peak so both trickles and bursts stay readable
            double peak = 100.0;
            for (double v : local_net_download_history) peak = std::max(peak, v);
            for (double v : local_net_upload_history) peak = std::max(peak, v);
            // Upload as ghost trace behind the bright download line
            draw_flux_graph(network_canvas, local_net_upload_history, graph.max_size, peak,
                            col3_w_px, net_h_px, gradient_mid, gradient_low, &dim_pal);
            draw_flux_graph(network_canvas, local_net_download_history, graph.max_size, peak,
                            col3_w_px, net_h_px, secondary_color, gradient_mid, &flux_pal);
            draw_scanlines(network_canvas, col3_w_px, net_h_px, local_globe_angle_y * 1.8, secondary_color);
        }

        auto network_link_box = window(
            hbox({ text(" ⇅ " + spaced(L("NETWORK LINK", "МРЕЖА")) + " ") | bold | color(primary_color) }),
            vbox({
            hbox({
                stat_cell(L("DOWN", "ПРИЈЕМ"), format_double(local_metrics.net_download_kb, 1) + " KB/s", primary_color) | flex,
                stat_cell(L("UP", "СЛАЊЕ"), format_double(local_metrics.net_upload_kb, 1) + " KB/s", secondary_color) | flex
            }),
            separator() | color(glow_color),
            canvas(&network_canvas) | size(HEIGHT, EQUAL, 6)
        })) | color(border_color) | flex;

        // 6. STORAGE ARRAY — smooth gauges per mount
        Elements drive_progress_rows;
        for (const auto& drv : local_metrics.drives) {
            Color drv_color;
            if (drv.percent_used > 90) drv_color = danger_color;
            else if (drv.percent_used > 75) drv_color = warning_color;
            else if (drv.percent_used > 50) drv_color = accent_color;
            else drv_color = success_color;

            string path_cut = drv.label.empty() ? drv.mount_point : drv.label;
            if (path_cut.size() > 11) path_cut = path_cut.substr(0, 9) + "..";

            drive_progress_rows.push_back(
                hbox({
                    text(" " + path_cut) | color(Color::GrayLight) | size(WIDTH, EQUAL, 12),
                    gauge(drv.percent_used / 100.0) | color(drv_color) | size(WIDTH, EQUAL, 12),
                    text(" " + format_double(drv.percent_used, 0) + "%") | bold | color(drv_color) | size(WIDTH, EQUAL, 5),
                    filler(),
                    text(to_string(drv.used_gb) + " / " + to_string(drv.total_gb) + " GB ") | color(glow_color)
                })
            );
        }

        auto storage_array_box = window(
            hbox({ text(" ◍ " + spaced(L("STORAGE ARRAY", "СКЛАДИШТЕ")) + " ") | bold | color(primary_color),
                   text(" " + format_double(local_metrics.disk_read_kb / 1024.0, 1) + " MB/s ") | bold | color(accent_color) }),
            vbox({
            hbox({
                stat_cell(L("READ", "ЧИТАЊЕ"), format_double(local_metrics.disk_read_kb / 1024.0, 1) + " MB/s", primary_color) | flex,
                stat_cell(L("WRITE", "УПИС"), format_double(local_metrics.disk_write_kb / 1024.0, 1) + " MB/s", secondary_color) | flex
            }),
            separator() | color(glow_color),
            vbox(move(drive_progress_rows))
        })) | color(border_color) | flex;

        // 7. GPU CORE — dim CPU flux behind, bright GPU flux line on top
        int gpu_h_px = 24;
        gpu_canvas = Canvas(col3_w_px, gpu_h_px);
        draw_flux_graph(gpu_canvas, local_cpu_load_history, graph.max_size, 100.0,
                        col3_w_px, gpu_h_px, gradient_mid, Color::RGB(5, 15, 40), &dim_pal);
        {
            double now_g = local_gpu_load_history.empty() ? 0.0 : local_gpu_load_history.back();
            Color gpu_col = (now_g > 75) ? danger_color : (now_g > 50) ? warning_color : secondary_color;
            draw_flux_graph(gpu_canvas, local_gpu_load_history, graph.max_size, 100.0,
                            col3_w_px, gpu_h_px, gpu_col, gradient_mid, &flux_pal);
            draw_scanlines(gpu_canvas, col3_w_px, gpu_h_px, local_globe_angle_y * 0.5, gpu_col);
        }

        auto gpu_core_box = window(
            hbox({ text(" ◉ " + spaced(L("GPU CORE", "ГРАФИКА")) + " ") | bold | color(primary_color),
                   text(" " + format_double(local_metrics.gpu_load, 0) + "% ") | bold | color(secondary_color) }),
            vbox({
            hbox({
                stat_cell(L("TEMP", "ТЕМП"), to_string(static_cast<int>(local_metrics.gpu_temp)) + " °C", accent_color) | flex,
                stat_cell(L("EST LOAD", "ОПТЕРЕЋЕЊЕ"), format_double(local_metrics.gpu_load, 0) + " %", secondary_color) | flex
            }),
            separator() | color(glow_color),
            canvas(&gpu_canvas) | size(HEIGHT, EQUAL, 6)
        })) | color(border_color) | flex;

        // --- RIGHT SIDE PANEL TABS DISPATCHER ---
        Element right_panel;

        if (current_tab == 0) {
            // DIAGNOSTICS TAB
            auto row1 = hbox({ cpu_matrix_box, memory_banks_box | size(WIDTH, EQUAL, right_w * 0.35) });
            auto row2 = hbox({ power_draw_box, thermals_box | size(WIDTH, EQUAL, right_w * 0.35) });
            auto row3 = hbox({ network_link_box, storage_array_box, gpu_core_box });

            Elements proc_rows;
            unsigned long long top_ram = 1;
            for (const auto& p : local_metrics.processes) top_ram = std::max(top_ram, p.ram_kb);
            proc_rows.push_back(hbox({
                text("  PID") | bold | color(glow_color) | size(WIDTH, EQUAL, 10),
                text(spaced(L("PROCESS", "ПРОЦЕС"))) | bold | color(glow_color),
                filler(),
                text(spaced(L("MEM RES", "МЕМОРИЈА")) + "  ") | bold | color(glow_color)
            }) | bgcolor(Color::RGB(12, 18, 34)));

            for (size_t i = 0; i < local_metrics.processes.size(); ++i) {
                const auto& p = local_metrics.processes[i];
                string ram_str = to_string(p.ram_kb / 1024) + " MB";
                string name_str = p.name;
                if (name_str.size() > 35) name_str = name_str.substr(0, 32) + "...";

                auto row = hbox({
                    text("  " + to_string(p.pid)) | color(glow_color) | size(WIDTH, EQUAL, 10),
                    text(name_str) | color(Color::GrayLight),
                    filler(),
                    gauge(static_cast<double>(p.ram_kb) / top_ram) | color(gradient_mid) | size(WIDTH, EQUAL, 10),
                    text(" " + ram_str + "  ") | color(primary_color) | size(WIDTH, EQUAL, 10)
                });
                if (static_cast<int>(i) == selected_proc_idx) {
                    row = row | bgcolor(bg_selected) | color(Color::White) | bold;
                } else if (i % 2 == 1) {
                    row = row | bgcolor(Color::RGB(8, 12, 24));
                }
                proc_rows.push_back(row);
            }

            auto process_box = window(
                hbox({ text(" ▣ " + spaced(L("ACTIVE TASKS", "АКТИВНИ ПРОЦЕСИ")) + " ") | bold | color(primary_color),
                       text(" " + L("top RAM · [k] kill", "топ РАМ · [k] заустави") + " ") | color(Color::GrayDark) }),
                vbox(move(proc_rows))) | color(border_color) | flex;

            auto header = hbox({
                text("  " + spaced(L("AURA", "АУРА")) + " ") | bold | color(primary_color),
                text(spaced(L("FLUX", "ФЛУКС")) + " ") | bold | color(secondary_color),
                text(spaced(L("DIAGNOSTICS", "ДИЈАГНОСТИКА")) + "  ") | bold | color(Color::White),
                text(" " + L("real-time telemetry · 10 Hz", "телеметрија у реалном времену · 10 Hz") + " ") | color(Color::GrayDark),
                filler(),
                text("  [T/t] " + L("Cycle Theme", "Промена Теме") + "  ") | color(accent_color)
            });

            right_panel = vbox({ header, row1, row2, row3, process_box });
        }
        else if (current_tab == 1) {
            // BENCHMARKS TAB — snapshot bench_res under lock
            BenchmarkResults local_bench;
            {
                std::lock_guard<std::mutex> lk(bench_mutex);
                local_bench = bench_res;
            }

            auto header = hbox(
                text("  " + L("AURA COCKPIT BENCHMARKS", "АУРА ТЕСТИРАЊЕ СИСТЕМА") + "  ") | bold | color(primary_color),
                text(" " + L("measure bare-metal performance", "мерите перформансе хардвера") + " ") | color(Color::GrayDark)
            );

            Element progress_bar = filler();
            if (local_bench.cpu_running || local_bench.mem_running || local_bench.disk_running || local_bench.llm_running) {
                int filled = local_bench.progress_pct / 5;
                string bar;
                for (int k = 0; k < 20; ++k) bar += (k < filled) ? "█" : "░";
                progress_bar = hbox(
                    text("  " + local_bench.progress_label + "  ") | bold | color(accent_color),
                    text("[" + bar + "] " + to_string(local_bench.progress_pct) + "%") | color(primary_color)
                ) | border;
            } else {
                progress_bar = hbox(
                    text("  " + L("Ready to run benchmark. Press [Enter] on selected option.", "Спреман за покретање. Притисните [Enter] на изабраној опцији.") + "  ") | color(Color::Green)
                ) | border;
            }

            auto make_menu_item = [&](int idx, const string& label) -> Element {
                bool is_sel = (idx == selected_bench_menu);
                auto prefix = is_sel ? "  [Enter] * " : "            ";
                auto col = is_sel ? primary_color : Color::White;
                return text(prefix + label) | color(col) | bold;
            };

            auto action_list = window(text("  " + L("SELECT TEST", "ОДАБЕРИТЕ ТЕСТ") + "  ") | bold, vbox(
                make_menu_item(0, L("Run CPU Burn (Bitwise Mixer MOPS)", "Покрени CPU Burn (Битвајз Миксер MOPS)")),
                separator(),
                make_menu_item(1, L("Run Memory Bandwidth (256MB Alloc)", "Покрени Меморијски Проток (256MB)")),
                separator(),
                make_menu_item(2, L("Run Disk Throughput (192MB Temp)", "Покрени Проток Диска (192MB Темп)")),
                separator(),
                make_menu_item(3, L("Run LLM Tokens Estimation", "Процени Брзину Локалног LLM-а")),
                separator(),
                make_menu_item(4, L("Probe Ollama / LM Studio Server", "Скенирај Ollama / LM Studio Сервер"))
            )) | color(border_color) | size(WIDTH, EQUAL, 48);

            Elements results_rows;
            results_rows.push_back(text("  " + L("BENCHMARK TELEMETRY ARCHIVE", "АРХИВА РЕЗУЛТАТА ТЕСТИРАЊА") + "  ") | bold | color(accent_color));
            results_rows.push_back(separator());

            results_rows.push_back(hbox(
                text("  " + L("CPU Single-Core Core Burn:", "Једнојезгарно CPU Сагоревање:") + " "),
                filler(),
                text(local_bench.cpu_single_mops > 0 ? format_double(local_bench.cpu_single_mops, 2) + " MOP/s" : "N/A") | bold | color(primary_color)
            ));
            results_rows.push_back(hbox(
                text("  " + L("CPU Multi-Core Core Burn:", "Вишејезгарно CPU Сагоревање:") + " "),
                filler(),
                text(local_bench.cpu_multi_mops > 0 ? format_double(local_bench.cpu_multi_mops, 2) + " MOP/s" : "N/A") | bold | color(primary_color)
            ));
            results_rows.push_back(hbox(
                text("  " + L("CPU Multi-Thread Scaling:", "CPU Вишени Задати Опсег:") + " "),
                filler(),
                text(local_bench.cpu_scaling > 0 ? format_double(local_bench.cpu_scaling, 2) + "x (" + to_string(local_bench.cpu_threads) + " " + L("Cores", "Језгара") + ")" : "N/A") | bold | color(accent_color)
            ));
            results_rows.push_back(separator());

            results_rows.push_back(hbox(
                text("  " + L("RAM Write Throughput:", "РАМ Проток Уписа:") + "      "),
                filler(),
                text(local_bench.mem_write_gbps > 0 ? format_double(local_bench.mem_write_gbps, 2) + " GB/s" : "N/A") | bold | color(secondary_color)
            ));
            results_rows.push_back(hbox(
                text("  " + L("RAM Read Throughput:", "РАМ Проток Читања:") + "       "),
                filler(),
                text(local_bench.mem_read_gbps > 0 ? format_double(local_bench.mem_read_gbps, 2) + " GB/s" : "N/A") | bold | color(secondary_color)
            ));
            results_rows.push_back(hbox(
                text("  " + L("RAM Copy/Triad Bandwidth:", "РАМ Копирање/Триад Проток:") + "  "),
                filler(),
                text(local_bench.mem_copy_gbps > 0 ? format_double(local_bench.mem_copy_gbps, 2) + " GB/s" : "N/A") | bold | color(accent_color)
            ));
            results_rows.push_back(separator());

            results_rows.push_back(hbox(
                text("  " + L("Disk Sequential Write:", "Диск Узастопни Упис:") + "     "),
                filler(),
                text(local_bench.disk_write_mbps > 0 ? format_double(local_bench.disk_write_mbps, 1) + " MB/s" : "N/A") | bold | color(primary_color)
            ));
            results_rows.push_back(hbox(
                text("  " + L("Disk Sequential Read:", "Диск Узастопно Читање:") + "      "),
                filler(),
                text(local_bench.disk_read_mbps > 0 ? format_double(local_bench.disk_read_mbps, 1) + " MB/s" : "N/A") | bold | color(primary_color)
            ));
            results_rows.push_back(separator());

            results_rows.push_back(text("  " + L("PROJECTED LOCAL AI SPEEDS (CPU INFERENCE)", "ПРОЈЕКТОВАНЕ БРЗИНЕ ЛОКАЛНОГ АИ-ја (CPU)") + "  ") | bold | color(accent_color));
            if (local_bench.llm_estimates.empty()) {
                results_rows.push_back(text("  " + L("Run LLM Tokens Estimation to calculate speeds...", "Покрени процену LLM токена за израчунавање брзине...")) | color(Color::GrayDark));
            } else {
                for (const auto& est : local_bench.llm_estimates) {
                    results_rows.push_back(hbox(
                        text("   - " + est.first + ": "),
                        filler(),
                        text(format_double(est.second, 1) + " tok/s") | bold | color(Color::Green)
                    ));
                }
            }
            results_rows.push_back(separator());

            results_rows.push_back(hbox(
                text("  " + L("Local Ollama API status:", "Статус локалног Ollama API-ја:") + "   "),
                filler(),
                text(local_bench.ollama_status) | bold | color(local_bench.ollama_status.find("ONLINE") != string::npos ? Color::Green : Color::Red)
            ));
            results_rows.push_back(hbox(
                text("  " + L("LM Studio Server status:", "Статус LM Studio сервера:") + "   "),
                filler(),
                text(local_bench.lmstudio_status) | bold | color(local_bench.lmstudio_status == "ONLINE" ? Color::Green : Color::Red)
            ));

            auto results_box = window(text("  " + L("TEST RESULTS ARCHIVE", "АРХИВА РЕЗУЛТАТА") + "  ") | bold, vbox(move(results_rows))) | color(border_color) | flex;

            right_panel = vbox(header, progress_bar, hbox(action_list, results_box) | flex);
        }
        else if (current_tab == 2) {
            // SYSTEM OPTIMIZATION TAB
            auto header = hbox(
                text("  " + L("AURA SYSTEM OPTIMIZER", "АУРА ОПТИМИЗАЦИЈА СИСТЕМА") + "  ") | bold | color(primary_color),
                text(" " + L("fine-tune OS settings for maximum efficiency", "прилагодите подешавања оперативног система") + " ") | color(Color::GrayDark)
            );

            string active_profile = local_opt_status.power_profile;
            bool active_boost = local_opt_status.cpu_boost;
            int swappiness_val = local_opt_status.swappiness;

            auto make_opt_item = [&](int idx, const string& title, const string& current_val, const string& action_label) -> Element {
                bool is_sel = (idx == selected_opt_menu);
                auto prefix = is_sel ? "  [Enter] * " : "            ";
                auto col = is_sel ? primary_color : Color::White;
                return vbox(
                    hbox(
                        text(prefix + title) | bold | color(col),
                        filler(),
                        text(current_val) | bold | color(accent_color)
                    ),
                    hbox(
                        text("            " + action_label) | color(Color::GrayLight)
                    ),
                    separator()
                );
            };

            auto tuning_list = window(text("  " + L("OS TUNING INTERFACES (REQUIRES POLKIT/PKEXEC)", "ИНТЕРФЕЈСИ ЗА ПРИЛАГОЂАВАЊЕ СИСТЕМА (ТРАЖИ СУДО)") + "  ") | bold, vbox(
                make_opt_item(0, L("Power Daemon: Balanced Profile", "Режим потрошње: Балансирано"), active_profile == "balanced" ? "ACTIVE" : "INACTIVE", L("Press [Enter] to switch power profiles daemon to Balanced.", "Притисните [Enter] да пребаците на балансиран режим потрошње.")),
                make_opt_item(1, L("Power Daemon: Performance Profile", "Режим потрошње: Перформансе"), active_profile == "performance" ? "ACTIVE" : "INACTIVE", L("Press [Enter] to switch power profiles daemon to Performance.", "Притисните [Enter] да пребаците на режим високих перформанси.")),
                make_opt_item(2, L("Power Daemon: Power Saver Profile", "Режим потрошње: Штедња батерије"), active_profile == "power-saver" ? "ACTIVE" : "INACTIVE", L("Press [Enter] to switch power profiles daemon to Power Saver.", "Притисните [Enter] да пребаците на режим штедње батерије.")),
                make_opt_item(3, L("CPU Core Turbo Boost", "CPU Турбо Буст"), active_boost ? "ENABLED" : "DISABLED", L("Press [Enter] to toggle dynamic CPU core clock boost.", "Притисните [Enter] да промените динамичко убрзање CPU језгара.")),
                make_opt_item(4, L("Linux Swappiness: Increase (+10)", "Линукс Свапинес: Повећај (+10)"), to_string(swappiness_val) + " / 100", L("Press [Enter] to increase swappiness threshold.", "Притисните [Enter] да повећате границу сваповања.")),
                make_opt_item(5, L("Linux Swappiness: Decrease (-10)", "Линукс Свапинес: Смањи (-10)"), to_string(swappiness_val) + " / 100", L("Press [Enter] to decrease swappiness threshold.", "Притисните [Enter] да смањите границу сваповања.")),
                make_opt_item(6, L("Rebalance & Online All CPU Cores", "Ребалансирање и Буђење CPU Језгара"), "SPREAD ON", L("Press [Enter] to run kernel online validator and spread hardware IRQ interrupts.", "Притисните [Enter] да покренете валидацију језгара и ребалансирате прекиде.")),
                make_opt_item(7, L("Drop OS RAM Page Cache", "Ослободи Системски РАМ Кеш"), "SYNC", L("Press [Enter] to safely sync storage queues and dump buffered caches.", "Притисните [Enter] да безбедно синхронизујете уписе и ослободите кеш меморију."))
            )) | color(border_color) | flex;

            right_panel = vbox(header, tuning_list);
        }
        else if (current_tab == 3) {
            // CLIPBOARD VAULT & AI TAB & LIVE GPU CALCULATOR
            auto header = hbox(
                text("  " + L("AURA CLIPBOARD VAULT, LIVE CALCULATOR & AI COPILOT", "АУРА СЕФ БЕЛЕЖНИК, ЛАЈВ КАЛКУЛАТОР И АИ КО-ПИЛОТ") + "  ") | bold | color(primary_color),
                text(" " + L("[Tab] switch pane · live datacenter & home GPU costing", "[Tab] фокус · лајв прорачун потрошње и трошкова") + " ") | color(Color::GrayDark)
            );

            // PANE 0: Clipboard Vault
            Elements vault_rows;
            vector<ClipItem> clips;
            {
                std::lock_guard<std::mutex> lk(vault_mutex);
                clips = get_clips(vault_db);
            }

            if (clips.empty()) {
                vault_rows.push_back(text("  " + L("No clipboard content logged yet.", "Свеска је тренутно празна.") + "  ") | color(Color::GrayDark));
            } else {
                for (size_t i = 0; i < clips.size(); ++i) {
                    string s = clips[i].content;
                    std::replace(s.begin(), s.end(), '\n', ' ');
                    if (s.size() > 38) s = s.substr(0, 35) + "...";
                    auto prefix = (static_cast<int>(i) == selected_vault_idx) ? " * " : "   ";
                    auto col = (static_cast<int>(i) == selected_vault_idx) ? ((vault_focus_pane == 0) ? primary_color : Color::White) : Color::White;
                    vault_rows.push_back(text(prefix + s) | color(col) | bold);
                }
            }
            auto vault_border_col = (vault_focus_pane == 0) ? primary_color : border_color;
            auto vault_box = window(
                text("  " + L("CLIPBOARD CORES", "СЕФ КЛИПБОРД БЕЛЕЖНИЦА") + (vault_focus_pane == 0 ? " [ACTIVE]" : "") + "  ") | bold | color((vault_focus_pane == 0) ? primary_color : Color::White),
                vbox(move(vault_rows)) | vscroll_indicator | frame
            ) | color(vault_border_col) | size(HEIGHT, EQUAL, 8);

            // PANE 1: Live GPU Power & Cost Calculator
            auto make_styled_text_box = [&](int f_idx, const string& label, const string& display_val, const string& sub_info, const string& key_hint) -> Element {
                bool is_sel = (vault_focus_pane == 1) && (gpu_calc.selected_field == f_idx);
                auto title_col = is_sel ? primary_color : Color::White;
                auto box_border_col = is_sel ? primary_color : Color::RGB(70, 70, 80);
                auto val_col = is_sel ? accent_color : Color::RGB(220, 220, 220);
                auto bg_col = is_sel ? Color::RGB(25, 30, 45) : Color::Default;

                std::string val_with_cursor = display_val;
                if (is_sel && f_idx >= 1 && f_idx <= 4) {
                    val_with_cursor += "_";
                }

                return vbox({
                    hbox({
                        text(is_sel ? " ▶ " : "   ") | bold | color(title_col),
                        text(label) | bold | color(title_col),
                        filler(),
                        text(key_hint) | color(is_sel ? secondary_color : Color::GrayDark)
                    }),
                    hbox({
                        text("   "),
                        hbox({
                            text(is_sel ? " ▌ " : "   ") | bold | color(primary_color),
                            text(val_with_cursor) | bold | color(val_col),
                            filler(),
                            text(sub_info.empty() ? "" : (" (" + sub_info + ") ")) | color(Color::GrayLight)
                        }) | borderRounded | color(box_border_col) | bgcolor(bg_col) | flex
                    })
                });
            };

            auto calc_inputs = vbox({
                make_styled_text_box(0, L("GPU Model / Architecture:", "GPU Модел / Архитектура:"), "< " + gpu_calc.get_model_name() + " >", to_string(gpu_calc.get_tdp()) + "W TDP", L("[←/→/Enter] Cycle", "[←/→/Enter] Избор")),
                make_styled_text_box(1, L("GPU Units Count:", "Број GPU-ова:"), gpu_calc.count_str, "", L("[Type digits / +/-]", "[Укуцај / +/-]")),
                make_styled_text_box(2, L("Average Running Load (%):", "Просечно Оптерећење (%):"), gpu_calc.load_str + " %", format_double(gpu_calc.get_avg_watts_per_gpu(), 1) + " W/GPU", L("[Type / +/-]", "[Укуцај / +/-]")),
                make_styled_text_box(3, L("Facility PUE Overhead:", "PUE Фактор Објекта:"), gpu_calc.pue_str + " x", format_double((gpu_calc.pue - 1.0) * 100.0, 0) + "% overhead", L("[Type / +/-]", "[Укуцај / +/-]")),
                make_styled_text_box(4, L("Electricity Tariff ($/kWh):", "Цена Струје ($/kWh):"), "$" + gpu_calc.price_str + " / kWh", "", L("[Type / +/-]", "[Укуцај / +/-]")),
                
                // Quick Presets Bar
                vbox({
                    hbox({
                        text((vault_focus_pane == 1 && gpu_calc.selected_field == 5) ? " ⚙ " : "   ") | bold | color((vault_focus_pane == 1 && gpu_calc.selected_field == 5) ? primary_color : Color::White),
                        text(L("Quick Hardware Presets:", "Брзи Хардверски Режими:")) | bold | color((vault_focus_pane == 1 && gpu_calc.selected_field == 5) ? primary_color : Color::White),
                        filler(),
                        text(L("[1-4 / Enter]", "[1-4 / Enter]")) | color((vault_focus_pane == 1 && gpu_calc.selected_field == 5) ? secondary_color : Color::GrayDark)
                    }),
                    hbox({
                        text("   "),
                        hbox({
                            text(" [1] 20K A100 DC ") | bold | color(Color::Cyan),
                            text(" [2] Home 2x4090 ") | bold | color(Color::Yellow),
                            text(" [3] 8x H100 ") | bold | color(Color::Magenta),
                            text(" [4] 1K H100 ") | bold | color(Color::Green)
                        }) | borderRounded | color((vault_focus_pane == 1 && gpu_calc.selected_field == 5) ? primary_color : Color::GrayDark) | flex
                    })
                }),

                // Real-Time Results Matrix Card
                vbox({
                    hbox({
                        text(" ⚡ " + L("LIVE TELEMETRY & COST MATRIX", "ЛАЈВ ТЕЛЕМЕТРИЈА И ТРОШКОВИ")) | bold | color(glow_color),
                        filler(),
                        text(L("Continuous 24/7/365", "Непрекидан рад 24/7/365")) | color(Color::GrayDark)
                    }),
                    vbox({
                        hbox({
                            text(" • " + L("IT Cluster Draw:", "ИТ Потрошња:") + " ") | color(Color::GrayLight),
                            filler(),
                            text(format_power(gpu_calc.get_total_it_watts())) | bold | color(primary_color)
                        }),
                        hbox({
                            text(" • " + L("Facility Power (PUE " + format_double(gpu_calc.pue, 2) + "):", "Датацентар (PUE):") + " ") | color(Color::GrayLight),
                            filler(),
                            text(format_power(gpu_calc.get_facility_watts())) | bold | color(accent_color)
                        }),
                        separator(),
                        hbox({
                            text(" • " + L("Annual Energy (8,760h):", "Годишња енергија:") + " ") | color(Color::GrayLight),
                            filler(),
                            text(format_energy(gpu_calc.get_yearly_kwh())) | bold | color(success_color)
                        }),
                        hbox({
                            text(" • " + L("Annual Electricity Cost:", "Годишњи трошак:") + " ") | bold | color(Color::White),
                            filler(),
                            text("$" + format_money(gpu_calc.get_yearly_cost()) + " / yr") | bold | color(Color::RGB(0, 255, 128))
                        }),
                        hbox({
                            text(" • " + L("Breakdown:", "Расподела:") + " ") | color(Color::GrayDark),
                            filler(),
                            text("$" + format_money(gpu_calc.get_monthly_cost()) + "/mo · $" + format_money(gpu_calc.get_daily_cost()) + "/d · $" + format_double(gpu_calc.get_hourly_cost(), 0) + "/h") | color(Color::GrayLight)
                        })
                    }) | border | color(Color::RGB(50, 70, 100))
                }),

                // Save Button
                hbox({
                    text("   "),
                    hbox({
                        filler(),
                        text(L("💾 [Enter / c] Save Breakdown to Vault & Copy to Clipboard", "💾 [Enter / c] Сачувај у Сеф и Копирај")) | bold | color((vault_focus_pane == 1 && gpu_calc.selected_field == 6) ? Color::Black : primary_color),
                        filler()
                    }) | borderRounded | bgcolor((vault_focus_pane == 1 && gpu_calc.selected_field == 6) ? primary_color : Color::Default) | color((vault_focus_pane == 1 && gpu_calc.selected_field == 6) ? Color::White : primary_color) | flex
                }),

                text(" " + L("Sources: NVIDIA Tech Specs · Uptime Institute PUE · U.S. EIA Electric Rates", "Извори: NVIDIA · Uptime Institute · U.S. EIA")) | color(Color::GrayDark)
            });

            auto calc_border_col = (vault_focus_pane == 1) ? primary_color : border_color;
            auto datacenter_box = window(
                text("  " + L("⚡ LIVE GPU POWER & COST CALCULATOR", "⚡ ЛАЈВ КАЛКУЛАТОР ПОТРОШЊЕ И ТРОШКОВА GPU-ОВА") + (vault_focus_pane == 1 ? " [ACTIVE]" : "") + "  ") | bold | color((vault_focus_pane == 1) ? primary_color : Color::White),
                calc_inputs | vscroll_indicator | frame
            ) | color(calc_border_col) | flex;

            auto left_pane = vbox(vault_box, datacenter_box) | size(WIDTH, EQUAL, 52);

            // PANE 2: AI COPILOT
            auto make_ai_input_row = [&](int idx, const string& label, const string& val) -> Element {
                bool is_sel = (idx == selected_ai_menu) && (vault_focus_pane == 2);
                auto prefix = is_sel ? " * " : "   ";
                auto col = is_sel ? primary_color : Color::White;
                return hbox(
                    text(prefix + label) | bold | color(col),
                    text(val) | color(accent_color) | bold
                );
            };

            auto make_recipe_row = [&](int idx, const string& label) -> Element {
                bool is_sel = (idx == selected_ai_menu) && (vault_focus_pane == 2);
                auto prefix = is_sel ? " [Enter] -> " : "            ";
                auto col = is_sel ? primary_color : Color::White;
                return text(prefix + label) | color(col) | bold;
            };

            auto ai_inputs = vbox(
                make_ai_input_row(0, L("Provider: ", "АИ Сервер: "), local_ai_config.provider),
                separator(),
                make_ai_input_row(1, L("Model:    ", "АИ Модел:  "), local_ai_config.model + " (type to edit)"),
                separator(),
                make_ai_input_row(2, L("API Key:  ", "АПИ Кључ:  "), local_ai_config.api_key.empty() ? "NONE" : string(std::min(static_cast<size_t>(10), local_ai_config.api_key.size()), '*') + " (type to edit)"),
                separator(),
                hbox({
                    text("   ") | bold,
                    text(L("Context: ", "Контекст: ")) | bold | color(Color::GrayLight),
                    filler(),
                    text(g_token_stats.format_token_count(token_remaining) + " left") | color(token_remaining < 1000 ? Color::Red : Color::Green)
                }),
                separator(),
                hbox({
                    text("   ") | bold,
                    text(L("Status: ", "Статус: ")) | bold | color(Color::GrayLight),
                    filler(),
                    text(local_ai_thinking ? L("Thinking...", "Размишља...") : L("Idle", "Мирно")) | color(local_ai_thinking ? accent_color : Color::Green)
                }),
                separator(),
                make_ai_input_row(3, L("Prompt:   ", "Питање:    "), custom_question + "_"),
                separator(),
                make_recipe_row(4, L("Submit Custom Prompt", "Пошаљи сопствени упит")),
                separator(),
                make_recipe_row(5, L("AI Recipe: Summarize Active Clip", "АИ Рецепт: Скрати активну белешку")),
                separator(),
                make_recipe_row(6, L("AI Recipe: Audit Code Clip", "АИ Рецепт: Ревидирај програмски код")),
                separator(),
                make_recipe_row(7, L("AI Recipe: Translate SRB/ENG", "АИ Рецепт: Преведи српски/енглески")),
                separator(),
                make_recipe_row(8, L("AI Recipe: 20K A100 Datacenter Cost", "АИ Рецепт: Анализа трошкова 20K A100"))
            );

            auto ai_border_col = (vault_focus_pane == 2) ? primary_color : border_color;
            auto ai_config_box = window(
                text("  " + L("AURA AI COPILOT", "КОНФИГУРАЦИЈА АИ-ја") + (vault_focus_pane == 2 ? " [ACTIVE]" : "") + "  ") | bold | color((vault_focus_pane == 2) ? primary_color : Color::White),
                ai_inputs
            ) | color(ai_border_col) | size(HEIGHT, EQUAL, 23);

            Elements resp_wrapped;
            size_t max_char = right_w > 52 ? right_w - 52 : 30;
            string current_word;
            size_t line_len = 0;
            for (char c : local_ai_response_text) {
                if (c == '\n') {
                    resp_wrapped.push_back(text(current_word));
                    current_word.clear();
                    line_len = 0;
                } else if (c == ' ') {
                    if (line_len + current_word.size() > max_char) {
                        resp_wrapped.push_back(text(current_word));
                        current_word.clear();
                        line_len = 0;
                    } else {
                        current_word += c;
                        line_len += current_word.size();
                    }
                } else {
                    current_word += c;
                }
            }
            if (!current_word.empty()) {
                resp_wrapped.push_back(text(current_word));
            }

            auto ai_response_box = window(text("  " + L("CO-PILOT ANSWERS FEED", "ОДГОВОРИ АИ КО-ПИЛОТА") + "  ") | bold, vbox(move(resp_wrapped)) | vscroll_indicator | frame) | color(border_color) | flex;

            right_panel = vbox(header, hbox(left_pane, vbox(ai_config_box, ai_response_box) | flex) | flex);
        }

        auto footer = hbox({
            text(" " + L("Navigation:", "Навигација:") + " ") | bold,
        });

        if (current_tab == 0) {
            footer = hbox({
                text(" " + L("Navigation:", "Навигација:") + " ") | bold, text(" [F1-F4] ") | bgcolor(Color::Blue), text(" " + L("Switch Tab", "Избор таба") + " "),
                text(" [↑/↓] ") | bgcolor(Color::GrayDark), text(" " + L("Select Process", "Одабир процеса") + " "),
                text(" [k] ") | bgcolor(Color::Red) | bold, text(" " + L("SIGKILL Process", "Принудно Заустави") + " "),
                text(" [1-5] ") | bgcolor(Color::GrayDark), text(" " + L("Switch Theme", "Избор теме") + " "),
                filler(),
                text(" [q] " + L("Exit", "Излаз") + " ") | bgcolor(Color::GrayDark)
            });
        } else if (current_tab == 1) {
            footer = hbox({
                text(" " + L("Navigation:", "Навигација:") + " ") | bold, text(" [F1-F4] ") | bgcolor(Color::Blue), text(" " + L("Switch Tab", "Избор таба") + " "),
                text(" [↑/↓] ") | bgcolor(Color::GrayDark), text(" " + L("Select Test", "Одабир теста") + " "),
                text(" [Enter] ") | bgcolor(Color::Green) | bold, text(" " + L("Run Selected Test", "Покрени тест") + " "),
                filler(),
                text(" [q] " + L("Exit", "Излаз") + " ") | bgcolor(Color::GrayDark)
            });
        } else if (current_tab == 2) {
            footer = hbox({
                text(" " + L("Navigation:", "Навигација:") + " ") | bold, text(" [F1-F4] ") | bgcolor(Color::Blue), text(" " + L("Switch Tab", "Избор таба") + " "),
                text(" [↑/↓] ") | bgcolor(Color::GrayDark), text(" " + L("Select Option", "Одабир опције") + " "),
                text(" [Enter] ") | bgcolor(Color::Green) | bold, text(" " + L("Trigger Optimization", "Примени") + " "),
                filler(),
                text(" [q] " + L("Exit", "Излаз") + " ") | bgcolor(Color::GrayDark)
            });
        } else if (current_tab == 3) {
            if (vault_focus_pane == 0) {
                footer = hbox({
                    text(" " + L("Navigation:", "Навигација:") + " ") | bold, text(" [F1-F4] ") | bgcolor(Color::Blue), text(" " + L("Switch Tab", "Избор таба") + " "),
                    text(" [Tab] ") | bgcolor(Color::Cyan) | bold, text(" " + L("Switch to Live Calc", "Пребаци на Калкулатор") + " "),
                    text(" [↑/↓] ") | bgcolor(Color::GrayDark), text(" " + L("Select Clip", "Одабир ставке") + " "),
                    text(" [Enter] ") | bgcolor(Color::Green) | bold, text(" " + L("Copy back", "Копирај назад") + " "),
                    text(" [d] ") | bgcolor(Color::Red) | bold, text(" " + L("Delete Clip", "Обриши белешку") + " "),
                    filler(),
                    text(" [q] " + L("Exit", "Излаз") + " ") | bgcolor(Color::GrayDark)
                });
            } else if (vault_focus_pane == 1) {
                footer = hbox({
                    text(" " + L("Live Calc:", "Лајв Калкулатор:") + " ") | bold, text(" [F1-F4] ") | bgcolor(Color::Blue), text(" " + L("Switch Tab", "Избор таба") + " "),
                    text(" [Tab] ") | bgcolor(Color::Cyan) | bold, text(" " + L("Switch to AI", "Пребаци на АИ") + " "),
                    text(" [↑/↓] ") | bgcolor(Color::GrayDark), text(" " + L("Select Field", "Одабир ставке") + " "),
                    text(" [←/→ / +/-] ") | bgcolor(Color::Green) | bold, text(" " + L("Tune Value", "Подеси вредност") + " "),
                    text(" [1-4] ") | bgcolor(Color::Yellow) | bold, text(" " + L("Presets", "Брзи режими") + " "),
                    text(" [c / Enter] ") | bgcolor(Color::Magenta) | bold, text(" " + L("Export & Save", "Сачувај у Сеф") + " "),
                    filler(),
                    text(" [q] " + L("Exit", "Излаз") + " ") | bgcolor(Color::GrayDark)
                });
            } else {
                footer = hbox({
                    text(" " + L("Aura AI:", "Аура АИ:") + " ") | bold, text(" [F1-F4] ") | bgcolor(Color::Blue), text(" " + L("Switch Tab", "Избор таба") + " "),
                    text(" [Tab] ") | bgcolor(Color::Cyan) | bold, text(" " + L("Switch to Vault", "Пребаци на Сеф") + " "),
                    text(" [↑/↓] ") | bgcolor(Color::GrayDark), text(" " + L("Select Option/Recipe", "Одабир ставке") + " "),
                    text(" [Enter] ") | bgcolor(Color::Green) | bold, text(" " + L("Execute Recipe/Prompt", "Покрени АИ упит") + " "),
                    filler(),
                    text(" [q] " + L("Exit", "Излаз") + " ") | bgcolor(Color::GrayDark)
                });
            }
        }

        return vbox({ hbox({ sidebar | size(WIDTH, EQUAL, 28), right_panel | flex }), footer }) | bgcolor(panel_bg);
    });

    screen.Loop(renderer);

    refresh_ui_loop = false;
    if (data_thread.joinable()) data_thread.join();

    // Join all background threads (benchmarks, optimizations, AI queries)
    for (auto& t : bg_threads) {
        if (t.joinable()) t.join();
    }

    // Cleanup SQLite
    if (vault_db) {
        sqlite3_close(vault_db);
        vault_db = nullptr;
    }

    return 0;
}
