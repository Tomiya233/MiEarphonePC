#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace miearphone {

// RCSP Framing Constants
constexpr uint8_t FRAME_HEAD[3] = {0xFE, 0xDC, 0xBA};
constexpr uint8_t FRAME_TAIL = 0xEF;

constexpr uint8_t APP_CONFIG = 0; // flags 0xC0
constexpr uint8_t APP_AUTH   = 1; // flags 0xC1

// RCSP Command Opcodes
constexpr uint8_t CMD_DATA                    = 0x01;
constexpr uint8_t CMD_GET_TARGET_INFO         = 0x02;
constexpr uint8_t CMD_REBOOT_DEVICE           = 0x03;
constexpr uint8_t CMD_SET_TARGET_INFO         = 0x08;
constexpr uint8_t CMD_GET_DEVICE_RUN_INFO     = 0x09;
constexpr uint8_t CMD_NOTIFY_COMM_WAY         = 0x0A;
constexpr uint8_t CMD_AUTH_NOTIFY_OP          = 0x0D;
constexpr uint8_t CMD_REPORT_DEVICE_STATUS    = 0x0E;
constexpr uint8_t CMD_FIND_DEVICE             = 0x42;
constexpr uint8_t CMD_AUTH_CHECK              = 0x50;
constexpr uint8_t CMD_AUTH_SEND_CALC_RESULT   = 0x51;
constexpr uint8_t CMD_SET_DEVICE_CONFIG       = 0xF2;
constexpr uint8_t CMD_GET_DEVICE_CONFIG       = 0xF3;
constexpr uint8_t CMD_NOTIFY_DEVICE_CONFIG    = 0xF4;

// DeviceConfig IDs (from DeviceConfigIdConstantKt)
constexpr uint16_t CFG_AUDIO_MODE             = 1;
constexpr uint16_t CFG_CUSTOM_CLICK           = 2;
constexpr uint16_t CFG_AUTO_ANSWER_PHONE      = 3;
constexpr uint16_t CFG_MULTIPOINT             = 4;
constexpr uint16_t CFG_FIT_DETECTION          = 5;
constexpr uint16_t CFG_WEAR_LISTENER          = 6;
constexpr uint16_t CFG_EQ_MODEL               = 7;
constexpr uint16_t CFG_FIND_DEVICE            = 9;
constexpr uint16_t CFG_NOISE_MODE_CHOOSE      = 10;
constexpr uint16_t CFG_NOISE_LEVEL_CHOOSE     = 11;
constexpr uint16_t CFG_REMIND_LOST            = 12;
constexpr uint16_t CFG_CALL_LISTENER          = 13;
constexpr uint16_t CFG_SWITCH_SPATIAL_AUDIO   = 29;
constexpr uint16_t CFG_SPATIAL_AUDIO_CONFIG   = 30;
constexpr uint16_t CFG_VIRTUAL_SURROUND       = 36;
constexpr uint16_t CFG_AUTO_NOISE             = 37;
constexpr uint16_t CFG_EARPHONE_SN            = 39;
constexpr uint16_t CFG_ADAPTIVE_SENSE         = 41;
constexpr uint16_t CFG_LOW_LATENCY            = 47;
constexpr uint16_t CFG_SCENE_RENDERING        = 54;
constexpr uint16_t CFG_CUSTOM_EQ              = 55;
constexpr uint16_t CFG_SPATIAL_NOTIFY_SOUND   = 58;
constexpr uint16_t CFG_PERSONALIZED_ANC       = 59;
constexpr uint16_t CFG_EARPHONE_VERSION       = 71;
constexpr uint16_t CFG_ADAPTIVE_VOLUME        = 72;
constexpr uint16_t CFG_BLUETOOTH_INFO         = 74;
constexpr uint16_t CFG_DUAL_CONNECTION_SYNC   = 126;

// TargetInfo / Status Attribute IDs
constexpr uint8_t TARGET_ATTR_NAME            = 0x00;
constexpr uint8_t TARGET_ATTR_VERSION         = 0x01;
constexpr uint8_t TARGET_ATTR_QUANTITY        = 0x02;
constexpr uint8_t TARGET_ATTR_VID_PID         = 0x03;
constexpr uint8_t TARGET_ATTR_UBOOT_VER       = 0x06;
constexpr uint8_t TARGET_ATTR_MUL_QUANTITY    = 0x07;
constexpr uint8_t TARGET_ATTR_CODEC_TYPE      = 0x08;
constexpr uint8_t TARGET_ATTR_COLOR_TYPE      = 0x0D;

constexpr uint8_t STATUS_ATTR_QUANTITY        = 0x00;
constexpr uint8_t STATUS_ATTR_TWS             = 0x01;
constexpr uint8_t STATUS_ATTR_ANC             = 0x04;

// ANC Mode & Level Constants (verified against Xiaomi Earbuds APK 1.39.0 & Redmi Buds 5 Pro N76)
constexpr uint8_t ANC_MODE_CLOSE              = 0;
constexpr uint8_t ANC_MODE_NOISE              = 1;
constexpr uint8_t ANC_MODE_TRANSPARENT        = 2;

// Manual ANC Gears in CFG 11: [1=轻度, 0=均衡, 2=深度].
// Adaptive ANC on modern models (N76 etc.) is controlled via CFG_AUTO_NOISE (37), NOT CFG 11!
constexpr uint8_t ANC_LEVEL_BALANCE           = 0; // 均衡 (device_settings_balanced)
constexpr uint8_t ANC_LEVEL_MILD              = 1; // 轻度 (device_settings_mild)
constexpr uint8_t ANC_LEVEL_COMFORTABLE       = 1; // alias
constexpr uint8_t ANC_LEVEL_DEEP              = 2; // 深度 (device_settings_deep)

// Transparency Gears in CFG 11: [0=标准通透, 1=人声增强, 2=环境增强]
constexpr uint8_t TRANS_LEVEL_STANDARD        = 0;
constexpr uint8_t TRANS_LEVEL_VOCAL           = 1;
constexpr uint8_t TRANS_LEVEL_ENV             = 2;

// EQ Preset IDs (SoundEffectType)
constexpr uint8_t EQ_DEFAULT                  = 0;  // 默认均衡
constexpr uint8_t EQ_HUMAN_VOICE              = 1;  // 人声增强
constexpr uint8_t EQ_ROCK                     = 2;  // 摇滚
constexpr uint8_t EQ_CLASSIC                  = 3;  // 古典
constexpr uint8_t EQ_POPULARITY               = 4;  // 流行
constexpr uint8_t EQ_LOW_BASS                 = 5;  // 低音增强
constexpr uint8_t EQ_HIGH_TREBLE              = 6;  // 高音增强
constexpr uint8_t EQ_VOLUME_BOOST             = 7;  // 音量增强
constexpr uint8_t EQ_CUSTOM                   = 10; // 自定义均衡器

struct CommonConfig {
    uint16_t type = 0;
    std::vector<uint8_t> value;

    std::vector<uint8_t> encode() const;
};

struct RspFrame {
    uint8_t flags = 0;
    uint8_t opcode = 0;
    uint8_t status = 0;
    uint8_t sn = 0;
    std::vector<uint8_t> payload;

    bool is_command() const { return (flags & 0x80u) != 0; }
    bool need_response() const { return (flags & 0x40u) != 0; }
    uint8_t target_app() const { return flags & 0x07u; }
    bool ok() const { return status == 0; }
};

struct BatteryUnit {
    int percent = -1;       // 0..100, or -1 if offline/closed
    bool charging = false;
    bool online() const { return percent >= 0 && percent <= 100; }
};

struct BatteryStatus {
    BatteryUnit left;
    BatteryUnit right;
    BatteryUnit box;
    int overall = -1;       // fallback single battery byte
};

struct EqBand {
    uint16_t freq = 0;
    int gain = 0;           // signed dB (-gain_bound .. +gain_bound)
};

struct CustomEqState {
    int mode = 0;
    int gain_bound = 6;
    std::vector<EqBand> bands; // typically 10 bands: 62, 125, 250, 500, 1k, 2k, 4k, 8k, 12k, 16k
};

struct SpatialState {
    bool open = false;
    int preference = 1;     // 0=high quality, 1=low latency
    bool head_tracking = false;
    bool virtual_surround = false;
};

struct SceneRenderingState {
    bool open = false;
    int mode = 1;           // 1=标准, 2=音乐, 3=视频, 4=游戏, 5=有声书
};

// Hex formatting helper
std::string to_hex(const uint8_t* data, size_t len, bool spaced = true);
inline std::string to_hex(const std::vector<uint8_t>& v, bool spaced = true) {
    return to_hex(v.data(), v.size(), spaced);
}

// ---------------------------------------------------------------------------
// Transport channel selection
//
// Official APK connect order (ua/i.java ConnectSppUUID.run):
//   1) 0000FD2D-... (MIUI SPP)          -> success sets auth-with-command = true
//   2) 00001101-...-008584D01810 (XiaoAi SPP)
//   3) 00001101-...-00805F9B34FB (Standard SPP)
// Channels 2/3 keep auth-with-command = false, i.e. raw stream authentication.
// ---------------------------------------------------------------------------
enum class ChannelKind {
    Unknown,
    MiuiSppFd2d,   // 0000FD2D: framed RCSP auth
    XiaoAiSpp,     // 1101 + node 008584D01810: raw stream auth
    StandardSpp    // 1101 + node 00805F9B34FB: raw stream auth
};

// Wire encoding of the authentication exchange on the active channel.
enum class AuthMode {
    None,       // not decided yet
    Framed,     // RCSP frames: FE DC BA C1 50 ... EF
    RawStream   // unframed 17-byte [tag, body16] packets (APK BluetoothAuth.k)
};

const char* channel_kind_name(ChannelKind k);
const char* auth_mode_name(AuthMode m);

// APK BluetoothAuth.k(): raw auth packet classifier.
// Returns 17 for [tag:0x00|0x01][body:16B], 5 for [0x02,'p','a','s','s'], else 0.
// RCSP frames always return 0 (they start with 0xFE).
size_t raw_auth_packet_len(const uint8_t* buf, size_t len);
inline size_t raw_auth_packet_len(const std::vector<uint8_t>& v) {
    return raw_auth_packet_len(v.data(), v.size());
}

// Builds an unframed auth packet: [tag][body16].
std::vector<uint8_t> make_raw_auth_packet(uint8_t tag, const uint8_t body[16]);

// How a received raw packet should be interpreted by the auth state machine.
enum class RawAuthVerdict {
    Ignore,       // unrelated / malformed
    ProvedKey,    // [0x01][our_enc]  -> device knows the shared key
    Challenge,    // [0x00][rnd16]    -> answer with [0x01][auth_response(rnd)]
    PassToken     // [0x02]'pass'     -> device accepts without challenge
};

// Judges one raw packet. On Challenge, out_challenge receives the 16-byte nonce.
RawAuthVerdict judge_raw_auth_packet(const uint8_t* pkt, size_t len,
                                     const uint8_t our_enc[16],
                                     uint8_t out_challenge[16]);
inline RawAuthVerdict judge_raw_auth_packet(const std::vector<uint8_t>& pkt,
                                            const uint8_t our_enc[16],
                                            uint8_t out_challenge[16]) {
    return judge_raw_auth_packet(pkt.data(), pkt.size(), our_enc, out_challenge);
}

// ---------------------------------------------------------------------------
// Per-model profile table, rebuilt from APK 1.39.0 DeviceVidPidTypeUtilsKt
// (all PID predicates are decimal there; these are the exact hex equivalents).
// ---------------------------------------------------------------------------
struct DeviceProfile {
    uint16_t vid = 0;
    uint16_t pid = 0;
    const char* codename = "";
    const char* market_name = "";
    const char* brand = "";      // "Xiaomi" / "Redmi" / "ZMI"
    uint8_t anc_gears = 3;       // 0 none, 1 single, 2 two, 3 three manual gears
    uint8_t trans_gears = 3;
    bool spatial = false;
    bool multipoint = false;
    bool raw_auth_likely = false; // no FD2D history -> expect raw stream auth
};

std::optional<DeviceProfile> find_device_profile(uint16_t vid, uint16_t pid);
const char* vendor_display_name(uint16_t vid);

// Frame codecs
std::vector<uint8_t> build_command(uint8_t opcode,
                                   const std::vector<uint8_t>& payload,
                                   uint8_t sn,
                                   uint8_t target_app = APP_CONFIG,
                                   bool has_response = true);

std::vector<uint8_t> build_response(uint8_t opcode,
                                    uint8_t sn,
                                    const std::vector<uint8_t>& business = {},
                                    uint8_t status = 0);

std::optional<RspFrame> parse_frame(const uint8_t* data, size_t len);
inline std::optional<RspFrame> parse_frame(const std::vector<uint8_t>& raw) {
    return parse_frame(raw.data(), raw.size());
}

// Extracts complete frames from `rx_buf` in-place (erasing consumed bytes).
std::vector<std::vector<uint8_t>> extract_frames(std::vector<uint8_t>& rx_buf);

// Config & TLV codecs
std::vector<uint8_t> encode_configs(const std::vector<CommonConfig>& cfgs);
std::vector<CommonConfig> decode_configs(const uint8_t* buf, size_t len);
inline std::vector<CommonConfig> decode_configs(const std::vector<uint8_t>& buf) {
    return decode_configs(buf.data(), buf.size());
}

std::map<uint8_t, std::vector<uint8_t>> parse_tlv_attrs(const uint8_t* buf, size_t len);
inline std::map<uint8_t, std::vector<uint8_t>> parse_tlv_attrs(const std::vector<uint8_t>& buf) {
    return parse_tlv_attrs(buf.data(), buf.size());
}

// Semantic decoders & encoders
BatteryUnit decode_battery_byte(uint8_t raw);
void update_battery_from_mul(BatteryStatus& bs, const std::vector<uint8_t>& mul);

CommonConfig make_noise_config(uint8_t mode, uint8_t level);
CommonConfig make_switch_config(uint16_t cfg_id, bool open);
CommonConfig make_eq_preset_config(uint8_t eq_mode);
CommonConfig make_custom_eq_save(const std::vector<EqBand>& bands);
CommonConfig make_custom_eq_preview(const std::vector<EqBand>& bands);
CommonConfig make_custom_eq_reset();
std::optional<CustomEqState> parse_custom_eq(const std::vector<uint8_t>& val);

CommonConfig make_spatial_audio_config(const SpatialState& st);
SpatialState parse_spatial_audio_config(const std::vector<uint8_t>& val);

CommonConfig make_scene_rendering_config(bool open, uint8_t mode);
SceneRenderingState parse_scene_rendering_config(const std::vector<uint8_t>& val);

CommonConfig make_find_device_config(bool ring, uint8_t target_ear = 3); // 1=L, 2=R, 3=Both

std::vector<EqBand> default_10band_eq();

const char* opcode_name(uint8_t opcode);
const char* config_name(uint16_t cfg_id);

} // namespace miearphone
