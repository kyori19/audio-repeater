#pragma once
#include "audio_core.h"
#include <algorithm>
#include <chrono>
#include <optional>

namespace audio {
struct RenderWindow {
    double input_fps{}, output_fps{}, seconds{};
    std::uint64_t drops{}, events{};
    double mean_gap_ms{}, max_gap_ms{}, mean_work_ms{}, max_work_ms{};
    unsigned slow_windows{};
};

// Single worker, one monitor per running session. The prefill is excluded.
class RenderMonitor {
public:
    using Clock = std::chrono::steady_clock;
    RenderMonitor(Clock::time_point start, Statistics counters) : start_(start), baseline_(counters) {}

    void render_event(Clock::time_point wake, Clock::time_point done) noexcept {
        if (previous_wake_) {
            const double gap = milliseconds(wake - *previous_wake_);
            ++gaps_;
            total_gap_ += gap;
            window_.max_gap_ms = std::max(window_.max_gap_ms, gap);
        }
        previous_wake_ = wake;
        const double work = milliseconds(done - wake);
        ++window_.events;
        total_work_ += work;
        window_.max_work_ms = std::max(window_.max_work_ms, work);
    }

    std::optional<RenderWindow> sample(Clock::time_point now, const Statistics& current,
                                       unsigned input_rate, unsigned output_rate) noexcept {
        if (now - start_ < std::chrono::seconds(1)) return {};
        const double seconds = std::chrono::duration<double>(now - start_).count();
        window_.seconds = seconds;
        window_.input_fps = static_cast<double>(current.input_frames - baseline_.input_frames) / seconds;
        window_.output_fps = static_cast<double>(current.output_frames - baseline_.output_frames) / seconds;
        window_.drops = current.dropped_frames - baseline_.dropped_frames;
        window_.mean_gap_ms = gaps_ ? total_gap_ / static_cast<double>(gaps_) : 0;
        window_.mean_work_ms = window_.events ? total_work_ / static_cast<double>(window_.events) : 0;
        const bool slow = input_rate && output_rate &&
            window_.input_fps >= 0.90 * input_rate && window_.input_fps <= 1.10 * input_rate &&
            window_.output_fps < 0.75 * output_rate &&
            static_cast<double>(window_.drops) >= 0.10 * input_rate * seconds;
        streak_ = slow ? streak_ + 1 : 0;
        window_.slow_windows = streak_;
        const auto finished = window_;
        start_ = now;
        baseline_ = current;
        window_ = {};
        gaps_ = 0;
        total_work_ = total_gap_ = 0;
        return finished;
    }

private:
    static double milliseconds(Clock::duration duration) noexcept {
        return std::chrono::duration<double, std::milli>(duration).count();
    }
    Clock::time_point start_;
    Statistics baseline_;
    std::optional<Clock::time_point> previous_wake_;
    RenderWindow window_;
    std::uint64_t gaps_{};
    double total_work_{}, total_gap_{};
    unsigned streak_{};
};
}
