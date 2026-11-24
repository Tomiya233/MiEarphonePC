#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2bth.h>
#include <windows.h>
#include <bluetoothapis.h>
#include <tlhelp32.h>
#include <bcrypt.h>

#include "bt_session.hpp"
#include "auth_cipher.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace miearphone {

namespace {

struct WsaGuard {
    WsaGuard() {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
};

void ensure_wsa() {
    static WsaGuard g;
    (void)g;
}

std::string wide_to_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

bool has_earphone_keyword(const std::string& name) {
    std::string low = name;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* kHints[] = {"buds", "redmi", "xiaomi", "mi ", "earphone", "flipbuds", "air"};
    for (const char* h : kHints) {
        if (low.find(h) != std::string::npos) return true;
    }
    return false;
}

// Primary MIUI SPP (Airoha GATT-over-RFCOMM) service UUID:
// 0000FD2D-0000-1000-8000-00805F9B34FB. Connecting here means auth-with-command.
constexpr GUID kServiceFd2d = {
    0x0000FD2D, 0x0000, 0x1000,
    {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}
};

// XiaoAi SPP record: 00001101-0000-1000-8000-008584D01810 (APK UUID_SPP_XIAOAI).
// Fallback channels keep auth-with-command = false, i.e. raw stream auth.
constexpr GUID kServiceXiaoAiSpp = {
    0x00001101, 0x0000, 0x1000,
    {0x80, 0x00, 0x00, 0x85, 0x84, 0xD0, 0x18, 0x10}
};

// Fallback Standard SPP UUID: 00001101-0000-1000-8000-00805F9B34FB
constexpr GUID kServiceSpp = {
    0x00001101, 0x0000, 0x1000,
    {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}
};

void fill_random_16(uint8_t out[16]) {
    if (BCryptGenRandom(nullptr, out, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
        return;
    }
    uint64_t t = GetTickCount64();
    for (int i = 0; i < 16; ++i) {
        t = t * 6364136223846793005ULL + 1442695040888963407ULL;
        out[i] = static_cast<uint8_t>((t >> 32) & 0xFF);
    }
}

} // namespace

bool parse_mac_u64(const std::string& mac, uint64_t& out_addr) {
    std::string hex;
    hex.reserve(12);
    for (char c : mac) {
        if (c == ':' || c == '-' || c == ' ') continue;
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        hex.push_back(c);
    }
    if (hex.size() != 12) return false;
    out_addr = std::strtoull(hex.c_str(), nullptr, 16);
    return true;
}

std::string format_mac_u64(uint64_t addr) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                  static_cast<unsigned>((addr >> 40) & 0xFF),
                  static_cast<unsigned>((addr >> 32) & 0xFF),
                  static_cast<unsigned>((addr >> 24) & 0xFF),
                  static_cast<unsigned>((addr >> 16) & 0xFF),
                  static_cast<unsigned>((addr >> 8) & 0xFF),
                  static_cast<unsigned>(addr & 0xFF));
    return buf;
}

std::vector<PairedBtDevice> enumerate_paired_devices() {
    std::vector<PairedBtDevice> out;
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp{};
    sp.dwSize = sizeof(sp);
    sp.fReturnAuthenticated = TRUE;
    sp.fReturnRemembered = TRUE;
    sp.fReturnConnected = TRUE;
    sp.fReturnUnknown = FALSE;
    sp.fIssueInquiry = FALSE;
    sp.cTimeoutMultiplier = 0;

    BLUETOOTH_DEVICE_INFO bdi{};
    bdi.dwSize = sizeof(bdi);

    HBLUETOOTH_DEVICE_FIND hFind = BluetoothFindFirstDevice(&sp, &bdi);
    if (hFind) {
        do {
            PairedBtDevice d;
            d.name = wide_to_utf8(bdi.szName);
            d.mac = format_mac_u64(bdi.Address.ullLong);
            if (d.name.empty()) d.name = d.mac;
            d.system_connected = (bdi.fConnected != FALSE);
            d.is_earphone_hint = has_earphone_keyword(d.name);
            out.push_back(std::move(d));
        } while (BluetoothFindNextDevice(hFind, &bdi));
        BluetoothFindDeviceClose(hFind);
    }

    std::sort(out.begin(), out.end(), [](const PairedBtDevice& a, const PairedBtDevice& b) {
        if (a.is_earphone_hint != b.is_earphone_hint) return a.is_earphone_hint > b.is_earphone_hint;
        if (a.system_connected != b.system_connected) return a.system_connected > b.system_connected;
        return a.name < b.name;
    });
    return out;
}

bool is_xiaomi_manager_running() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"XiaomiPcManager.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

BtSession::BtSession() {
    ensure_wsa();
}

BtSession::~BtSession() {
    close();
}

void BtSession::set_callbacks(LogCallback on_log,
                              StateCallback on_state,
                              DisconnectCallback on_disconnect) {
    std::lock_guard<std::mutex> lk(cb_mtx_);
    on_log_ = std::move(on_log);
    on_state_ = std::move(on_state);
    on_disconnect_ = std::move(on_disconnect);
}

EarphoneSnapshot BtSession::snapshot() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    return state_;
}

uint8_t BtSession::next_sn() {
    uint8_t v = ++sn_;
    if (v == 0) v = ++sn_;
    return v;
}

void BtSession::emit_log(const std::string& s) {
    LogCallback cb;
    {
        std::lock_guard<std::mutex> lk(cb_mtx_);
        cb = on_log_;
    }
    if (cb) cb(s);
}

void BtSession::emit_state() {
    StateCallback cb;
    {
        std::lock_guard<std::mutex> lk(cb_mtx_);
        cb = on_state_;
    }
    if (cb) cb(snapshot());
}

bool BtSession::connect_and_auth(const std::string& mac, int connect_timeout_ms, int* out_wsa_err) {
    close();

    uint64_t bt_addr = 0;
    if (!parse_mac_u64(mac, bt_addr)) {
        emit_log("[错误] MAC 地址格式无效: " + mac);
        if (out_wsa_err) *out_wsa_err = WSAEINVAL;
        return false;
    }

    std::string norm_mac = format_mac_u64(bt_addr);
    auto try_connect_uuid = [&](const GUID& uuid, const char* tag, int& err_out) -> SOCKET {
        SOCKET sock = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (sock == INVALID_SOCKET) {
            err_out = WSAGetLastError();
            return INVALID_SOCKET;
        }
        SOCKADDR_BTH sa{};
        sa.addressFamily = AF_BTH;
        sa.btAddr = bt_addr;
        sa.serviceClassId = uuid;
        sa.port = 0;

        u_long nb = 1;
        ioctlsocket(sock, FIONBIO, &nb);
        int rc = ::connect(sock, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
        if (rc == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
                fd_set wfds, efds;
                FD_ZERO(&wfds);
                FD_ZERO(&efds);
                FD_SET(sock, &wfds);
                FD_SET(sock, &efds);
                timeval tv{};
                tv.tv_sec = connect_timeout_ms / 1000;
                tv.tv_usec = (connect_timeout_ms % 1000) * 1000;
                int sel = ::select(0, nullptr, &wfds, &efds, &tv);
                if (sel <= 0 || FD_ISSET(sock, &efds) || !FD_ISSET(sock, &wfds)) {
                    int so_err = WSAETIMEDOUT;
                    int optlen = sizeof(so_err);
                    getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &optlen);
                    if (so_err == 0) so_err = WSAETIMEDOUT;
                    err_out = so_err;
                    ::closesocket(sock);
                    return INVALID_SOCKET;
                }
            } else {
                err_out = err;
                ::closesocket(sock);
                return INVALID_SOCKET;
            }
        }
        (void)tag;
        err_out = 0;
        return sock;
    };

    // Channel order mirrors the official APK (ua/i.java ConnectSppUUID.run):
    //   FD2D (MIUI SPP)  -> framed RCSP auth
    //   XiaoAi SPP       -> raw stream auth
    //   Standard SPP     -> raw stream auth
    // Trying only FD2D made Xiaomi-branded models fail: they expose no FD2D
    // service and fall back to a channel whose auth is unframed.
    struct ChannelAttempt {
        const GUID* uuid;
        ChannelKind kind;
        AuthMode mode;
        const char* label;
    };
    static const ChannelAttempt kAttempts[] = {
        {&kServiceFd2d, ChannelKind::MiuiSppFd2d, AuthMode::Framed, "FD2D (MIUI SPP)"},
        {&kServiceXiaoAiSpp, ChannelKind::XiaoAiSpp, AuthMode::RawStream, "1101 (XiaoAi SPP)"},
        {&kServiceSpp, ChannelKind::StandardSpp, AuthMode::RawStream, "1101 (Standard SPP)"},
    };

    SOCKET s = INVALID_SOCKET;
    int last_err = 0;
    channel_ = ChannelKind::Unknown;
    auth_mode_ = AuthMode::None;

    for (const auto& attempt : kAttempts) {
        emit_log(">>> RFCOMM 连接 " + norm_mac + "  [" + attempt.label + "]");
        int err = 0;
        SOCKET candidate = try_connect_uuid(*attempt.uuid, attempt.label, err);
        if (candidate != INVALID_SOCKET) {
            s = candidate;
            channel_ = attempt.kind;
            auth_mode_ = attempt.mode;
            last_err = 0;
            break;
        }
        last_err = err;
        // 10049/10108 = service not registered on the peer; other errors mean the
        // link itself is down, so stop retrying further service records.
        if (err != 10049 && err != 10108) {
            emit_log("    " + std::string(attempt.label) + " 连接失败 WSA=" + std::to_string(err));
            break;
        }
        emit_log("    " + std::string(attempt.label) + " 未注册，尝试下一个通道...");
    }

    if (s == INVALID_SOCKET) {
        if (out_wsa_err) *out_wsa_err = last_err;
        emit_log("[失败] 连接失败 WSA=" + std::to_string(last_err));
        return false;
    }
    emit_log(std::string("通道: ") + channel_kind_name(channel_) +
             " · 认证方式: " + auth_mode_name(auth_mode_));

    u_long nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    DWORD rcv_to = 400;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv_to), sizeof(rcv_to));
    DWORD snd_to = 2500;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&snd_to), sizeof(snd_to));

    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        state_ = EarphoneSnapshot{};
        state_.connected = true;
        state_.mac = norm_mac;
        state_.name = norm_mac;
        state_.channel = channel_;
        state_.auth_mode = auth_mode_;
    }

    {
        std::lock_guard<std::mutex> lk(send_mtx_);
        sock_ = static_cast<uintptr_t>(s);
    }
    alive_.store(true);
    dev_challenged_.store(false);
    {
        std::lock_guard<std::mutex> lk(raw_mtx_);
        raw_inbox_.clear();
        raw_stage_ = 0;
        raw_ok_ = false;
    }
    drain_inbox();

    rx_thread_ = std::thread(&BtSession::rx_loop, this);

    emit_log("RFCOMM 链路已建立 ✓");
    emit_state();

    if (!reauthenticate()) {
        emit_log("[警告] 认证握手未完成，部分控制可能受限");
    }
    refresh_all();
    if (out_wsa_err) *out_wsa_err = 0;
    return true;
}

void BtSession::close() {
    bool was_alive = alive_.exchange(false);
    inbox_cv_.notify_all();

    SOCKET s = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lk(send_mtx_);
        if (sock_ != ~uintptr_t(0)) {
            s = static_cast<SOCKET>(sock_);
            sock_ = ~uintptr_t(0);
        }
    }
    if (s != INVALID_SOCKET) {
        ::shutdown(s, SD_BOTH);
        ::closesocket(s);
    }
    if (rx_thread_.joinable() && rx_thread_.get_id() != std::this_thread::get_id()) {
        rx_thread_.join();
    }
    if (was_alive) {
        std::lock_guard<std::mutex> lk(state_mtx_);
        state_.connected = false;
        state_.authenticated = false;
    }
}

void BtSession::fail_link(const std::string& reason) {
    bool expected = true;
    if (!alive_.compare_exchange_strong(expected, false)) {
        return;
    }
    inbox_cv_.notify_all();
    {
        std::lock_guard<std::mutex> lk(send_mtx_);
        if (sock_ != ~uintptr_t(0)) {
            SOCKET s = static_cast<SOCKET>(sock_);
            sock_ = ~uintptr_t(0);
            ::shutdown(s, SD_BOTH);
            ::closesocket(s);
        }
    }
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        state_.connected = false;
        state_.authenticated = false;
    }
    emit_log("[连接断开] " + reason);
    emit_state();
    DisconnectCallback cb;
    {
        std::lock_guard<std::mutex> lk(cb_mtx_);
        cb = on_disconnect_;
    }
    if (cb) cb(reason);
}

bool BtSession::send_raw(const std::vector<uint8_t>& raw) {
    std::lock_guard<std::mutex> lk(send_mtx_);
    if (!alive_.load() || sock_ == ~uintptr_t(0)) return false;
    SOCKET s = static_cast<SOCKET>(sock_);
    size_t sent = 0;
    while (sent < raw.size()) {
        int n = ::send(s, reinterpret_cast<const char*>(raw.data() + sent),
                       static_cast<int>(raw.size() - sent), 0);
        if (n <= 0) {
            emit_log("[发送失败] WSA=" + std::to_string(WSAGetLastError()));
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    emit_log("→ " + to_hex(raw));
    return true;
}

bool BtSession::send_command(uint8_t opcode, const std::vector<uint8_t>& payload,
                             uint8_t app, std::optional<uint8_t> fixed_sn) {
    uint8_t s = fixed_sn.has_value() ? *fixed_sn : next_sn();
    auto raw = build_command(opcode, payload, s, app, true);
    return send_raw(raw);
}

bool BtSession::send_response(uint8_t opcode, uint8_t sn,
                              const std::vector<uint8_t>& business, uint8_t status) {
    auto raw = build_response(opcode, sn, business, status);
    return send_raw(raw);
}

void BtSession::drain_inbox() {
    std::lock_guard<std::mutex> lk(inbox_mtx_);
    inbox_.clear();
}

std::optional<RspFrame> BtSession::wait_response(uint8_t opcode, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lk(inbox_mtx_);
    while (alive_.load()) {
        for (auto it = inbox_.begin(); it != inbox_.end(); ++it) {
            if (!it->is_command() && it->opcode == opcode) {
                RspFrame res = std::move(*it);
                inbox_.erase(it);
                return res;
            }
        }
        if (inbox_cv_.wait_until(lk, deadline) == std::cv_status::timeout) {
            break;
        }
    }
    return std::nullopt;
}

void BtSession::rx_loop() {
    std::vector<uint8_t> rx_buf;
    rx_buf.reserve(4096);
    uint8_t chunk[1024];

    while (alive_.load()) {
        SOCKET s = static_cast<SOCKET>(sock_);
        if (s == INVALID_SOCKET) break;
        int n = ::recv(s, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
        if (n == 0) {
            fail_link("耳机主动关闭了连接");
            return;
        }
        if (n < 0) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
                continue;
            }
            if (alive_.load()) {
                fail_link("蓝牙链路中断 (WSA=" + std::to_string(err) + ")");
            }
            return;
        }
        rx_buf.insert(rx_buf.end(), chunk, chunk + n);

        // Demux: a legacy/raw-auth channel sends unframed 17/5-byte packets
        // (APK BluetoothAuth.k), everything else is a framed RCSP packet.
        while (!rx_buf.empty()) {
            size_t raw_len = 0;
            if (auth_mode_ == AuthMode::RawStream || !raw_ok_) {
                raw_len = raw_auth_packet_len(rx_buf);
            }
            if (raw_len > 0 && rx_buf.size() >= raw_len) {
                std::vector<uint8_t> pkt(rx_buf.begin(), rx_buf.begin() + static_cast<ptrdiff_t>(raw_len));
                rx_buf.erase(rx_buf.begin(), rx_buf.begin() + static_cast<ptrdiff_t>(raw_len));
                emit_log("← [raw] " + to_hex(pkt));
                if (!handle_raw_auth_packet(pkt)) {
                    emit_log("   [raw] 未匹配到认证状态机，丢弃");
                }
                continue;
            }
            // Raw packet still incomplete: wait for the rest.
            if (raw_len > 0 && rx_buf.size() < raw_len) break;
            if (!rx_buf.empty() && rx_buf[0] != FRAME_HEAD[0]) {
                // Not a frame head and not a raw auth packet: resync.
                auto it = std::find(rx_buf.begin(), rx_buf.end(), FRAME_HEAD[0]);
                if (it == rx_buf.end()) {
                    emit_log("   [丢弃] 无法识别的数据 " + to_hex(rx_buf));
                    rx_buf.clear();
                } else if (it != rx_buf.begin()) {
                    rx_buf.erase(rx_buf.begin(), it);
                }
                continue;
            }
            break;
        }

        auto frames = extract_frames(rx_buf);
        for (const auto& raw : frames) {
            auto parsed = parse_frame(raw);
            if (!parsed) continue;
            emit_log("← " + to_hex(raw));
            if (parsed->is_command()) {
                handle_device_command(*parsed);
            } else {
                handle_device_response(*parsed);
            }
        }
    }
}

void BtSession::handle_device_command(const RspFrame& f) {
    if (f.opcode == CMD_AUTH_CHECK) {
        if (f.payload.size() >= 17) {
            auto ans = auth_response(f.payload.data() + 1);
            std::vector<uint8_t> bus;
            bus.reserve(17);
            bus.push_back(0x01);
            bus.insert(bus.end(), ans.begin(), ans.end());
            emit_log("   [双向认证] 收到耳机反向挑战 → 已计算并回复应答 ✓");
            send_response(f.opcode, f.sn, bus, 0);
            dev_challenged_.store(true);
        } else {
            send_response(f.opcode, f.sn);
        }
        return;
    }
    if (f.opcode == CMD_AUTH_SEND_CALC_RESULT) {
        send_response(f.opcode, f.sn, {0x01}, 0);
        return;
    }
    if (f.opcode == CMD_NOTIFY_DEVICE_CONFIG) {
        auto cfgs = decode_configs(f.payload);
        for (const auto& c : cfgs) {
            emit_log(std::string("   [设备上报] ") + config_name(c.type) +
                     " (id=" + std::to_string(c.type) + ") = " + to_hex(c.value));
            apply_config_update(c.type, c.value);
        }
        report_seq_.fetch_add(1);
        send_response(f.opcode, f.sn);
        emit_state();
        return;
    }
    if (f.opcode == CMD_REPORT_DEVICE_STATUS) {
        auto attrs = parse_tlv_attrs(f.payload);
        apply_status_attrs(attrs);
        send_response(f.opcode, f.sn);
        emit_state();
        return;
    }
    // Always ACK any other spontaneous command so device never stalls
    send_response(f.opcode, f.sn);
}

void BtSession::handle_device_response(const RspFrame& f) {
    bool changed = false;
    if (f.opcode == CMD_GET_DEVICE_CONFIG && f.ok()) {
        auto cfgs = decode_configs(f.payload);
        for (const auto& c : cfgs) {
            apply_config_update(c.type, c.value);
            changed = true;
        }
    } else if (f.opcode == CMD_GET_TARGET_INFO && f.ok()) {
        auto attrs = parse_tlv_attrs(f.payload);
        apply_target_info_attrs(attrs);
        changed = true;
    }

    {
        std::lock_guard<std::mutex> lk(inbox_mtx_);
        if (inbox_.size() > 64) inbox_.pop_front();
        inbox_.push_back(f);
    }
    inbox_cv_.notify_all();

    if (changed) emit_state();
}

void BtSession::apply_config_update(uint16_t type, const std::vector<uint8_t>& val) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (val.empty()) return;
    switch (type) {
        case CFG_NOISE_LEVEL_CHOOSE:
            if (val.size() >= 2) {
                state_.anc_mode = val[0];
                state_.anc_level = val[1];
            }
            break;
        case CFG_EQ_MODEL:
            state_.eq_mode = val[0];
            break;
        case CFG_CUSTOM_EQ: {
            auto parsed = parse_custom_eq(val);
            if (parsed) {
                state_.custom_eq = *parsed;
                state_.eq_mode = static_cast<uint8_t>(parsed->mode);
            }
            break;
        }
        case CFG_SPATIAL_AUDIO_CONFIG:
        case CFG_SWITCH_SPATIAL_AUDIO:
            state_.spatial = parse_spatial_audio_config(val);
            break;
        case CFG_VIRTUAL_SURROUND:
            state_.spatial.virtual_surround = (val[0] == 1);
            break;
        case CFG_SCENE_RENDERING:
            state_.scene = parse_scene_rendering_config(val);
            break;
        case CFG_LOW_LATENCY:
            state_.low_latency = (val[0] == 1);
            break;
        case CFG_MULTIPOINT:
            state_.multipoint = (val[0] == 1);
            break;
        case CFG_PERSONALIZED_ANC:
            state_.personalized_anc = (val[0] == 1);
            break;
        case CFG_ADAPTIVE_VOLUME:
            state_.adaptive_volume = (val[0] == 1);
            break;
        case CFG_AUTO_NOISE:
            state_.auto_noise = (val[0] == 1);
            break;
        case CFG_AUTO_ANSWER_PHONE:
            state_.auto_answer = (val[0] == 1);
            break;
        case CFG_EARPHONE_SN: {
            std::string s(reinterpret_cast<const char*>(val.data()), val.size());
            while (!s.empty() && s.back() == '\0') s.pop_back();
            if (!s.empty()) state_.sn = s;
            break;
        }
        default:
            break;
    }
}

void BtSession::apply_target_info_attrs(const std::map<uint8_t, std::vector<uint8_t>>& attrs) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (auto it = attrs.find(TARGET_ATTR_NAME); it != attrs.end() && !it->second.empty()) {
        std::string nm(reinterpret_cast<const char*>(it->second.data()), it->second.size());
        while (!nm.empty() && nm.back() == '\0') nm.pop_back();
        if (!nm.empty()) state_.name = nm;
    }
    if (auto it = attrs.find(TARGET_ATTR_VERSION); it != attrs.end() && it->second.size() >= 2) {
        uint8_t b0 = it->second[0], b1 = it->second[1];
        char vbuf[32];
        std::snprintf(vbuf, sizeof(vbuf), "%X.%X.%X.%X",
                      (b0 >> 4) & 0xF, b0 & 0xF, (b1 >> 4) & 0xF, b1 & 0xF);
        state_.fw_version = vbuf;
    }
    if (auto it = attrs.find(TARGET_ATTR_VID_PID); it != attrs.end() && it->second.size() >= 4) {
        state_.vid = static_cast<uint16_t>((it->second[0] << 8) | it->second[1]);
        state_.pid = static_cast<uint16_t>((it->second[2] << 8) | it->second[3]);
        // Resolve brand/model so the UI can tell 小米 from 红米 and the log can
        // show whether the model is in the adaptation table at all.
        if (auto prof = find_device_profile(state_.vid, state_.pid)) {
            state_.brand = prof->brand;
            state_.model_name = prof->market_name;
        } else {
            state_.brand = vendor_display_name(state_.vid);
            state_.model_name.clear();
        }
    }
    if (auto it = attrs.find(TARGET_ATTR_QUANTITY); it != attrs.end() && !it->second.empty()) {
        BatteryUnit u = decode_battery_byte(it->second[0]);
        if (u.online()) state_.battery.overall = u.percent;
    }
    if (auto it = attrs.find(TARGET_ATTR_MUL_QUANTITY); it != attrs.end() && !it->second.empty()) {
        update_battery_from_mul(state_.battery, it->second);
    }
}

void BtSession::apply_status_attrs(const std::map<uint8_t, std::vector<uint8_t>>& attrs) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (auto it = attrs.find(STATUS_ATTR_QUANTITY); it != attrs.end() && !it->second.empty()) {
        if (it->second.size() >= 2) {
            update_battery_from_mul(state_.battery, it->second);
        } else {
            BatteryUnit u = decode_battery_byte(it->second[0]);
            if (u.online()) state_.battery.overall = u.percent;
        }
    }
    // Note: Do NOT overwrite state_.anc_mode from STATUS_ATTR_ANC (0x0E attr 4) — when earbuds
    // are out of ear, 0x0E reports physical DSP standby (0) even while CFG 11 is Noise/Trans.
    // Official NoiseReductionView tracks CFG 11 (0xF3 / 0xF4) exclusively.
}

std::optional<DeviceProfile> BtSession::profile() const {
    EarphoneSnapshot s = snapshot();
    return find_device_profile(s.vid, s.pid);
}

bool BtSession::reauthenticate() {
    // Pick the auth encoding the channel expects; if it fails, retry with the
    // other one. Xiaomi-branded models answer only one of the two, and the
    // channel -> encoding mapping is a heuristic (FD2D => framed) that some
    // firmware violates.
    AuthMode first = (auth_mode_ == AuthMode::RawStream) ? AuthMode::RawStream : AuthMode::Framed;
    AuthMode second = (first == AuthMode::Framed) ? AuthMode::RawStream : AuthMode::Framed;

    for (int attempt = 0; attempt < 2; ++attempt) {
        AuthMode mode = (attempt == 0) ? first : second;
        if (mode == AuthMode::RawStream) {
            if (authenticate_raw_stream()) return true;
        } else {
            if (authenticate_rcsp_cmd()) return true;
        }
        if (!alive_.load()) return false;
        emit_log(std::string("   ") + auth_mode_name(mode) +
                 " 认证未通过，改用 " + auth_mode_name((mode == AuthMode::Framed) ? AuthMode::RawStream : AuthMode::Framed) + " 重试...");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return false;
}

bool BtSession::authenticate_rcsp_cmd() {
    drain_inbox();
    dev_challenged_.store(false);

    uint8_t rnd[16];
    fill_random_16(rnd);

    emit_log("=== RCSP 帧式双向认证 ===");
    std::vector<uint8_t> req;
    req.reserve(17);
    req.push_back(0x01);
    req.insert(req.end(), rnd, rnd + 16);

    if (!send_command(CMD_AUTH_CHECK, req, APP_AUTH)) return false;
    auto r1 = wait_response(CMD_AUTH_CHECK, 3500);
    if (!r1 || r1->payload.size() < 17) {
        emit_log("✗ AUTH_CHECK 未收到有效响应");
        return false;
    }

    auto expected = auth_response(rnd);
    bool match = (std::memcmp(r1->payload.data() + 1, expected.data(), 16) == 0);
    emit_log(std::string("   设备应答校验: ") + (match ? "逐字节一致 ✓" : "不一致 ✗"));

    send_command(CMD_AUTH_SEND_CALC_RESULT, {0x01, 0x00}, APP_AUTH);
    wait_response(CMD_AUTH_SEND_CALC_RESULT, 2000);

    // Wait briefly for device's reverse challenge if it hasn't arrived yet
    auto wait_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    while (alive_.load() && !dev_challenged_.load() && std::chrono::steady_clock::now() < wait_until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    auth_mode_ = AuthMode::Framed;
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        state_.authenticated = match;
        state_.auth_mode = auth_mode_;
    }
    emit_state();
    emit_log(match ? "=== 认证通过 ✓ ===" : "=== 认证未通过 ===");
    return match;
}

// Raw (unframed) authentication, ported from APK BluetoothAuth.s()/k():
//   stage 0 -> we push a 16B challenge, device echoes [0x01][enc16]
//   stage 1 -> device pushes [0x00][rnd16]; we answer [0x01][enc16]
//   stage 2 -> device confirms; auth stage becomes OK
bool BtSession::authenticate_raw_stream() {
    {
        std::lock_guard<std::mutex> lk(raw_mtx_);
        raw_inbox_.clear();
        raw_stage_ = 0;
        raw_ok_ = false;
    }
    drain_inbox();

    emit_log("=== 裸流式认证 (raw stream) ===");

    uint8_t rnd[16];
    fill_random_16(rnd);
    auto enc = auth_response(rnd);
    {
        std::lock_guard<std::mutex> lk(raw_mtx_);
        std::memcpy(raw_challenge_.data(), rnd, 16);
        std::memcpy(raw_challenge_enc_.data(), enc.data(), 16);
        raw_stage_ = 1;
    }

    // Stage 0: send our challenge unframed.
    if (!send_raw(std::vector<uint8_t>(rnd, rnd + 16))) return false;

    auto recv_raw = [&](int timeout_ms) -> std::optional<std::vector<uint8_t>> {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        std::unique_lock<std::mutex> lk(raw_mtx_);
        while (alive_.load()) {
            if (!raw_inbox_.empty()) {
                auto pkt = std::move(raw_inbox_.front());
                raw_inbox_.pop_front();
                return pkt;
            }
            if (raw_cv_.wait_until(lk, deadline) == std::cv_status::timeout) break;
        }
        return std::nullopt;
    };

    auto pkt = recv_raw(3000);
    if (!pkt) {
        emit_log("✗ 裸流认证: 设备未回应挑战");
        return false;
    }

    bool device_proved_key = false;
    bool device_challenged = false;
    std::array<uint8_t, 16> dev_rnd{};

    if (pkt->size() == 17 && (*pkt)[0] == 0x01) {
        device_proved_key = (std::memcmp(pkt->data() + 1, raw_challenge_enc_.data(), 16) == 0);
        emit_log(std::string("   设备回显加密挑战: ") + (device_proved_key ? "一致 ✓" : "不一致 ✗"));
    } else if (pkt->size() == 17 && (*pkt)[0] == 0x00) {
        device_challenged = true;
        std::memcpy(dev_rnd.data(), pkt->data() + 1, 16);
    } else if (pkt->size() == 5 && (*pkt)[0] == 0x02) {
        emit_log("   设备回送 pass 令牌 → 视为认证通过");
        device_proved_key = true;
    }

    // Answer a device challenge at most twice (device may challenge immediately).
    for (int round = 0; round < 2; ++round) {
        if (device_challenged) {
            auto ans = auth_response(dev_rnd.data());
            auto out = make_raw_auth_packet(0x01, ans.data());
            emit_log("   → 应答设备挑战 " + to_hex(out));
            if (!send_raw(out)) return false;
            {
                std::lock_guard<std::mutex> lk(raw_mtx_);
                raw_stage_ = 2;
            }
            device_challenged = false;
        }
        auto next = recv_raw(round == 0 ? 2000 : 1200);
        if (!next) break;
        if (next->size() == 17 && (*next)[0] == 0x00) {
            device_challenged = true;
            std::memcpy(dev_rnd.data(), next->data() + 1, 16);
            continue;
        }
        if (next->size() == 17 && (*next)[0] == 0x01) {
            device_proved_key = device_proved_key ||
                                (std::memcmp(next->data() + 1, raw_challenge_enc_.data(), 16) == 0);
            continue;
        }
        if (next->size() == 5 && (*next)[0] == 0x02) {
            device_proved_key = true;
            break;
        }
        // Framed data on a raw channel means the device already switched to
        // command mode; treat that as success and let the frame path take over.
        if (!next->empty() && (*next)[0] == FRAME_HEAD[0]) {
            emit_log("   收到 RCSP 帧 → 设备已切到命令模式");
            device_proved_key = true;
        }
    }

    // Raw channels do not carry an explicit ACK for our final answer, so the
    // state of the exchange is the best signal available.
    bool ok = device_proved_key;
    {
        std::lock_guard<std::mutex> lk(raw_mtx_);
        raw_ok_ = ok;
        if (ok) raw_stage_ = 3;
    }
    auth_mode_ = AuthMode::RawStream;
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        state_.authenticated = ok;
        state_.auth_mode = auth_mode_;
    }
    emit_state();
    emit_log(ok ? "=== 裸流认证通过 ✓ ===" : "=== 裸流认证未通过 ===");
    return ok;
}

bool BtSession::handle_raw_auth_packet(const std::vector<uint8_t>& pkt) {
    if (pkt.empty()) return false;
    {
        std::lock_guard<std::mutex> lk(raw_mtx_);
        if (raw_inbox_.size() > 32) raw_inbox_.pop_front();
        if (raw_inbox_.empty() || raw_inbox_.back() != pkt) raw_inbox_.push_back(pkt);
    }
    raw_cv_.notify_all();
    return true;
}

bool BtSession::refresh_battery() {
    drain_inbox();
    for (int i = 0; i < 2; ++i) {
        if (!send_command(CMD_GET_TARGET_INFO, {0xFF, 0xFF, 0xFF, 0xFF}, APP_AUTH, uint8_t(0))) {
            return false;
        }
        if (wait_response(CMD_GET_TARGET_INFO, 2000).has_value()) {
            return true;
        }
    }
    return false;
}

bool BtSession::query_configs_sync(const std::vector<uint16_t>& ids, int timeout_ms) {
    if (ids.empty()) return true;
    std::vector<uint8_t> payload;
    payload.reserve(ids.size() * 2);
    for (uint16_t id : ids) {
        payload.push_back(static_cast<uint8_t>((id >> 8) & 0xFF));
        payload.push_back(static_cast<uint8_t>(id & 0xFF));
    }
    drain_inbox();
    if (!send_command(CMD_GET_DEVICE_CONFIG, payload, APP_CONFIG)) return false;
    return wait_response(CMD_GET_DEVICE_CONFIG, timeout_ms).has_value();
}

bool BtSession::refresh_all() {
    bool ok = refresh_battery();
    // Query core configs in compact groups so every firmware responds cleanly
    query_configs_sync({CFG_NOISE_LEVEL_CHOOSE, CFG_EQ_MODEL, CFG_LOW_LATENCY, CFG_MULTIPOINT}, 2000);
    query_configs_sync({CFG_SPATIAL_AUDIO_CONFIG, CFG_SCENE_RENDERING, CFG_PERSONALIZED_ANC,
                        CFG_ADAPTIVE_VOLUME, CFG_AUTO_NOISE, CFG_AUTO_ANSWER_PHONE}, 2000);
    query_configs_sync({CFG_CUSTOM_EQ}, 2000);
    query_configs_sync({CFG_EARPHONE_SN}, 1500);
    emit_state();
    return ok;
}

bool BtSession::set_config_sync(const CommonConfig& cfg, int timeout_ms) {
    uint64_t before_seq = report_seq_.load();
    drain_inbox();
    if (!send_command(CMD_SET_DEVICE_CONFIG, cfg.encode(), APP_CONFIG)) {
        return false;
    }
    auto rsp = wait_response(CMD_SET_DEVICE_CONFIG, timeout_ms);
    if (rsp && rsp->ok()) {
        return true;
    }
    // Some configs push 0xF4 NOTIFY_DEVICE_CONFIG without a direct 0xF2 response
    return report_seq_.load() != before_seq;
}

bool BtSession::set_anc(uint8_t mode, uint8_t level, bool keep_auto_noise) {
    // Sanitize level strictly to valid gears (0..2) so N76 firmware never rejects and drops ANC!
    if (mode == ANC_MODE_NOISE) {
        if (level > ANC_LEVEL_DEEP) level = ANC_LEVEL_DEEP;
        bool was_auto = snapshot().auto_noise;
        if (!keep_auto_noise && was_auto) {
            // Switching to a manual gear (轻度/均衡/深度) disables Adaptive ANC (CFG 37)
            if (set_config_sync(make_switch_config(CFG_AUTO_NOISE, false), 1500)) {
                std::lock_guard<std::mutex> lk(state_mtx_);
                state_.auto_noise = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
    } else if (mode == ANC_MODE_TRANSPARENT) {
        if (level > TRANS_LEVEL_ENV) level = TRANS_LEVEL_STANDARD;
    } else {
        mode = ANC_MODE_CLOSE;
        level = 0;
    }

    bool ok = set_config_sync(make_noise_config(mode, level));
    if (ok) {
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            state_.anc_mode = mode;
            state_.anc_level = level;
        }
        emit_state();
    }
    return ok;
}

bool BtSession::set_eq_preset(uint8_t eq_mode) {
    bool ok = set_config_sync(make_eq_preset_config(eq_mode));
    if (ok) {
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            state_.eq_mode = eq_mode;
        }
        emit_state();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    query_configs_sync({CFG_EQ_MODEL, CFG_CUSTOM_EQ}, 1500);
    emit_state();
    return ok;
}

bool BtSession::set_custom_eq(const std::vector<EqBand>& bands, bool preview_only) {
    if (preview_only) {
        return set_config_sync(make_custom_eq_preview(bands), 1200);
    }
    set_config_sync(make_eq_preset_config(EQ_CUSTOM), 1200);
    bool ok = set_config_sync(make_custom_eq_save(bands), 2000);
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    query_configs_sync({CFG_CUSTOM_EQ, CFG_EQ_MODEL}, 1500);
    emit_state();
    return ok;
}

bool BtSession::reset_custom_eq() {
    bool ok = set_config_sync(make_custom_eq_reset(), 2000);
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    query_configs_sync({CFG_CUSTOM_EQ, CFG_EQ_MODEL}, 1500);
    emit_state();
    return ok;
}

bool BtSession::set_spatial_audio(bool open, bool head_tracking) {
    SpatialState st = snapshot().spatial;
    st.open = open;
    st.head_tracking = open ? head_tracking : false;
    if (st.preference == 0) st.preference = 1;
    bool ok = set_config_sync(make_spatial_audio_config(st));
    if (ok) {
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            state_.spatial = st;
        }
        emit_state();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    // Spatial Audio writes to CFG 29 and reads back from CFG 30
    query_configs_sync({CFG_SPATIAL_AUDIO_CONFIG}, 1500);
    emit_state();
    return ok;
}

bool BtSession::set_scene_rendering(bool open, uint8_t mode) {
    bool ok = set_config_sync(make_scene_rendering_config(open, mode));
    if (ok) {
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            state_.scene.open = open;
            if (open) state_.scene.mode = mode;
        }
        emit_state();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    query_configs_sync({CFG_SCENE_RENDERING}, 1500);
    emit_state();
    return ok;
}

bool BtSession::set_low_latency(bool open) {
    bool ok = set_config_sync(make_switch_config(CFG_LOW_LATENCY, open));
    if (ok) {
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            state_.low_latency = open;
        }
        emit_state();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        query_configs_sync({CFG_LOW_LATENCY}, 1200);
        emit_state();
    }
    return ok;
}

bool BtSession::set_multipoint(bool open) {
    bool ok = set_config_sync(make_switch_config(CFG_MULTIPOINT, open));
    if (ok) {
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            state_.multipoint = open;
        }
        emit_state();
        std::this_thread::sleep_for(std::chrono::milliseconds(220));
        query_configs_sync({CFG_MULTIPOINT}, 1200);
        emit_state();
    }
    return ok;
}

bool BtSession::set_personalized_anc(bool open) {
    bool ok = set_config_sync(make_switch_config(CFG_PERSONALIZED_ANC, open));
    if (ok) {
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            state_.personalized_anc = open;
        }
        emit_state();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        query_configs_sync({CFG_PERSONALIZED_ANC}, 1200);
        emit_state();
    }
    return ok;
}

bool BtSession::set_adaptive_volume(bool open) {
    bool ok = set_config_sync(make_switch_config(CFG_ADAPTIVE_VOLUME, open));
    if (ok) {
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            state_.adaptive_volume = open;
        }
        emit_state();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        query_configs_sync({CFG_ADAPTIVE_VOLUME}, 1200);
        emit_state();
    }
    return ok;
}

bool BtSession::set_auto_noise(bool open) {
    // In Xiaomi Earbuds APK (NoiseReductionVM.setAutoNoise), Adaptive ANC uses CFG_AUTO_NOISE (37)
    // while ANC mode (1) is active. If ANC isn't currently active and user turns Adaptive ANC ON,
    // activate ANC mode first so Adaptive ANC immediately takes effect without shutting off ANC!
    EarphoneSnapshot cur = snapshot();
    if (open && cur.anc_mode != ANC_MODE_NOISE) {
        uint8_t base_lvl = (cur.anc_level <= ANC_LEVEL_DEEP) ? cur.anc_level : ANC_LEVEL_DEEP;
        if (set_config_sync(make_noise_config(ANC_MODE_NOISE, base_lvl))) {
            {
                std::lock_guard<std::mutex> lk(state_mtx_);
                state_.anc_mode = ANC_MODE_NOISE;
                state_.anc_level = base_lvl;
            }
            emit_state();
            std::this_thread::sleep_for(std::chrono::milliseconds(220));
        }
    }

    bool ok = set_config_sync(make_switch_config(CFG_AUTO_NOISE, open));
    if (ok) {
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            state_.auto_noise = open;
        }
        emit_state();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        query_configs_sync({CFG_AUTO_NOISE}, 1200);
        emit_state();
    }
    return ok;
}

bool BtSession::set_auto_answer(bool open) {
    bool ok = set_config_sync(make_switch_config(CFG_AUTO_ANSWER_PHONE, open));
    if (ok) {
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            state_.auto_answer = open;
        }
        emit_state();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        query_configs_sync({CFG_AUTO_ANSWER_PHONE}, 1200);
        emit_state();
    }
    return ok;
}

bool BtSession::find_device(bool ring, uint8_t target_ear) {
    return set_config_sync(make_find_device_config(ring, target_ear));
}

} // namespace miearphone
