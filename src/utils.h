#pragma once
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <random>
#include "types.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/canvas.hpp"
#include "ftxui/component/screen_interactive.hpp"

// Race-free screen wake: routes a no-op through FTXUI's internally-locked
// task queue instead of copying the static Event::Custom object (which races
// with the main loop's MultiReceiverBuffer and eventually deadlocks).
inline void safe_post_event(ftxui::ScreenInteractive& screen) {
    screen.Post([] {});
}

inline std::string format_double(double val, int precision) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << val;
    return oss.str();
}

inline std::string format_power(double watts) {
    if (watts >= 1e6) return format_double(watts / 1e6, 2) + " MW";
    if (watts >= 1e3) return format_double(watts / 1e3, 1) + " kW";
    return format_double(watts, 1) + " W";
}

inline std::string format_energy(double kwh) {
    if (kwh >= 1e6) return format_double(kwh / 1e6, 2) + "M kWh (" + format_double(kwh / 1e6, 2) + " GWh)";
    if (kwh >= 1e3) return format_double(kwh / 1e3, 1) + "k kWh";
    return format_double(kwh, 0) + " kWh";
}

inline std::string format_money(double usd) {
    if (usd >= 1e6) return format_double(usd / 1e6, 2) + "M";
    if (usd >= 1e3) return format_double(usd / 1e3, 1) + "K";
    return format_double(usd, 2);
}

inline std::string escape_json_string(const std::string& s) {
    std::ostringstream o;
    for (auto c : s) {
        if (c == '"') o << "\\\"";
        else if (c == '\\') o << "\\\\";
        else if (c == '\b') o << "\\b";
        else if (c == '\f') o << "\\f";
        else if (c == '\n') o << "\\n";
        else if (c == '\r') o << "\\r";
        else if (c == '\t') o << "\\t";
        else if (c < 0x20) {
            o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
        } else {
            o << c;
        }
    }
    return o.str();
}

inline void draw_arc(ftxui::Canvas& canvas, int cx, int cy, int r, double percentage, ftxui::Color col) {
    double start_angle = -225.0 * (M_PI / 180.0);
    double total_angle = 270.0 * (M_PI / 180.0) * (percentage / 100.0);
    double end_angle = start_angle + total_angle;

    for (double a = start_angle; a <= end_angle; a += 0.03) {
        int x = static_cast<int>(cx + r * cos(a) * 1.5);
        int y = static_cast<int>(cy + r * sin(a));
        canvas.DrawPoint(x, y, true, col);
    }

    double max_end_angle = start_angle + 270.0 * (M_PI / 180.0);
    for (double a = end_angle; a <= max_end_angle; a += 0.03) {
        int x = static_cast<int>(cx + r * cos(a) * 1.5);
        int y = static_cast<int>(cy + r * sin(a));
        canvas.DrawPoint(x, y, true, ftxui::Color::GrayDark);
    }
}

// Car-style rev gauge: 270° sweep with fixed color zones (ok / warn / redline),
// tick marks every 10%, and a needle pointing at the current value.
inline void draw_rev_gauge(ftxui::Canvas& c, int cx, int cy, int r, double pct,
                           ftxui::Color zone_ok, ftxui::Color zone_warn, ftxui::Color zone_red,
                           ftxui::Color needle_col, ftxui::Color dim_col) {
    const double a0 = -225.0 * M_PI / 180.0;
    const double sweep = 270.0 * M_PI / 180.0;
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    double fill_t = pct / 100.0;

    // Zoned track: filled part bright + thickened, rest dimmed
    for (double t = 0.0; t <= 1.0; t += 0.006) {
        double a = a0 + t * sweep;
        ftxui::Color zone = (t < 0.6) ? zone_ok : (t < 0.8) ? zone_warn : zone_red;
        int x = static_cast<int>(cx + r * cos(a) * 1.5);
        int y = static_cast<int>(cy + r * sin(a));
        if (t <= fill_t) {
            c.DrawPoint(x, y, true, zone);
            int x2 = static_cast<int>(cx + (r - 1) * cos(a) * 1.5);
            int y2 = static_cast<int>(cy + (r - 1) * sin(a));
            c.DrawPoint(x2, y2, true, zone);
        } else {
            c.DrawPoint(x, y, true, dim_col);
        }
    }

    // Tick marks every 10%
    for (int k = 0; k <= 10; ++k) {
        double a = a0 + (k / 10.0) * sweep;
        ftxui::Color tc = (k < 6) ? zone_ok : (k < 8) ? zone_warn : zone_red;
        int x1 = static_cast<int>(cx + (r - 1) * cos(a) * 1.5);
        int y1 = static_cast<int>(cy + (r - 1) * sin(a));
        int x2 = static_cast<int>(cx + (r + 2) * cos(a) * 1.5);
        int y2 = static_cast<int>(cy + (r + 2) * sin(a));
        c.DrawPointLine(x1, y1, x2, y2, tc);
    }

    // Needle + hub
    double an = a0 + fill_t * sweep;
    int nx = static_cast<int>(cx + (r - 3) * cos(an) * 1.5);
    int ny = static_cast<int>(cy + (r - 3) * sin(an));
    c.DrawPointLine(cx, cy, nx, ny, needle_col);
    c.DrawPoint(cx + 1, cy, true, needle_col);
    c.DrawPoint(cx - 1, cy, true, needle_col);
}

// Letter-spaced header text ("CPU MATRIX" -> "C P U  M A T R I X"), UTF-8 aware.
inline std::string spaced(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char uc = static_cast<unsigned char>(s[i]);
        size_t len = (uc < 0x80) ? 1 : ((uc & 0xE0) == 0xC0) ? 2 : ((uc & 0xF0) == 0xE0) ? 3 : 4;
        if (s[i] == ' ') {
            out += "  ";
            i += 1;
            continue;
        }
        out += s.substr(i, len);
        out += ' ';
        i += len;
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Four-stop vertical gradient used for the fill under flux graph lines.
// top is drawn right under the line, base at the graph floor.
struct FluxPalette {
    ftxui::Color top;
    ftxui::Color mid1;
    ftxui::Color mid2;
    ftxui::Color base;
};

// Neon line graph: bloom halo + thin bright polyline + gradient fill under it.
// history values are scaled against scale_max into [0, h_px-4].
// pal == nullptr uses the signature AURA palette (pink -> amber -> red -> blue).
inline void draw_flux_graph(ftxui::Canvas& canvas, const std::vector<double>& history,
                            size_t max_size, double scale_max, int w_px, int h_px,
                            ftxui::Color line_col, ftxui::Color fill_col,
                            const FluxPalette* pal = nullptr) {
    if (history.size() < 2 || scale_max <= 0.0 || max_size < 2) return;
    (void)fill_col;
    static const FluxPalette signature = {
        ftxui::Color::RGB(255, 45, 130), ftxui::Color::RGB(255, 210, 0),
        ftxui::Color::RGB(255, 70, 70), ftxui::Color::RGB(15, 60, 150)
    };
    const FluxPalette& p = pal ? *pal : signature;
    auto flux_grad = [&p](float t) {
        if (t < 0.35f) return ftxui::Color::Interpolate(t / 0.35f, p.top, p.mid1);
        if (t < 0.75f) return ftxui::Color::Interpolate((t - 0.35f) / 0.40f, p.mid1, p.mid2);
        return ftxui::Color::Interpolate((t - 0.75f) / 0.25f, p.mid2, p.base);
    };
    ftxui::Color halo = ftxui::Color::Interpolate(0.35f, ftxui::Color::Black, line_col);
    int max_y = h_px - 4;
    int base_y = h_px - 2;
    int lx = -1, ly = -1;
    for (size_t i = 0; i < history.size(); ++i) {
        int x = static_cast<int>((static_cast<double>(i) / (max_size - 1)) * (w_px - 1));
        double v = history[i] / scale_max;
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        int y = base_y - static_cast<int>(v * max_y);
        if (lx != -1) {
            canvas.DrawPointLine(lx, ly - 1, x, y - 1, halo);
            canvas.DrawPointLine(lx, ly, x, y, line_col);
            for (int fx = lx; fx <= x; ++fx) {
                int top = ly + (x == lx ? 0 : ((fx - lx) * (y - ly)) / (x - lx));
                int span = base_y - top;
                for (int fy = top + 1; fy <= base_y; ++fy) {
                    float depth = span > 0 ? static_cast<float>(fy - top) / span : 1.0f;
                    canvas.DrawPoint(fx, fy, true, flux_grad(depth));
                }
            }
        }
        lx = x; ly = y;
    }
}

// Translation helper: returns Serbian Cyrillic when theme is SRBIJA, English otherwise.
// Usage: L("English text", "Српски текст")
inline std::string L(const std::string& en, const std::string& sr, Theme current_theme) {
    return (current_theme == SRBIJA) ? sr : en;
}

// ===========================================================================
// GRAPHICS ENHANCEMENTS
// ===========================================================================

// Deterministic PRNG seeded once, reused across frames for stable starfield.
inline std::mt19937& rng_engine() {
    static std::mt19937 gen(42);
    return gen;
}

// Draw a parallax starfield into the globe canvas background.
// Two layers: distant (dim, slow) and near (bright, fast), all drifting.
inline void draw_starfield(ftxui::Canvas& c, int w, int /*h*/, double t,
                           ftxui::Color deep, ftxui::Color near_col) {
    // Pre-computed star positions (fixed seed = stable layout).
    // 18 distant stars, 10 near stars.
    static const int star_tbl[][3] = {
        {3,5,0}, {12,3,0}, {22,7,0}, {31,2,0}, {40,6,0}, {48,1,0},
        {6,11,0}, {17,9,0}, {27,12,0}, {37,8,0}, {45,11,0}, {52,5,0},
        {8,1,0}, {25,4,0}, {44,3,0}, {2,9,0}, {35,13,0}, {50,10,0},
        {5,3,1}, {20,6,1}, {33,4,1}, {42,9,1}, {15,11,1},
        {49,7,1}, {10,8,1}, {29,1,1}, {38,12,1}, {24,10,1}
    };
    for (const auto& s : star_tbl) {
        int base_x = s[0], base_y = s[1], layer = s[2];
        if (layer == 0) {
            // Distant stars: very slow horizontal drift, subtle twinkle.
            int x = (base_x + static_cast<int>(t * 0.3)) % w;
            if (x < 0) x += w;
            double twinkle = 0.6 + 0.4 * sin(t * 0.8 + base_x * 1.7);
            auto col = ftxui::Color::Interpolate(static_cast<float>(1.0 - twinkle),
                                                 ftxui::Color::Black, deep);
            c.DrawPoint(x, base_y, true, col);
        } else {
            // Near stars: faster drift, brighter, small cross sparkle.
            int x = (base_x + static_cast<int>(t * 0.8)) % w;
            if (x < 0) x += w;
            double twinkle = 0.5 + 0.5 * sin(t * 1.5 + base_x);
            auto col = ftxui::Color::Interpolate(static_cast<float>(1.0 - twinkle),
                                                 ftxui::Color::Black, near_col);
            c.DrawPoint(x, base_y, true, col);
            if (twinkle > 0.75) {
                c.DrawPoint(x + 1, base_y, true, col);
                c.DrawPoint(x, base_y + 1, true, col);
            }
        }
    }
}

// Draw a wireframe latitude/longitude grid on the projected globe points.
// This adds a 3D mesh feel. Called AFTER globe points are projected.
struct ProjectedPoint { int sx, sy; double depth; };

inline void draw_globe_grid(ftxui::Canvas& c, double cx, double cy, double r,
                            double angle_y, double angle_x,
                            ftxui::Color grid_col, ftxui::Color grid_dim) {
    // 6 latitude rings, 8 longitude meridians.
    for (int lat = 0; lat < 6; ++lat) {
        double phi = (lat + 0.5) / 6.0 * M_PI;  // 0..PI
        double ring_r = sin(phi);
        double ring_z_base = cos(phi);
        int prev_sx = -1, prev_sy = -1;
        for (int lon = 0; lon <= 32; ++lon) {
            double theta = lon / 32.0 * 2.0 * M_PI;
            double x0 = ring_r * cos(theta);
            double y0 = ring_r * sin(theta);
            double z0 = ring_z_base;

            // Y rotation
            double x1 = x0 * cos(angle_y) + z0 * sin(angle_y);
            double z1 = -x0 * sin(angle_y) + z0 * cos(angle_y);
            // X rotation
            double y2 = y0 * cos(angle_x) - z1 * sin(angle_x);
            double z2 = y0 * sin(angle_x) + z1 * cos(angle_x);

            int sx = static_cast<int>(cx + x1 * r);
            int sy = static_cast<int>(cy + y2 * r);

            if (prev_sx >= 0) {
                auto col = (z2 > 0) ? grid_col : grid_dim;
                c.DrawPointLine(prev_sx, prev_sy, sx, sy, col);
            }
            prev_sx = sx; prev_sy = sy;
        }
    }
    // Longitude meridians
    for (int lon = 0; lon < 8; ++lon) {
        double theta = lon / 8.0 * 2.0 * M_PI;
        double mx0 = cos(theta);
        double mz0 = sin(theta);
        int prev_sx = -1, prev_sy = -1;
        for (int lat = 0; lat <= 24; ++lat) {
            double phi = lat / 24.0 * M_PI;
            double x0 = sin(phi) * mx0;
            double y0 = cos(phi);
            double z0 = sin(phi) * mz0;

            double x1 = x0 * cos(angle_y) + z0 * sin(angle_y);
            double z1 = -x0 * sin(angle_y) + z0 * cos(angle_y);
            double y2 = y0 * cos(angle_x) - z1 * sin(angle_x);
            double z2 = y0 * sin(angle_x) + z1 * cos(angle_x);

            int sx = static_cast<int>(cx + x1 * r);
            int sy = static_cast<int>(cy + y2 * r);

            if (prev_sx >= 0) {
                auto col = (z2 > 0) ? grid_col : grid_dim;
                c.DrawPointLine(prev_sx, prev_sy, sx, sy, col);
            }
            prev_sx = sx; prev_sy = sy;
        }
    }
}

// Draw animated scanline overlay on a flux graph canvas.
// A horizontal sweep line moves up and down, plus subtle CRT noise dots.
inline void draw_scanlines(ftxui::Canvas& c, int w, int h, double t,
                           ftxui::Color scan_col) {
    // Sweeping scan line: bounces between top and bottom.
    double phase = (sin(t * 0.6) + 1.0) * 0.5;  // 0..1
    int scan_y = static_cast<int>(phase * (h - 3)) + 1;
    auto dim_scan = ftxui::Color::Interpolate(0.7f, ftxui::Color::Black, scan_col);
    for (int x = 0; x < w; ++x) {
        c.DrawPoint(x, scan_y, true, dim_scan);
    }
    // Faint glow line one pixel above.
    auto faint = ftxui::Color::Interpolate(0.85f, ftxui::Color::Black, scan_col);
    if (scan_y > 1) {
        for (int x = 0; x < w; ++x) {
            c.DrawPoint(x, scan_y - 1, true, faint);
        }
    }
}

// Draw a pulsing border frame around a canvas region using dim characters.
// Creates an animated "energy field" effect.
inline void draw_pulse_border(ftxui::Canvas& c, int w, int h, double t,
                              ftxui::Color border_col) {
    double pulse = 0.5 + 0.5 * sin(t * 2.0);
    auto col = ftxui::Color::Interpolate(static_cast<float>(0.5 - pulse * 0.3),
                                         ftxui::Color::Black, border_col);
    // Corner accent pixels — pulse in brightness.
    c.DrawPoint(0, 0, true, col);
    c.DrawPoint(w - 1, 0, true, col);
    c.DrawPoint(0, h - 1, true, col);
    c.DrawPoint(w - 1, h - 1, true, col);
}
