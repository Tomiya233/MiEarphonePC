#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>

#include "bt_session.hpp"
#include "config_store.hpp"
#include "icon_gen.hpp"
#include "rcsp.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace miearphone;

constexpr wchar_t kWndClass[] = L"MiEarphonePC_MainWnd";
constexpr wchar_t kAppTitle[] = L"小米耳机 · PC 原生控制中心";
constexpr float kPi = 3.14159265358979323846f;

constexpr UINT WM_APP_STATE    = WM_APP + 1;
constexpr UINT WM_APP_LOG      = WM_APP + 2;
constexpr UINT WM_APP_DEVICES  = WM_APP + 3;
constexpr UINT WM_APP_TOAST    = WM_APP + 4;
constexpr UINT WM_APP_BUSY     = WM_APP + 5;
constexpr UINT WM_APP_TRAY     = WM_APP + 6;
constexpr UINT WM_APP_SHOW_ME  = WM_APP + 10;
constexpr UINT WM_APP_QUIT_FOR_UPDATE = WM_APP + 11;

constexpr UINT_PTR TIMER_AUTORECONNECT = 101;
constexpr UINT_PTR TIMER_TOAST_CLEAR   = 102;
constexpr UINT_PTR TIMER_ANIM          = 104; // Demand-driven ~70fps (14ms)

// Tray Menu Command IDs
enum TrayCmd : UINT {
    ID_TRAY_SHOW = 1001,
    ID_TRAY_EXIT = 1002,
    ID_TRAY_CONNECT = 1003,
    ID_TRAY_DISCONNECT = 1004,
    ID_TRAY_REFRESH = 1005,
    ID_TRAY_AUTOSTART = 1006,
    ID_TRAY_THEME = 1007,

    ID_TRAY_ANC_OFF = 1100,
    ID_TRAY_ANC_NOISE_BASE = 1110, // +0..2 (轻度, 均衡, 深度)
    ID_TRAY_ANC_ADAPTIVE   = 1118, // 自适应降噪 (CFG 37)
    ID_TRAY_ANC_TRANS_BASE = 1120, // +0..2 (标准通透, 人声增强, 环境增强)

    ID_TRAY_EQ_BASE = 1200,        // + preset index
    ID_TRAY_SPATIAL_OFF = 1300,
    ID_TRAY_SPATIAL_ON = 1301,
    ID_TRAY_SPATIAL_HEAD = 1302,
    ID_TRAY_SCENE_BASE = 1310,     // +0..5 (0=off, 1..5=modes)

    ID_TRAY_MULTIPOINT = 1401,
    ID_TRAY_LOW_LATENCY = 1402,
    ID_TRAY_PERS_ANC = 1403,
    ID_TRAY_ADAPT_VOL = 1404,

    ID_TRAY_FIND_BOTH = 1501,
    ID_TRAY_FIND_LEFT = 1502,
    ID_TRAY_FIND_RIGHT = 1503,
    ID_TRAY_FIND_STOP = 1504,
};

struct PresetItem {
    uint8_t code;
    const wchar_t* label;
};

// Official Xiaomi Earbuds App 3-gear manual ANC order & naming (NoiseReductionVM.java):
// 1 = 轻度 (mild), 0 = 均衡 (balanced), 2 = 深度 (deep)
static constexpr PresetItem kNoiseLevels[] = {
    {ANC_LEVEL_MILD,    L"轻度"},
    {ANC_LEVEL_BALANCE, L"均衡"},
    {ANC_LEVEL_DEEP,    L"深度"},
};

// Official Xiaomi Earbuds App 3-gear Transparency naming (NoiseReductionVM.java):
// 0 = 标准通透, 1 = 人声增强, 2 = 环境增强
static constexpr PresetItem kTransLevels[] = {
    {TRANS_LEVEL_STANDARD, L"标准通透"},
    {TRANS_LEVEL_VOCAL,    L"人声增强"},
    {TRANS_LEVEL_ENV,      L"环境增强"},
};

static constexpr PresetItem kEqPresets[] = {
    {EQ_DEFAULT,      L"默认均衡"},
    {EQ_HUMAN_VOICE,  L"人声增强"},
    {EQ_LOW_BASS,     L"低音增强"},
    {EQ_HIGH_TREBLE,  L"高音增强"},
    {EQ_VOLUME_BOOST, L"音量增强"},
    {EQ_ROCK,         L"摇滚"},
    {EQ_POPULARITY,   L"流行"},
    {EQ_CLASSIC,      L"古典"},
    {EQ_CUSTOM,       L"自定义均衡器"},
};

static constexpr PresetItem kSceneModes[] = {
    {0, L"关闭"},
    {1, L"标准"},
    {2, L"音乐"},
    {3, L"视频"},
    {4, L"游戏"},
    {5, L"有声书"},
};

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string wide_to_utf8_str(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

const wchar_t* anc_mode_name(uint8_t mode) {
    switch (mode) {
        case ANC_MODE_CLOSE: return L"关闭";
        case ANC_MODE_NOISE: return L"降噪";
        case ANC_MODE_TRANSPARENT: return L"通透";
        default: return L"未知";
    }
}

const wchar_t* anc_level_name(uint8_t mode, uint8_t level, bool auto_noise = false) {
    if (mode == ANC_MODE_NOISE) {
        if (auto_noise) return L"自适应降噪";
        for (const auto& it : kNoiseLevels) if (it.code == level) return it.label;
        return L"深度";
    } else if (mode == ANC_MODE_TRANSPARENT) {
        for (const auto& it : kTransLevels) if (it.code == level) return it.label;
        return L"标准通透";
    }
    return L"默认";
}

const wchar_t* eq_mode_name(uint8_t mode) {
    for (const auto& it : kEqPresets) if (it.code == mode) return it.label;
    return L"均衡";
}

// ---------------- Color, Math & Animation Primitives ----------------
float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

float lerp(float a, float b, float t) { return a + (b - a) * t; }

D2D1_COLOR_F rgb(uint32_t hex, float a = 1.0f) {
    return D2D1::ColorF(
        static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
        static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
        static_cast<float>(hex & 0xFF) / 255.0f,
        a);
}

D2D1_COLOR_F with_alpha(D2D1_COLOR_F c, float a) {
    c.a *= a;
    return c;
}

D2D1_COLOR_F lerp_col(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    t = clamp01(t);
    return D2D1::ColorF(
        lerp(a.r, b.r, t),
        lerp(a.g, b.g, t),
        lerp(a.b, b.b, t),
        lerp(a.a, b.a, t));
}

bool smooth_to(float& cur, float target, float speed, float dt) {
    float diff = target - cur;
    if (std::fabs(diff) <= 0.0012f) {
        bool was_diff = (cur != target);
        cur = target;
        return was_diff;
    }
    cur += diff * (1.0f - std::exp(-speed * dt));
    if (std::fabs(target - cur) <= 0.0012f) {
        cur = target;
    }
    return true;
}

// HyperOS Light <-> HyperOS Pro Dark interpolated theme palette
struct Theme {
    float dark_t = 0.0f;
    D2D1_COLOR_F canvas{};
    D2D1_COLOR_F sidebar{};
    D2D1_COLOR_F card{};
    D2D1_COLOR_F subcard{};
    D2D1_COLOR_F seg_track{};
    D2D1_COLOR_F border{};
    D2D1_COLOR_F border_hot{};
    D2D1_COLOR_F specular{};
    D2D1_COLOR_F text_pri{};
    D2D1_COLOR_F text_sec{};
    D2D1_COLOR_F text_mut{};
    D2D1_COLOR_F btn_bg{};
    D2D1_COLOR_F btn_hot{};
    D2D1_COLOR_F sw_off{};
    D2D1_COLOR_F sw_off_hot{};
    D2D1_COLOR_F shadow{};

    static Theme build(float t) {
        Theme th;
        th.dark_t     = clamp01(t);
        th.canvas     = lerp_col(rgb(0xEEF3FA), rgb(0x0B0F19), t);
        th.sidebar    = lerp_col(rgb(0xFFFFFF), rgb(0x111827), t);
        th.card       = lerp_col(rgb(0xFFFFFF), rgb(0x151E2E), t);
        th.subcard    = lerp_col(rgb(0xF7FAFC), rgb(0x1C273B), t);
        th.seg_track  = lerp_col(rgb(0xE6ECF5), rgb(0x0E1524), t);
        th.border     = lerp_col(rgb(0xE2E8F0), rgb(0x263449), t);
        th.border_hot = lerp_col(rgb(0xCBD5E1), rgb(0x3B4D6B), t);
        th.specular   = lerp_col(rgb(0xFFFFFF, 0.75f), rgb(0xFFFFFF, 0.07f), t);
        th.text_pri   = lerp_col(rgb(0x0F172A), rgb(0xF8FAFC), t);
        th.text_sec   = lerp_col(rgb(0x334155), rgb(0xCBD5E1), t);
        th.text_mut   = lerp_col(rgb(0x64748B), rgb(0x8B9BB4), t);
        th.btn_bg     = lerp_col(rgb(0xF1F5F9), rgb(0x1F2B40), t);
        th.btn_hot    = lerp_col(rgb(0xE5ECF6), rgb(0x2A3A56), t);
        th.sw_off     = lerp_col(rgb(0xCBD5E1), rgb(0x334155), t);
        th.sw_off_hot = lerp_col(rgb(0x94A3B8), rgb(0x475569), t);
        th.shadow     = lerp_col(rgb(0x0F172A, 0.06f), rgb(0x000000, 0.28f), t);
        return th;
    }
};

// Single background worker thread so UI never blocks on RFCOMM I/O
class WorkerQueue {
public:
    WorkerQueue() : running_(true), th_(&WorkerQueue::loop, this) {}
    ~WorkerQueue() { stop(); }

    void post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!running_) return;
            q_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!running_) return;
            running_ = false;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }

private:
    void loop() {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this] { return !running_ || !q_.empty(); });
                if (!running_ && q_.empty()) return;
                job = std::move(q_.front());
                q_.pop_front();
            }
            if (job) job();
        }
    }

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool running_ = false;
    std::thread th_;
};

// Immediate-mode hit zone IDs
enum HitId : int {
    HIT_NONE = 0,
    HIT_THEME_TOGGLE = 9,
    HIT_REFRESH_DEVS = 10,
    HIT_MAC_BOX = 11,
    HIT_CONNECT_BTN = 12,
    HIT_HIDE_TRAY_BTN = 13,
    HIT_EXIT_APP_BTN = 14,

    HIT_OPT_CLOSE_TRAY = 20,
    HIT_OPT_AUTO_CONN = 21,
    HIT_OPT_AUTO_RECONN = 22,
    HIT_OPT_AUTOSTART = 23,

    HIT_REFRESH_STATE = 30,

    HIT_TAB_BASE = 40, // +0..3

    HIT_ANC_MODE_BASE = 100, // +0..2
    HIT_ANC_LVL_BASE  = 110, // +index
    HIT_ANC_ADAPTIVE  = 125, // 自适应降噪 (CFG 37)

    HIT_SW_MULTIPOINT  = 200,
    HIT_SW_LOW_LATENCY = 201,
    HIT_SW_PERS_ANC    = 202,
    HIT_SW_ADAPT_VOL   = 203,
    HIT_SW_SPATIAL     = 204,
    HIT_SW_HEAD_TRACK  = 205,
    HIT_SW_AUTO_ANS    = 207,

    HIT_EQ_PRESET_BASE = 300, // +index
    HIT_EQ_BAND_BASE   = 320, // +0..9
    HIT_EQ_APPLY       = 340,
    HIT_EQ_READ        = 341,
    HIT_EQ_RESET       = 342,

    HIT_SCENE_BASE     = 400, // +index (0..5)
    HIT_FIND_BOTH      = 420,
    HIT_FIND_LEFT      = 421,
    HIT_FIND_RIGHT     = 422,
    HIT_FIND_STOP      = 423,

    HIT_LOG_COPY       = 500,
    HIT_LOG_CLEAR      = 501,
    HIT_LOG_REAUTH     = 502,

    HIT_DEV_ITEM_BASE  = 1000, // +index
};

struct HitZone {
    D2D1_RECT_F rect;
    int id = HIT_NONE;
};

class MainWindow {
public:
    bool init(HINSTANCE hInst, bool start_in_tray) {
        hInst_ = hInst;
        QueryPerformanceFrequency(&qpc_freq_);
        QueryPerformanceCounter(&last_qpc_);

        cfg_.load();
        mac_input_ = cfg_.last_mac;
        autostart_cached_ = AppConfig::is_autostart_enabled();
        theme_t_ = cfg_.dark_mode ? 1.0f : 0.0f;
        sw_anim_[0] = cfg_.close_to_tray ? 1.0f : 0.0f;
        sw_anim_[1] = cfg_.auto_connect ? 1.0f : 0.0f;
        sw_anim_[2] = cfg_.auto_reconnect ? 1.0f : 0.0f;
        sw_anim_[3] = autostart_cached_ ? 1.0f : 0.0f;

        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_);
        if (FAILED(hr)) return false;
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(&dw_factory_));
        if (FAILED(hr)) return false;

        D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
        d2d_factory_->CreateStrokeStyle(ssp, nullptr, 0, &stroke_round_);

        create_text_formats();

        icon_app_   = create_earbud_icon(64, 0xFF6900);
        tray_disc_  = create_earbud_icon(32, 0x7A8494);
        tray_off_   = create_earbud_icon(32, 0xFF6900);
        tray_noise_ = create_earbud_icon(32, 0x2563EB);
        tray_trans_ = create_earbud_icon(32, 0x10B981);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &MainWindow::wnd_proc_static;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = icon_app_;
        wc.hIconSm = tray_off_;
        wc.lpszClassName = kWndClass;
        RegisterClassExW(&wc);

        UINT dpi = GetDpiForSystem();
        float scale = static_cast<float>(dpi) / 96.0f;
        int w = static_cast<int>(std::round(1080.0f * scale));
        int h = static_cast<int>(std::round(720.0f * scale));
        int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

        hwnd_ = CreateWindowExW(
            0, kWndClass, kAppTitle,
            WS_OVERLAPPEDWINDOW,
            std::max(0, sx), std::max(0, sy), w, h,
            nullptr, nullptr, hInst, this);
        if (!hwnd_) return false;

        // Windows 11 rounded corners & immersive titlebar theme sync
        DWORD corner_pref = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner_pref, sizeof(corner_pref));
        apply_titlebar_theme();

        wm_taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");

        session_.set_callbacks(
            [this](const std::string& line) {
                {
                    std::lock_guard<std::mutex> lk(data_mtx_);
                    if (logs_.size() >= 300) logs_.pop_front();
                    logs_.push_back(utf8_to_wide(line));
                }
                if (hwnd_) PostMessageW(hwnd_, WM_APP_LOG, 0, 0);
            },
            [this](const EarphoneSnapshot& snap) {
                {
                    std::lock_guard<std::mutex> lk(data_mtx_);
                    snap_ = snap;
                    if (!eq_dirty_ && !snap.custom_eq.bands.empty()) {
                        ui_eq_bands_ = snap.custom_eq.bands;
                    }
                }
                if (hwnd_) PostMessageW(hwnd_, WM_APP_STATE, 0, 0);
            },
            [this](const std::string& reason) {
                show_toast(L"连接已断开：" + utf8_to_wide(reason), true);
                if (hwnd_) PostMessageW(hwnd_, WM_APP_STATE, 0, 0);
            });

        init_tray();
        SetTimer(hwnd_, TIMER_AUTORECONNECT, 10000, nullptr);

        if (!start_in_tray) {
            ShowWindow(hwnd_, SW_SHOW);
            UpdateWindow(hwnd_);
            wake_anim();
        } else {
            show_toast(L"已在系统托盘静默运行", false);
        }

        refresh_devices_async(cfg_.auto_connect);
        return true;
    }

    int message_loop() {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

private:
    void apply_titlebar_theme() {
        if (!hwnd_) return;
        BOOL dark = cfg_.dark_mode ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    }

    void create_text_formats() {
        auto make_fmt = [&](const wchar_t* font, float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
            dw_factory_->CreateTextFormat(
                font, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                size, L"zh-CN", out);
            if (*out) (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        };
        make_fmt(L"Microsoft YaHei UI", 17.5f, DWRITE_FONT_WEIGHT_BOLD, &tf_brand_);
        make_fmt(L"Microsoft YaHei UI", 18.0f, DWRITE_FONT_WEIGHT_BOLD, &tf_h1_);
        make_fmt(L"Microsoft YaHei UI", 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &tf_h2_);
        make_fmt(L"Microsoft YaHei UI", 12.5f, DWRITE_FONT_WEIGHT_MEDIUM, &tf_body_);
        make_fmt(L"Microsoft YaHei UI", 11.0f, DWRITE_FONT_WEIGHT_NORMAL, &tf_sub_);
        make_fmt(L"Microsoft YaHei UI", 15.5f, DWRITE_FONT_WEIGHT_BOLD, &tf_bat_big_);
        make_fmt(L"Consolas", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, &tf_mono_);
        if (tf_mono_) tf_mono_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }

    // ---------------- Demand-Driven 70 FPS Animation Engine ----------------
    void wake_anim() {
        if (!hwnd_ || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) return;
        if (!anim_timer_on_) {
            QueryPerformanceCounter(&last_qpc_);
            SetTimer(hwnd_, TIMER_ANIM, 14, nullptr);
            anim_timer_on_ = true;
        }
    }

    void stop_anim_timer() {
        if (anim_timer_on_ && hwnd_) {
            KillTimer(hwnd_, TIMER_ANIM);
        }
        anim_timer_on_ = false;
    }

    float hover_val(int id) const {
        auto it = hover_map_.find(id);
        return (it != hover_map_.end()) ? it->second : 0.0f;
    }

    float press_val(int id) const {
        return (press_id_ == id) ? press_anim_ : 0.0f;
    }

    bool step_animations(float dt) {
        EarphoneSnapshot s;
        std::vector<EqBand> bands;
        bool mac_foc = mac_focused_;
        {
            std::lock_guard<std::mutex> lk(data_mtx_);
            s = snap_;
            bands = ui_eq_bands_;
        }

        bool busy = false;

        // 1. Theme crossfade
        busy |= smooth_to(theme_t_, cfg_.dark_mode ? 1.0f : 0.0f, 11.0f, dt);

        // 2. Segmented tab sliding pill
        busy |= smooth_to(tab_pill_pos_, static_cast<float>(active_tab_), 16.0f, dt);

        // 3. Page slide+fade transition
        busy |= smooth_to(page_enter_t_, 1.0f, 14.0f, dt);

        // 4. Floating glass toast overlay
        bool toast_moving = smooth_to(toast_anim_, toast_visible_ ? 1.0f : 0.0f, 15.0f, dt);
        busy |= toast_moving;
        if (!toast_visible_ && toast_anim_ <= 0.0012f) {
            std::lock_guard<std::mutex> lk(data_mtx_);
            toast_msg_.clear();
        }

        // 5. Battery 270-degree ring gauges (L / R / Case)
        float bat_targets[3] = {
            (s.connected && s.battery.left.online()) ? static_cast<float>(s.battery.left.percent) : 0.0f,
            (s.connected && s.battery.right.online()) ? static_cast<float>(s.battery.right.percent) : 0.0f,
            (s.connected && s.battery.box.online()) ? static_cast<float>(s.battery.box.percent) : 0.0f,
        };
        for (int i = 0; i < 3; ++i) {
            busy |= smooth_to(bat_anim_[i], bat_targets[i], 6.5f, dt);
        }

        // 6. 11 Liquid toggle switches
        float sw_targets[11] = {
            cfg_.close_to_tray ? 1.0f : 0.0f,
            cfg_.auto_connect ? 1.0f : 0.0f,
            cfg_.auto_reconnect ? 1.0f : 0.0f,
            autostart_cached_ ? 1.0f : 0.0f,
            (s.connected && s.multipoint) ? 1.0f : 0.0f,
            (s.connected && s.low_latency) ? 1.0f : 0.0f,
            (s.connected && s.personalized_anc) ? 1.0f : 0.0f,
            (s.connected && s.adaptive_volume) ? 1.0f : 0.0f,
            (s.connected && s.spatial.open) ? 1.0f : 0.0f,
            (s.connected && s.spatial.open && s.spatial.head_tracking) ? 1.0f : 0.0f,
            (s.connected && s.auto_answer) ? 1.0f : 0.0f,
        };
        for (int i = 0; i < 11; ++i) {
            busy |= smooth_to(sw_anim_[i], sw_targets[i], 16.0f, dt);
        }

        // 7. 10-Band EQ smooth spline morphing
        for (int i = 0; i < 10; ++i) {
            float tgt = (i < static_cast<int>(bands.size())) ? static_cast<float>(bands[i].gain) : 0.0f;
            if (dragging_eq_band_ == i) {
                eq_anim_[i] = tgt;
            } else {
                busy |= smooth_to(eq_anim_[i], tgt, 14.0f, dt);
            }
        }

        // 8. Hover transitions & tactile press squish
        if (hot_id_ != HIT_NONE && hover_map_.find(hot_id_) == hover_map_.end()) {
            hover_map_[hot_id_] = 0.0f;
        }
        for (auto it = hover_map_.begin(); it != hover_map_.end();) {
            float tgt = (it->first == hot_id_) ? 1.0f : 0.0f;
            busy |= smooth_to(it->second, tgt, 18.0f, dt);
            if (tgt == 0.0f && it->second <= 0.0012f) {
                it = hover_map_.erase(it);
            } else {
                ++it;
            }
        }
        if (smooth_to(press_anim_, 0.0f, 24.0f, dt)) {
            busy = true;
        } else if (press_anim_ <= 0.0012f) {
            press_id_ = HIT_NONE;
        }

        // 9. Refresh spin animation
        if (smooth_to(refresh_spin_deg_, refresh_spin_target_, 10.0f, dt)) {
            busy = true;
        } else if (refresh_spin_deg_ >= 360.0f) {
            refresh_spin_deg_ = std::fmod(refresh_spin_deg_, 360.0f);
            refresh_spin_target_ = refresh_spin_deg_;
        }

        // 10. Find device sonar countdown & ambient wave/breathing oscillator
        if (ring_pulse_rem_ > 0.0f) {
            ring_pulse_rem_ = std::max(0.0f, ring_pulse_rem_ - dt);
            busy = true;
        }

        bool need_ambient =
            busy_.load() ||
            mac_foc ||
            (ring_pulse_rem_ > 0.0f) ||
            (s.connected && (s.battery.left.charging || s.battery.right.charging || s.battery.box.charging)) ||
            (s.connected && active_tab_ == 0 && (s.anc_mode == ANC_MODE_NOISE || s.anc_mode == ANC_MODE_TRANSPARENT)) ||
            (s.connected && active_tab_ == 2 && s.spatial.open);

        if (need_ambient) {
            ambient_phase_ += 2.2f * dt;
            if (ambient_phase_ > 2000.0f * kPi) ambient_phase_ = std::fmod(ambient_phase_, 2.0f * kPi);
            busy = true;
        }

        return busy;
    }

    // ---------------- System Tray ----------------
    static void set_nid_tip(NOTIFYICONDATAW& nid, const std::wstring& s) {
        constexpr size_t kMax = std::size(nid.szTip) - 1;
        size_t n = std::min(s.size(), kMax);
        std::wmemset(nid.szTip, 0, std::size(nid.szTip));
        if (n > 0) std::wmemcpy(nid.szTip, s.data(), n);
    }

    void init_tray() {
        nid_ = {};
        nid_.cbSize = sizeof(nid_);
        nid_.hWnd = hwnd_;
        nid_.uID = 1;
        nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid_.uCallbackMessage = WM_APP_TRAY;
        nid_.hIcon = tray_disc_;
        set_nid_tip(nid_, L"小米耳机 · PC (未连接)");
        Shell_NotifyIconW(NIM_ADD, &nid_);
    }

    void update_tray() {
        EarphoneSnapshot s;
        {
            std::lock_guard<std::mutex> lk(data_mtx_);
            s = snap_;
        }
        if (!s.connected) {
            nid_.hIcon = tray_disc_;
            set_nid_tip(nid_, L"小米耳机 · PC (未连接)\n左键打开面板 · 右键快捷控制");
        } else {
            nid_.hIcon = (s.anc_mode == ANC_MODE_NOISE) ? tray_noise_
                       : (s.anc_mode == ANC_MODE_TRANSPARENT) ? tray_trans_
                       : tray_off_;
            std::wstring nm = s.name.empty() ? L"小米耳机" : utf8_to_wide(s.name);
            std::wstring lb = s.battery.left.online() ? (std::to_wstring(s.battery.left.percent) + L"%") : L"--";
            std::wstring rb = s.battery.right.online() ? (std::to_wstring(s.battery.right.percent) + L"%") : L"--";
            std::wstring bb = s.battery.box.online() ? (std::to_wstring(s.battery.box.percent) + L"%") : L"离线";
            std::wstring anc_desc = anc_mode_name(s.anc_mode);
            if (s.anc_mode != ANC_MODE_CLOSE) {
                anc_desc += std::wstring(L"·") + anc_level_name(s.anc_mode, s.anc_level, s.auto_noise);
            }
            std::wstring tip = nm +
                               L"\nL:" + lb + L" R:" + rb + L" 盒:" + bb +
                               L"\n" + anc_desc + L" | EQ:" + eq_mode_name(s.eq_mode);
            set_nid_tip(nid_, tip);
        }
        Shell_NotifyIconW(NIM_MODIFY, &nid_);
    }

    void remove_tray() {
        if (nid_.hWnd) {
            Shell_NotifyIconW(NIM_DELETE, &nid_);
            nid_.hWnd = nullptr;
        }
    }

    void show_tray_menu() {
        EarphoneSnapshot s;
        {
            std::lock_guard<std::mutex> lk(data_mtx_);
            s = snap_;
        }
        HMENU menu = CreatePopupMenu();

        if (s.connected) {
            std::wstring hdr = L"🎧 " + utf8_to_wide(s.name.empty() ? s.mac : s.name);
            hdr += L"  [L:" + (s.battery.left.online() ? std::to_wstring(s.battery.left.percent) + L"%" : L"--");
            hdr += L" R:" + (s.battery.right.online() ? std::to_wstring(s.battery.right.percent) + L"%" : L"--");
            if (s.battery.box.online()) hdr += L" 盒:" + std::to_wstring(s.battery.box.percent) + L"%";
            hdr += L"]";
            AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, hdr.c_str());
            AppendMenuW(menu, MF_STRING, ID_TRAY_REFRESH, L"↻ 刷新耳机状态与电量");
            AppendMenuW(menu, MF_STRING, ID_TRAY_DISCONNECT, L"断开耳机连接");
        } else {
            AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"🎧 小米耳机 · 未连接");
            std::wstring ctext = L"连接耳机 (" + utf8_to_wide(mac_input_) + L")";
            AppendMenuW(menu, MF_STRING | (busy_ ? MF_DISABLED : 0), ID_TRAY_CONNECT, ctext.c_str());
        }

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        UINT conn_flag = s.connected ? 0 : (MF_DISABLED | MF_GRAYED);

        AppendMenuW(menu, MF_STRING | conn_flag | ((s.connected && s.anc_mode == ANC_MODE_CLOSE) ? MF_CHECKED : 0),
                    ID_TRAY_ANC_OFF, L"关闭降噪与通透");

        HMENU sub_noise = CreatePopupMenu();
        for (size_t i = 0; i < std::size(kNoiseLevels); ++i) {
            bool chk = s.connected && s.anc_mode == ANC_MODE_NOISE && !s.auto_noise && s.anc_level == kNoiseLevels[i].code;
            AppendMenuW(sub_noise, MF_STRING | (chk ? MF_CHECKED : 0),
                        ID_TRAY_ANC_NOISE_BASE + i, kNoiseLevels[i].label);
        }
        AppendMenuW(sub_noise, MF_SEPARATOR, 0, nullptr);
        bool auto_chk = s.connected && s.anc_mode == ANC_MODE_NOISE && s.auto_noise;
        AppendMenuW(sub_noise, MF_STRING | (auto_chk ? MF_CHECKED : 0),
                    ID_TRAY_ANC_ADAPTIVE, L"✦ 自适应降噪");

        std::wstring noise_title = L"降噪模式";
        if (s.connected && s.anc_mode == ANC_MODE_NOISE) {
            noise_title = std::wstring(L"● 降噪模式 (") + anc_level_name(ANC_MODE_NOISE, s.anc_level, s.auto_noise) + L")";
        }
        AppendMenuW(menu, MF_POPUP | conn_flag, reinterpret_cast<UINT_PTR>(sub_noise), noise_title.c_str());

        HMENU sub_trans = CreatePopupMenu();
        for (size_t i = 0; i < std::size(kTransLevels); ++i) {
            bool chk = s.connected && s.anc_mode == ANC_MODE_TRANSPARENT && s.anc_level == kTransLevels[i].code;
            AppendMenuW(sub_trans, MF_STRING | (chk ? MF_CHECKED : 0),
                        ID_TRAY_ANC_TRANS_BASE + i, kTransLevels[i].label);
        }
        std::wstring trans_title = L"通透模式";
        if (s.connected && s.anc_mode == ANC_MODE_TRANSPARENT) {
            trans_title = std::wstring(L"● 通透模式 (") + anc_level_name(ANC_MODE_TRANSPARENT, s.anc_level) + L")";
        }
        AppendMenuW(menu, MF_POPUP | conn_flag, reinterpret_cast<UINT_PTR>(sub_trans), trans_title.c_str());

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        HMENU sub_eq = CreatePopupMenu();
        for (size_t i = 0; i < std::size(kEqPresets); ++i) {
            bool chk = s.connected && s.eq_mode == kEqPresets[i].code;
            AppendMenuW(sub_eq, MF_STRING | (chk ? MF_CHECKED : 0),
                        ID_TRAY_EQ_BASE + i, kEqPresets[i].label);
        }
        std::wstring eq_title = std::wstring(L"EQ 音效 (") + (s.connected ? eq_mode_name(s.eq_mode) : L"--") + L")";
        AppendMenuW(menu, MF_POPUP | conn_flag, reinterpret_cast<UINT_PTR>(sub_eq), eq_title.c_str());

        HMENU sub_sp = CreatePopupMenu();
        AppendMenuW(sub_sp, MF_STRING | ((s.connected && !s.spatial.open) ? MF_CHECKED : 0),
                    ID_TRAY_SPATIAL_OFF, L"空间音频：关闭");
        AppendMenuW(sub_sp, MF_STRING | ((s.connected && s.spatial.open && !s.spatial.head_tracking) ? MF_CHECKED : 0),
                    ID_TRAY_SPATIAL_ON, L"空间音频：固定环绕");
        AppendMenuW(sub_sp, MF_STRING | ((s.connected && s.spatial.open && s.spatial.head_tracking) ? MF_CHECKED : 0),
                    ID_TRAY_SPATIAL_HEAD, L"空间音频：头部追踪");
        AppendMenuW(sub_sp, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < std::size(kSceneModes); ++i) {
            bool chk = s.connected && ((i == 0 && !s.scene.open) ||
                       (i > 0 && s.scene.open && s.scene.mode == kSceneModes[i].code));
            std::wstring lbl = std::wstring(L"沉浸声：") + kSceneModes[i].label;
            AppendMenuW(sub_sp, MF_STRING | (chk ? MF_CHECKED : 0),
                        ID_TRAY_SCENE_BASE + i, lbl.c_str());
        }
        AppendMenuW(menu, MF_POPUP | conn_flag, reinterpret_cast<UINT_PTR>(sub_sp), L"空间音频与沉浸声");

        AppendMenuW(menu, MF_STRING | conn_flag | ((s.connected && s.multipoint) ? MF_CHECKED : 0),
                    ID_TRAY_MULTIPOINT, L"双设备连接");
        AppendMenuW(menu, MF_STRING | conn_flag | ((s.connected && s.low_latency) ? MF_CHECKED : 0),
                    ID_TRAY_LOW_LATENCY, L"低延迟游戏模式");
        AppendMenuW(menu, MF_STRING | conn_flag | ((s.connected && s.personalized_anc) ? MF_CHECKED : 0),
                    ID_TRAY_PERS_ANC, L"个性化降噪");
        AppendMenuW(menu, MF_STRING | conn_flag | ((s.connected && s.adaptive_volume) ? MF_CHECKED : 0),
                    ID_TRAY_ADAPT_VOL, L"自适应音量");

        HMENU sub_find = CreatePopupMenu();
        AppendMenuW(sub_find, MF_STRING, ID_TRAY_FIND_BOTH, L"双耳响铃查找");
        AppendMenuW(sub_find, MF_STRING, ID_TRAY_FIND_LEFT, L"仅左耳响铃");
        AppendMenuW(sub_find, MF_STRING, ID_TRAY_FIND_RIGHT, L"仅右耳响铃");
        AppendMenuW(sub_find, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub_find, MF_STRING, ID_TRAY_FIND_STOP, L"停止响铃");
        AppendMenuW(menu, MF_POPUP | conn_flag, reinterpret_cast<UINT_PTR>(sub_find), L"查找耳机");

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        AppendMenuW(menu, MF_STRING | (cfg_.dark_mode ? MF_CHECKED : 0),
                    ID_TRAY_THEME, L"深色主题 (HyperOS Pro Dark)");
        autostart_cached_ = AppConfig::is_autostart_enabled();
        AppendMenuW(menu, MF_STRING | (autostart_cached_ ? MF_CHECKED : 0),
                    ID_TRAY_AUTOSTART, L"开机自动启动 (驻留托盘)");
        AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"打开主控制面板");
        AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"彻底退出");

        SetMenuDefaultItem(menu, ID_TRAY_SHOW, FALSE);

        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                  pt.x, pt.y, 0, hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(menu);

        if (cmd != 0) handle_tray_command(cmd);
    }

    void handle_tray_command(UINT cmd) {
        if (cmd == ID_TRAY_SHOW) {
            restore_window();
            return;
        }
        if (cmd == ID_TRAY_EXIT) {
            really_quit();
            return;
        }
        if (cmd == ID_TRAY_THEME) {
            toggle_theme();
            return;
        }
        if (cmd == ID_TRAY_CONNECT) {
            connect_async(mac_input_, false);
            return;
        }
        if (cmd == ID_TRAY_DISCONNECT) {
            disconnect_async();
            return;
        }
        if (cmd == ID_TRAY_REFRESH) {
            worker_.post([this] { session_.refresh_all(); });
            return;
        }
        if (cmd == ID_TRAY_AUTOSTART) {
            bool next = !AppConfig::is_autostart_enabled();
            if (AppConfig::set_autostart_enabled(next)) {
                autostart_cached_ = next;
                show_toast(next ? L"✓ 已开启开机自启 (静默驻留托盘)" : L"已关闭开机自启", false);
                wake_anim();
            }
            return;
        }
        if (cmd == ID_TRAY_ANC_OFF) {
            set_anc_async(ANC_MODE_CLOSE, 0, false);
            return;
        }
        if (cmd >= ID_TRAY_ANC_NOISE_BASE && cmd < ID_TRAY_ANC_NOISE_BASE + std::size(kNoiseLevels)) {
            set_anc_async(ANC_MODE_NOISE, kNoiseLevels[cmd - ID_TRAY_ANC_NOISE_BASE].code, false);
            return;
        }
        if (cmd == ID_TRAY_ANC_ADAPTIVE) {
            auto cur = session_.snapshot();
            bool next = !(cur.anc_mode == ANC_MODE_NOISE && cur.auto_noise);
            toggle_auto_noise_async(next);
            return;
        }
        if (cmd >= ID_TRAY_ANC_TRANS_BASE && cmd < ID_TRAY_ANC_TRANS_BASE + std::size(kTransLevels)) {
            set_anc_async(ANC_MODE_TRANSPARENT, kTransLevels[cmd - ID_TRAY_ANC_TRANS_BASE].code, false);
            return;
        }
        if (cmd >= ID_TRAY_EQ_BASE && cmd < ID_TRAY_EQ_BASE + std::size(kEqPresets)) {
            uint8_t eqc = kEqPresets[cmd - ID_TRAY_EQ_BASE].code;
            worker_.post([this, eqc] {
                if (session_.set_eq_preset(eqc)) {
                    show_toast(std::wstring(L"✓ EQ 音效已切换：") + eq_mode_name(eqc), false);
                }
            });
            return;
        }
        if (cmd == ID_TRAY_SPATIAL_OFF) {
            worker_.post([this] { session_.set_spatial_audio(false, false); show_toast(L"✓ 空间音频已关闭", false); });
            return;
        }
        if (cmd == ID_TRAY_SPATIAL_ON) {
            worker_.post([this] { session_.set_spatial_audio(true, false); show_toast(L"✓ 已开启空间音频 (固定)", false); });
            return;
        }
        if (cmd == ID_TRAY_SPATIAL_HEAD) {
            worker_.post([this] { session_.set_spatial_audio(true, true); show_toast(L"✓ 已开启空间音频 + 头部追踪", false); });
            return;
        }
        if (cmd >= ID_TRAY_SCENE_BASE && cmd < ID_TRAY_SCENE_BASE + std::size(kSceneModes)) {
            size_t idx = cmd - ID_TRAY_SCENE_BASE;
            bool open = idx > 0;
            uint8_t mode = open ? kSceneModes[idx].code : 1;
            worker_.post([this, open, mode, idx] {
                session_.set_scene_rendering(open, mode);
                show_toast(std::wstring(L"✓ 沉浸声：") + kSceneModes[idx].label, false);
            });
            return;
        }
        if (cmd == ID_TRAY_MULTIPOINT) {
            bool next = !session_.snapshot().multipoint;
            worker_.post([this, next] {
                bool ok = session_.set_multipoint(next);
                show_toast(ok ? (next ? L"✓ 双设备连接已开启" : L"✓ 双设备连接已关闭")
                              : L"⚠ 双设备连接设置未响应，请重试", !ok);
            });
            return;
        }
        if (cmd == ID_TRAY_LOW_LATENCY) {
            bool next = !session_.snapshot().low_latency;
            worker_.post([this, next] {
                bool ok = session_.set_low_latency(next);
                show_toast(ok ? (next ? L"✓ 低延迟游戏模式已开启" : L"✓ 低延迟游戏模式已关闭")
                              : L"⚠ 低延迟模式设置未响应", !ok);
            });
            return;
        }
        if (cmd == ID_TRAY_PERS_ANC) {
            bool next = !session_.snapshot().personalized_anc;
            worker_.post([this, next] {
                bool ok = session_.set_personalized_anc(next);
                show_toast(ok ? (next ? L"✓ 个性化降噪已开启" : L"✓ 个性化降噪已关闭")
                              : L"⚠ 个性化降噪设置未响应", !ok);
            });
            return;
        }
        if (cmd == ID_TRAY_ADAPT_VOL) {
            bool next = !session_.snapshot().adaptive_volume;
            worker_.post([this, next] {
                bool ok = session_.set_adaptive_volume(next);
                show_toast(ok ? (next ? L"✓ 自适应音量已开启" : L"✓ 自适应音量已关闭")
                              : L"⚠ 自适应音量设置未响应", !ok);
            });
            return;
        }
        if (cmd == ID_TRAY_FIND_BOTH)  { ring_pulse_rem_ = 4.0f; wake_anim(); worker_.post([this] { session_.find_device(true, 3); show_toast(L"🔊 已发送双耳响铃指令", false); }); }
        if (cmd == ID_TRAY_FIND_LEFT)  { ring_pulse_rem_ = 4.0f; wake_anim(); worker_.post([this] { session_.find_device(true, 1); show_toast(L"🔉 已发送左耳响铃指令", false); }); }
        if (cmd == ID_TRAY_FIND_RIGHT) { ring_pulse_rem_ = 4.0f; wake_anim(); worker_.post([this] { session_.find_device(true, 2); show_toast(L"🔉 已发送右耳响铃指令", false); }); }
        if (cmd == ID_TRAY_FIND_STOP)  { ring_pulse_rem_ = 0.0f; wake_anim(); worker_.post([this] { session_.find_device(false, 3); show_toast(L"🔇 已停止查找响铃", false); }); }
    }

    void toggle_theme() {
        cfg_.dark_mode = !cfg_.dark_mode;
        cfg_.save();
        apply_titlebar_theme();
        wake_anim();
        invalidate();
    }

    void restore_window() {
        ShowWindow(hwnd_, SW_SHOW);
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        wake_anim();
        invalidate();
    }

    void hide_to_tray() {
        stop_anim_timer();
        hot_id_ = HIT_NONE;
        hover_map_.clear();
        ShowWindow(hwnd_, SW_HIDE);
    }

    void really_quit() {
        stop_anim_timer();
        quitting_ = true;
        DestroyWindow(hwnd_);
    }

    // ---------------- Async Actions ----------------
    void show_toast(const std::wstring& msg, bool is_warn) {
        {
            std::lock_guard<std::mutex> lk(data_mtx_);
            toast_msg_ = msg;
            toast_warn_ = is_warn;
        }
        if (hwnd_) PostMessageW(hwnd_, WM_APP_TOAST, 0, 0);
    }

    void set_busy(bool b, const std::wstring& label = {}) {
        busy_ = b;
        {
            std::lock_guard<std::mutex> lk(data_mtx_);
            busy_text_ = label;
        }
        if (hwnd_) PostMessageW(hwnd_, WM_APP_BUSY, 0, 0);
    }

    void refresh_devices_async(bool auto_conn_after = false) {
        worker_.post([this, auto_conn_after] {
            auto list = enumerate_paired_devices();
            std::string pick_mac;
            {
                std::lock_guard<std::mutex> lk(data_mtx_);
                devices_ = list;
                for (const auto& d : list) {
                    if (d.system_connected && d.is_earphone_hint) {
                        pick_mac = d.mac;
                        break;
                    }
                }
            }
            if (hwnd_) PostMessageW(hwnd_, WM_APP_DEVICES, 0, 0);
            if (auto_conn_after && !session_.is_connected()) {
                std::string target = !pick_mac.empty() ? pick_mac : cfg_.last_mac;
                if (!target.empty()) {
                    connect_worker_impl(target, true);
                }
            }
        });
    }

    void connect_async(const std::string& mac, bool silent_fail) {
        manual_disconnected_ = false;
        cfg_.last_mac = mac;
        cfg_.save();
        worker_.post([this, mac, silent_fail] {
            connect_worker_impl(mac, silent_fail);
        });
    }

    void connect_worker_impl(const std::string& mac, bool silent_fail) {
        if (busy_.exchange(true)) return;
        {
            std::lock_guard<std::mutex> lk(data_mtx_);
            busy_text_ = L"正在连接…";
        }
        if (hwnd_) PostMessageW(hwnd_, WM_APP_BUSY, 0, 0);

        int wsa_err = 0;
        bool ok = session_.connect_and_auth(mac, silent_fail ? 4000 : 6500, &wsa_err);
        set_busy(false);

        if (ok) {
            eq_dirty_ = false;
            auto s = session_.snapshot();
            show_toast(L"✓ 已连接 " + utf8_to_wide(s.name.empty() ? s.mac : s.name), false);
        } else if (!silent_fail) {
            if (is_xiaomi_manager_running()) {
                show_toast(L"⚠ 连接被占用：请先彻底退出「小米电脑管家」后再试", true);
            } else {
                show_toast(L"⚠ 连接失败 (WSA=" + std::to_wstring(wsa_err) + L")：请确认耳机已开盖或播放音频唤醒", true);
            }
        }
    }

    void disconnect_async() {
        manual_disconnected_ = true;
        worker_.post([this] {
            session_.close();
            {
                std::lock_guard<std::mutex> lk(data_mtx_);
                snap_ = EarphoneSnapshot{};
            }
            show_toast(L"已断开耳机连接", false);
            if (hwnd_) PostMessageW(hwnd_, WM_APP_STATE, 0, 0);
        });
    }

    void set_anc_async(uint8_t mode, uint8_t level, bool keep_auto_noise) {
        if (!session_.is_connected()) {
            show_toast(L"请先连接耳机", true);
            return;
        }
        worker_.post([this, mode, level, keep_auto_noise] {
            if (session_.set_anc(mode, level, keep_auto_noise)) {
                auto cur = session_.snapshot();
                std::wstring msg = std::wstring(L"✓ 噪声控制：") + anc_mode_name(cur.anc_mode);
                if (cur.anc_mode != ANC_MODE_CLOSE) {
                    msg += std::wstring(L" · ") + anc_level_name(cur.anc_mode, cur.anc_level, cur.auto_noise);
                }
                show_toast(msg, false);
            } else {
                show_toast(L"⚠ 切换未收到确认，请重试", true);
            }
        });
    }

    void toggle_auto_noise_async(bool enable) {
        if (!session_.is_connected()) {
            show_toast(L"请先连接耳机", true);
            return;
        }
        worker_.post([this, enable] {
            if (session_.set_auto_noise(enable)) {
                auto cur = session_.snapshot();
                if (enable) {
                    show_toast(L"✓ 已开启「自适应降噪」(耳机将随环境噪声自动调节降噪深度)", false);
                } else {
                    show_toast(std::wstring(L"✓ 已切换回手动降噪档位：") + anc_level_name(ANC_MODE_NOISE, cur.anc_level, false), false);
                }
            } else {
                show_toast(L"⚠ 自适应降噪切换未响应，请重试", true);
            }
        });
    }

    // ---------------- Direct2D Resources & Modern Drawing Primitives ----------------
    void ensure_rt() {
        if (rt_) return;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        D2D1_SIZE_U sz = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
        D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties();
        UINT dpi = GetDpiForWindow(hwnd_);
        rtp.dpiX = static_cast<float>(dpi);
        rtp.dpiY = static_cast<float>(dpi);
        d2d_factory_->CreateHwndRenderTarget(rtp, D2D1::HwndRenderTargetProperties(hwnd_, sz), &rt_);
        if (rt_) {
            rt_->CreateSolidColorBrush(rgb(0x000000), &brush_);
        }
    }

    void discard_rt() {
        if (brush_) { brush_->Release(); brush_ = nullptr; }
        if (rt_) { rt_->Release(); rt_ = nullptr; }
    }

    void invalidate() {
        if (hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void fill_round(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F col) {
        brush_->SetColor(col);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_);
    }

    void stroke_round(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F col, float width = 1.0f) {
        brush_->SetColor(col);
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_, width);
    }

    void draw_soft_shadow(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F base_col, float elevation = 1.0f) {
        if (elevation <= 0.01f) return;
        static constexpr float kDy[3]     = {2.0f, 5.0f, 9.0f};
        static constexpr float kExpand[3] = {1.0f, 3.0f, 6.0f};
        static constexpr float kMul[3]    = {0.65f, 0.42f, 0.22f};
        for (int i = 0; i < 3; ++i) {
            float ex = kExpand[i] * elevation;
            float dy = kDy[i] * elevation;
            D2D1_RECT_F sr = D2D1::RectF(r.left - ex, r.top - ex * 0.5f + dy,
                                          r.right + ex, r.bottom + ex + dy);
            fill_round(sr, radius + ex * 0.6f, with_alpha(base_col, kMul[i]));
        }
    }

    void fill_grad_round(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c0, D2D1_COLOR_F c1, bool diagonal = true) {
        D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, c0},
            {1.0f, c1},
        };
        ID2D1GradientStopCollection* gsc = nullptr;
        if (FAILED(rt_->CreateGradientStopCollection(stops, 2, &gsc)) || !gsc) {
            fill_round(r, radius, c0);
            return;
        }
        ID2D1LinearGradientBrush* lgb = nullptr;
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props = D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(r.left, r.top),
            diagonal ? D2D1::Point2F(r.right, r.bottom) : D2D1::Point2F(r.left, r.bottom));
        if (SUCCEEDED(rt_->CreateLinearGradientBrush(props, gsc, &lgb)) && lgb) {
            rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), lgb);
            lgb->Release();
        } else {
            fill_round(r, radius, c0);
        }
        gsc->Release();
    }

    void fill_radial_orb(D2D1_POINT_2F center, float rx, float ry, D2D1_COLOR_F inner, D2D1_COLOR_F outer) {
        D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, inner},
            {1.0f, outer},
        };
        ID2D1GradientStopCollection* gsc = nullptr;
        if (FAILED(rt_->CreateGradientStopCollection(stops, 2, &gsc)) || !gsc) return;
        ID2D1RadialGradientBrush* rgb_b = nullptr;
        D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES props = D2D1::RadialGradientBrushProperties(
            center, D2D1::Point2F(0.0f, 0.0f), rx, ry);
        if (SUCCEEDED(rt_->CreateRadialGradientBrush(props, gsc, &rgb_b)) && rgb_b) {
            rt_->FillEllipse(D2D1::Ellipse(center, rx, ry), rgb_b);
            rgb_b->Release();
        }
        gsc->Release();
    }

    void draw_card_surface(const D2D1_RECT_F& r, float radius, const Theme& th, float hover_t = 0.0f) {
        draw_soft_shadow(r, radius, th.shadow, 1.0f + hover_t * 0.45f);
        fill_round(r, radius, th.card);
        // Subtle top inner specular rim for depth
        D2D1_RECT_F specR = D2D1::RectF(r.left + radius * 0.4f, r.top + 0.8f, r.right - radius * 0.4f, r.top + 1.8f);
        brush_->SetColor(th.specular);
        rt_->DrawLine(D2D1::Point2F(specR.left, specR.top), D2D1::Point2F(specR.right, specR.top), brush_, 1.0f);
        stroke_round(r, radius, lerp_col(th.border, th.border_hot, hover_t), 1.0f);
    }

    void draw_arc_ring(float cx, float cy, float radius, float thickness,
                       float start_deg, float sweep_deg, D2D1_COLOR_F col) {
        if (sweep_deg < 0.6f) return;
        sweep_deg = std::min(sweep_deg, 359.5f);
        float a0 = start_deg * (kPi / 180.0f);
        float a1 = (start_deg + sweep_deg) * (kPi / 180.0f);
        D2D1_POINT_2F p0 = D2D1::Point2F(cx + radius * std::cos(a0), cy + radius * std::sin(a0));
        D2D1_POINT_2F p1 = D2D1::Point2F(cx + radius * std::cos(a1), cy + radius * std::sin(a1));

        ID2D1PathGeometry* path = nullptr;
        if (FAILED(d2d_factory_->CreatePathGeometry(&path)) || !path) return;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(path->Open(&sink)) && sink) {
            sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(
                p1,
                D2D1::SizeF(radius, radius),
                0.0f,
                D2D1_SWEEP_DIRECTION_CLOCKWISE,
                sweep_deg >= 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            brush_->SetColor(col);
            rt_->DrawGeometry(path, brush_, thickness, stroke_round_);
        }
        path->Release();
    }

    void draw_text(const std::wstring& txt, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                   D2D1_COLOR_F col,
                   DWRITE_TEXT_ALIGNMENT ha = DWRITE_TEXT_ALIGNMENT_LEADING,
                   DWRITE_PARAGRAPH_ALIGNMENT va = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
        if (!fmt || txt.empty()) return;
        fmt->SetTextAlignment(ha);
        fmt->SetParagraphAlignment(va);
        brush_->SetColor(col);
        rt_->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush_,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }

    void draw_refresh_glyph(float cx, float cy, float r, float deg, D2D1_COLOR_F col) {
        draw_arc_ring(cx, cy, r, 1.8f, deg - 65.0f, 275.0f, col);
        float tip_rad = (deg + 210.0f) * (kPi / 180.0f);
        float tx = cx + r * std::cos(tip_rad);
        float ty = cy + r * std::sin(tip_rad);
        brush_->SetColor(col);
        float tang = tip_rad - kPi * 0.5f;
        D2D1_POINT_2F pA = D2D1::Point2F(tx + 3.2f * std::cos(tang - 0.55f), ty + 3.2f * std::sin(tang - 0.55f));
        D2D1_POINT_2F pB = D2D1::Point2F(tx + 3.2f * std::cos(tang + 1.85f), ty + 3.2f * std::sin(tang + 1.85f));
        rt_->DrawLine(D2D1::Point2F(tx, ty), pA, brush_, 1.7f, stroke_round_);
        rt_->DrawLine(D2D1::Point2F(tx, ty), pB, brush_, 1.7f, stroke_round_);
    }

    void register_hit(const D2D1_RECT_F& r, int id) {
        hits_.push_back(HitZone{r, id});
    }

    // Fluid spring switch with liquid capsule stretch during motion
    void draw_switch(float right_x, float center_y, int sw_idx, float target_on,
                     bool enabled, int hit_id, const Theme& th, bool register_own_hit = true) {
        float pos = (sw_idx >= 0 && sw_idx < 11) ? sw_anim_[sw_idx] : target_on;
        float hot_t = hover_val(hit_id);

        D2D1_RECT_F r = D2D1::RectF(right_x - 44.0f, center_y - 12.0f, right_x, center_y + 12.0f);
        D2D1_COLOR_F off_col = lerp_col(th.sw_off, th.sw_off_hot, enabled ? hot_t : 0.0f);
        D2D1_COLOR_F on_col  = lerp_col(rgb(0xFF6900), rgb(0xFF8124), hot_t);
        D2D1_COLOR_F track   = lerp_col(off_col, on_col, enabled ? pos : pos * 0.45f);
        if (!enabled) track = with_alpha(track, 0.55f);

        fill_round(r, 12.0f, track);

        // Liquid capsule thumb stretch proportional to remaining travel distance
        float vel = std::fabs(target_on - pos);
        float thumb_w = 17.6f + vel * 9.0f;
        float min_cx = (right_x - 44.0f) + 3.2f + thumb_w * 0.5f;
        float max_cx = right_x - 3.2f - thumb_w * 0.5f;
        float cx = lerp(min_cx, max_cx, pos);

        D2D1_RECT_F knobR = D2D1::RectF(cx - thumb_w * 0.5f, center_y - 8.8f,
                                        cx + thumb_w * 0.5f, center_y + 8.8f);
        // Subtle knob shadow
        D2D1_RECT_F shR = D2D1::RectF(knobR.left, knobR.top + 1.2f, knobR.right, knobR.bottom + 1.4f);
        fill_round(shR, 8.8f, rgb(0x000000, 0.16f));
        fill_round(knobR, 8.8f, enabled ? rgb(0xFFFFFF) : rgb(0xFFFFFF, 0.8f));

        if (register_own_hit) {
            register_hit(r, hit_id);
        }
    }

    void draw_switch_card(const D2D1_RECT_F& cr, const wchar_t* title, const wchar_t* sub,
                          int sw_idx, bool on, bool enabled, int hit_id, const Theme& th,
                          const wchar_t* icon_glyph = nullptr, D2D1_COLOR_F icon_col = rgb(0xFF6900)) {
        float hot_t = enabled ? hover_val(hit_id) : 0.0f;
        float prs_t = enabled ? press_val(hit_id) : 0.0f;
        float lift = -2.0f * hot_t + 1.4f * prs_t;

        D2D1_RECT_F dr = D2D1::RectF(cr.left, cr.top + lift, cr.right, cr.bottom + lift);
        if (hot_t > 0.02f || on) {
            draw_soft_shadow(dr, 12.0f, with_alpha(th.shadow, 0.4f + 0.5f * hot_t), 0.7f + 0.4f * hot_t);
        }

        float pos = (sw_idx >= 0 && sw_idx < 11) ? sw_anim_[sw_idx] : (on ? 1.0f : 0.0f);
        D2D1_COLOR_F active_tint = lerp_col(th.subcard, lerp_col(th.subcard, with_alpha(icon_col, 1.0f), th.dark_t > 0.5f ? 0.14f : 0.07f), pos);
        D2D1_COLOR_F bg = lerp_col(active_tint, lerp_col(active_tint, th.card, 0.55f), hot_t);
        D2D1_COLOR_F bd = lerp_col(th.border, with_alpha(icon_col, 0.55f), std::max(hot_t * 0.7f, pos * 0.8f));

        fill_round(dr, 12.0f, bg);
        stroke_round(dr, 12.0f, bd, 1.0f + 0.35f * pos);

        float text_left = dr.left + 16.0f;
        float cy_mid = (dr.top + dr.bottom) * 0.5f;

        if (icon_glyph) {
            D2D1_RECT_F ib = D2D1::RectF(dr.left + 14.0f, cy_mid - 19.0f, dr.left + 52.0f, cy_mid + 19.0f);
            D2D1_COLOR_F ib_bg = lerp_col(with_alpha(icon_col, th.dark_t > 0.5f ? 0.16f : 0.10f),
                                          with_alpha(icon_col, th.dark_t > 0.5f ? 0.28f : 0.18f), pos);
            fill_round(ib, 10.0f, ib_bg);
            draw_text(icon_glyph, tf_h2_, ib, lerp_col(th.text_sec, icon_col, 0.45f + 0.55f * pos),
                      DWRITE_TEXT_ALIGNMENT_CENTER);
            text_left = dr.left + 64.0f;
        }

        if (sub && sub[0] != L'\0') {
            draw_text(title, tf_h2_, D2D1::RectF(text_left, dr.top + 10.0f, dr.right - 68.0f, dr.top + 33.0f), th.text_pri);
            draw_text(sub, tf_sub_, D2D1::RectF(text_left, dr.top + 33.0f, dr.right - 68.0f, dr.bottom - 8.0f), th.text_mut);
        } else {
            draw_text(title, tf_body_, D2D1::RectF(text_left, dr.top, dr.right - 68.0f, dr.bottom), th.text_pri);
        }

        draw_switch(dr.right - 16.0f, cy_mid, sw_idx, on ? 1.0f : 0.0f, enabled, hit_id, th, false);
        register_hit(cr, hit_id);
    }

    void draw_button(const D2D1_RECT_F& r, const std::wstring& label, int hit_id, const Theme& th,
                     bool primary = false, bool active = false, uint32_t active_hex = 0xFF6900,
                     uint32_t active_hex2 = 0) {
        float hot_t = hover_val(hit_id);
        float prs_t = press_val(hit_id);
        float lift = (primary || active) ? (-1.5f * hot_t + 1.2f * prs_t) : (-1.0f * hot_t + 1.0f * prs_t);
        D2D1_RECT_F dr = D2D1::RectF(r.left, r.top + lift, r.right, r.bottom + lift);
        float rad = std::min(10.0f, (r.bottom - r.top) * 0.35f);

        if (primary) {
            D2D1_COLOR_F c0 = lerp_col(rgb(0xFF6900), rgb(0xFF7B1C), hot_t);
            D2D1_COLOR_F c1 = lerp_col(rgb(0xFF3D00), rgb(0xFF5514), hot_t);
            draw_soft_shadow(dr, rad, rgb(0xFF5500, 0.16f + 0.14f * hot_t), 0.7f + 0.3f * hot_t);
            fill_grad_round(dr, rad, c0, c1, true);
            draw_text(label, tf_body_, dr, rgb(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else if (active) {
            D2D1_COLOR_F c0 = rgb(active_hex);
            D2D1_COLOR_F c1 = active_hex2 ? rgb(active_hex2) : lerp_col(c0, rgb(0x0F172A), 0.12f);
            draw_soft_shadow(dr, rad, with_alpha(c0, 0.22f + 0.10f * hot_t), 0.65f + 0.25f * hot_t);
            fill_grad_round(dr, rad, c0, c1, true);
            draw_text(label, tf_body_, dr, rgb(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            D2D1_COLOR_F bg = lerp_col(th.btn_bg, th.btn_hot, hot_t);
            D2D1_COLOR_F bd = lerp_col(th.border, th.border_hot, hot_t);
            fill_round(dr, rad, bg);
            stroke_round(dr, rad, bd);
            draw_text(label, tf_body_, dr, lerp_col(th.text_sec, th.text_pri, hot_t), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        register_hit(r, hit_id);
    }

    // ---------------- Painting ----------------
    void on_paint() {
        ensure_rt();
        if (!rt_) return;

        Theme th = Theme::build(theme_t_);

        EarphoneSnapshot s;
        std::vector<PairedBtDevice> devs;
        std::deque<std::wstring> logs_copy;
        std::vector<EqBand> eq_bands;
        std::wstring toast;
        bool toast_warn = false;
        std::wstring busy_lbl;
        {
            std::lock_guard<std::mutex> lk(data_mtx_);
            s = snap_;
            devs = devices_;
            logs_copy = logs_;
            if (ui_eq_bands_.size() < 10) ui_eq_bands_ = default_10band_eq();
            eq_bands = ui_eq_bands_;
            toast = toast_msg_;
            toast_warn = toast_warn_;
            busy_lbl = busy_text_;
        }

        hits_.clear();
        eq_track_rects_.clear();

        D2D1_SIZE_F sz = rt_->GetSize();
        float W = sz.width;
        float H = sz.height;

        rt_->BeginDraw();
        rt_->Clear(th.canvas);

        // ================= LEFT SIDEBAR (0..272) =================
        const float sideW = 272.0f;
        brush_->SetColor(th.sidebar);
        rt_->FillRectangle(D2D1::RectF(0, 0, sideW, H), brush_);
        brush_->SetColor(th.border);
        rt_->DrawLine(D2D1::Point2F(sideW, 0), D2D1::Point2F(sideW, H), brush_, 1.0f);

        // Brand Header + Theme Toggle Pill
        D2D1_RECT_F logoR = D2D1::RectF(18, 18, 54, 54);
        draw_soft_shadow(logoR, 11.0f, rgb(0xFF5500, 0.22f), 0.7f);
        fill_grad_round(logoR, 11.0f, rgb(0xFF7B1C), rgb(0xFF3D00), true);
        draw_text(L"🎧", tf_h1_, logoR, rgb(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);

        draw_text(L"小米耳机 · PC", tf_brand_, D2D1::RectF(64, 16, sideW - 56, 38), th.text_pri);

        // Animated breathing status indicator
        float dot_cx = 69.0f;
        float dot_cy = 48.0f;
        if (s.connected) {
            float pulse = 0.5f + 0.5f * std::sin(ambient_phase_ * 2.0f);
            brush_->SetColor(rgb(0x10B981, 0.16f + 0.16f * pulse));
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dot_cx, dot_cy), 6.5f + 2.0f * pulse, 6.5f + 2.0f * pulse), brush_);
            brush_->SetColor(rgb(0x10B981));
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dot_cx, dot_cy), 3.6f, 3.6f), brush_);
        } else {
            D2D1_COLOR_F dc = busy_ ? rgb(0xF59E0B) : th.text_mut;
            brush_->SetColor(dc);
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dot_cx, dot_cy), 3.5f, 3.5f), brush_);
        }
        std::wstring conn_badge = busy_ ? busy_lbl : (s.connected ? L"已连接 · RCSP 已认证" : L"未连接耳机");
        D2D1_COLOR_F badge_col = busy_ ? rgb(0xF59E0B) : s.connected ? rgb(0x10B981) : th.text_mut;
        draw_text(conn_badge, tf_sub_, D2D1::RectF(79, 39, sideW - 54, 57), badge_col);

        // Light/Dark Theme Toggle Capsule in top-right of sidebar header
        {
            D2D1_RECT_F thR = D2D1::RectF(sideW - 48, 20, sideW - 14, 48);
            float h_th = hover_val(HIT_THEME_TOGGLE);
            fill_round(thR, 9.0f, lerp_col(th.btn_bg, th.btn_hot, h_th));
            stroke_round(thR, 9.0f, lerp_col(th.border, th.border_hot, h_th));
            draw_text(cfg_.dark_mode ? L"☀" : L"🌙", tf_body_, thR,
                      cfg_.dark_mode ? rgb(0xFBBF24) : th.text_sec, DWRITE_TEXT_ALIGNMENT_CENTER);
            register_hit(thR, HIT_THEME_TOGGLE);
        }

        // Paired Bluetooth Devices Section
        draw_text(L"已配对蓝牙设备", tf_h2_, D2D1::RectF(18, 70, 178, 94), th.text_sec);
        draw_button(D2D1::RectF(sideW - 78, 68, sideW - 16, 94), L"↻ 刷新", HIT_REFRESH_DEVS, th);

        float dy = 102.0f;
        const float devListBottom = 318.0f;
        if (devs.empty()) {
            fill_round(D2D1::RectF(16, dy, sideW - 16, dy + 68), 10.0f, th.subcard);
            stroke_round(D2D1::RectF(16, dy, sideW - 16, dy + 68), 10.0f, th.border);
            draw_text(L"未发现已配对蓝牙设备\n可在下方直接输入 MAC", tf_sub_,
                      D2D1::RectF(26, dy + 8, sideW - 26, dy + 60), th.text_mut);
        } else {
            size_t max_show = std::min<size_t>(devs.size(), 5);
            for (size_t i = 0; i < max_show && dy + 41.0f <= devListBottom; ++i) {
                const auto& d = devs[i];
                bool selected = (_stricmp(d.mac.c_str(), mac_input_.c_str()) == 0);
                int hid = HIT_DEV_ITEM_BASE + static_cast<int>(i);
                float h_dev = hover_val(hid);
                float lift = -1.2f * h_dev;

                D2D1_RECT_F itemHit = D2D1::RectF(14, dy, sideW - 14, dy + 40.0f);
                D2D1_RECT_F itemR = D2D1::RectF(14, dy + lift, sideW - 14, dy + 40.0f + lift);
                D2D1_COLOR_F bg = selected
                    ? lerp_col(rgb(0xFFF3EB), rgb(0x2D1B14), th.dark_t)
                    : lerp_col(th.subcard, th.btn_hot, h_dev);
                fill_round(itemR, 9.0f, bg);
                stroke_round(itemR, 9.0f, selected ? rgb(0xFF6900, 0.75f) : lerp_col(th.border, th.border_hot, h_dev),
                             selected ? 1.25f : 1.0f);
                if (selected) {
                    fill_round(D2D1::RectF(itemR.left + 4, itemR.top + 8, itemR.left + 7.5f, itemR.bottom - 8),
                               2.0f, rgb(0xFF6900));
                }

                std::wstring dname = utf8_to_wide(d.name);
                if (d.is_earphone_hint) dname += L" ⭐";
                draw_text(dname, tf_body_, D2D1::RectF(itemR.left + 13, itemR.top + 3, itemR.right - 10, itemR.top + 22),
                          selected ? lerp_col(rgb(0xC2410C), rgb(0xFF8C42), th.dark_t) : th.text_pri);

                std::wstring sub = utf8_to_wide(d.mac) + (d.system_connected ? L"  ● 在线" : L"");
                draw_text(sub, tf_sub_, D2D1::RectF(itemR.left + 13, itemR.top + 21, itemR.right - 10, itemR.top + 38),
                          d.system_connected ? rgb(0x10B981) : th.text_mut);
                register_hit(itemHit, hid);
                dy += 44.0f;
            }
        }

        // MAC Input & Connect CTA
        draw_text(L"目标耳机 MAC 地址 (点击编辑 / Ctrl+V)", tf_sub_,
                  D2D1::RectF(18, 326, sideW - 16, 344), th.text_mut);
        D2D1_RECT_F macR = D2D1::RectF(16, 346, sideW - 16, 378);
        fill_round(macR, 9.0f, mac_focused_ ? th.card : th.subcard);
        if (mac_focused_) {
            stroke_round(D2D1::RectF(macR.left - 1.5f, macR.top - 1.5f, macR.right + 1.5f, macR.bottom + 1.5f),
                         10.0f, rgb(0xFF6900, 0.25f), 2.5f);
        }
        stroke_round(macR, 9.0f, mac_focused_ ? rgb(0xFF6900) : th.border, mac_focused_ ? 1.5f : 1.0f);

        bool show_caret = mac_focused_ && (0.5f + 0.5f * std::sin(ambient_phase_ * 3.0f) > 0.35f);
        std::wstring mac_disp = utf8_to_wide(mac_input_) + (show_caret ? L"|" : L"");
        draw_text(mac_disp, tf_mono_, D2D1::RectF(26, 346, sideW - 24, 378), th.text_pri);
        register_hit(macR, HIT_MAC_BOX);

        D2D1_RECT_F connR = D2D1::RectF(16, 388, sideW - 16, 428);
        if (busy_) {
            float shim = 0.5f + 0.5f * std::sin(ambient_phase_ * 2.5f);
            fill_grad_round(connR, 10.0f, lerp_col(rgb(0xFF6900, 0.25f), rgb(0xFF6900, 0.45f), shim),
                            th.subcard, true);
            stroke_round(connR, 10.0f, rgb(0xFF6900, 0.65f));
            draw_text(busy_lbl.empty() ? L"正在建立蓝牙连接…" : busy_lbl, tf_h2_, connR, th.text_pri, DWRITE_TEXT_ALIGNMENT_CENTER);
        } else if (s.connected) {
            draw_button(connR, L"断开耳机连接", HIT_CONNECT_BTN, th, false, true, 0x475569, 0x334155);
        } else {
            draw_button(connR, L"连接耳机", HIT_CONNECT_BTN, th, true);
        }

        // App & Tray Preferences
        brush_->SetColor(th.border);
        rt_->DrawLine(D2D1::Point2F(16, 442), D2D1::Point2F(sideW - 16, 442), brush_, 1.0f);
        draw_text(L"常驻与自动化设置", tf_h2_, D2D1::RectF(18, 450, sideW - 16, 472), th.text_sec);

        struct SideOpt { const wchar_t* label; bool val; int id; int sw_idx; };
        SideOpt opts[] = {
            {L"关闭按钮最小化到托盘",     cfg_.close_to_tray,  HIT_OPT_CLOSE_TRAY,  0},
            {L"启动时自动连接耳机",       cfg_.auto_connect,   HIT_OPT_AUTO_CONN,   1},
            {L"耳机唤醒后台自动重连",     cfg_.auto_reconnect, HIT_OPT_AUTO_RECONN, 2},
            {L"开机自启 (静默驻留托盘)", autostart_cached_,   HIT_OPT_AUTOSTART,   3},
        };
        float oy = 478.0f;
        for (const auto& op : opts) {
            float h_op = hover_val(op.id);
            D2D1_RECT_F rowR = D2D1::RectF(14, oy, sideW - 14, oy + 31.0f);
            if (h_op > 0.02f) fill_round(rowR, 8.0f, with_alpha(th.subcard, 0.85f * h_op));
            draw_text(op.label, tf_sub_, D2D1::RectF(18, oy, sideW - 66, oy + 30), th.text_sec);
            draw_switch(sideW - 16, oy + 15.5f, op.sw_idx, op.val ? 1.0f : 0.0f, true, op.id, th, false);
            register_hit(rowR, op.id);
            oy += 34.0f;
        }

        // Bottom Tray/Quit Buttons
        float botY = H - 48.0f;
        draw_button(D2D1::RectF(16, botY, 144, botY + 33), L"最小化到托盘", HIT_HIDE_TRAY_BTN, th);
        draw_button(D2D1::RectF(152, botY, sideW - 16, botY + 33), L"彻底退出", HIT_EXIT_APP_BTN, th);

        // ================= RIGHT MAIN STAGE =================
        float mainX = sideW + 20.0f;
        float mainR = W - 20.0f;
        float mainW = mainR - mainX;

        // Fixed Hero Showcase Card (never jumps vertically for toasts!)
        float heroTop = 18.0f;
        D2D1_RECT_F heroR = D2D1::RectF(mainX, heroTop, mainR, heroTop + 128.0f);
        draw_card_surface(heroR, 16.0f, th);

        // Ambient radial aura orb on Hero card
        D2D1_COLOR_F orb_col = rgb(0xFF6900);
        if (s.connected) {
            if (s.anc_mode == ANC_MODE_NOISE) {
                orb_col = s.auto_noise ? rgb(0x6366F1) : rgb(0x2563EB);
            } else if (s.anc_mode == ANC_MODE_TRANSPARENT) {
                orb_col = rgb(0x10B981);
            } else {
                orb_col = rgb(0xF59E0B);
            }
        }
        float orb_a = s.connected ? (th.dark_t > 0.5f ? 0.22f : 0.13f) : (th.dark_t > 0.5f ? 0.12f : 0.07f);
        rt_->PushAxisAlignedClip(heroR, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        fill_radial_orb(D2D1::Point2F(mainX + 110.0f, heroTop + 62.0f), 195.0f, 92.0f,
                        with_alpha(orb_col, orb_a), with_alpha(orb_col, 0.0f));
        rt_->PopAxisAlignedClip();

        float batRightStart = mainR - (108.0f * 3 + 12.0f * 2 + 18.0f);

        // Hero Left: Device Info & Metadata Pills
        std::wstring title = s.connected ? utf8_to_wide(s.name.empty() ? s.mac : s.name) : L"尚未连接小米耳机";
        draw_text(title, tf_h1_, D2D1::RectF(mainX + 22, heroTop + 14, batRightStart - 12, heroTop + 44), th.text_pri);

        if (s.connected) {
            std::wstring meta = L"MAC " + utf8_to_wide(s.mac);
            if (!s.fw_version.empty()) meta += L"  ·  固件 v" + utf8_to_wide(s.fw_version);
            if (s.vid && s.pid) {
                char vpc[32];
                std::snprintf(vpc, sizeof(vpc), "  ·  %04X:%04X", s.vid, s.pid);
                meta += utf8_to_wide(vpc);
            }
            draw_text(meta, tf_sub_, D2D1::RectF(mainX + 22, heroTop + 46, batRightStart - 12, heroTop + 67), th.text_mut);

            // Authentication Pill + Refresh Pill with spinning icon
            D2D1_RECT_F authPill = D2D1::RectF(mainX + 22, heroTop + 73, mainX + 152, heroTop + 98);
            D2D1_COLOR_F ap_bg = s.authenticated ? rgb(0x10B981, th.dark_t > 0.5f ? 0.18f : 0.12f)
                                                 : rgb(0xF59E0B, th.dark_t > 0.5f ? 0.18f : 0.12f);
            fill_round(authPill, 7.0f, ap_bg);
            stroke_round(authPill, 7.0f, s.authenticated ? rgb(0x10B981, 0.45f) : rgb(0xF59E0B, 0.45f));
            draw_text(s.authenticated ? L"🔐 RCSP 已认证" : L"⚠ 未完成认证", tf_sub_, authPill,
                      s.authenticated ? rgb(0x10B981) : rgb(0xF59E0B), DWRITE_TEXT_ALIGNMENT_CENTER);

            if (!s.sn.empty()) {
                draw_text(L"SN: " + utf8_to_wide(s.sn), tf_sub_,
                          D2D1::RectF(mainX + 162, heroTop + 73, batRightStart - 12, heroTop + 98), th.text_mut);
            }

            D2D1_RECT_F refBtn = D2D1::RectF(mainX + 22, heroTop + 102, mainX + 136, heroTop + 123);
            draw_button(refBtn, L"    刷新电量状态", HIT_REFRESH_STATE, th);
            draw_refresh_glyph(refBtn.left + 15.0f, (refBtn.top + refBtn.bottom) * 0.5f, 4.6f, refresh_spin_deg_, th.text_sec);
        } else {
            draw_text(L"支持分体独立电量环形表盘、自适应降噪、10段EQ曲线与双设备连接\n从左侧选择已配对耳机后点击「连接耳机」（请先退出小米电脑管家）",
                      tf_sub_, D2D1::RectF(mainX + 22, heroTop + 50, batRightStart - 12, heroTop + 112), th.text_mut);
        }

        // Hero Right: 3 Circular 270° Ring Battery Pods (L / R / Case)
        auto draw_bat_pod = [&](float bx, int idx, const wchar_t* name, const BatteryUnit& u) {
            D2D1_RECT_F br = D2D1::RectF(bx, heroTop + 14, bx + 108.0f, heroTop + 114.0f);
            fill_round(br, 12.0f, th.subcard);
            stroke_round(br, 12.0f, th.border);

            float cx = bx + 54.0f;
            float cy = heroTop + 58.0f;
            float rad = 28.0f;
            float thick = 5.5f;

            // Background 270-degree track (start 135° bottom-left clockwise to 45° bottom-right)
            draw_arc_ring(cx, cy, rad, thick, 135.0f, 270.0f, lerp_col(th.border, th.sw_off, 0.35f));

            bool is_on = s.connected && u.online();
            float anim_pct = std::clamp(bat_anim_[idx], 0.0f, 100.0f);
            D2D1_COLOR_F arc_col = (anim_pct > 50.0f) ? rgb(0x10B981) : (anim_pct >= 20.0f ? rgb(0xF59E0B) : rgb(0xEF4444));
            if (is_on && anim_pct > 0.3f) {
                float sweep = 270.0f * (anim_pct / 100.0f);
                draw_arc_ring(cx, cy, rad, thick + 3.0f, 135.0f, sweep, with_alpha(arc_col, 0.20f));
                draw_arc_ring(cx, cy, rad, thick, 135.0f, sweep, arc_col);
            }

            // Inner percentage readout
            std::wstring val_str = is_on ? (std::to_wstring(u.percent) + L"%") : (s.connected ? L"离线" : L"--");
            draw_text(val_str, is_on ? tf_bat_big_ : tf_sub_,
                      D2D1::RectF(bx + 12.0f, cy - 15.0f, bx + 96.0f, cy + 15.0f),
                      is_on ? th.text_pri : th.text_mut, DWRITE_TEXT_ALIGNMENT_CENTER);

            // Pod bottom label + pulsing charging icon
            std::wstring lbl = name;
            D2D1_COLOR_F lbl_col = th.text_sec;
            if (is_on && u.charging) {
                lbl += L" ⚡";
                float chg_p = 0.6f + 0.4f * std::sin(ambient_phase_ * 2.5f);
                lbl_col = rgb(0x10B981, chg_p);
            }
            draw_text(lbl, tf_sub_, D2D1::RectF(bx + 4.0f, heroTop + 88.0f, bx + 104.0f, heroTop + 109.0f),
                      lbl_col, DWRITE_TEXT_ALIGNMENT_CENTER);
        };

        draw_bat_pod(batRightStart,          0, L"左耳 L", s.battery.left);
        draw_bat_pod(batRightStart + 120.0f, 1, L"右耳 R", s.battery.right);
        draw_bat_pod(batRightStart + 240.0f, 2, L"充电盒", s.battery.box);

        // ================= SEGMENTED SLIDING TAB BAR =================
        float tabY = heroTop + 140.0f;
        D2D1_RECT_F trackR = D2D1::RectF(mainX, tabY, mainR, tabY + 42.0f);
        fill_round(trackR, 12.0f, th.seg_track);
        stroke_round(trackR, 12.0f, th.border);

        static const wchar_t* kTabs[4] = {
            L"🎚 降噪与日常开关",
            L"🎵 EQ 音效与均衡器",
            L"🌌 空间音频与查找",
            L"📟 协议帧实时日志"
        };
        float innerPad = 4.0f;
        float slotW = (mainW - innerPad * 2.0f) / 4.0f;

        // Spring-sliding elevated active pill
        float pillX = mainX + innerPad + tab_pill_pos_ * slotW;
        D2D1_RECT_F slidePill = D2D1::RectF(pillX + 1.0f, tabY + innerPad, pillX + slotW - 1.0f, tabY + 42.0f - innerPad);
        draw_soft_shadow(slidePill, 9.0f, th.shadow, 0.75f);
        fill_grad_round(slidePill, 9.0f,
                        th.dark_t > 0.5f ? rgb(0x25334A) : rgb(0xFFFFFF),
                        th.dark_t > 0.5f ? rgb(0x1D293D) : rgb(0xF8FAFC), false);
        stroke_round(slidePill, 9.0f, th.border);

        for (int i = 0; i < 4; ++i) {
            float tx = mainX + innerPad + i * slotW;
            D2D1_RECT_F tr = D2D1::RectF(tx, tabY + innerPad, tx + slotW, tabY + 42.0f - innerPad);
            int hid = HIT_TAB_BASE + i;
            float dist = clamp01(1.0f - std::fabs(tab_pill_pos_ - static_cast<float>(i)));
            float h_tab = hover_val(hid);
            D2D1_COLOR_F tcol = lerp_col(lerp_col(th.text_mut, th.text_pri, h_tab), th.text_pri, dist);
            draw_text(kTabs[i], tf_body_, tr, tcol, DWRITE_TEXT_ALIGNMENT_CENTER);
            register_hit(tr, hid);
        }

        // Content Stage with non-transform slide-in offset (1:1 DIP hit rects!)
        float contentY = tabY + 52.0f;
        float animY = contentY + (1.0f - page_enter_t_) * 10.0f;
        float contentBottom = H - 16.0f;

        if (active_tab_ == 0) {
            paint_tab_anc(s, th, mainX, mainR, mainW, animY, contentBottom);
        } else if (active_tab_ == 1) {
            paint_tab_eq(s, eq_bands, th, mainX, mainR, mainW, animY, contentBottom);
        } else if (active_tab_ == 2) {
            paint_tab_spatial(s, th, mainX, mainR, mainW, animY, contentBottom);
        } else {
            paint_tab_logs(logs_copy, th, mainX, mainR, animY, contentBottom);
        }

        // ================= FLOATING GLASS TOAST OVERLAY =================
        if (!toast.empty() && toast_anim_ > 0.005f) {
            float ty = -44.0f + toast_anim_ * 58.0f; // settles at y = 14.0f
            float tw = std::min(mainW - 120.0f, 520.0f);
            float tx = mainX + (mainW - tw) * 0.5f;
            D2D1_RECT_F tR = D2D1::RectF(tx, ty, tx + tw, ty + 36.0f);
            draw_soft_shadow(tR, 18.0f, with_alpha(th.shadow, toast_anim_ * 1.4f), 1.15f);
            D2D1_COLOR_F tbg = toast_warn
                ? lerp_col(rgb(0xFEF2F2, 0.96f), rgb(0x3F181D, 0.96f), th.dark_t)
                : lerp_col(rgb(0xECFDF5, 0.96f), rgb(0x102E26, 0.96f), th.dark_t);
            D2D1_COLOR_F tbd = toast_warn
                ? rgb(0xEF4444, 0.55f * toast_anim_)
                : rgb(0x10B981, 0.55f * toast_anim_);
            D2D1_COLOR_F tfg = toast_warn
                ? lerp_col(rgb(0xB91C1C), rgb(0xFCA5A5), th.dark_t)
                : lerp_col(rgb(0x047857), rgb(0x6EE7B7), th.dark_t);
            fill_round(tR, 18.0f, with_alpha(tbg, toast_anim_));
            stroke_round(tR, 18.0f, tbd, 1.15f);
            draw_text(toast, tf_sub_, D2D1::RectF(tR.left + 16, tR.top, tR.right - 16, tR.bottom),
                      with_alpha(tfg, toast_anim_), DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        HRESULT hr = rt_->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            discard_rt();
        }
    }

    // ---------------- Tab 0: ANC + Sine Wave Visualizer + Core Switches ----------------
    void draw_anc_wave(const D2D1_RECT_F& mr, bool is_trans, bool adaptive) {
        D2D1_RECT_F clipR = D2D1::RectF(mr.left + 10.0f, mr.bottom - 24.0f, mr.right - 10.0f, mr.bottom - 4.0f);
        float w = clipR.right - clipR.left;
        float mid_y = (clipR.top + clipR.bottom) * 0.5f;
        if (w < 20.0f) return;

        auto draw_sine = [&](float amp, float freq, float phase_mul, D2D1_COLOR_F col, float thick) {
            ID2D1PathGeometry* pg = nullptr;
            if (FAILED(d2d_factory_->CreatePathGeometry(&pg)) || !pg) return;
            ID2D1GeometrySink* sink = nullptr;
            if (SUCCEEDED(pg->Open(&sink)) && sink) {
                constexpr int N = 28;
                for (int i = 0; i <= N; ++i) {
                    float u = static_cast<float>(i) / static_cast<float>(N);
                    float env = std::sin(u * kPi); // smooth taper at edges
                    float x = clipR.left + u * w;
                    float y = mid_y + std::sin(u * freq * 2.0f * kPi + ambient_phase_ * phase_mul) * amp * env;
                    D2D1_POINT_2F pt = D2D1::Point2F(x, y);
                    if (i == 0) sink->BeginFigure(pt, D2D1_FIGURE_BEGIN_HOLLOW);
                    else sink->AddLine(pt);
                }
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                sink->Release();
                brush_->SetColor(col);
                rt_->DrawGeometry(pg, brush_, thick, stroke_round_);
            }
            pg->Release();
        };

        if (is_trans) {
            draw_sine(5.2f, 2.2f, 1.3f, rgb(0xFFFFFF, 0.36f), 1.8f);
            draw_sine(3.4f, 3.6f, -1.7f, rgb(0xFFFFFF, 0.22f), 1.3f);
        } else {
            float fmod = adaptive ? (2.6f + 0.8f * std::sin(ambient_phase_ * 0.6f)) : 2.8f;
            draw_sine(5.4f, fmod, 1.5f, rgb(0xFFFFFF, 0.40f), 1.8f);
            draw_sine(3.6f, fmod * 1.45f, -1.2f, rgb(0xFFFFFF, 0.22f), 1.2f);
            draw_sine(2.2f, fmod * 2.1f, 2.1f, rgb(0xFFFFFF, 0.14f), 1.0f);
        }
    }

    void paint_tab_anc(const EarphoneSnapshot& s, const Theme& th, float x, float rx, float w, float y, float bot) {
        // Card 1: ANC Mode + Sub-levels + Adaptive ANC
        D2D1_RECT_F c1 = D2D1::RectF(x, y, rx, y + 206.0f);
        draw_card_surface(c1, 16.0f, th);

        draw_text(L"主动降噪与环境音控制", tf_h2_, D2D1::RectF(x + 20, y + 12, x + 300, y + 36), th.text_pri);
        std::wstring cur_anc = s.connected
            ? (std::wstring(L"当前状态：") + anc_mode_name(s.anc_mode) +
               (s.anc_mode == ANC_MODE_CLOSE ? L"" : (std::wstring(L" · ") + anc_level_name(s.anc_mode, s.anc_level, s.auto_noise))))
            : L"连接耳机后即可实时切换并同步状态";
        D2D1_COLOR_F st_col = !s.connected ? th.text_mut
                            : (s.anc_mode == ANC_MODE_NOISE) ? (s.auto_noise ? rgb(0x6366F1) : rgb(0x3B82F6))
                            : (s.anc_mode == ANC_MODE_TRANSPARENT) ? rgb(0x10B981)
                            : th.text_sec;
        draw_text(cur_anc, tf_sub_, D2D1::RectF(rx - 360, y + 12, rx - 20, y + 36),
                  st_col, DWRITE_TEXT_ALIGNMENT_TRAILING);

        struct ModeCard { uint8_t m; const wchar_t* title; const wchar_t* desc; uint32_t c0; uint32_t c1; };
        ModeCard mcards[3] = {
            {ANC_MODE_CLOSE,       L"关闭",   L"关闭主动降噪与通透",   0x64748B, 0x475569},
            {ANC_MODE_NOISE,       L"降噪",   L"主动消除低频与环境噪声", 0x3B82F6, 0x1D4ED8},
            {ANC_MODE_TRANSPARENT, L"通透",   L"自然听清外界交谈与环境", 0x10B981, 0x059669},
        };
        float mcW = (w - 40.0f - 24.0f) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            float mx = x + 20.0f + i * (mcW + 12.0f);
            bool act = s.connected && (s.anc_mode == mcards[i].m);
            int hid = HIT_ANC_MODE_BASE + i;
            float h_m = hover_val(hid);
            float lift = -2.5f * h_m + 1.4f * press_val(hid);
            D2D1_RECT_F hitR = D2D1::RectF(mx, y + 46.0f, mx + mcW, y + 128.0f);
            D2D1_RECT_F mr = D2D1::RectF(mx, y + 46.0f + lift, mx + mcW, y + 128.0f + lift);

            if (act) {
                uint32_t g0 = (i == 1 && s.auto_noise) ? 0x6366F1 : mcards[i].c0;
                uint32_t g1 = (i == 1 && s.auto_noise) ? 0x4338CA : mcards[i].c1;
                draw_soft_shadow(mr, 12.0f, with_alpha(rgb(g0), 0.26f + 0.10f * h_m), 0.9f + 0.3f * h_m);
                fill_grad_round(mr, 12.0f, rgb(g0), rgb(g1), true);
                draw_text(mcards[i].title, tf_h1_, D2D1::RectF(mr.left + 16, mr.top + 8, mr.right - 16, mr.top + 36), rgb(0xFFFFFF));
                draw_text(mcards[i].desc, tf_sub_, D2D1::RectF(mr.left + 16, mr.top + 36, mr.right - 16, mr.top + 56), rgb(0xFFFFFF, 0.88f));
                if (mcards[i].m == ANC_MODE_NOISE || mcards[i].m == ANC_MODE_TRANSPARENT) {
                    draw_anc_wave(mr, mcards[i].m == ANC_MODE_TRANSPARENT, s.auto_noise);
                }
            } else {
                D2D1_COLOR_F bg = lerp_col(th.subcard, th.btn_hot, h_m);
                fill_round(mr, 12.0f, bg);
                stroke_round(mr, 12.0f, lerp_col(th.border, th.border_hot, h_m));
                draw_text(mcards[i].title, tf_h1_, D2D1::RectF(mr.left + 16, mr.top + 12, mr.right - 16, mr.top + 42), th.text_pri);
                draw_text(mcards[i].desc, tf_sub_, D2D1::RectF(mr.left + 16, mr.top + 44, mr.right - 16, mr.top + 68), th.text_mut);
            }
            register_hit(hitR, hid);
        }

        // Sub-level pills
        draw_text(L"细分档位：", tf_body_, D2D1::RectF(x + 20, y + 146.0f, x + 100, y + 184.0f), th.text_sec);
        uint8_t disp_mode = s.connected ? s.anc_mode : ANC_MODE_NOISE;
        if (disp_mode == ANC_MODE_CLOSE) {
            draw_text(L"当前为「关闭」模式，点击上方「降噪」或「通透」可选择细分档位",
                      tf_sub_, D2D1::RectF(x + 102, y + 146.0f, rx - 156, y + 184.0f), th.text_mut);
            draw_button(D2D1::RectF(rx - 148.0f, y + 148.0f, rx - 20.0f, y + 182.0f),
                        L"✦ 自适应降噪", HIT_ANC_ADAPTIVE, th, false, false, 0x6366F1, 0x4F46E5);
        } else if (disp_mode == ANC_MODE_NOISE) {
            float px = x + 102.0f;
            const float pillW = 88.0f;
            for (size_t i = 0; i < std::size(kNoiseLevels); ++i) {
                D2D1_RECT_F pr = D2D1::RectF(px, y + 148.0f, px + pillW, y + 182.0f);
                bool act = s.connected && !s.auto_noise && (s.anc_level == kNoiseLevels[i].code);
                draw_button(pr, kNoiseLevels[i].label, HIT_ANC_LVL_BASE + static_cast<int>(i), th,
                            false, act, 0x3B82F6, 0x2563EB);
                px += pillW + 8.0f;
            }

            brush_->SetColor(th.border);
            rt_->DrawLine(D2D1::Point2F(px + 4.0f, y + 152.0f), D2D1::Point2F(px + 4.0f, y + 178.0f), brush_, 1.0f);
            px += 14.0f;

            D2D1_RECT_F autoR = D2D1::RectF(px, y + 148.0f, px + 122.0f, y + 182.0f);
            bool auto_act = s.connected && s.auto_noise;
            draw_button(autoR, L"✦ 自适应降噪", HIT_ANC_ADAPTIVE, th, false, auto_act, 0x6366F1, 0x4F46E5);

            float noteLeft = autoR.right + 12.0f;
            if (rx - 20.0f - noteLeft >= 110.0f) {
                draw_text(L"抗风噪由固件自动生效", tf_sub_,
                          D2D1::RectF(noteLeft, y + 148.0f, rx - 20.0f, y + 182.0f),
                          th.text_mut, DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
        } else {
            float px = x + 102.0f;
            const float pillW = 104.0f;
            for (size_t i = 0; i < std::size(kTransLevels); ++i) {
                D2D1_RECT_F pr = D2D1::RectF(px, y + 148.0f, px + pillW, y + 182.0f);
                bool act = s.connected && (s.anc_level == kTransLevels[i].code);
                draw_button(pr, kTransLevels[i].label, HIT_ANC_LVL_BASE + static_cast<int>(i), th,
                            false, act, 0x10B981, 0x059669);
                px += pillW + 10.0f;
            }
        }

        // Card 2: 2x2 Feature Switch Matrix (whole card clickable + icon badges)
        float y2 = y + 218.0f;
        float c2_bot = std::max(y2 + 216.0f, std::min(bot, y2 + 228.0f));
        D2D1_RECT_F c2 = D2D1::RectF(x, y2, rx, c2_bot);
        draw_card_surface(c2, 16.0f, th);
        draw_text(L"核心特性快捷开关 (点击整张卡片即可切换)", tf_h2_, D2D1::RectF(x + 20, y2 + 12, x + 380, y2 + 36), th.text_pri);

        struct SwCard { const wchar_t* icon; uint32_t icol; const wchar_t* title; const wchar_t* sub; int sw_idx; bool val; int id; };
        SwCard sws[4] = {
            {L"🔗", 0x3B82F6, L"双设备连接",     L"同时连接电脑与手机，音频无缝切换",   4, s.multipoint,       HIT_SW_MULTIPOINT},
            {L"⚡", 0xF59E0B, L"低延迟游戏模式", L"缩短蓝牙无线音频缓冲，音画更跟手",   5, s.low_latency,      HIT_SW_LOW_LATENCY},
            {L"🎧", 0x8B5CF6, L"个性化降噪",     L"结合耳道声学特征匹配最佳降噪滤波",   6, s.personalized_anc, HIT_SW_PERS_ANC},
            {L"🔊", 0x10B981, L"自适应音量",     L"随周围环境噪声变化平滑调节媒体音量", 7, s.adaptive_volume,  HIT_SW_ADAPT_VOL},
        };
        float cellW = (w - 40.0f - 14.0f) / 2.0f;
        for (int i = 0; i < 4; ++i) {
            int col = i % 2, row = i / 2;
            float cx = x + 20.0f + col * (cellW + 14.0f);
            float cy = y2 + 44.0f + row * 82.0f;
            D2D1_RECT_F cr = D2D1::RectF(cx, cy, cx + cellW, cy + 72.0f);
            draw_switch_card(cr, sws[i].title, sws[i].sub, sws[i].sw_idx, sws[i].val, s.connected, sws[i].id, th,
                             sws[i].icon, rgb(sws[i].icol));
        }
    }

    // ---------------- Tab 1: EQ Presets + Smooth Cubic Bezier 10-Band EQ ----------------
    void draw_smooth_eq_curve(const D2D1_POINT_2F pts[10], float trackTop, float trackBot, const Theme& th) {
        ID2D1PathGeometry* stroke_pg = nullptr;
        ID2D1PathGeometry* fill_pg = nullptr;
        if (FAILED(d2d_factory_->CreatePathGeometry(&stroke_pg)) || !stroke_pg) return;
        if (FAILED(d2d_factory_->CreatePathGeometry(&fill_pg)) || !fill_pg) {
            stroke_pg->Release();
            return;
        }

        auto add_beziers = [&](ID2D1GeometrySink* sink) {
            constexpr float kTension = 1.0f / 6.0f;
            for (int i = 0; i < 9; ++i) {
                const D2D1_POINT_2F& p0 = (i == 0) ? pts[0] : pts[i - 1];
                const D2D1_POINT_2F& p1 = pts[i];
                const D2D1_POINT_2F& p2 = pts[i + 1];
                const D2D1_POINT_2F& p3 = (i + 2 < 10) ? pts[i + 2] : pts[9];

                D2D1_POINT_2F cp1 = D2D1::Point2F(
                    p1.x + (p2.x - p0.x) * kTension,
                    std::clamp(p1.y + (p2.y - p0.y) * kTension, trackTop, trackBot));
                D2D1_POINT_2F cp2 = D2D1::Point2F(
                    p2.x - (p3.x - p1.x) * kTension,
                    std::clamp(p2.y - (p3.y - p1.y) * kTension, trackTop, trackBot));
                sink->AddBezier(D2D1::BezierSegment(cp1, cp2, p2));
            }
        };

        ID2D1GeometrySink* s_sink = nullptr;
        if (SUCCEEDED(stroke_pg->Open(&s_sink)) && s_sink) {
            s_sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
            add_beziers(s_sink);
            s_sink->EndFigure(D2D1_FIGURE_END_OPEN);
            s_sink->Close();
            s_sink->Release();
        }

        ID2D1GeometrySink* f_sink = nullptr;
        if (SUCCEEDED(fill_pg->Open(&f_sink)) && f_sink) {
            f_sink->BeginFigure(D2D1::Point2F(pts[0].x, trackBot), D2D1_FIGURE_BEGIN_FILLED);
            f_sink->AddLine(pts[0]);
            add_beziers(f_sink);
            f_sink->AddLine(D2D1::Point2F(pts[9].x, trackBot));
            f_sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            f_sink->Close();
            f_sink->Release();
        }

        // Translucent vertical gradient area fill under the spline
        D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, rgb(0xFF6900, th.dark_t > 0.5f ? 0.30f : 0.20f)},
            {1.0f, rgb(0xFF6900, 0.01f)},
        };
        ID2D1GradientStopCollection* gsc = nullptr;
        if (SUCCEEDED(rt_->CreateGradientStopCollection(stops, 2, &gsc)) && gsc) {
            ID2D1LinearGradientBrush* lgb = nullptr;
            D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props = D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(pts[0].x, trackTop), D2D1::Point2F(pts[0].x, trackBot));
            if (SUCCEEDED(rt_->CreateLinearGradientBrush(props, gsc, &lgb)) && lgb) {
                rt_->FillGeometry(fill_pg, lgb);
                lgb->Release();
            }
            gsc->Release();
        }

        // Outer neon glow + crisp core spline
        brush_->SetColor(rgb(0xFF6900, 0.24f));
        rt_->DrawGeometry(stroke_pg, brush_, 6.5f, stroke_round_);
        brush_->SetColor(rgb(0xFF6900, 0.92f));
        rt_->DrawGeometry(stroke_pg, brush_, 2.4f, stroke_round_);

        stroke_pg->Release();
        fill_pg->Release();
    }

    void paint_tab_eq(const EarphoneSnapshot& s, const std::vector<EqBand>& eq_bands, const Theme& th,
                      float x, float rx, float w, float y, float bot) {
        // Card 1: Presets
        D2D1_RECT_F c1 = D2D1::RectF(x, y, rx, y + 126.0f);
        draw_card_surface(c1, 16.0f, th);
        draw_text(L"官方调音预设 (一键切换)", tf_h2_, D2D1::RectF(x + 20, y + 12, x + 320, y + 36), th.text_pri);
        draw_text(std::wstring(L"当前音效：") + (s.connected ? eq_mode_name(s.eq_mode) : L"--"),
                  tf_sub_, D2D1::RectF(rx - 240, y + 12, rx - 20, y + 36), rgb(0xFF6900), DWRITE_TEXT_ALIGNMENT_TRAILING);

        int cols = 5;
        float chipW = (w - 40.0f - (cols - 1) * 10.0f) / static_cast<float>(cols);
        for (size_t i = 0; i < std::size(kEqPresets); ++i) {
            int col = static_cast<int>(i % cols);
            int row = static_cast<int>(i / cols);
            float cx = x + 20.0f + col * (chipW + 10.0f);
            float cy = y + 44.0f + row * 38.0f;
            bool act = s.connected && (s.eq_mode == kEqPresets[i].code);
            draw_button(D2D1::RectF(cx, cy, cx + chipW, cy + 31.0f),
                        kEqPresets[i].label, HIT_EQ_PRESET_BASE + static_cast<int>(i), th,
                        false, act, 0xFF6900, 0xFF4500);
        }

        // Card 2: 10-Band Spline Graphic Equalizer
        float y2 = y + 138.0f;
        D2D1_RECT_F c2 = D2D1::RectF(x, y2, rx, std::max(y2 + 230.0f, bot));
        draw_card_surface(c2, 16.0f, th);

        int bound = std::max(1, s.custom_eq.gain_bound);
        std::wstring eq_hdr = L"10段自定义均衡器 (范围 ±" + std::to_wstring(bound) + L" dB，拖动滑块调节)";
        draw_text(eq_hdr, tf_h2_, D2D1::RectF(x + 20, y2 + 12, x + 460, y2 + 38), th.text_pri);

        draw_button(D2D1::RectF(rx - 304, y2 + 10, rx - 186, y2 + 38), L"保存到耳机", HIT_EQ_APPLY, th, true);
        draw_button(D2D1::RectF(rx - 178, y2 + 10, rx - 98,  y2 + 38), L"读取曲线", HIT_EQ_READ, th);
        draw_button(D2D1::RectF(rx - 90,  y2 + 10, rx - 20,  y2 + 38), L"归零重置", HIT_EQ_RESET, th);

        float trackTop = y2 + 68.0f;
        float trackBot = std::max(trackTop + 92.0f, bot - 40.0f);
        float trackMid = 0.5f * (trackTop + trackBot);

        // Horizontal dB reference gridlines
        float gridYs[5] = {
            trackTop,
            lerp(trackTop, trackMid, 0.5f),
            trackMid,
            lerp(trackMid, trackBot, 0.5f),
            trackBot
        };
        const wchar_t* gridLbls[5] = {L"+6", L"+3", L"0", L"-3", L"-6"};
        for (int gi = 0; gi < 5; ++gi) {
            bool is_zero = (gi == 2);
            brush_->SetColor(is_zero ? lerp_col(th.border_hot, th.text_mut, 0.35f) : with_alpha(th.border, 0.65f));
            rt_->DrawLine(D2D1::Point2F(x + 38, gridYs[gi]), D2D1::Point2F(rx - 24, gridYs[gi]),
                          brush_, is_zero ? 1.2f : 0.8f);
            if (gi % 2 == 0) {
                std::wstring gstr = (gi == 0) ? (L"+" + std::to_wstring(bound))
                                  : (gi == 4) ? (L"-" + std::to_wstring(bound))
                                  : gridLbls[gi];
                draw_text(gstr, tf_sub_, D2D1::RectF(x + 8, gridYs[gi] - 9, x + 36, gridYs[gi] + 9),
                          th.text_mut, DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
        }

        float bandStep = (w - 82.0f) / 10.0f;
        D2D1_POINT_2F pts[10];

        for (int i = 0; i < 10; ++i) {
            float cx = x + 46.0f + bandStep * (i + 0.5f);
            float g_anim = std::clamp(eq_anim_[i], static_cast<float>(-bound), static_cast<float>(bound));
            int g_disp = (i < static_cast<int>(eq_bands.size())) ? std::clamp(eq_bands[i].gain, -bound, bound) : 0;
            float norm = g_anim / static_cast<float>(bound);
            float py = trackMid - norm * ((trackBot - trackTop) * 0.5f);
            pts[i] = D2D1::Point2F(cx, py);

            // Track background rail
            D2D1_RECT_F tr = D2D1::RectF(cx - 2.5f, trackTop, cx + 2.5f, trackBot);
            fill_round(tr, 2.5f, with_alpha(th.border, 0.7f));

            // Active fill from zero line
            float fy0 = std::min(trackMid, py);
            float fy1 = std::max(trackMid, py);
            if (fy1 - fy0 > 1.0f) {
                fill_round(D2D1::RectF(cx - 3.0f, fy0, cx + 3.0f, fy1), 3.0f, rgb(0xFF6900, 0.65f));
            }

            // Target dB numeric readout
            std::wstring db_lbl = (g_disp > 0 ? L"+" : L"") + std::to_wstring(g_disp);
            draw_text(db_lbl, tf_sub_, D2D1::RectF(cx - 24, y2 + 43, cx + 24, y2 + 63),
                      g_disp != 0 ? rgb(0xFF6900) : th.text_mut, DWRITE_TEXT_ALIGNMENT_CENTER);

            // Frequency label at bottom
            uint16_t fq = (i < static_cast<int>(eq_bands.size())) ? eq_bands[i].freq : 1000;
            std::wstring fq_lbl = (fq >= 1000) ? (std::to_wstring(fq / 1000) + L"k") : std::to_wstring(fq);
            draw_text(fq_lbl, tf_sub_, D2D1::RectF(cx - 28, trackBot + 8, cx + 28, trackBot + 28),
                      th.text_sec, DWRITE_TEXT_ALIGNMENT_CENTER);

            D2D1_RECT_F hitR = D2D1::RectF(cx - bandStep * 0.45f, trackTop - 10.0f,
                                           cx + bandStep * 0.45f, trackBot + 10.0f);
            eq_track_rects_.push_back(D2D1::RectF(cx - 10, trackTop, cx + 10, trackBot));
            register_hit(hitR, HIT_EQ_BAND_BASE + i);
        }

        // Smooth Cubic Bezier Spline & Area Fill
        draw_smooth_eq_curve(pts, trackTop, trackBot, th);

        // Glowing interactive thumb handles
        for (int i = 0; i < 10; ++i) {
            float h_b = hover_val(HIT_EQ_BAND_BASE + i);
            bool drag = (dragging_eq_band_ == i);
            float active_f = drag ? 1.0f : h_b;
            if (active_f > 0.05f) {
                brush_->SetColor(rgb(0xFF6900, 0.22f * active_f));
                rt_->FillEllipse(D2D1::Ellipse(pts[i], 10.5f + 4.0f * active_f, 10.5f + 4.0f * active_f), brush_);
            }
            float rad = 6.8f + 1.8f * active_f;
            brush_->SetColor(th.card);
            rt_->FillEllipse(D2D1::Ellipse(pts[i], rad, rad), brush_);
            brush_->SetColor(rgb(0xFF6900));
            rt_->DrawEllipse(D2D1::Ellipse(pts[i], rad, rad), brush_, 2.6f);
        }
    }

    // ---------------- Tab 2: Spatial Audio Radar + Immersive Sound + Find Sonar ----------------
    void draw_spatial_radar(const D2D1_RECT_F& rr, bool spatial_on, bool head_track, const Theme& th) {
        fill_round(rr, 12.0f, th.subcard);
        stroke_round(rr, 12.0f, lerp_col(th.border, rgb(0x8B5CF6, 0.55f), spatial_on ? 0.8f : 0.0f));

        float cx = (rr.left + rr.right) * 0.5f;
        float cy = (rr.top + rr.bottom) * 0.5f - 6.0f;
        float max_r = std::min(rr.right - rr.left, rr.bottom - rr.top) * 0.36f;

        if (spatial_on) {
            fill_radial_orb(D2D1::Point2F(cx, cy), max_r * 1.15f, max_r * 1.15f,
                            rgb(0x8B5CF6, th.dark_t > 0.5f ? 0.24f : 0.14f), rgb(0x8B5CF6, 0.0f));
        }

        // Expanding concentric ripples
        for (int k = 0; k < 3; ++k) {
            float prog = spatial_on ? std::fmod(ambient_phase_ * 0.22f + k / 3.0f, 1.0f) : (0.38f + k * 0.26f);
            float r = max_r * (0.28f + 0.72f * prog);
            float alpha = spatial_on ? (1.0f - prog) * 0.55f : 0.22f;
            brush_->SetColor(spatial_on ? rgb(0x8B5CF6, alpha) : with_alpha(th.border_hot, 0.55f));
            rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush_, 1.4f);
        }

        // Orbiting L/R virtual speaker nodes
        float ang = (spatial_on && head_track) ? (std::sin(ambient_phase_ * 0.65f) * 0.85f) : 0.0f;
        float orb_r = max_r * 0.72f;
        float la = kPi + ang;
        float ra = ang;
        D2D1_POINT_2F l_pt = D2D1::Point2F(cx + orb_r * std::cos(la), cy + orb_r * std::sin(la));
        D2D1_POINT_2F r_pt = D2D1::Point2F(cx + orb_r * std::cos(ra), cy + orb_r * std::sin(ra));

        brush_->SetColor(spatial_on ? rgb(0x6366F1, 0.9f) : th.text_mut);
        rt_->FillEllipse(D2D1::Ellipse(l_pt, 4.5f, 4.5f), brush_);
        rt_->FillEllipse(D2D1::Ellipse(r_pt, 4.5f, 4.5f), brush_);

        // Center listener node
        brush_->SetColor(spatial_on ? rgb(0x8B5CF6) : th.text_mut);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.0f, 8.0f), brush_);
        brush_->SetColor(rgb(0xFFFFFF, 0.9f));
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.0f, 3.0f), brush_);

        std::wstring status = !spatial_on ? L"声场待开启" : (head_track ? L"360° 头部追踪中" : L"3D 环绕声场");
        draw_text(status, tf_sub_, D2D1::RectF(rr.left + 8, rr.bottom - 22, rr.right - 8, rr.bottom - 4),
                  spatial_on ? rgb(0x8B5CF6) : th.text_mut, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    void paint_tab_spatial(const EarphoneSnapshot& s, const Theme& th, float x, float rx, float w, float y, float bot) {
        // Card 1: Spatial Audio Controls (Left 66%) + 3D Soundstage Radar (Right 34%)
        D2D1_RECT_F c1 = D2D1::RectF(x, y, rx, y + 168.0f);
        draw_card_surface(c1, 16.0f, th);
        draw_text(L"空间音频与头部追踪", tf_h2_, D2D1::RectF(x + 20, y + 10, x + 320, y + 34), th.text_pri);

        float innerW = w - 40.0f;
        float radarW = std::clamp(innerW * 0.30f, 155.0f, 230.0f);
        float leftW = innerW - radarW - 14.0f;

        D2D1_RECT_F sp1 = D2D1::RectF(x + 20, y + 38, x + 20 + leftW, y + 93);
        D2D1_RECT_F sp2 = D2D1::RectF(x + 20, y + 101, x + 20 + leftW, y + 156);
        draw_switch_card(sp1, L"开启空间音频", L"突破左右声道限制，营造三维环绕声场",
                         8, s.spatial.open, s.connected, HIT_SW_SPATIAL, th, L"🌌", rgb(0x8B5CF6));
        draw_switch_card(sp2, L"动态头部追踪", L"声源方位随头部转动实时定位 (需开启空间音频)",
                         9, s.spatial.open && s.spatial.head_tracking, s.connected && s.spatial.open,
                         HIT_SW_HEAD_TRACK, th, L"🧭", rgb(0x6366F1));

        D2D1_RECT_F radarR = D2D1::RectF(rx - 20 - radarW, y + 38, rx - 20, y + 156);
        draw_spatial_radar(radarR, s.connected && s.spatial.open, s.spatial.head_tracking, th);

        // Card 2: Scene Immersive Sound (CFG 54)
        float y2 = y + 178.0f;
        D2D1_RECT_F c2 = D2D1::RectF(x, y2, rx, y2 + 96.0f);
        draw_card_surface(c2, 16.0f, th);
        draw_text(L"场景沉浸声渲染 (Redmi Buds 5 Pro 等支持)", tf_h2_,
                  D2D1::RectF(x + 20, y2 + 10, x + 440, y2 + 34), th.text_pri);

        float scW = (w - 40.0f - 50.0f) / 6.0f;
        for (size_t i = 0; i < std::size(kSceneModes); ++i) {
            float sx = x + 20.0f + i * (scW + 10.0f);
            bool act = s.connected && ((i == 0 && !s.scene.open) ||
                       (i > 0 && s.scene.open && s.scene.mode == kSceneModes[i].code));
            draw_button(D2D1::RectF(sx, y2 + 42, sx + scW, y2 + 80),
                        kSceneModes[i].label, HIT_SCENE_BASE + static_cast<int>(i), th,
                        false, act, 0x8B5CF6, 0x6D28D9);
        }

        // Card 3: Find My Earbuds (with active Sonar Ping visual) + Auto Answer Call
        float y3 = y2 + 106.0f;
        D2D1_RECT_F c3 = D2D1::RectF(x, y3, rx, std::max(y3 + 156.0f, std::min(bot, y3 + 168.0f)));
        draw_card_surface(c3, 16.0f, th);
        draw_text(L"查找耳机响铃 (佩戴在耳内时请勿触发，以免音量过大)", tf_h2_,
                  D2D1::RectF(x + 20, y3 + 10, rx - 140, y3 + 34), th.text_pri);

        // Live sonar wave badge when Find ring is active
        if (ring_pulse_rem_ > 0.0f) {
            float sx = rx - 110.0f;
            float sy = y3 + 22.0f;
            float p = std::fmod(ambient_phase_ * 0.7f, 1.0f);
            brush_->SetColor(rgb(0xFF6900, (1.0f - p) * 0.65f));
            rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), 4.0f + p * 10.0f, 4.0f + p * 10.0f), brush_, 1.5f);
            brush_->SetColor(rgb(0xFF6900));
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), 3.5f, 3.5f), brush_);
            draw_text(L"响铃探测中", tf_sub_, D2D1::RectF(sx + 10, y3 + 10, rx - 20, y3 + 34), rgb(0xFF6900));
        }

        float fbW = (w - 40.0f - 30.0f) / 4.0f;
        draw_button(D2D1::RectF(x + 20 + 0 * (fbW + 10), y3 + 40, x + 20 + 0 * (fbW + 10) + fbW, y3 + 76), L"🔊 双耳响铃", HIT_FIND_BOTH, th, true);
        draw_button(D2D1::RectF(x + 20 + 1 * (fbW + 10), y3 + 40, x + 20 + 1 * (fbW + 10) + fbW, y3 + 76), L"🔉 仅响左耳", HIT_FIND_LEFT, th);
        draw_button(D2D1::RectF(x + 20 + 2 * (fbW + 10), y3 + 40, x + 20 + 2 * (fbW + 10) + fbW, y3 + 76), L"🔉 仅响右耳", HIT_FIND_RIGHT, th);
        draw_button(D2D1::RectF(x + 20 + 3 * (fbW + 10), y3 + 40, x + 20 + 3 * (fbW + 10) + fbW, y3 + 76), L"🔇 停止响铃", HIT_FIND_STOP, th);

        D2D1_RECT_F exAns = D2D1::RectF(x + 20, y3 + 86, rx - 20, y3 + 146);
        draw_switch_card(exAns, L"来电自动接听", L"手机来电时，若正在佩戴耳机则自动接听电话",
                         10, s.auto_answer, s.connected, HIT_SW_AUTO_ANS, th, L"📞", rgb(0x10B981));
    }

    // ---------------- Tab 3: Live RCSP Syntax-Highlighted Frame Inspector ----------------
    static D2D1_COLOR_F classify_log_color(const std::wstring& line) {
        if (line.find(L"[错误]") != std::wstring::npos || line.find(L"[失败]") != std::wstring::npos ||
            line.find(L"⚠") != std::wstring::npos || line.find(L"✗") != std::wstring::npos) {
            return rgb(0xF87171); // red
        }
        if (line.find(L"[双向认证]") != std::wstring::npos || line.find(L"===") != std::wstring::npos) {
            return rgb(0xFBBF24); // amber
        }
        if (line.find(L"[设备上报]") != std::wstring::npos) {
            return rgb(0xC084FC); // purple
        }
        if (line.find(L"→") != std::wstring::npos) {
            return rgb(0x38BDF8); // sky blue outgoing
        }
        if (line.find(L"←") != std::wstring::npos) {
            return rgb(0x34D399); // emerald incoming
        }
        return rgb(0xCBD5E1);
    }

    void paint_tab_logs(const std::deque<std::wstring>& logs_copy, const Theme& th, float x, float rx, float y, float bot) {
        D2D1_RECT_F c = D2D1::RectF(x, y, rx, std::max(y + 200.0f, bot));
        draw_card_surface(c, 16.0f, th);

        draw_text(L"RCSP / RFCOMM 实时协议帧日志", tf_h2_, D2D1::RectF(x + 20, y + 12, x + 340, y + 40), th.text_pri);
        draw_button(D2D1::RectF(rx - 290, y + 10, rx - 196, y + 38), L"重新认证", HIT_LOG_REAUTH, th);
        draw_button(D2D1::RectF(rx - 188, y + 10, rx - 104, y + 38), L"复制日志", HIT_LOG_COPY, th);
        draw_button(D2D1::RectF(rx - 96,  y + 10, rx - 20,  y + 38), L"清空",     HIT_LOG_CLEAR, th);

        D2D1_RECT_F termR = D2D1::RectF(x + 16, y + 48, rx - 16, c.bottom - 16);
        fill_round(termR, 12.0f, rgb(0x0D1117));
        stroke_round(termR, 12.0f, th.dark_t > 0.5f ? rgb(0x263449) : rgb(0x1E293B));

        if (logs_copy.empty()) {
            draw_text(L"[等待连接通信…]", tf_mono_,
                      D2D1::RectF(termR.left + 14, termR.top + 12, termR.right - 14, termR.bottom - 10),
                      rgb(0x64748B), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            return;
        }

        float availW = std::max(120.0f, termR.right - termR.left - 28.0f);
        float availH = std::max(40.0f, termR.bottom - termR.top - 20.0f);
        int total = static_cast<int>(logs_copy.size());
        int max_scroll = std::max(0, total - 1);
        log_scroll_ = std::clamp(log_scroll_, 0, max_scroll);
        int end = total - log_scroll_;

        struct VisLine {
            std::wstring text;
            D2D1_COLOR_F col;
            IDWriteTextLayout* layout = nullptr;
            float height = 16.0f;
        };
        std::vector<VisLine> lines;
        float usedH = 0.0f;
        for (int idx = end - 1; idx >= 0 && usedH < availH; --idx) {
            const std::wstring& l = logs_copy[idx];
            IDWriteTextLayout* layout = nullptr;
            if (FAILED(dw_factory_->CreateTextLayout(l.c_str(), static_cast<UINT32>(l.size()),
                                                     tf_mono_, availW, 200.0f, &layout)) || !layout) {
                continue;
            }
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            float lh = std::max(16.0f, tm.height + 3.0f);
            if (usedH + lh > availH && !lines.empty()) {
                layout->Release();
                break;
            }
            lines.push_back(VisLine{l, classify_log_color(l), layout, lh});
            usedH += lh;
        }

        rt_->PushAxisAlignedClip(termR, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        float cy = termR.top + 10.0f;
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            brush_->SetColor(it->col);
            rt_->DrawTextLayout(D2D1::Point2F(termR.left + 14.0f, cy), it->layout, brush_,
                                D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            cy += it->height;
        }
        rt_->PopAxisAlignedClip();

        for (auto& vl : lines) {
            if (vl.layout) vl.layout->Release();
        }
    }

    // ---------------- Input Handling ----------------
    float px_to_dip(int px) const {
        UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
        return static_cast<float>(px) * 96.0f / static_cast<float>(dpi);
    }

    int hit_test(float dx, float dy) const {
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            if (dx >= it->rect.left && dx <= it->rect.right &&
                dy >= it->rect.top && dy <= it->rect.bottom) {
                return it->id;
            }
        }
        return HIT_NONE;
    }

    void update_eq_from_mouse(int band, float dy) {
        if (band < 0 || band >= static_cast<int>(eq_track_rects_.size())) {
            return;
        }
        const auto& tr = eq_track_rects_[band];
        float mid = 0.5f * (tr.top + tr.bottom);
        float half = std::max(20.0f, (tr.bottom - tr.top) * 0.5f);
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(data_mtx_);
            if (band >= static_cast<int>(ui_eq_bands_.size())) return;
            int bound = std::max(1, snap_.custom_eq.gain_bound);
            float norm = std::clamp((mid - dy) / half, -1.0f, 1.0f);
            int g = static_cast<int>(std::round(norm * static_cast<float>(bound)));
            if (ui_eq_bands_[band].gain != g) {
                ui_eq_bands_[band].gain = g;
                eq_anim_[band] = static_cast<float>(g);
                eq_dirty_ = true;
                changed = true;
            }
        }
        if (changed) invalidate();
    }

    void on_click(int id, bool double_click = false) {
        if (id != HIT_MAC_BOX && mac_focused_) {
            mac_focused_ = false;
            invalidate();
        }
        if (id == HIT_NONE) return;

        if (id == HIT_THEME_TOGGLE) {
            toggle_theme();
            return;
        }
        if (id == HIT_MAC_BOX) {
            mac_focused_ = true;
            wake_anim();
            invalidate();
            return;
        }
        if (id == HIT_REFRESH_DEVS) {
            refresh_spin_target_ += 360.0f;
            wake_anim();
            refresh_devices_async(false);
            show_toast(L"已刷新已配对蓝牙设备列表", false);
            return;
        }
        if (id >= HIT_DEV_ITEM_BASE && id < HIT_DEV_ITEM_BASE + 50) {
            size_t idx = static_cast<size_t>(id - HIT_DEV_ITEM_BASE);
            std::string chosen;
            {
                std::lock_guard<std::mutex> lk(data_mtx_);
                if (idx < devices_.size()) chosen = devices_[idx].mac;
            }
            if (!chosen.empty()) {
                mac_input_ = chosen;
                invalidate();
                if (double_click) connect_async(mac_input_, false);
            }
            return;
        }
        if (id == HIT_CONNECT_BTN) {
            if (session_.is_connected()) disconnect_async();
            else connect_async(mac_input_, false);
            return;
        }
        if (id == HIT_HIDE_TRAY_BTN) {
            hide_to_tray();
            return;
        }
        if (id == HIT_EXIT_APP_BTN) {
            really_quit();
            return;
        }
        if (id == HIT_OPT_CLOSE_TRAY) {
            cfg_.close_to_tray = !cfg_.close_to_tray;
            cfg_.save();
            wake_anim();
            invalidate();
            return;
        }
        if (id == HIT_OPT_AUTO_CONN) {
            cfg_.auto_connect = !cfg_.auto_connect;
            cfg_.save();
            wake_anim();
            invalidate();
            return;
        }
        if (id == HIT_OPT_AUTO_RECONN) {
            cfg_.auto_reconnect = !cfg_.auto_reconnect;
            cfg_.save();
            wake_anim();
            invalidate();
            return;
        }
        if (id == HIT_OPT_AUTOSTART) {
            bool next = !AppConfig::is_autostart_enabled();
            if (AppConfig::set_autostart_enabled(next)) {
                autostart_cached_ = next;
                show_toast(next ? L"✓ 已开启开机自动启动 (静默驻留托盘)" : L"已关闭开机自动启动", false);
                wake_anim();
                invalidate();
            }
            return;
        }
        if (id == HIT_REFRESH_STATE) {
            refresh_spin_target_ += 360.0f;
            wake_anim();
            worker_.post([this] {
                session_.refresh_all();
                show_toast(L"✓ 已刷新设备状态与电量", false);
            });
            return;
        }
        if (id >= HIT_TAB_BASE && id < HIT_TAB_BASE + 4) {
            int next_tab = id - HIT_TAB_BASE;
            if (next_tab != active_tab_) {
                active_tab_ = next_tab;
                page_enter_t_ = 0.0f;
                wake_anim();
                invalidate();
            }
            return;
        }

        // Log tab buttons that work anytime
        if (id == HIT_LOG_CLEAR) {
            std::lock_guard<std::mutex> lk(data_mtx_);
            logs_.clear();
            log_scroll_ = 0;
            invalidate();
            return;
        }
        if (id == HIT_LOG_COPY) {
            std::wstring all;
            {
                std::lock_guard<std::mutex> lk(data_mtx_);
                for (const auto& l : logs_) all += l + L"\r\n";
            }
            if (!all.empty() && OpenClipboard(hwnd_)) {
                EmptyClipboard();
                size_t cb = (all.size() + 1) * sizeof(wchar_t);
                HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, cb);
                if (hg) {
                    void* p = GlobalLock(hg);
                    if (p) { std::memcpy(p, all.c_str(), cb); GlobalUnlock(hg); }
                    SetClipboardData(CF_UNICODETEXT, hg);
                }
                CloseClipboard();
                show_toast(L"✓ 通信日志已复制到剪贴板", false);
            }
            return;
        }

        // Remaining controls require an active earphone connection
        if (!session_.is_connected()) {
            show_toast(L"请先连接耳机后再操作该设置", true);
            return;
        }

        auto s = session_.snapshot();

        if (id == HIT_LOG_REAUTH) {
            worker_.post([this] {
                bool ok = session_.reauthenticate();
                show_toast(ok ? L"✓ 重新双向认证通过" : L"⚠ 重新认证失败", !ok);
            });
            return;
        }
        if (id >= HIT_ANC_MODE_BASE && id < HIT_ANC_MODE_BASE + 3) {
            uint8_t m = static_cast<uint8_t>(id - HIT_ANC_MODE_BASE);
            uint8_t lvl = 0;
            if (m == ANC_MODE_NOISE) {
                lvl = (s.anc_mode == ANC_MODE_NOISE && s.anc_level <= ANC_LEVEL_DEEP) ? s.anc_level : ANC_LEVEL_DEEP;
            } else if (m == ANC_MODE_TRANSPARENT) {
                lvl = (s.anc_mode == ANC_MODE_TRANSPARENT && s.anc_level <= TRANS_LEVEL_ENV) ? s.anc_level : TRANS_LEVEL_STANDARD;
            }
            set_anc_async(m, lvl, /*keep_auto_noise=*/true);
            return;
        }
        if (id >= HIT_ANC_LVL_BASE && id < HIT_ANC_LVL_BASE + 10) {
            size_t idx = static_cast<size_t>(id - HIT_ANC_LVL_BASE);
            if (s.anc_mode == ANC_MODE_NOISE && idx < std::size(kNoiseLevels)) {
                set_anc_async(ANC_MODE_NOISE, kNoiseLevels[idx].code, /*keep_auto_noise=*/false);
            } else if (s.anc_mode == ANC_MODE_TRANSPARENT && idx < std::size(kTransLevels)) {
                set_anc_async(ANC_MODE_TRANSPARENT, kTransLevels[idx].code, /*keep_auto_noise=*/false);
            }
            return;
        }
        if (id == HIT_ANC_ADAPTIVE) {
            bool next = !(s.anc_mode == ANC_MODE_NOISE && s.auto_noise);
            toggle_auto_noise_async(next);
            return;
        }
        if (id == HIT_SW_MULTIPOINT) {
            bool next = !s.multipoint;
            worker_.post([this, next] {
                bool ok = session_.set_multipoint(next);
                show_toast(ok ? (next ? L"✓ 双设备连接已开启" : L"✓ 双设备连接已关闭")
                              : L"⚠ 双设备连接设置未响应，请重试", !ok);
            });
            return;
        }
        if (id == HIT_SW_LOW_LATENCY) {
            bool next = !s.low_latency;
            worker_.post([this, next] {
                bool ok = session_.set_low_latency(next);
                show_toast(ok ? (next ? L"✓ 低延迟游戏模式已开启" : L"✓ 低延迟游戏模式已关闭")
                              : L"⚠ 低延迟模式设置未响应", !ok);
            });
            return;
        }
        if (id == HIT_SW_PERS_ANC) {
            bool next = !s.personalized_anc;
            worker_.post([this, next] {
                bool ok = session_.set_personalized_anc(next);
                show_toast(ok ? (next ? L"✓ 个性化降噪已开启" : L"✓ 个性化降噪已关闭")
                              : L"⚠ 个性化降噪设置未响应", !ok);
            });
            return;
        }
        if (id == HIT_SW_ADAPT_VOL) {
            bool next = !s.adaptive_volume;
            worker_.post([this, next] {
                bool ok = session_.set_adaptive_volume(next);
                show_toast(ok ? (next ? L"✓ 自适应音量已开启" : L"✓ 自适应音量已关闭")
                              : L"⚠ 自适应音量设置未响应", !ok);
            });
            return;
        }
        if (id == HIT_SW_SPATIAL) {
            bool next = !s.spatial.open;
            worker_.post([this, next, s] {
                bool ok = session_.set_spatial_audio(next, next ? s.spatial.head_tracking : false);
                show_toast(ok ? (next ? L"✓ 空间音频已开启" : L"✓ 空间音频已关闭")
                              : L"⚠ 空间音频设置未响应", !ok);
            });
            return;
        }
        if (id == HIT_SW_HEAD_TRACK) {
            if (!s.spatial.open) {
                show_toast(L"请先开启上方「开启空间音频」开关", true);
                return;
            }
            bool next = !s.spatial.head_tracking;
            worker_.post([this, next] {
                bool ok = session_.set_spatial_audio(true, next);
                show_toast(ok ? (next ? L"✓ 动态头部追踪已开启" : L"✓ 动态头部追踪已关闭")
                              : L"⚠ 动态头部追踪设置未响应", !ok);
            });
            return;
        }
        if (id == HIT_SW_AUTO_ANS) {
            bool next = !s.auto_answer;
            worker_.post([this, next] {
                bool ok = session_.set_auto_answer(next);
                show_toast(ok ? (next ? L"✓ 来电自动接听已开启" : L"✓ 来电自动接听已关闭")
                              : L"⚠ 来电自动接听设置未响应", !ok);
            });
            return;
        }
        if (id >= HIT_EQ_PRESET_BASE && id < HIT_EQ_PRESET_BASE + static_cast<int>(std::size(kEqPresets))) {
            uint8_t eqc = kEqPresets[id - HIT_EQ_PRESET_BASE].code;
            eq_dirty_ = false;
            worker_.post([this, eqc] {
                session_.set_eq_preset(eqc);
                show_toast(std::wstring(L"✓ EQ 音效已切换：") + eq_mode_name(eqc), false);
            });
            return;
        }
        if (id == HIT_EQ_APPLY) {
            std::vector<EqBand> bands;
            {
                std::lock_guard<std::mutex> lk(data_mtx_);
                bands = ui_eq_bands_;
            }
            eq_dirty_ = false;
            worker_.post([this, bands] {
                bool ok = session_.set_custom_eq(bands, false);
                show_toast(ok ? L"✓ 自定义 10 段均衡器曲线已写入耳机" : L"⚠ 写入均衡器未确认 (请确认双耳佩戴或播放音乐)", !ok);
            });
            return;
        }
        if (id == HIT_EQ_READ) {
            eq_dirty_ = false;
            worker_.post([this] {
                session_.refresh_all();
                show_toast(L"✓ 已从耳机回读当前均衡器曲线", false);
            });
            return;
        }
        if (id == HIT_EQ_RESET) {
            {
                std::lock_guard<std::mutex> lk(data_mtx_);
                for (auto& b : ui_eq_bands_) b.gain = 0;
            }
            eq_dirty_ = false;
            wake_anim();
            invalidate();
            worker_.post([this] {
                session_.reset_custom_eq();
                show_toast(L"✓ 自定义均衡器已归零重置", false);
            });
            return;
        }
        if (id >= HIT_SCENE_BASE && id < HIT_SCENE_BASE + static_cast<int>(std::size(kSceneModes))) {
            size_t idx = static_cast<size_t>(id - HIT_SCENE_BASE);
            bool open = idx > 0;
            uint8_t mode = open ? kSceneModes[idx].code : 1;
            worker_.post([this, open, mode, idx] {
                session_.set_scene_rendering(open, mode);
                show_toast(std::wstring(L"✓ 场景沉浸声：") + kSceneModes[idx].label, false);
            });
            return;
        }
        if (id == HIT_FIND_BOTH)  { ring_pulse_rem_ = 4.0f; wake_anim(); worker_.post([this] { session_.find_device(true, 3); show_toast(L"🔊 已发送双耳响铃查找指令", false); }); }
        if (id == HIT_FIND_LEFT)  { ring_pulse_rem_ = 4.0f; wake_anim(); worker_.post([this] { session_.find_device(true, 1); show_toast(L"🔉 已发送左耳响铃查找指令", false); }); }
        if (id == HIT_FIND_RIGHT) { ring_pulse_rem_ = 4.0f; wake_anim(); worker_.post([this] { session_.find_device(true, 2); show_toast(L"🔉 已发送右耳响铃查找指令", false); }); }
        if (id == HIT_FIND_STOP)  { ring_pulse_rem_ = 0.0f; wake_anim(); worker_.post([this] { session_.find_device(false, 3); show_toast(L"🔇 已停止查找响铃", false); }); }
    }

    void on_char(wchar_t ch) {
        if (!mac_focused_) return;
        if (ch == L'\b') {
            if (!mac_input_.empty()) mac_input_.pop_back();
            invalidate();
        } else if (ch == L'\r' || ch == L'\n') {
            mac_focused_ = false;
            connect_async(mac_input_, false);
            invalidate();
        } else if (ch == 22) { // Ctrl+V
            if (OpenClipboard(hwnd_)) {
                HANDLE h = GetClipboardData(CF_UNICODETEXT);
                if (h) {
                    const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h));
                    if (w) {
                        std::string p = wide_to_utf8_str(w);
                        std::string clean;
                        for (char c : p) {
                            if (std::isxdigit(static_cast<unsigned char>(c)) || c == ':' || c == '-') {
                                clean.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                            }
                        }
                        if (!clean.empty() && clean.size() <= 17) mac_input_ = clean;
                        GlobalUnlock(h);
                    }
                }
                CloseClipboard();
                invalidate();
            }
        } else if ((std::iswxdigit(ch) || ch == L':' || ch == L'-') && mac_input_.size() < 17) {
            char c = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            mac_input_.push_back(c);
            invalidate();
        }
    }

    // ---------------- Window Procedure ----------------
    static LRESULT CALLBACK wnd_proc_static(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        MainWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<MainWindow*>(cs->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->wnd_proc(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (wm_taskbar_created_ != 0 && msg == wm_taskbar_created_) {
            init_tray();
            update_tray();
            return 0;
        }
        switch (msg) {
            case WM_SIZE:
                if (wp == SIZE_MINIMIZED) {
                    stop_anim_timer();
                    return 0;
                }
                if (rt_) {
                    rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
                }
                wake_anim();
                return 0;

            case WM_SHOWWINDOW:
                if (wp == FALSE) stop_anim_timer();
                else wake_anim();
                return 0;

            case WM_DPICHANGED: {
                UINT dpi = HIWORD(wp);
                if (rt_) rt_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
                const RECT* prc = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(hwnd, nullptr, prc->left, prc->top,
                             prc->right - prc->left, prc->bottom - prc->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                invalidate();
                return 0;
            }

            case WM_GETMINMAXINFO: {
                UINT dpi = GetDpiForWindow(hwnd);
                float sc = static_cast<float>(dpi) / 96.0f;
                auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
                mmi->ptMinTrackSize.x = static_cast<LONG>(960 * sc);
                mmi->ptMinTrackSize.y = static_cast<LONG>(680 * sc);
                return 0;
            }

            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT: {
                PAINTSTRUCT ps{};
                BeginPaint(hwnd, &ps);
                on_paint();
                EndPaint(hwnd, &ps);
                return 0;
            }

            case WM_MOUSEMOVE: {
                float dx = px_to_dip(GET_X_LPARAM(lp));
                float dy = px_to_dip(GET_Y_LPARAM(lp));
                if (!mouse_tracked_) {
                    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                    TrackMouseEvent(&tme);
                    mouse_tracked_ = true;
                }
                if (dragging_eq_band_ >= 0) {
                    update_eq_from_mouse(dragging_eq_band_, dy);
                    return 0;
                }
                int h = hit_test(dx, dy);
                if (h != hot_id_) {
                    hot_id_ = h;
                    wake_anim();
                    invalidate();
                }
                SetCursor(LoadCursorW(nullptr, h != HIT_NONE ? IDC_HAND : IDC_ARROW));
                return 0;
            }

            case WM_MOUSELEAVE:
                mouse_tracked_ = false;
                if (hot_id_ != HIT_NONE) {
                    hot_id_ = HIT_NONE;
                    wake_anim();
                    invalidate();
                }
                return 0;

            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) {
                    SetCursor(LoadCursorW(nullptr, hot_id_ != HIT_NONE ? IDC_HAND : IDC_ARROW));
                    return TRUE;
                }
                break;

            case WM_LBUTTONDOWN: {
                float dx = px_to_dip(GET_X_LPARAM(lp));
                float dy = px_to_dip(GET_Y_LPARAM(lp));
                int h = hit_test(dx, dy);
                if (h >= HIT_EQ_BAND_BASE && h < HIT_EQ_BAND_BASE + 10) {
                    dragging_eq_band_ = h - HIT_EQ_BAND_BASE;
                    SetCapture(hwnd);
                    update_eq_from_mouse(dragging_eq_band_, dy);
                    wake_anim();
                    return 0;
                }
                if (h != HIT_NONE) {
                    press_id_ = h;
                    press_anim_ = 1.0f;
                    wake_anim();
                }
                on_click(h, false);
                return 0;
            }

            case WM_LBUTTONDBLCLK: {
                float dx = px_to_dip(GET_X_LPARAM(lp));
                float dy = px_to_dip(GET_Y_LPARAM(lp));
                int h = hit_test(dx, dy);
                if (h >= HIT_DEV_ITEM_BASE && h < HIT_DEV_ITEM_BASE + 50) {
                    on_click(h, true);
                }
                return 0;
            }

            case WM_LBUTTONUP:
                if (dragging_eq_band_ >= 0) {
                    dragging_eq_band_ = -1;
                    ReleaseCapture();
                    if (session_.is_connected()) {
                        std::vector<EqBand> bands;
                        {
                            std::lock_guard<std::mutex> lk(data_mtx_);
                            bands = ui_eq_bands_;
                        }
                        worker_.post([this, bands] { session_.set_custom_eq(bands, true); });
                    }
                    wake_anim();
                    invalidate();
                }
                return 0;

            case WM_MOUSEWHEEL: {
                int delta = GET_WHEEL_DELTA_WPARAM(wp);
                if (active_tab_ == 3) {
                    log_scroll_ = std::max(0, log_scroll_ + (delta > 0 ? 3 : -3));
                    invalidate();
                }
                return 0;
            }

            case WM_CHAR:
                on_char(static_cast<wchar_t>(wp));
                return 0;

            case WM_KEYDOWN:
                if (wp == VK_ESCAPE && mac_focused_) {
                    mac_focused_ = false;
                    invalidate();
                    return 0;
                }
                break;

            case WM_CLOSE:
                if (!quitting_ && cfg_.close_to_tray) {
                    hide_to_tray();
                    return 0;
                }
                DestroyWindow(hwnd);
                return 0;

            case WM_TIMER:
                if (wp == TIMER_ANIM) {
                    if (!IsWindowVisible(hwnd_) || IsIconic(hwnd_)) {
                        stop_anim_timer();
                        return 0;
                    }
                    LARGE_INTEGER now{};
                    QueryPerformanceCounter(&now);
                    float dt = static_cast<float>(now.QuadPart - last_qpc_.QuadPart) /
                               static_cast<float>(std::max<LONGLONG>(1, qpc_freq_.QuadPart));
                    last_qpc_ = now;
                    dt = std::clamp(dt, 0.001f, 0.05f);

                    bool keep_running = step_animations(dt);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    if (!keep_running) {
                        stop_anim_timer();
                    }
                } else if (wp == TIMER_TOAST_CLEAR) {
                    KillTimer(hwnd, TIMER_TOAST_CLEAR);
                    toast_visible_ = false;
                    wake_anim();
                    invalidate();
                } else if (wp == TIMER_AUTORECONNECT) {
                    if (!session_.is_connected() && !busy_ && cfg_.auto_reconnect && !manual_disconnected_) {
                        worker_.post([this] {
                            if (session_.is_connected() || busy_ || is_xiaomi_manager_running()) return;
                            auto list = enumerate_paired_devices();
                            for (const auto& d : list) {
                                if (d.system_connected && (_stricmp(d.mac.c_str(), mac_input_.c_str()) == 0 || d.is_earphone_hint)) {
                                    connect_worker_impl(d.mac, true);
                                    return;
                                }
                            }
                        });
                    }
                }
                return 0;

            case WM_APP_STATE:
                update_tray();
                wake_anim();
                invalidate();
                return 0;

            case WM_APP_LOG:
                if (active_tab_ == 3) invalidate();
                return 0;

            case WM_APP_DEVICES:
                invalidate();
                return 0;

            case WM_APP_BUSY:
                wake_anim();
                invalidate();
                return 0;

            case WM_APP_TOAST:
                toast_visible_ = true;
                SetTimer(hwnd, TIMER_TOAST_CLEAR, 4200, nullptr);
                wake_anim();
                invalidate();
                return 0;

            case WM_APP_SHOW_ME:
                restore_window();
                return 0;

            case WM_APP_QUIT_FOR_UPDATE:
                really_quit();
                return 0;

            case WM_APP_TRAY:
                if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
                    if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                        hide_to_tray();
                    } else {
                        restore_window();
                    }
                } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                    show_tray_menu();
                }
                return 0;

            case WM_DESTROY:
                stop_anim_timer();
                KillTimer(hwnd, TIMER_AUTORECONNECT);
                KillTimer(hwnd, TIMER_TOAST_CLEAR);
                remove_tray();
                session_.close();
                worker_.stop();
                discard_rt();
                if (stroke_round_) { stroke_round_->Release(); stroke_round_ = nullptr; }
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW nid_{};
    UINT wm_taskbar_created_ = 0;

    ID2D1Factory* d2d_factory_ = nullptr;
    IDWriteFactory* dw_factory_ = nullptr;
    ID2D1StrokeStyle* stroke_round_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;

    IDWriteTextFormat* tf_brand_ = nullptr;
    IDWriteTextFormat* tf_h1_ = nullptr;
    IDWriteTextFormat* tf_h2_ = nullptr;
    IDWriteTextFormat* tf_body_ = nullptr;
    IDWriteTextFormat* tf_sub_ = nullptr;
    IDWriteTextFormat* tf_bat_big_ = nullptr;
    IDWriteTextFormat* tf_mono_ = nullptr;

    HICON icon_app_ = nullptr;
    HICON tray_disc_ = nullptr;
    HICON tray_off_ = nullptr;
    HICON tray_noise_ = nullptr;
    HICON tray_trans_ = nullptr;

    AppConfig cfg_;
    BtSession session_;
    WorkerQueue worker_;

    std::mutex data_mtx_;
    EarphoneSnapshot snap_;
    std::vector<PairedBtDevice> devices_;
    std::deque<std::wstring> logs_;
    std::wstring toast_msg_;
    bool toast_warn_ = false;
    std::wstring busy_text_;

    std::atomic<bool> busy_{false};
    std::atomic<bool> manual_disconnected_{false};
    bool quitting_ = false;
    bool autostart_cached_ = false;
    bool mouse_tracked_ = false;

    std::string mac_input_;
    bool mac_focused_ = false;
    int active_tab_ = 0;
    int hot_id_ = HIT_NONE;
    int press_id_ = HIT_NONE;
    int log_scroll_ = 0;

    std::vector<EqBand> ui_eq_bands_ = default_10band_eq();
    std::vector<D2D1_RECT_F> eq_track_rects_;
    int dragging_eq_band_ = -1;
    std::atomic<bool> eq_dirty_{false};

    // Fluid animation state
    LARGE_INTEGER qpc_freq_{};
    LARGE_INTEGER last_qpc_{};
    bool anim_timer_on_ = false;
    float theme_t_ = 0.0f;
    float tab_pill_pos_ = 0.0f;
    float page_enter_t_ = 1.0f;
    float toast_anim_ = 0.0f;
    bool toast_visible_ = false;
    float bat_anim_[3] = {0.0f, 0.0f, 0.0f};
    float sw_anim_[11] = {1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float eq_anim_[10] = {0.0f};
    std::map<int, float> hover_map_;
    float press_anim_ = 0.0f;
    float refresh_spin_deg_ = 0.0f;
    float refresh_spin_target_ = 0.0f;
    float ring_pulse_rem_ = 0.0f;
    float ambient_phase_ = 0.0f;

    std::vector<HitZone> hits_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int) {
    std::wstring args = lpCmdLine ? lpCmdLine : L"";
    if (args.find(L"--quit") != std::wstring::npos) {
        HWND existing = FindWindowW(kWndClass, nullptr);
        if (existing) {
            PostMessageW(existing, WM_APP_QUIT_FOR_UPDATE, 0, 0);
        }
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Single-instance check: wake up existing window if already running
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\MiEarphonePC_Native_Singleton_v2");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kWndClass, nullptr);
        if (existing) {
            PostMessageW(existing, WM_APP_SHOW_ME, 0, 0);
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    bool start_in_tray = (args.find(L"--tray") != std::wstring::npos ||
                          args.find(L"--minimized") != std::wstring::npos);

    MainWindow app;
    if (!app.init(hInstance, start_in_tray)) {
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }
    int rc = app.message_loop();
    if (hMutex) CloseHandle(hMutex);
    return rc;
}
