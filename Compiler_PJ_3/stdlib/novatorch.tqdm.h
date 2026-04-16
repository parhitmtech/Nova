#pragma once
#include <stdio.h>
#include <string>
#include <vector>
#include <chrono>

struct TqdmBar {
    int total;
    int width;
    int current;
    std::string desc;
    std::chrono::steady_clock::time_point start_time;
};

static std::vector<TqdmBar> tqdm_bars;

inline int tqdm_init(int total, int width = 40, const std::string& desc = "")
{
    TqdmBar pb;
    pb.total      = total;
    pb.width      = width;
    pb.current    = 0;
    pb.desc       = desc;
    pb.start_time = std::chrono::steady_clock::now();
    tqdm_bars.push_back(pb);
    return (int)tqdm_bars.size() - 1;
}

inline void tqdm_update(int handle, int step, double loss)
{
    if (handle < 0 || handle >= (int)tqdm_bars.size()) return;
    TqdmBar& pb = tqdm_bars[handle];
    pb.current = step;

    int total  = pb.total;
    int width  = pb.width;
    int filled = (int)((double)step / total * width);
    int empty  = width - filled;
    int pct    = (int)((double)step / total * 100);

    // elapsed time
    auto now     = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - pb.start_time).count();

    // estimated time remaining
    double eta = (step > 0) ? (elapsed / step) * (total - step) : 0.0;

    int e_min = (int)(elapsed / 60); double e_sec = elapsed - e_min * 60;
    int r_min = (int)(eta    / 60); double r_sec = eta     - r_min * 60;

    // build bar string
    if (!pb.desc.empty()) fprintf(stderr, "\r%s: ", pb.desc.c_str());
    else                  fprintf(stderr, "\r");

    fprintf(stderr, "[");
    for (int i = 0; i < filled; i++) fprintf(stderr, "\xe2\x96\x88");  // █
    for (int i = 0; i < empty;  i++) fprintf(stderr, "\xe2\x96\x91");  // ░
    fprintf(stderr, "] %3d%% %d/%d", pct, step, total);
    fprintf(stderr, "  loss: %.4f", loss);
    fprintf(stderr, "  [%02d:%05.2f<%02d:%05.2f]", e_min, e_sec, r_min, r_sec);
    fflush(stderr);
}

inline void tqdm_done(int handle)
{
    if (handle < 0 || handle >= (int)tqdm_bars.size()) return;
    TqdmBar& pb = tqdm_bars[handle];

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - pb.start_time).count();
    int e_min = (int)(elapsed / 60); double e_sec = elapsed - e_min * 60;

    if (!pb.desc.empty()) fprintf(stderr, "\r%s: ", pb.desc.c_str());
    else                  fprintf(stderr, "\r");

    fprintf(stderr, "[");
    for (int i = 0; i < pb.width; i++) fprintf(stderr, "\xe2\x96\x88");
    fprintf(stderr, "] 100%% %d/%d", pb.total, pb.total);
    fprintf(stderr, "  [%02d:%05.2f]\n", e_min, e_sec);
    fflush(stderr);
}
