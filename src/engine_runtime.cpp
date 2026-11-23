#include "engine_runtime.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <string_view>
#include <limits>

namespace audio {
std::size_t select_queue_target(unsigned input_rate, std::size_t minimum_frames,
                              double period_seconds, Preset preset) noexcept {
    // A 12 ms floor leaves scheduling headroom even for very short device periods.
    return std::max({minimum_frames, (std::size_t(input_rate) * 12 + 999) / 1000,
        static_cast<std::size_t>(std::ceil(period_seconds * input_rate * (preset == Preset::stable ? 2 : 1)))});
}
void Worker::start() {
    stop();
    { std::lock_guard lock(setup_mutex_); setup_ = {}; render_window_ = {}; last_slow_render_ = {}; }
    sessions_ = 0; recoveries_ = 0; prior_capture_misses_ = 0; prior_render_misses_ = 0;
    last_capture_hr_ = 0; last_render_hr_ = 0; recovery_hr_ = 0; recovery_stage_ = "none";
    publish({}); error_ = 0; stage_ = "initialize"; state_ = State::starting;
    try { thread_ = std::jthread([this](std::stop_token token) { run(token); }); }
    catch (...) { state_ = State::failed; throw; }
}
void Worker::stop() noexcept {
    if (thread_.joinable()) { thread_.request_stop(); thread_.join(); }
    if (state_ != State::failed) state_ = State::stopped;
}
void Worker::publish(Metrics m) noexcept {
    input_ = m.core.input_frames; output_ = m.core.output_frames;
    consumed_ = m.core.consumed_frames; generated_ = m.core.generated_frames;
    underflows_ = m.core.underflows; overflows_ = m.core.overflows;
    resyncs_ = m.core.resyncs; dropped_ = m.core.dropped_frames;
    silent_ = m.core.silent_frames; startup_ = m.core.startup_silent_frames;
    discontinuities_ = m.core.discontinuities; src_errors_ = m.core.src_errors;
    timestamps_ = m.timestamp_errors; capture_silent_ = m.capture_silent_frames;
    queue_ = m.queued_frames; ppm_ = m.correction_ppm;
    capture_misses_ = prior_capture_misses_.load() + m.capture_misses;
    render_misses_ = prior_render_misses_.load() + m.render_misses;
    if (m.capture_misses) last_capture_hr_ = m.capture_hr;
    if (m.render_misses) last_render_hr_ = m.render_hr;
}
Snapshot Worker::snapshot() const {
    Snapshot s;
    s.state = state_.load(); s.error = error_.load(); s.stage = stage_.load();
    {
        std::lock_guard lock(setup_mutex_);
        s.setup = setup_; s.render_window = render_window_; s.last_slow_render = last_slow_render_;
    }
    s.metrics.core = {input_, output_, consumed_, generated_, underflows_, overflows_,
                      resyncs_, dropped_, silent_, startup_, discontinuities_, src_errors_};
    s.metrics.timestamp_errors = timestamps_; s.metrics.capture_silent_frames = capture_silent_;
    s.metrics.queued_frames = queue_; s.metrics.correction_ppm = ppm_;
    s.metrics.capture_misses = capture_misses_; s.metrics.render_misses = render_misses_;
    s.metrics.capture_hr = last_capture_hr_; s.metrics.render_hr = last_render_hr_;
    s.sessions = sessions_; s.recoveries = recoveries_;
    s.recovery_stage = recovery_stage_; s.recovery_hr = recovery_hr_;
    return s;
}
void Worker::run(std::stop_token token) noexcept {
    std::unique_ptr<Session> session;
    std::int32_t result = 0;
    const char* stage = "initialize";
    while (!token.stop_requested()) {
        bool timed_out = false;
        std::optional<RenderWindow> slow_render;
        result = 0; stage = "initialize";
        try {
            session = factory_();
            ++sessions_;
            SetupInfo setup;
            result = session->initialize(token, setup);
            { std::lock_guard lock(setup_mutex_); setup_ = setup; }
            if (result >= 0 && !token.stop_requested()) {
                stage = "start capture"; result = session->start_capture();
                std::optional<RenderMonitor> monitor;
                auto last_capture = std::chrono::steady_clock::now();
                auto last_render = last_capture;
                const auto startup = last_capture;
                while (result >= 0 && !token.stop_requested()) {
                    stage = "wait";
                    const auto wake = session->wait();
                    if (token.stop_requested() || wake == Wake::stopped) break;
                    if (wake == Wake::failed) { result = static_cast<std::int32_t>(0x80004005U); break; }
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_capture > std::chrono::seconds(2) ||
                        (monitor && now - last_render > std::chrono::seconds(2)) ||
                        (!monitor && now - startup > std::chrono::seconds(2))) {
                        stage = "audio event/prefill timeout";
                        result = static_cast<std::int32_t>(0x800705B4U); timed_out = true; break;
                    }
                    if (wake == Wake::idle) continue;
                    if (wake == Wake::capture) {
                        stage = "capture"; result = session->capture(token);
                        if (result == 0) last_capture = now;
                        if (result < 0 || token.stop_requested()) break;
                        if (!monitor && session->ready()) {
                            stage = "start render"; result = session->start_render();
                            if (result < 0) break;
                            last_render = now;
                            monitor.emplace(std::chrono::steady_clock::now(), session->metrics().core);
                            state_ = State::running;
                        }
                    } else if (monitor && wake == Wake::render) {
                        stage = "render"; result = session->render();
                        monitor->render_event(now, std::chrono::steady_clock::now());
                        if (result == 0) last_render = now;
                    }
                    if (result < 0) break;
                    const auto metrics = session->metrics();
                    publish(metrics);
                    if (!monitor) continue;
                    const auto sample = monitor->sample(std::chrono::steady_clock::now(), metrics.core,
                                                        setup.input.format.rate, setup.output.format.rate);
                    if (!sample) continue;
                    // The audio thread may skip a UI refresh while a snapshot is being read.
                    std::unique_lock lock(setup_mutex_, std::try_to_lock);
                    if (lock.owns_lock()) render_window_ = *sample;
                    if (sample->slow_windows >= 2) {
                        slow_render = sample;
                        stage = "sustained slow render";
                        break;
                    }
                }
            }
        } catch (...) { result = static_cast<std::int32_t>(0x80004005U); }
        if (session) {
            publish(session->metrics());
            session->stop(); session.reset();
        }
        const bool unhealthy_buffer = result == static_cast<std::int32_t>(0x88890018U) &&
            (std::string_view(stage) == "capture" || std::string_view(stage) == "render");
        if (token.stop_requested() || (!session_invalidated(result) && !timed_out && !unhealthy_buffer && !slow_render)) break;
        prior_capture_misses_ = capture_misses_.load(); prior_render_misses_ = render_misses_.load();
        recovery_stage_ = stage; recovery_hr_ = result; ++recoveries_;
        // Release every old client/service before rebuilding the same route. No
        // processing state or endpoint buffers survive an invalidated session.
        state_ = State::starting;
        {
            std::lock_guard lock(setup_mutex_);
            setup_ = {}; render_window_ = {};
            if (slow_render) last_slow_render_ = *slow_render;
        }
        publish({}); error_ = 0; stage = "reinitialize"; stage_ = stage;
        try {
            std::mutex mutex;
            std::condition_variable_any retry;
            std::unique_lock lock(mutex);
            retry.wait_for(lock, token, std::chrono::milliseconds(100), [] { return false; });
        } catch (...) { result = static_cast<std::int32_t>(0x80004005U); break; }
    }
    if (token.stop_requested()) result = 0;
    error_ = result; stage_ = result < 0 ? stage : "stopped";
    state_ = result < 0 ? State::failed : State::stopped;
}
}
