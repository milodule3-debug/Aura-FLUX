#pragma once
#include <cstdint>
#include <chrono>
#include <vector>
#include <thread>
#include <string>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <mutex>
#include "types.h"
#include "utils.h"
#include "ftxui/component/screen_interactive.hpp"

namespace fs = std::filesystem;

inline uint64_t mix_cpp(uint64_t x, uint64_t iters) {
    for (uint64_t i = 0; i < iters; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x = x * 0x2545F4914F6CDD1DULL;
    }
    return x;
}

inline double cpu_burn_cpp(double secs) {
    uint64_t chunk = 2000000;
    uint64_t x = 0x9E3779B97F4A7C15ULL;
    uint64_t done = 0;
    auto t0 = std::chrono::high_resolution_clock::now();
    while (true) {
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(now - t0).count();
        if (elapsed >= secs) break;
        volatile uint64_t res = mix_cpp(x, chunk);
        x = res;
        done += chunk;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double real_elapsed = std::chrono::duration<double>(t1 - t0).count();
    return (real_elapsed > 0) ? (static_cast<double>(done) / real_elapsed / 1e6) : 0.0;
}

template <typename Fn>
inline void update_bench(BenchmarkResults& res, std::mutex* bench_mutex, Fn&& fn) {
    if (bench_mutex) {
        std::lock_guard<std::mutex> lk(*bench_mutex);
        fn(res);
        return;
    }
    fn(res);
}

inline void run_cpu_test(BenchmarkResults& res, ftxui::ScreenInteractive& screen, std::mutex* bench_mutex = nullptr) {
    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.cpu_running = true;
        r.progress_pct = 5;
        r.progress_label = "Running single-core burn...";
    });
    safe_post_event(screen);

    double cpu_single_mops = cpu_burn_cpp(1.2);

    int cpu_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (cpu_threads <= 0) cpu_threads = 1;
    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.cpu_single_mops = cpu_single_mops;
        r.progress_pct = 50;
        r.cpu_threads = cpu_threads;
        r.progress_label = "Running multi-core burn (" + std::to_string(cpu_threads) + " threads)...";
    });
    safe_post_event(screen);

    std::vector<std::thread> pool;
    std::vector<double> scores(cpu_threads, 0.0);
    for (int i = 0; i < cpu_threads; ++i) {
        pool.push_back(std::thread([i, &scores]() {
            scores[i] = cpu_burn_cpp(1.2);
        }));
    }
    for (auto& t : pool) t.join();

    double sum = 0.0;
    for (double s : scores) sum += s;
    double scaling = (cpu_single_mops > 0) ? (sum / cpu_single_mops) : 0.0;

    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.cpu_multi_mops = sum;
        r.cpu_scaling = scaling;
        r.progress_pct = 100;
        r.progress_label = "CPU benchmark done!";
        r.cpu_running = false;
    });
    safe_post_event(screen);
}

inline void run_mem_test(BenchmarkResults& res, ftxui::ScreenInteractive& screen, std::mutex* bench_mutex = nullptr) {
    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.mem_running = true;
        r.progress_pct = 10;
        r.progress_label = "Allocating 256 MB buffer...";
    });
    safe_post_event(screen);

    size_t N = 16 * 1024 * 1024;
    double bytes = static_cast<double>(N * 8);

    std::vector<uint64_t> a(N, 0);
    std::vector<uint64_t> b(N, 0);

    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.progress_pct = 30;
        r.progress_label = "Running write pass...";
    });
    safe_post_event(screen);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < N; ++i) {
        a[i] = i;
    }
    volatile uint64_t* barrier_ptr = a.data();
    (void)barrier_ptr[N - 1];
    auto t1 = std::chrono::high_resolution_clock::now();
    double w_elapsed = std::chrono::duration<double>(t1 - t0).count();
    double mem_write_gbps = bytes / w_elapsed / 1e9;
    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.mem_write_gbps = mem_write_gbps;
        r.progress_pct = 60;
        r.progress_label = "Running read pass...";
    });
    safe_post_event(screen);

    t0 = std::chrono::high_resolution_clock::now();
    uint64_t sum = 0;
    for (size_t i = 0; i < N; ++i) {
        sum += a[i];
    }
    volatile uint64_t sum_barrier = sum;
    (void)sum_barrier;
    t1 = std::chrono::high_resolution_clock::now();
    double r_elapsed = std::chrono::duration<double>(t1 - t0).count();
    double mem_read_gbps = bytes / r_elapsed / 1e9;
    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.mem_read_gbps = mem_read_gbps;
        r.progress_pct = 80;
        r.progress_label = "Running copy pass...";
    });
    safe_post_event(screen);

    t0 = std::chrono::high_resolution_clock::now();
    std::copy(a.begin(), a.end(), b.begin());
    volatile uint64_t* b_barrier = b.data();
    (void)b_barrier[N - 1];
    t1 = std::chrono::high_resolution_clock::now();
    double c_elapsed = std::chrono::duration<double>(t1 - t0).count();
    double mem_copy_gbps = (bytes * 2.0) / c_elapsed / 1e9;
    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.mem_copy_gbps = mem_copy_gbps;
        r.progress_pct = 100;
        r.progress_label = "Memory benchmark done!";
        r.mem_running = false;
    });
    safe_post_event(screen);
}

inline void run_disk_test(BenchmarkResults& res, ftxui::ScreenInteractive& screen, std::mutex* bench_mutex = nullptr) {
    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.disk_running = true;
        r.progress_pct = 10;
        r.progress_label = "Preparing write buffer (192 MB)...";
    });
    safe_post_event(screen);

    size_t block_size = 8 * 1024 * 1024;
    size_t num_blocks = 24;
    std::vector<uint8_t> block(block_size);
    for (size_t i = 0; i < block_size; ++i) {
        block[i] = static_cast<uint8_t>((i * 2654435761ULL) >> 16);
    }

    const char* home_c = getenv("HOME");
    if (!home_c) {
        update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
            r.progress_label = "Error: HOME not set";
            r.disk_running = false;
        });
        safe_post_event(screen);
        return;
    }
    std::string home(home_c);
    std::string path = home + "/.local/share/powerboard/bench.tmp";
    fs::create_directories(home + "/.local/share/powerboard");

    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.progress_pct = 20;
        r.progress_label = "Writing blocks...";
    });
    safe_post_event(screen);

    auto t0 = std::chrono::high_resolution_clock::now();
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) {
        update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
            r.progress_label = "Disk error: could not write file";
            r.disk_running = false;
        });
        safe_post_event(screen);
        return;
    }

    for (size_t i = 0; i < num_blocks; ++i) {
        f.write(reinterpret_cast<const char*>(block.data()), block_size);
        int progress_pct = 20 + static_cast<int>(30 * (static_cast<double>(i) / num_blocks));
        update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
            r.progress_pct = progress_pct;
        });
        safe_post_event(screen);
    }
    f.flush();
    int fd = open(path.c_str(), O_WRONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    f.close();
    auto t1 = std::chrono::high_resolution_clock::now();
    double w_elapsed = std::chrono::duration<double>(t1 - t0).count();
    double disk_write_mbps = (static_cast<double>(block_size * num_blocks)) / w_elapsed / 1e6;
    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.disk_write_mbps = disk_write_mbps;
        r.progress_pct = 60;
        r.progress_label = "Reading blocks...";
    });
    safe_post_event(screen);

    t0 = std::chrono::high_resolution_clock::now();
    std::ifstream f_in(path, std::ios::binary);
    if (!f_in.is_open()) {
        update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
            r.progress_label = "Disk error: could not read file";
            r.disk_running = false;
        });
        safe_post_event(screen);
        return;
    }
    std::vector<uint8_t> read_buf(block_size);
    while (f_in.read(reinterpret_cast<char*>(read_buf.data()), block_size)) {
        volatile uint8_t* barrier = read_buf.data();
        (void)barrier[0];
    }
    f_in.close();
    t1 = std::chrono::high_resolution_clock::now();
    double r_elapsed = std::chrono::duration<double>(t1 - t0).count();
    double disk_read_mbps = (static_cast<double>(block_size * num_blocks)) / r_elapsed / 1e6;

    fs::remove(path);

    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.disk_read_mbps = disk_read_mbps;
        r.progress_pct = 100;
        r.progress_label = "Disk benchmark done!";
        r.disk_running = false;
    });
    safe_post_event(screen);
}

inline void run_llm_test(BenchmarkResults& res, ftxui::ScreenInteractive& screen, std::mutex* bench_mutex = nullptr) {
    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.llm_running = true;
        r.progress_pct = 10;
        r.progress_label = "Measuring memory copy bandwidth...";
    });
    safe_post_event(screen);

    run_mem_test(res, screen, bench_mutex);

    double bw = 12.5;
    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        if (r.mem_copy_gbps > 0) bw = r.mem_copy_gbps;
    });

    double eff = bw * 0.72;
    std::vector<std::pair<std::string, double>> llm_estimates;
    llm_estimates.push_back({"1B q4 (0.75 GB)", eff / 0.75});
    llm_estimates.push_back({"3B q4 (2.00 GB)", eff / 2.0});
    llm_estimates.push_back({"7B q4 (4.40 GB)", eff / 4.4});
    llm_estimates.push_back({"13B q4 (7.90 GB)", eff / 7.9});
    llm_estimates.push_back({"34B q4 (19.5 GB)", eff / 19.5});
    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.llm_bw = bw;
        r.llm_estimates = llm_estimates;
        r.progress_pct = 70;
        r.progress_label = "Probing local Ollama and LM Studio...";
    });
    safe_post_event(screen);

    std::string ollama_status = "OFFLINE";
    FILE* p_ollama = popen("curl -s -m 2 -H \"User-Agent: Aura-Pulse/0.3.0\" http://127.0.0.1:11434/api/tags 2>/dev/null", "r");
    if (p_ollama) {
        std::string out; char buf[128];
        while (fgets(buf, sizeof(buf), p_ollama)) out += buf;
        pclose(p_ollama);
        if (out.find("models") != std::string::npos) {
            size_t m_pos = out.find("\"name\":\"");
            if (m_pos != std::string::npos) {
                size_t end_pos = out.find("\"", m_pos + 8);
                std::string model = out.substr(m_pos + 8, end_pos - (m_pos + 8));
                ollama_status = "ONLINE (" + model + ")";
            } else {
                ollama_status = "ONLINE (No models)";
            }
        }
    }

    std::string lmstudio_status = "OFFLINE";
    FILE* p_lms = popen("curl -s -m 2 -H \"User-Agent: Aura-Pulse/0.3.0\" http://127.0.0.1:1234/v1/models 2>/dev/null", "r");
    if (p_lms) {
        std::string out; char buf[128];
        while (fgets(buf, sizeof(buf), p_lms)) out += buf;
        pclose(p_lms);
        if (out.find("\"id\"") != std::string::npos) {
            lmstudio_status = "ONLINE";
        }
    }

    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.ollama_status = ollama_status;
        r.lmstudio_status = lmstudio_status;
        r.progress_pct = 100;
        r.progress_label = "LLM estimator done!";
        r.llm_running = false;
    });
    safe_post_event(screen);
}

inline void probe_local_servers(BenchmarkResults& res, ftxui::ScreenInteractive& screen, std::mutex* bench_mutex = nullptr) {
    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.progress_pct = 50;
        r.progress_label = "Probing local API servers...";
    });
    safe_post_event(screen);

    std::string ollama_status = "OFFLINE";
    FILE* p_ollama = popen("curl -s -m 2 -H \"User-Agent: Aura-Pulse/0.3.0\" http://127.0.0.1:11434/api/tags 2>/dev/null", "r");
    if (p_ollama) {
        std::string out; char buf[128];
        while (fgets(buf, sizeof(buf), p_ollama)) out += buf;
        pclose(p_ollama);
        if (out.find("models") != std::string::npos) {
            size_t m_pos = out.find("\"name\":\"");
            if (m_pos != std::string::npos) {
                size_t end_pos = out.find("\"", m_pos + 8);
                ollama_status = "ONLINE (" + out.substr(m_pos + 8, end_pos - (m_pos + 8)) + ")";
            } else {
                ollama_status = "ONLINE (No models)";
            }
        }
    }

    std::string lmstudio_status = "OFFLINE";
    FILE* p_lms = popen("curl -s -m 2 -H \"User-Agent: Aura-Pulse/0.3.0\" http://127.0.0.1:1234/v1/models 2>/dev/null", "r");
    if (p_lms) {
        std::string out; char buf[128];
        while (fgets(buf, sizeof(buf), p_lms)) out += buf;
        pclose(p_lms);
        if (out.find("\"id\"") != std::string::npos) lmstudio_status = "ONLINE";
    }

    update_bench(res, bench_mutex, [&](BenchmarkResults& r) {
        r.ollama_status = ollama_status;
        r.lmstudio_status = lmstudio_status;
        r.progress_pct = 100;
        r.progress_label = "Probes completed!";
    });
    safe_post_event(screen);
}
