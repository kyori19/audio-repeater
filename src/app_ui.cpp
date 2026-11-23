#include "desktop_host.h"
#include "wasapi_engine.h"
#include <objbase.h>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <chrono>
#include <fstream>
#include <deque>
#include <commdlg.h>

namespace desktop {
// Deterministic native-window checks use the same Worker and control paths with
// synthetic endpoints/session, and cannot route audio to the machine's hardware.
struct RouteTestState {
    std::atomic<unsigned> starts{}, stops{};
    std::atomic<bool> hold_start{};
    audio::Settings last;
};
class RouteTestSession final : public audio::Session {
public:
    explicit RouteTestSession(std::shared_ptr<RouteTestState> value) : state_(std::move(value)) {}
    std::int32_t initialize(std::stop_token token, audio::SetupInfo&) override {
        while (state_->hold_start && !token.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return 0;
    }
    std::int32_t start_capture() noexcept override { return 0; }
    audio::Wake wait() noexcept override {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        capture_ = !capture_; return capture_ ? audio::Wake::capture : audio::Wake::render;
    }
    std::int32_t capture(std::stop_token) noexcept override { return 0; }
    bool ready() const noexcept override { return true; }
    std::int32_t start_render() noexcept override { return 0; }
    std::int32_t render() noexcept override { return 0; }
    audio::Metrics metrics() const noexcept override { return {}; }
    void stop() noexcept override { ++state_->stops; }
private:
    std::shared_ptr<RouteTestState> state_;
    bool capture_{};
};
// All HWNDs, endpoint values, and Worker start/stop calls belong to this thread.
// The audio thread never calls UI code or waits for a UI lock.
struct Window {
    enum : int { input_id = 101, output_id, preset_id, refresh_id, start_id, stop_id, register_id, export_id, save_id, settings_id, exit_id };
    HINSTANCE instance;
    HWND hwnd{}, input{}, output{}, preset{}, refresh_button{}, start_button{}, stop_button{}, status{}, details{}, registration{}, save_button{};
    bool registered{};
    std::deque<std::string> history;
    std::uint64_t next_sample{}, device_changes{}, route_changes{};
    std::vector<audio::Endpoint> inputs, outputs;
    // Empty means no endpoint has ever been selected. Keep an absent ID across
    // Refresh so a missing selection cannot become an unrelated initial choice.
    std::wstring input_selection, output_selection;
    std::unique_ptr<audio::Worker> worker;
    Preferences preferences;
    Paths paths;
    Recovery recovery;
    Tray tray;
    Notifications notifications;
    UINT taskbar_created{RegisterWindowMessageW(L"TaskbarCreated")};
    UINT show_settings{RegisterWindowMessageW(L"AudioRepeaterWasapi.ShowSettings")};
    bool configured{}, load_error{}, subscription_active{};
    std::uint64_t next_subscription{};
    std::function<bool(HWND)> testing_attach;
    std::wstring instance_identity, instance_class;
    unsigned tray_retry{};
    bool active{}, testing{}, passed{}, synthetic_devices{}, launch_probe{};
    std::function<std::unique_ptr<audio::Session>(const audio::Settings&)> testing_factory;
    std::shared_ptr<RouteTestState> route_test;
    unsigned test_step{}, ticks{};
    int exit_code{};

    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
        auto child = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
            x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        if (!child) throw std::runtime_error("CreateWindowEx");
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return child;
    }
    static std::wstring selected(HWND combo, const std::vector<audio::Endpoint>& endpoints) {
        const auto index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        return index >= 0 && static_cast<std::size_t>(index) < endpoints.size()
            ? endpoints[static_cast<std::size_t>(index)].id : L"";
    }
    bool supported(HWND combo, const std::vector<audio::Endpoint>& endpoints) const {
        const auto index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        return index >= 0 && static_cast<std::size_t>(index) < endpoints.size();
    }
    static void fill(HWND combo, const std::vector<audio::Endpoint>& endpoints, std::wstring& previous) {
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        int index = previous.empty() && !endpoints.empty() ? 0 : -1;
        for (std::size_t i = 0; i < endpoints.size(); ++i) {
            const auto& text = endpoints[i].name;
            if (SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())) < 0)
                throw std::runtime_error("CB_ADDSTRING");
            if (endpoints[i].id == previous) index = static_cast<int>(i);
        }
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
        if (index >= 0) previous = endpoints[static_cast<std::size_t>(index)].id;
    }
    void enable() {
        const bool locked = active || recovery.wanted();
        for (auto c : {input, output, preset, refresh_button, save_button}) EnableWindow(c, !locked);
        EnableWindow(start_button, !locked && supported(input, inputs) && supported(output, outputs));
        EnableWindow(stop_button, locked);
    }
    void selection_status() {
        std::wstring message = L"Ready. Select endpoints and press Start.";
        if (inputs.empty() || outputs.empty())
            message = L"No active " + std::wstring(inputs.empty() ? L"input" : L"output") +
                L" endpoints. Connect/enable devices, then Refresh. Routing is unverified.";
        else if (!supported(input, inputs) || !supported(output, outputs))
            message = L"Select supported input and output endpoints. A removed selection is not replaced automatically.";
        SetWindowTextW(status, message.c_str());
        const auto text = L"Selected input ID: " + selected(input, inputs) + L"\r\nSelected output ID: " + selected(output, outputs) +
            L"\r\nExclusive WASAPI: 48 kHz, PCM24 input / PCM16 output." +
            L"\r\nAPI/queue values are not measured end-to-end latency.";
        SetWindowTextW(details, text.c_str());
        enable();
    }
    void refresh(bool automatic = false) {
        if ((active || recovery.wanted()) && !automatic) return;
        if (!active) worker.reset();
        try {
            if (!testing || !synthetic_devices) {
                auto next_inputs = audio::enumerate_endpoints(true);
                auto next_outputs = audio::enumerate_endpoints(false);
                inputs = std::move(next_inputs); outputs = std::move(next_outputs);
            }
        } catch (const audio::WasapiError& e) {
            fill(input, inputs, input_selection); fill(output, outputs, output_selection); enable();
            SetWindowTextW(status, (L"Device enumeration failed: " + hex(e.code) + L". Refresh to retry.").c_str());
            return;
        }
        fill(input, inputs, input_selection); fill(output, outputs, output_selection);
        if (!active && !recovery.wanted()) selection_status();
        else enable();
    }
    bool save() {
        try {
            preferences = {utf8(input_selection), utf8(output_selection),
                SendMessageW(preset, CB_GETCURSEL, 0, 0) == 1 ? audio::Preset::stable : audio::Preset::low_latency};
            save_preferences(paths, preferences);
            configured = preferences.configured();
            SetWindowTextW(status, L"Settings saved. Close or minimize to continue in the tray.");
            return true;
        } catch (...) {
            SetWindowTextW(status, L"Could not save settings/startup shortcut. Check file permissions or a conflicting Startup shortcut.");
            return false;
        }
    }
    void registration_label() {
        SetWindowTextW(registration, registered ? L"Unregister at sign-in" : L"Register at sign-in");
    }
    void toggle_registration() {
        try {
            const bool enable_registration = !startup_enabled(paths);
            if (enable_registration && (input_selection.empty() || output_selection.empty())) {
                SetWindowTextW(status, L"Select both endpoints before registering.");
                return;
            }
            if (enable_registration && !save()) return;
            set_startup(paths, enable_registration);
            registered = startup_enabled(paths);
            registration_label();
            SetWindowTextW(status, registered ? L"Registered. The saved route will resume at sign-in."
                                              : L"Unregistered. The saved route is available for manual use.");
        } catch (...) {
            MessageBoxW(hwnd, L"Could not change the Startup shortcut. Check its ownership and permissions.",
                        L"Audio Repeater", MB_OK | MB_ICONERROR);
        }
    }
    void sample(bool force = false) {
        const auto now = GetTickCount64();
        if (!force && now < next_sample) return;
        next_sample = now + 1000;
        const auto s = worker ? worker->snapshot() : audio::Snapshot{};
        const char* state = "stopped";
        switch (s.state) {
        case audio::State::starting: state = "starting"; break;
        case audio::State::running: state = "running"; break;
        case audio::State::failed: state = "failed"; break;
        case audio::State::stopped: break;
        }
        const auto& m = s.metrics;
        const auto& w = s.render_window;
        std::ostringstream row;
        row << now << ',' << state << ',' << s.sessions << ',' << s.recoveries << ',' << s.recovery_stage
            << ',' << utf8(hex(s.recovery_hr)) << ',' << m.capture_misses << ',' << m.render_misses
            << ',' << utf8(hex(m.capture_hr)) << ',' << utf8(hex(m.render_hr))
            << ',' << m.core.input_frames << ',' << m.core.output_frames << ',' << m.core.generated_frames
            << ',' << m.core.underflows << ',' << m.core.resyncs << ',' << m.core.dropped_frames
            << ',' << m.core.discontinuities << ',' << m.queued_frames << ',' << m.correction_ppm
            << ',' << s.stage << ',' << utf8(hex(s.error)) << ',' << device_changes << ',' << route_changes
            << ',' << w.seconds << ',' << w.input_fps << ',' << w.output_fps << ',' << w.drops << ',' << w.events
            << ',' << w.mean_gap_ms << ',' << w.max_gap_ms << ',' << w.mean_work_ms << ',' << w.max_work_ms
            << ',' << w.slow_windows << '\n';
        if (history.size() == 120) history.pop_front();
        history.push_back(row.str());
    }
    void save_report(const std::filesystem::path& path) {
        sample(true);
        std::ofstream report(path, std::ios::binary | std::ios::trunc);
        report.exceptions(std::ios::badbit | std::ios::failbit);
        SYSTEMTIME utc{}; GetSystemTime(&utc);
        report << "Audio Repeater Exclusive diagnostics v2\nUTC " << utc.wYear << '-' << utc.wMonth
               << '-' << utc.wDay << ' ' << utc.wHour << ':' << utc.wMinute << ':' << utc.wSecond
               << "\ninput_id=" << utf8(input_selection) << "\noutput_id=" << utf8(output_selection) << '\n';
        if (worker) report << utf8(diagnostics(worker->snapshot()));
        report << "\nuptime_ms,state,session,recoveries,last_recovery,recovery_hr,capture_misses,render_misses,"
                  "last_capture_hr,last_render_hr,input_frames,output_frames,generated_frames,underflows,resyncs,"
                  "dropped_frames,discontinuities,queue_frames,correction_ppm,stage,error_hr,device_changes,route_changes,"
                  "window_s,input_fps,output_fps,window_drops,render_events,render_gap_ms_avg,render_gap_ms_max,"
                  "render_work_ms_avg,render_work_ms_max,slow_windows\n";
        for (const auto& row : history) report << row;
        report.close();
    }
    void export_diagnostics() {
        wchar_t filename[MAX_PATH] = L"audio-repeater-diagnostics.txt";
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = hwnd;
        dialog.lpstrFilter = L"Text files\0*.txt\0\0"; dialog.lpstrDefExt = L"txt";
        dialog.lpstrFile = filename; dialog.nMaxFile = MAX_PATH;
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&dialog)) return;
        try { save_report(filename); }
        catch (...) { MessageBoxW(hwnd, L"Could not save diagnostics.", L"Audio Repeater", MB_OK | MB_ICONERROR); }
    }
    void attempt(bool enumerate = true) {
        if (active || !recovery.due(GetTickCount64())) return;
        if (enumerate) refresh(true);
        if (selected(input, inputs) != input_selection || selected(output, outputs) != output_selection ||
            !supported(input, inputs) || !supported(output, outputs)) {
            recovery.failed(GetTickCount64());
            SetWindowTextW(status, L"Waiting for the saved devices. Reconnection resumes automatically. Stop cancels recovery.");
            tray.tooltip(L"Audio Repeater WASAPI: waiting for saved devices");
            return;
        }
        const audio::Settings settings{input_selection, output_selection,
            SendMessageW(preset, CB_GETCURSEL, 0, 0) == 1 ? audio::Preset::stable : audio::Preset::low_latency};
        worker = std::make_unique<audio::Worker>([settings, factory = testing_factory] {
            return factory ? factory(settings) : audio::make_wasapi_session(settings);
        });
        notifications.watch_route(input_selection, output_selection);
        worker->start(); active = true; enable();
        SetWindowTextW(status, L"Starting saved route; failures retry quietly. Stop cancels recovery.");
    }
    void start() {
        if (active || recovery.wanted() || input_selection.empty() || output_selection.empty() || !save()) return;
        recovery.start(GetTickCount64()); enable(); attempt(false);
    }
    void stop() {
        sample(true);
        recovery.stop();
        if (worker) worker->stop(); // worker releases streams and joins
        active = false; enable(); update();
        SetWindowTextW(status, L"Stopped. Automatic reconnect is cancelled until Start.");
        tray.tooltip(L"Audio Repeater WASAPI: stopped");
    }
    void show() {
        ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); SetFocus(input);
    }
    void hide() {
        if (tray.present()) ShowWindow(hwnd, SW_HIDE);
        else show(); // never strand an inaccessible process if the shell is absent
    }
    void devices_changed() {
        const bool route_changed = notifications.acknowledge();
        if (route_changed) ++route_changes;
        ++device_changes;
        if (active) {
            const auto state = worker->snapshot().state;
            if (state == audio::State::failed || state == audio::State::stopped) { worker->stop(); active = false; }
        }
        refresh(true);
        if (active && (route_changed || !supported(input, inputs) || !supported(output, outputs))) {
            sample(true);
            worker->stop(); active = false;
        }
        recovery.changed(GetTickCount64());
        attempt(false); enable();
    }
    void menu() {
        const auto popup = CreatePopupMenu();
        if (!popup) return;
        AppendMenuW(popup, MF_STRING, settings_id, L"&Settings");
        AppendMenuW(popup, MF_STRING | (recovery.wanted() ? MF_GRAYED : 0), start_id, L"&Start / resume");
        AppendMenuW(popup, MF_STRING | (recovery.wanted() ? 0 : MF_GRAYED), stop_id, L"S&top");
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, register_id, registered ? L"Unregister at sign-in" : L"Register at sign-in");
        AppendMenuW(popup, MF_STRING, export_id, L"Save diagnostics...");
        AppendMenuW(popup, MF_STRING, exit_id, L"E&xit");
        POINT point{}; GetCursorPos(&point); SetForegroundWindow(hwnd);
        const auto command = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr);
        DestroyMenu(popup); PostMessageW(hwnd, WM_NULL, 0, 0);
        if (command) SendMessageW(hwnd, WM_COMMAND, static_cast<WPARAM>(command), 0);
    }
    void subscribe(std::uint64_t now) {
        if (subscription_active || now < next_subscription || (testing && !testing_attach)) return;
        const bool retry = next_subscription != 0;
        next_subscription = now + 2000;
        subscription_active = testing_attach ? testing_attach(hwnd) : notifications.attach(hwnd);
        if (subscription_active && retry) devices_changed(); // rescan events missed while unattached
    }
    void show_on_launch(int requested, bool tray_ready) {
        if (!configured || !registered || load_error || !tray_ready) ShowWindow(hwnd, requested == SW_HIDE ? SW_SHOW : requested);
    }
    void tick() {
        subscribe(GetTickCount64());
        sample();
        update();
        if (!active) attempt();
        // The notification callback is the fast path. Retry polling also handles
        // service readiness, exclusive-device contention and missed notifications.
        if (!tray.present() && ++tray_retry >= 8) { tray_retry = 0; tray.add(hwnd); }
    }
    void update() {
        if (!worker) return;
        const auto s = worker->snapshot();
        if (active && (s.state == audio::State::failed || s.state == audio::State::stopped)) {
            worker->stop(); active = false; recovery.failed(GetTickCount64()); enable();
        }
        std::wstring message;
        if (s.state == audio::State::failed) {
            message = L"Stopped with error " + hex(s.error) + L" at " +
                std::wstring(s.stage, s.stage + std::char_traits<char>::length(s.stage)) +
                (recovery.wanted() ? L". Retrying the saved route quietly; Stop cancels recovery." : L". Automatic recovery is stopped.");
        } else if (s.state == audio::State::starting) message = L"Starting: negotiating streams and waiting for prefill...";
        else if (s.state == audio::State::running) {
            recovery.running(); message = L"Running. Settings are locked until Stop.";
        }
        else message = L"Stopped. Worker joined.";
        SetWindowTextW(status, message.c_str());
        tray.tooltip(s.state == audio::State::running ? L"Audio Repeater WASAPI: running" :
            recovery.wanted() ? L"Audio Repeater WASAPI: waiting / retrying" : L"Audio Repeater WASAPI: stopped");
        if (!IsWindowVisible(hwnd) && !testing) return;
        auto text = L"Input ID: " + selected(input, inputs) + L"\nOutput ID: " + selected(output, outputs) + L"\n" + diagnostics(s) +
            L"API/queue values are diagnostics, not measured end-to-end latency.";
        // Win32 multiline edit controls expect CRLF.
        for (std::size_t pos = 0; (pos = text.find(L'\n', pos)) != std::wstring::npos; pos += 2) text.insert(pos, 1, L'\r');
        const auto first_line = SendMessageW(details, EM_GETFIRSTVISIBLELINE, 0, 0);
        DWORD selection_start = 0, selection_end = 0;
        SendMessageW(details, EM_GETSEL, reinterpret_cast<WPARAM>(&selection_start), reinterpret_cast<LPARAM>(&selection_end));
        SetWindowTextW(details, text.c_str());
        SendMessageW(details, EM_SETSEL, selection_start, selection_end);
        SendMessageW(details, EM_LINESCROLL, 0, first_line - SendMessageW(details, EM_GETFIRSTVISIBLELINE, 0, 0));
    }
    void create() {
        control(L"STATIC", L"&Input (capture)", 0, 0, 16, 16, 150, 20);
        input = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, input_id, 16, 40, 780, 240);
        control(L"STATIC", L"&Output (render)", 0, 0, 16, 76, 150, 20);
        output = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, output_id, 16, 100, 780, 240);
        control(L"STATIC", L"&Buffering preset", 0, 0, 16, 138, 160, 20);
        preset = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, preset_id, 16, 162, 270, 150);
        SendMessageW(preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Low latency (minimum legal period)"));
        SendMessageW(preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Stable (default period, larger queue)"));
        SendMessageW(preset, CB_SETCURSEL, 0, 0);
        control(L"STATIC", L"WASAPI Exclusive   /   48 kHz", 0, 0, 310, 166, 350, 22);
        refresh_button = control(L"BUTTON", L"&Refresh", WS_TABSTOP, refresh_id, 16, 204, 110, 28);
        start_button = control(L"BUTTON", L"&Start", WS_TABSTOP | BS_DEFPUSHBUTTON, start_id, 142, 204, 110, 28);
        stop_button = control(L"BUTTON", L"S&top", WS_TABSTOP, stop_id, 268, 204, 110, 28);
        registration = control(L"BUTTON", L"Register at sign-in", WS_TABSTOP, register_id, 16, 240, 240, 28);
        control(L"BUTTON", L"Save diagnostics...", WS_TABSTOP, export_id, 280, 240, 200, 28);
        save_button = control(L"BUTTON", L"Save settings", WS_TABSTOP, save_id, 530, 240, 120, 28);
        control(L"BUTTON", L"Exit", WS_TABSTOP, exit_id, 666, 240, 110, 28);
        status = control(L"STATIC", L"", 0, 0, 16, 284, 780, 48);
        details = control(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
            0, 16, 338, 780, 330);
        {
            bool exists{};
            try {
                load_error = !load_preferences(paths, preferences, exists);
                configured = !load_error && exists && preferences.configured();
                input_selection = wide(preferences.input_id); output_selection = wide(preferences.output_id);
                SendMessageW(preset, CB_SETCURSEL, preferences.preset == audio::Preset::stable ? 1 : 0, 0);
                registered = startup_enabled(paths);
                registration_label();
            } catch (...) { load_error = true; }
        }
        refresh();
        tray.add(hwnd);
        subscribe(GetTickCount64());
        if (configured && registered && !load_error) { recovery.start(GetTickCount64()); attempt(); }
        if (load_error) SetWindowTextW(status, L"Saved settings could not be read. Review and save settings before enabling automatic audio.");
        if (!launch_probe && !SetTimer(hwnd, 1, testing ? 50 : 250, nullptr)) throw std::runtime_error("SetTimer");
    }
    static void require(bool value) { if (!value) throw std::runtime_error("GUI self-test"); }
    void test_launches() {
        auto isolated = paths; isolated.settings = paths.settings.parent_path() / L"launch.conf";
        const Preferences saved{"capture-\xe6\x97\xa5", "render-\xf0\x9f\x8e\xa7",
            audio::Preset::stable};
        for (unsigned scenario = 0; scenario < 6; ++scenario) {
            std::filesystem::create_directories(isolated.settings.parent_path());
            if (scenario == 0) std::filesystem::remove(isolated.settings);
            else if (scenario == 1) std::ofstream(isolated.settings, std::ios::binary) << "corrupt settings";
            auto expected = saved;
            set_startup(isolated, scenario >= 3);
            if (scenario == 3) expected.preset = audio::Preset::low_latency;
            if (scenario >= 2) save_preferences(isolated, expected);
            Window probe{};
            probe.instance = instance; probe.paths = isolated; probe.testing = true;
            probe.launch_probe = true; probe.synthetic_devices = true;
            auto counts = std::make_shared<RouteTestState>(); counts->hold_start = scenario == 4;
            probe.testing_factory = [counts](const audio::Settings& settings) {
                counts->last = settings; ++counts->starts; return std::make_unique<RouteTestSession>(counts);
            };
            probe.inputs = {{scenario == 3 ? L"unrelated-input" : wide(saved.input_id), L"Input", {}, 0}};
            probe.outputs = {{scenario == 3 ? L"unrelated-output" : wide(saved.output_id), L"Output", {}, 0}};
            const auto window = CreateWindowExW(WS_EX_CONTROLPARENT, instance_class.c_str(), L"Isolated launch check",
                WS_OVERLAPPEDWINDOW, 0, 0, 860, 764, nullptr, nullptr, instance, &probe);
            require(window != nullptr);
            probe.show_on_launch(SW_SHOW, true);
            if (scenario < 2) {
                require(IsWindowVisible(window) && !probe.configured && probe.load_error == (scenario == 1) &&
                    !probe.worker && !probe.recovery.wanted() && IsWindowEnabled(probe.save_button));
            } else {
                require(probe.configured && !probe.load_error && (IsWindowVisible(window) != FALSE) == (scenario == 2) &&
                    probe.input_selection == wide(expected.input_id) && probe.output_selection == wide(expected.output_id) &&
                    probe.registered == (scenario >= 3) &&
                    SendMessageW(probe.preset, CB_GETCURSEL, 0, 0) == (expected.preset == audio::Preset::stable ? 1 : 0));
                probe.show_on_launch(SW_SHOW, false); require(IsWindowVisible(window)); // failed tray registration
                if (scenario == 2) require(!probe.worker && !probe.recovery.wanted());
                if (scenario == 3) require(probe.recovery.wanted() && !probe.worker &&
                    selected(probe.input, probe.inputs).empty() && selected(probe.output, probe.outputs).empty());
                if (scenario == 4) require(probe.worker && probe.worker->snapshot().state == audio::State::starting);
                if (scenario == 5) {
                    const auto deadline = GetTickCount64() + 1000;
                    while (probe.worker->snapshot().state == audio::State::starting && GetTickCount64() < deadline) Sleep(1);
                    require(probe.worker->snapshot().state == audio::State::running && counts->last.input_id == probe.input_selection &&
                        counts->last.output_id == probe.output_selection &&
                        counts->last.preset == expected.preset);
                }
            }
            SendMessageW(window, WM_COMMAND, exit_id, 0);
            require(!IsWindow(window) && !probe.active && !probe.recovery.wanted());
            if (scenario >= 4) require(counts->starts == 1 && counts->stops == 1 &&
                probe.worker->snapshot().state == audio::State::stopped); // Exit joined during startup/running
        }
        set_startup(isolated, false);
        write(L"Isolated native launches: passed (missing/corrupt/configured settings, restored IDs/mode/preset/resume, tray availability, resume/wait, Exit during startup/running).\n");
        // Exercise a first subscription failure, bounded quiet retry, and success
        // through the same controller entry point used by the production timer.
        unsigned attachments{};
        testing_attach = [&](HWND) { return ++attachments == 2; };
        subscribe(1000); require(!subscription_active && attachments == 1);
        subscribe(2999); require(!subscription_active && attachments == 1);
        subscribe(3000); require(subscription_active && attachments == 2);
        subscribe(9000); require(subscription_active && attachments == 2);
        testing_attach = {}; subscription_active = false;
        // Two real child processes use the same mutex and forwarding branch as
        // normal launch, but a GUID-isolated identity/window and no audio paths.
        std::wstring executable(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        require(length && length < executable.size()); executable.resize(length);
        for (const bool background : {true, false}) {
            ShowWindow(hwnd, SW_HIDE);
            auto command = L"\"" + executable + L"\" --instance-self-test-child \"" + instance_identity + L"\" \"" +
                instance_class + (background ? L"\" background" : L"\" normal");
            STARTUPINFOW startup_info{}; startup_info.cb = sizeof(startup_info);
            startup_info.dwFlags = STARTF_USESHOWWINDOW; startup_info.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION process{};
            require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                nullptr, nullptr, &startup_info, &process) != FALSE);
            CloseHandle(process.hThread);
            const auto waited = WaitForSingleObject(process.hProcess, 3000);
            DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
            if (waited != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 500); }
            CloseHandle(process.hProcess); require(waited == WAIT_OBJECT_0 && code == 0);
            MSG forwarded{};
            while (PeekMessageW(&forwarded, hwnd, show_settings, show_settings, PM_REMOVE)) DispatchMessageW(&forwarded);
            require((IsWindowVisible(hwnd) != FALSE) == !background && !worker && !recovery.wanted());
        }
        write(L"Isolated second-process forwarding and notification-subscription retry: passed.\n");
        MINMAXINFO limits{}; SendMessageW(hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
        SetWindowPos(hwnd, nullptr, 0, 0, limits.ptMinTrackSize.x, limits.ptMinTrackSize.y, SWP_NOMOVE | SWP_NOZORDER);
        RECT client{}, button{}; GetClientRect(hwnd, &client); GetWindowRect(GetDlgItem(hwnd, exit_id), &button);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&button), 2);
        require(button.right + 16 <= client.right && button.left >= 0);
    }
    void test_tick() {
        if (++ticks > 100) throw std::runtime_error("GUI self-test timeout");
        if (test_step == 0) {
            test_launches();
            write(L"GUI native window created; enumerated inputs=" + std::to_wstring(inputs.size()) +
                L" outputs=" + std::to_wstring(outputs.size()) + L" (no routing claim).\n");
            // Deterministic empty-state test uses the same real controls. Native
            // enumeration above is reported separately from these synthetic IDs.
            inputs.clear(); outputs.clear(); fill(input, inputs, input_selection); fill(output, outputs, output_selection); selection_status();
            wchar_t text[256]{}; GetWindowTextW(status, text, 256);
            require(std::wstring_view(text).find(L"No active input endpoints") != std::wstring_view::npos);
            require(!IsWindowEnabled(start_button) && IsWindowEnabled(refresh_button) && IsWindowEnabled(preset));
            SendMessageW(hwnd, WM_COMMAND, refresh_id, 0);
            require(!active && IsWindowEnabled(refresh_button));
            // Feed synthetic enumeration results into the production fill path.
            // Both sides use real controls and the real selection notification.
            input_selection.clear(); output_selection.clear();
            inputs = {{L"nonexistent-input", L"Synthetic input", {}, 0}};
            outputs = {{L"nonexistent-output", L"Synthetic output", {}, 0}};
            fill(input, inputs, input_selection); fill(output, outputs, output_selection);
            selection_status();
            require(IsWindowEnabled(start_button));
            SendMessageW(hwnd, WM_COMMAND, register_id, 0);
            require(registered && startup_enabled(paths));
            SendMessageW(hwnd, WM_COMMAND, register_id, 0);
            require(!registered && !startup_enabled(paths));
            const auto report_path = paths.settings.parent_path() / L"test-diagnostics.txt";
            save_report(report_path);
            std::ifstream report(report_path);
            const std::string contents{std::istreambuf_iterator<char>(report), {}};
            require(contents.find("capture_misses,render_misses") != std::string::npos &&
                    contents.find("input_id=nonexistent-input") != std::string::npos);
            for (const bool capture : {true, false}) {
                const auto combo = capture ? input : output;
                auto& endpoints = capture ? inputs : outputs;
                auto& remembered = capture ? input_selection : output_selection;
                const auto id = capture ? input_id : output_id;
                const std::vector<audio::Endpoint> pair{
                    {L"nonexistent-A", L"Duplicate name", {}, 0}, {L"nonexistent-B", L"Duplicate name", {}, 0}};
                auto refill = [&](const std::vector<audio::Endpoint>& next) {
                    endpoints = next;
                    fill(combo, endpoints, remembered);
                    selection_status();
                };
                auto choose = [&](WPARAM index) {
                    SendMessageW(combo, CB_SETCURSEL, index, 0);
                    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
                };
                remembered.clear();
                refill({}); refill({}); refill(pair);
                require(selected(combo, endpoints) == L"nonexistent-A"); // first available list only
                choose(1);
                require(remembered == L"nonexistent-B");
                refill({pair[1], pair[0]});
                require(selected(combo, endpoints) == L"nonexistent-B" && IsWindowEnabled(start_button));
                refill({pair[0]}); refill({pair[0]});
                require(selected(combo, endpoints).empty() && remembered == L"nonexistent-B" && !IsWindowEnabled(start_button));
                refill(pair);
                require(selected(combo, endpoints) == L"nonexistent-B" && IsWindowEnabled(start_button));
                refill({}); refill({}); refill({pair[0]});
                require(selected(combo, endpoints).empty() && remembered == L"nonexistent-B" && !IsWindowEnabled(start_button));
                refill(pair);
                require(selected(combo, endpoints) == L"nonexistent-B" && IsWindowEnabled(start_button));
                refill({pair[0]}); choose(0); // explicit replacement while B is absent
                require(remembered == L"nonexistent-A" && IsWindowEnabled(start_button));
                refill({pair[1], pair[0]}); refill({}); refill(pair);
                require(selected(combo, endpoints) == L"nonexistent-A" && IsWindowEnabled(start_button));
                write(std::wstring(capture ? L"Input" : L"Output") +
                    L" selection regression: passed (initial empty, duplicate names/reorder, repeated removal Refresh, empty-list return, ID reappearance, explicit reselection).\n");
            }
            SendMessageW(hwnd, WM_COMMAND, start_id, 0);
            require(active && !IsWindowEnabled(input) && !IsWindowEnabled(output) && !IsWindowEnabled(preset) &&
                !IsWindowEnabled(refresh_button) && !IsWindowEnabled(start_button) && IsWindowEnabled(stop_button));
            ++test_step;
        } else if (test_step == 1 && worker->snapshot().state == audio::State::failed) {
            update(); require(!active && recovery.wanted() && IsWindowEnabled(stop_button) && !IsWindowEnabled(refresh_button));
            SendMessageW(hwnd, WM_COMMAND, stop_id, 0);
            require(!recovery.wanted() && IsWindowEnabled(start_button) && IsWindowEnabled(refresh_button));
            SendMessageW(hwnd, WM_COMMAND, start_id, 0);
            SendMessageW(hwnd, WM_COMMAND, stop_id, 0);
            SendMessageW(hwnd, WM_COMMAND, stop_id, 0);
            require(!active && IsWindowEnabled(input));
            SendMessageW(hwnd, WM_COMMAND, start_id, 0);
            // Close hides without stopping; explicit Exit joins. Tray creation
            // may fail on a hosted shell, in which case Close keeps settings usable.
            const bool icon = tray.present();
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
            require(IsWindow(hwnd) && active && recovery.wanted() && (icon ? !IsWindowVisible(hwnd) : IsWindowVisible(hwnd)));
            SendMessageW(hwnd, show_settings, 0, 0); require(IsWindowVisible(hwnd));
            SendMessageW(hwnd, WM_SIZE, SIZE_MINIMIZED, 0);
            require(icon ? !IsWindowVisible(hwnd) : IsWindowVisible(hwnd));
            SendMessageW(hwnd, taskbar_created, 0, 0);
            SendMessageW(hwnd, WM_COMMAND, stop_id, 0);
            input_selection = L"saved-missing-input"; output_selection = L"saved-missing-output";
            inputs.clear(); outputs.clear(); fill(input, inputs, input_selection); fill(output, outputs, output_selection);
            SendMessageW(hwnd, WM_COMMAND, start_id, 0); require(recovery.wanted() && !active);
            SendMessageW(hwnd, devices_message, 0, 0);
            require(recovery.wanted() && !active && input_selection == L"saved-missing-input" && output_selection == L"saved-missing-output");
            SendMessageW(hwnd, WM_COMMAND, stop_id, 0);
            SendMessageW(hwnd, devices_message, 0, 0); tick();
            require(!recovery.wanted() && !active);
            // A real Worker with a fake Session proves notification-driven recovery
            // actually restarts, preserves settings, and does not duplicate routing.
            synthetic_devices = true; inputs.clear(); outputs.clear();
            fill(input, inputs, input_selection); fill(output, outputs, output_selection);
            route_test = std::make_shared<RouteTestState>();
            testing_factory = [state = route_test](const audio::Settings& settings) {
                state->last = settings; ++state->starts;
                return std::make_unique<RouteTestSession>(state);
            };
            SendMessageW(preset, CB_SETCURSEL, 1, 0);
            SendMessageW(hwnd, WM_COMMAND, start_id, 0);
            require(recovery.wanted() && !active && route_test->starts == 0);
            inputs = {{input_selection, L"Returned input", {}, 0}};
            outputs = {{output_selection, L"Returned output", {}, 0}};
            SendMessageW(hwnd, devices_message, 0, 0);
            require(active); ++test_step;
        } else if (test_step == 2 && worker->snapshot().state == audio::State::running) {
            require(route_test->starts == 1 && route_test->last.input_id == input_selection &&
                route_test->last.output_id == output_selection &&
                route_test->last.preset == audio::Preset::stable);
            inputs.insert(inputs.begin(), {L"other-input", L"Unrelated device", {}, 0});
            SendMessageW(hwnd, devices_message, 0, 0);
            require(active && route_test->starts == 1); // unrelated/default notifications never restart healthy audio
            inputs.erase(inputs.begin() + 1);
            SendMessageW(hwnd, devices_message, 0, 0);
            require(!active && recovery.wanted() && route_test->stops == 1 && route_test->starts == 1);
            inputs.push_back({input_selection, L"Returned input", {}, 0});
            SendMessageW(hwnd, devices_message, 0, 0); require(active); ++test_step;
        } else if (test_step == 3 && worker->snapshot().state == audio::State::running) {
            require(route_test->starts == 2 && selected(input, inputs) == L"saved-missing-input");
            inputs.clear(); SendMessageW(hwnd, devices_message, 0, 0);
            require(!active && recovery.wanted() && route_test->stops == 2);
            SendMessageW(hwnd, WM_COMMAND, stop_id, 0);
            inputs.push_back({input_selection, L"Returned input", {}, 0});
            SendMessageW(hwnd, devices_message, 0, 0); tick();
            require(!active && !recovery.wanted() && route_test->starts == 2);
            passed = true;
            SendMessageW(hwnd, WM_COMMAND, exit_id, 0);
        }
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) noexcept {
        auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wp, lp);
        try {
            if (message == self->taskbar_created) {
                self->tray.remove();
                if (!self->tray.add(window)) self->show();
                return 0;
            }
            if (message == self->show_settings) { self->show(); return 0; }
            switch (message) {
            case WM_CREATE: self->create(); return 0;
            case WM_COMMAND:
                switch (LOWORD(wp)) {
                case refresh_id: self->refresh(); break;
                case start_id: self->start(); break;
                case stop_id: self->stop(); break;
                case save_id: self->save(); break;
                case settings_id: self->show(); break;
                case exit_id: self->stop(); DestroyWindow(window); break;
                case register_id: self->toggle_registration(); break;
                case export_id: self->export_diagnostics(); break;
                case input_id: case output_id:
                    if (HIWORD(wp) == CBN_SELCHANGE && !self->active) {
                        auto& remembered = LOWORD(wp) == input_id ? self->input_selection : self->output_selection;
                        const auto current = selected(LOWORD(wp) == input_id ? self->input : self->output,
                            LOWORD(wp) == input_id ? self->inputs : self->outputs);
                        if (!current.empty()) remembered = current;
                        self->worker.reset(); self->selection_status();
                    }
                    break;
                }
                return 0;
            case WM_TIMER: if (self->testing) self->test_tick(); else self->tick(); return 0;
            case devices_message: self->devices_changed(); return 0;
            case tray_message:
                if (LOWORD(lp) == WM_CONTEXTMENU) self->menu();
                else if (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT) self->show();
                return 0;
            case WM_SIZE:
                if (wp == SIZE_MINIMIZED) { self->hide(); return 0; }
                if (self->details) {
                    const int width = std::max(680, static_cast<int>(LOWORD(lp)) - 32);
                    MoveWindow(self->input, 16, 40, width, 240, TRUE);
                    MoveWindow(self->output, 16, 100, width, 240, TRUE);
                    MoveWindow(self->status, 16, 284, width, 48, TRUE);
                    MoveWindow(self->details, 16, 338, width, std::max(80, static_cast<int>(HIWORD(lp)) - 354), TRUE);
                }
                return 0;
            case WM_GETMINMAXINFO:
                {
                    RECT bounds{0, 0, 792, 456};
                    AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CONTROLPARENT);
                    reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {bounds.right - bounds.left, bounds.bottom - bounds.top};
                    return 0;
                }
            case WM_QUERYENDSESSION: return TRUE;
            case WM_ENDSESSION: if (wp) self->stop(); return 0;
            case WM_CLOSE: self->hide(); return 0;
            case WM_DESTROY:
                KillTimer(window, 1);
                self->notifications.detach(); self->tray.remove(); self->recovery.stop();
                if (self->worker) self->worker->stop();
                if (self->testing && !self->launch_probe) {
                    if (!self->passed) self->exit_code = 1;
                    write(self->exit_code ? L"gui self-test: FAILED\n" :
                        L"gui self-test: passed (native controls, synthetic empty/ID selection, settings lock, invalid endpoint failure, restart, repeated stop, tray hide/show, device wait/cancel, explicit exit/join; no interactive/hardware QA)\n");
                }
                if (!self->launch_probe) PostQuitMessage(self->exit_code);
                return 0;
            }
        } catch (...) {
            self->exit_code = 1;
            if (self->worker) self->worker->stop();
            if (message == WM_CREATE) return -1;
            DestroyWindow(window);
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }
};
int gui(HINSTANCE instance, int show, bool testing, bool background) {
    const auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 3;
    struct Uninit { ~Uninit() { CoUninitialize(); } } uninit;
    if (testing && !host_self_test()) { write(L"desktop host self-test: FAILED\n"); return 1; }
    struct Scratch {
        std::filesystem::path root;
        ~Scratch() { if (!root.empty()) { std::error_code error; std::filesystem::remove_all(root, error); } }
    } scratch;
    Window app{}; app.instance = instance; app.testing = testing;
    if (testing) {
        GUID id{}; if (FAILED(CoCreateGuid(&id))) return 3;
        wchar_t identity[40]{}; if (!StringFromGUID2(id, identity, 40)) return 3;
        app.instance_identity = identity;
        app.instance_class = L"AudioRepeaterWasapiTestWindow-" + app.instance_identity;
        scratch.root = std::filesystem::temp_directory_path() / (L"AudioRepeaterWasapi-test-" + app.instance_identity);
        app.paths = {scratch.root / L"config" / L"settings.conf", scratch.root / L"startup" / L"AudioRepeaterWasapi.lnk",
            scratch.root / L"audio-repeater.exe"};
    } else { app.paths = user_paths(); app.instance_class = window_class; }
    Instance instance_guard(app.instance_identity);
    if (instance_guard.existing()) return forward_existing(app.instance_class.c_str(), background) ? 0 : 3;
    WNDCLASSW type{};
    type.lpfnWndProc = Window::procedure; type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    type.lpszClassName = app.instance_class.c_str();
    if (!RegisterClassW(&type)) return 3;
    const auto window = CreateWindowExW(WS_EX_CONTROLPARENT, type.lpszClassName, L"Audio Repeater WASAPI",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 860, 764, nullptr, nullptr, instance, &app);
    if (!window) return 3;
    app.show_on_launch(show, app.tray.present());
    MSG message{};
    int result = 0;
    while ((result = static_cast<int>(GetMessageW(&message, nullptr, 0, 0))) > 0) {
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    // A message-loop error also joins before Window storage is released.
    if (app.worker) app.worker->stop();
    if (IsWindow(window)) DestroyWindow(window);
    return result < 0 ? 3 : static_cast<int>(message.wParam);
}

}
