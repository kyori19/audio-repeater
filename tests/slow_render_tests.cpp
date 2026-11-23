#include "engine_runtime.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string_view>

using namespace std::chrono_literals;
namespace {
using Clock = audio::RenderMonitor::Clock;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

void measurement_windows() {
    Clock::time_point now{};
    audio::Statistics totals{};
    audio::RenderMonitor monitor(now, totals);
    monitor.render_event(now + 8ms, now + 9ms);
    monitor.render_event(now + 16ms, now + 19ms);
    require(!monitor.sample(now + 999ms, totals, 48000, 48000), "partial window");
    auto tick = [&](unsigned in, unsigned out, unsigned drops) {
        totals.input_frames += in; totals.output_frames += out; totals.dropped_frames += drops;
        now += 1s;
        return monitor.sample(now, totals, 48000, 48000).value();
    };
    const auto first = tick(48000,18000,30000);
    require(first.events == 2 && first.mean_gap_ms == 8 && first.max_gap_ms == 8 &&
            first.mean_work_ms == 2 && first.max_work_ms == 3, "observed render timing");
    require(first.slow_windows == 1 && tick(48000,48000,0).slow_windows == 0, "transient recovery");
    require(tick(24000,18000,6000).slow_windows == 0, "capture is also slow");
    require(tick(48000,18000,0).slow_windows == 0, "no discarded input");
    require(tick(48000,47952,576).slow_windows == 0, "ordinary queue correction");
    require(tick(48000,18000,30000).slow_windows == 1 &&
            tick(48000,18000,30000).slow_windows == 2, "sustained slowdown");
}

// Both endpoints deliver successful 144-frame buffers. Only the event cadence
// changes: a 3 ms input and an 8 ms output give precisely 48k and 18k frames/s.
class TimedSession final : public audio::Session {
public:
    explicit TimedSession(bool slow) : render_period_(slow ? 8ms : 3ms) {}
    std::int32_t initialize(std::stop_token, audio::SetupInfo& setup) override {
        setup.input.format.rate = setup.output.format.rate = 48000;
        return 0;
    }
    std::int32_t start_capture() noexcept override { capture_at_ = Clock::now(); return 0; }
    audio::Wake wait() noexcept override {
        const bool capture = !rendering_ || capture_at_ <= render_at_;
        auto& deadline = capture ? capture_at_ : render_at_;
        std::this_thread::sleep_until(deadline);
        deadline += capture ? 3ms : render_period_;
        return capture ? audio::Wake::capture : audio::Wake::render;
    }
    std::int32_t capture(std::stop_token) noexcept override { transport_.push(input_); return 0; }
    bool ready() const noexcept override { return transport_.queued_input() >= 576; }
    std::int32_t start_render() noexcept override {
        rendering_ = true; render_at_ = Clock::now() + render_period_;
        return render();
    }
    std::int32_t render() noexcept override { transport_.render(output_); return 0; }
    audio::Metrics metrics() const noexcept override { audio::Metrics m; m.core = transport_.statistics(); return m; }
    void stop() noexcept override {}
private:
    audio::Transport transport_{48000,48000,1,2577,576,144};
    std::array<float,144> input_{}, output_{};
    std::chrono::milliseconds render_period_;
    Clock::time_point capture_at_, render_at_;
    bool rendering_{};
};

void worker_recovery() {
    int created = 0;
    audio::Worker worker([&] { return std::make_unique<TimedSession>(created++ == 0); });
    worker.start();
    const auto deadline = Clock::now() + 9s;
    while (Clock::now() < deadline) {
        const auto s = worker.snapshot();
        if (s.sessions == 2 && s.state == audio::State::running && s.render_window.output_fps > 44000) {
            const auto& w = s.last_slow_render;
            require(s.recoveries == 1 && std::string_view(s.recovery_stage) == "sustained slow render", "rebuild reason");
            require(s.metrics.capture_misses == 0 && s.metrics.render_misses == 0 && s.recovery_hr == 0, "unexpected API failure");
            require(w.input_fps > 43000 && w.output_fps < 22000 && w.drops > 20000 && w.slow_windows == 2, "lost fault evidence");
            require(w.events > 80 && w.mean_gap_ms > 5 && w.mean_work_ms >= 0, "missing render timings");
            std::printf("Recovered from %.0f in / %.0f out fps, gap %.2f ms, work %.3f ms; new output %.0f fps\n",
                        w.input_fps, w.output_fps, w.mean_gap_ms, w.mean_work_ms, s.render_window.output_fps);
            worker.stop();
            return;
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("did not recover to healthy output");
}
}
int main() {
    try { measurement_windows(); worker_recovery(); }
    catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
