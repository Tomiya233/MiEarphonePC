#pragma once
#include "rcsp.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace miearphone {

struct PairedBtDevice {
    std::string name;
    std::string mac;
    bool system_connected = false;
    bool is_earphone_hint = false;
};

struct EarphoneSnapshot {
    bool connected = false;
    bool authenticated = false;
    std::string mac;
    std::string name;
    std::string fw_version;
    std::string sn;
    uint16_t vid = 0;
    uint16_t pid = 0;

    // Transport / model identification (APK ua/i.java channel order + vid/pid)
    ChannelKind channel = ChannelKind::Unknown;
    AuthMode auth_mode = AuthMode::None;
    std::string brand;        // Xiaomi / Redmi / ZMI
    std::string model_name;   // market name from the profile table, if known

    BatteryStatus battery;

    uint8_t anc_mode = ANC_MODE_NOISE;
    uint8_t anc_level = ANC_LEVEL_DEEP;

    uint8_t eq_mode = EQ_DEFAULT;
    CustomEqState custom_eq{0, 6, default_10band_eq()};

    SpatialState spatial;
    SceneRenderingState scene;

    bool low_latency = false;
    bool multipoint = false;
    bool personalized_anc = false;
    bool adaptive_volume = false;
    bool auto_noise = false; // CFG 37: 自适应降噪 (Adaptive ANC)
    bool auto_answer = false;
};

// System helpers (zero subprocess overhead)
std::vector<PairedBtDevice> enumerate_paired_devices();
bool is_xiaomi_manager_running();
bool parse_mac_u64(const std::string& mac, uint64_t& out_addr);
std::string format_mac_u64(uint64_t addr);

class BtSession {
public:
    using LogCallback = std::function<void(const std::string&)>;
    using StateCallback = std::function<void(const EarphoneSnapshot&)>;
    using DisconnectCallback = std::function<void(const std::string&)>;

    BtSession();
    ~BtSession();

    BtSession(const BtSession&) = delete;
    BtSession& operator=(const BtSession&) = delete;

    void set_callbacks(LogCallback on_log,
                       StateCallback on_state,
                       DisconnectCallback on_disconnect);

    bool connect_and_auth(const std::string& mac, int connect_timeout_ms = 6000, int* out_wsa_err = nullptr);
    void close();

    bool is_connected() const { return alive_.load(); }
    EarphoneSnapshot snapshot() const;
    ChannelKind channel_kind() const { return channel_; }
    std::optional<DeviceProfile> profile() const;
    AuthMode auth_mode() const { return auth_mode_; }

    // Synchronous protocol operations (safe to call from worker threads)
    bool refresh_all();
    bool refresh_battery();

    bool set_anc(uint8_t mode, uint8_t level, bool keep_auto_noise = false);
    bool set_eq_preset(uint8_t eq_mode);
    bool set_custom_eq(const std::vector<EqBand>& bands, bool preview_only = false);
    bool reset_custom_eq();
    bool set_spatial_audio(bool open, bool head_tracking);
    bool set_scene_rendering(bool open, uint8_t mode);
    bool set_low_latency(bool open);
    bool set_multipoint(bool open);
    bool set_personalized_anc(bool open);
    bool set_adaptive_volume(bool open);
    bool set_auto_noise(bool open);
    bool set_auto_answer(bool open);
    bool find_device(bool ring, uint8_t target_ear = 3);
    bool reauthenticate();

private:
    uint8_t next_sn();
    bool send_raw(const std::vector<uint8_t>& raw);
    bool send_command(uint8_t opcode, const std::vector<uint8_t>& payload, uint8_t app, std::optional<uint8_t> fixed_sn = std::nullopt);
    bool send_response(uint8_t opcode, uint8_t sn, const std::vector<uint8_t>& business = {}, uint8_t status = 0);
    std::optional<RspFrame> wait_response(uint8_t opcode, int timeout_ms = 2500);
    void drain_inbox();

    bool authenticate_rcsp_cmd();
    bool authenticate_raw_stream();
    // Feeds one unframed auth packet (17 or 5 bytes) through the state machine.
    bool handle_raw_auth_packet(const std::vector<uint8_t>& pkt);

    bool set_config_sync(const CommonConfig& cfg, int timeout_ms = 2000);
    bool query_configs_sync(const std::vector<uint16_t>& ids, int timeout_ms = 2500);

    void rx_loop();
    void handle_device_command(const RspFrame& f);
    void handle_device_response(const RspFrame& f);
    void apply_config_update(uint16_t type, const std::vector<uint8_t>& val);
    void apply_target_info_attrs(const std::map<uint8_t, std::vector<uint8_t>>& attrs);
    void apply_status_attrs(const std::map<uint8_t, std::vector<uint8_t>>& attrs);
    void emit_log(const std::string& s);
    void emit_state();
    void fail_link(const std::string& reason);

    uintptr_t sock_ = ~uintptr_t(0); // INVALID_SOCKET
    std::atomic<bool> alive_{false};
    std::atomic<uint8_t> sn_{0};

    mutable std::mutex send_mtx_;
    mutable std::mutex state_mtx_;
    EarphoneSnapshot state_;

    std::mutex inbox_mtx_;
    std::condition_variable inbox_cv_;
    std::deque<RspFrame> inbox_;
    std::atomic<uint64_t> report_seq_{0};
    std::atomic<bool> dev_challenged_{false};

    // Transport channel actually established (drives the auth encoding).
    ChannelKind channel_ = ChannelKind::Unknown;
    AuthMode auth_mode_ = AuthMode::None;

    // Raw stream auth state (APK BluetoothAuth.s / stages 0..3)
    std::mutex raw_mtx_;
    std::condition_variable raw_cv_;
    std::deque<std::vector<uint8_t>> raw_inbox_;
    std::array<uint8_t, 16> raw_challenge_{};
    std::array<uint8_t, 16> raw_challenge_enc_{};
    int raw_stage_ = 0;          // 0 idle, 1 challenged us, 2 we replied, 3 authenticated
    bool raw_ok_ = false;

    std::thread rx_thread_;

    mutable std::mutex cb_mtx_;
    LogCallback on_log_;
    StateCallback on_state_;
    DisconnectCallback on_disconnect_;
};

} // namespace miearphone
