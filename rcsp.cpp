#include "rcsp.hpp"
#include <algorithm>
#include <cstdio>

namespace miearphone {

std::string to_hex(const uint8_t* data, size_t len, bool spaced) {
    if (!data || len == 0) return {};
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * (spaced ? 3 : 2));
    for (size_t i = 0; i < len; ++i) {
        if (spaced && i > 0) out.push_back(' ');
        out.push_back(kHex[(data[i] >> 4) & 0x0F]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

std::vector<uint8_t> CommonConfig::encode() const {
    std::vector<uint8_t> out;
    if (type == 52) {
        uint16_t vlen = static_cast<uint16_t>(value.size());
        out.reserve(5 + vlen);
        out.push_back(static_cast<uint8_t>((vlen + 2) & 0xFF));
        out.push_back(static_cast<uint8_t>((type >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(type & 0xFF));
        out.push_back(static_cast<uint8_t>((vlen >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(vlen & 0xFF));
        out.insert(out.end(), value.begin(), value.end());
        return out;
    }
    uint8_t n = static_cast<uint8_t>(value.size() + 2);
    out.reserve(1 + n);
    out.push_back(n);
    out.push_back(static_cast<uint8_t>((type >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(type & 0xFF));
    out.insert(out.end(), value.begin(), value.end());
    return out;
}

std::vector<uint8_t> encode_configs(const std::vector<CommonConfig>& cfgs) {
    std::vector<uint8_t> out;
    for (const auto& c : cfgs) {
        auto enc = c.encode();
        out.insert(out.end(), enc.begin(), enc.end());
    }
    return out;
}

std::vector<CommonConfig> decode_configs(const uint8_t* buf, size_t len) {
    std::vector<CommonConfig> out;
    size_t off = 0;
    while (off < len) {
        uint8_t n = buf[off];
        if (n == 0xFF && off + 5 <= len) {
            uint16_t t = static_cast<uint16_t>((buf[off + 1] << 8) | buf[off + 2]);
            if (t != 52) {
                ++off;
                continue;
            }
            uint16_t vlen = static_cast<uint16_t>((buf[off + 3] << 8) | buf[off + 4]);
            if (off + 5 + vlen > len) break;
            CommonConfig c;
            c.type = 52;
            c.value.assign(buf + off + 5, buf + off + 5 + vlen);
            out.push_back(std::move(c));
            off += 5 + vlen;
            continue;
        }
        if (n < 2 || off + 1 + n > len) {
            break;
        }
        CommonConfig c;
        c.type = static_cast<uint16_t>((buf[off + 1] << 8) | buf[off + 2]);
        c.value.assign(buf + off + 3, buf + off + 1 + n);
        out.push_back(std::move(c));
        off += 1 + n;
    }
    return out;
}

std::vector<uint8_t> build_command(uint8_t opcode,
                                   const std::vector<uint8_t>& payload,
                                   uint8_t sn,
                                   uint8_t target_app,
                                   bool has_response) {
    uint8_t flags = 0x80u | (target_app & 0x07u);
    if (has_response) flags |= 0x40u;

    uint16_t body_len = static_cast<uint16_t>(1 + payload.size());
    std::vector<uint8_t> frame;
    frame.reserve(3 + 4 + body_len + 1);
    frame.push_back(FRAME_HEAD[0]);
    frame.push_back(FRAME_HEAD[1]);
    frame.push_back(FRAME_HEAD[2]);
    frame.push_back(flags);
    frame.push_back(opcode);
    frame.push_back(static_cast<uint8_t>((body_len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(body_len & 0xFF));
    frame.push_back(sn);
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(FRAME_TAIL);
    return frame;
}

std::vector<uint8_t> build_response(uint8_t opcode,
                                    uint8_t sn,
                                    const std::vector<uint8_t>& business,
                                    uint8_t status) {
    uint16_t body_len = static_cast<uint16_t>(2 + business.size());
    std::vector<uint8_t> frame;
    frame.reserve(3 + 4 + body_len + 1);
    frame.push_back(FRAME_HEAD[0]);
    frame.push_back(FRAME_HEAD[1]);
    frame.push_back(FRAME_HEAD[2]);
    frame.push_back(0x01u); // response + target_app=1
    frame.push_back(opcode);
    frame.push_back(static_cast<uint8_t>((body_len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(body_len & 0xFF));
    frame.push_back(status);
    frame.push_back(sn);
    frame.insert(frame.end(), business.begin(), business.end());
    frame.push_back(FRAME_TAIL);
    return frame;
}

std::optional<RspFrame> parse_frame(const uint8_t* data, size_t len) {
    if (!data || len < 8) return std::nullopt;
    if (data[0] != FRAME_HEAD[0] || data[1] != FRAME_HEAD[1] || data[2] != FRAME_HEAD[2]) {
        return std::nullopt;
    }
    if (data[len - 1] != FRAME_TAIL) return std::nullopt;

    uint8_t flags = data[3];
    uint8_t opcode = data[4];
    uint16_t plen = static_cast<uint16_t>((data[5] << 8) | data[6]);
    if (len != static_cast<size_t>(3 + 4 + plen + 1) || plen == 0) {
        return std::nullopt;
    }

    const uint8_t* param = data + 7;
    RspFrame f;
    f.flags = flags;
    f.opcode = opcode;
    if (flags & 0x80u) {
        f.status = 0;
        f.sn = param[0];
        if (plen > 1) {
            f.payload.assign(param + 1, param + plen);
        }
    } else {
        f.status = param[0];
        f.sn = (plen > 1) ? param[1] : 0;
        if (plen > 2) {
            f.payload.assign(param + 2, param + plen);
        }
    }
    return f;
}

std::vector<std::vector<uint8_t>> extract_frames(std::vector<uint8_t>& rx_buf) {
    std::vector<std::vector<uint8_t>> frames;
    size_t pos = 0;
    while (pos < rx_buf.size()) {
        // Find FRAME_HEAD
        bool found = false;
        for (; pos + 2 < rx_buf.size(); ++pos) {
            if (rx_buf[pos] == FRAME_HEAD[0] &&
                rx_buf[pos + 1] == FRAME_HEAD[1] &&
                rx_buf[pos + 2] == FRAME_HEAD[2]) {
                found = true;
                break;
            }
        }
        if (!found) {
            // Keep at most last 2 bytes that could be partial header
            size_t keep = std::min<size_t>(2, rx_buf.size() - pos);
            if (rx_buf.size() > keep) {
                rx_buf.erase(rx_buf.begin(), rx_buf.end() - static_cast<ptrdiff_t>(keep));
            }
            return frames;
        }
        if (rx_buf.size() - pos < 8) {
            break; // Wait for more bytes
        }
        uint16_t plen = static_cast<uint16_t>((rx_buf[pos + 5] << 8) | rx_buf[pos + 6]);
        if (plen > 4096) {
            // Invalid length, skip 1 byte
            ++pos;
            continue;
        }
        size_t total = 3 + 4 + static_cast<size_t>(plen) + 1;
        if (rx_buf.size() - pos < total) {
            break; // Wait for rest of frame
        }
        if (rx_buf[pos + total - 1] != FRAME_TAIL) {
            ++pos;
            continue;
        }
        frames.emplace_back(rx_buf.begin() + static_cast<ptrdiff_t>(pos),
                            rx_buf.begin() + static_cast<ptrdiff_t>(pos + total));
        pos += total;
    }
    if (pos > 0) {
        rx_buf.erase(rx_buf.begin(), rx_buf.begin() + static_cast<ptrdiff_t>(pos));
    }
    return frames;
}

std::map<uint8_t, std::vector<uint8_t>> parse_tlv_attrs(const uint8_t* buf, size_t len) {
    std::map<uint8_t, std::vector<uint8_t>> out;
    size_t off = 0;
    while (off + 1 < len) {
        uint8_t n = buf[off++];
        if (n == 0 || off + n > len) break;
        uint8_t attr_type = buf[off];
        out[attr_type] = std::vector<uint8_t>(buf + off + 1, buf + off + n);
        off += n;
    }
    return out;
}

BatteryUnit decode_battery_byte(uint8_t raw) {
    BatteryUnit u;
    if (raw == 0xFF) {
        u.percent = -1;
        u.charging = false;
        return u;
    }
    int pct = static_cast<int>(raw & 0x7Fu);
    if (pct > 100) {
        u.percent = -1;
        u.charging = false;
        return u;
    }
    u.percent = pct;
    u.charging = (raw & 0x80u) != 0;
    return u;
}

void update_battery_from_mul(BatteryStatus& bs, const std::vector<uint8_t>& mul) {
    if (mul.size() >= 1) bs.left = decode_battery_byte(mul[0]);
    if (mul.size() >= 2) bs.right = decode_battery_byte(mul[1]);
    if (mul.size() >= 3) bs.box = decode_battery_byte(mul[2]);
    // Compute sensible overall fallback
    int best = -1;
    if (bs.left.online()) best = bs.left.percent;
    if (bs.right.online()) best = (best < 0) ? bs.right.percent : std::min(best, bs.right.percent);
    if (best >= 0) bs.overall = best;
}

CommonConfig make_noise_config(uint8_t mode, uint8_t level) {
    return CommonConfig{CFG_NOISE_LEVEL_CHOOSE, {mode, level}};
}

CommonConfig make_switch_config(uint16_t cfg_id, bool open) {
    return CommonConfig{cfg_id, {static_cast<uint8_t>(open ? 1 : 0)}};
}

CommonConfig make_eq_preset_config(uint8_t eq_mode) {
    return CommonConfig{CFG_EQ_MODEL, {eq_mode}};
}

static uint8_t encode_eq_gain(int g) {
    int clamped = std::clamp(g, -12, 12);
    return clamped >= 0 ? static_cast<uint8_t>(clamped)
                        : static_cast<uint8_t>((-clamped) + 128);
}

CommonConfig make_custom_eq_save(const std::vector<EqBand>& bands) {
    // DeviceConfigCustomEq operationType=4 (CMD_SET_CUSTOM_EQ):
    // [1, 10, 1, 1, 1, 0, count, (freq_hi, freq_lo, gain_enc)...]
    std::vector<uint8_t> v = {0x01, 0x0A, 0x01, 0x01, 0x01, 0x00,
                              static_cast<uint8_t>(bands.size())};
    v.reserve(7 + bands.size() * 3);
    for (const auto& b : bands) {
        v.push_back(static_cast<uint8_t>((b.freq >> 8) & 0xFF));
        v.push_back(static_cast<uint8_t>(b.freq & 0xFF));
        v.push_back(encode_eq_gain(b.gain));
    }
    return CommonConfig{CFG_CUSTOM_EQ, std::move(v)};
}

CommonConfig make_custom_eq_preview(const std::vector<EqBand>& bands) {
    // operationType=8 (CMD_CUSTOM_EQ_PREVIEW): [5, 1, 1, count, ...]
    std::vector<uint8_t> v = {0x05, 0x01, 0x01, static_cast<uint8_t>(bands.size())};
    v.reserve(4 + bands.size() * 3);
    for (const auto& b : bands) {
        v.push_back(static_cast<uint8_t>((b.freq >> 8) & 0xFF));
        v.push_back(static_cast<uint8_t>(b.freq & 0xFF));
        v.push_back(encode_eq_gain(b.gain));
    }
    return CommonConfig{CFG_CUSTOM_EQ, std::move(v)};
}

CommonConfig make_custom_eq_reset() {
    // operationType=6 (CMD_RESET_CUSTOM_EQ): [4, 1, 1, 1]
    return CommonConfig{CFG_CUSTOM_EQ, {0x04, 0x01, 0x01, 0x01}};
}

std::optional<CustomEqState> parse_custom_eq(const std::vector<uint8_t>& val) {
    if (val.size() < 7 || val[0] != 0x01) return std::nullopt;
    CustomEqState st;
    st.mode = val[1];
    st.gain_bound = (val[2] == 0) ? 10 : static_cast<int>(val[2]);
    uint8_t name_len = val[5];
    size_t pos = 6 + static_cast<size_t>(name_len);
    if (pos >= val.size()) return std::nullopt;
    uint8_t count = val[pos++];
    while (pos + 2 < val.size() && st.bands.size() < count) {
        uint16_t freq = static_cast<uint16_t>((val[pos] << 8) | val[pos + 1]);
        uint8_t raw_g = val[pos + 2];
        int g = (raw_g < 128) ? static_cast<int>(raw_g) : (128 - static_cast<int>(raw_g));
        st.bands.push_back(EqBand{freq, g});
        pos += 3;
    }
    return st;
}

CommonConfig make_spatial_audio_config(const SpatialState& st) {
    // DeviceConfigSpatialAudio:
    // bit0 = spatialAudioOpen, bits1..2 = preferenceSetting, bit3 = headTrackingOpen, bit4 = virtualSurroundOpen
    uint8_t b = 0;
    if (st.open) b |= 0x01u;
    b |= static_cast<uint8_t>((st.preference & 0x03) << 1);
    if (st.head_tracking) b |= 0x08u;
    if (st.virtual_surround) b |= 0x10u;
    return CommonConfig{CFG_SWITCH_SPATIAL_AUDIO, {b}};
}

SpatialState parse_spatial_audio_config(const std::vector<uint8_t>& val) {
    SpatialState st;
    if (val.empty()) return st;
    uint8_t b = val[0];
    st.open = (b & 0x01u) != 0;
    st.preference = (b >> 1) & 0x03;
    st.head_tracking = (b & 0x08u) != 0;
    st.virtual_surround = (b & 0x10u) != 0;
    return st;
}

CommonConfig make_scene_rendering_config(bool open, uint8_t mode) {
    if (!open) {
        return CommonConfig{CFG_SCENE_RENDERING, {0x00}};
    }
    return CommonConfig{CFG_SCENE_RENDERING, {0x01, mode}};
}

SceneRenderingState parse_scene_rendering_config(const std::vector<uint8_t>& val) {
    SceneRenderingState st;
    if (val.empty()) return st;
    st.open = (val[0] == 1);
    if (val.size() > 1 && val[1] != 0xFF && val[1] != 0) {
        st.mode = val[1];
    }
    return st;
}

CommonConfig make_find_device_config(bool ring, uint8_t target_ear) {
    return CommonConfig{CFG_FIND_DEVICE, {static_cast<uint8_t>(ring ? 1 : 0), target_ear}};
}

std::vector<EqBand> default_10band_eq() {
    static constexpr uint16_t kFreqs[10] = {62, 125, 250, 500, 1000, 2000, 4000, 8000, 12000, 16000};
    std::vector<EqBand> out;
    out.reserve(10);
    for (uint16_t f : kFreqs) out.push_back(EqBand{f, 0});
    return out;
}

const char* opcode_name(uint8_t opcode) {
    switch (opcode) {
        case CMD_DATA: return "DATA";
        case CMD_GET_TARGET_INFO: return "GET_TARGET_INFO";
        case CMD_REBOOT_DEVICE: return "REBOOT";
        case CMD_SET_TARGET_INFO: return "SET_TARGET_INFO";
        case CMD_GET_DEVICE_RUN_INFO: return "GET_RUN_INFO";
        case CMD_NOTIFY_COMM_WAY: return "NOTIFY_COMM_WAY";
        case CMD_AUTH_NOTIFY_OP: return "AUTH_NOTIFY_OP";
        case CMD_REPORT_DEVICE_STATUS: return "REPORT_STATUS";
        case CMD_FIND_DEVICE: return "FIND_DEVICE";
        case CMD_AUTH_CHECK: return "AUTH_CHECK";
        case CMD_AUTH_SEND_CALC_RESULT: return "AUTH_CALC";
        case CMD_SET_DEVICE_CONFIG: return "SET_CONFIG";
        case CMD_GET_DEVICE_CONFIG: return "GET_CONFIG";
        case CMD_NOTIFY_DEVICE_CONFIG: return "NOTIFY_CONFIG";
        default: return "CMD";
    }
}

const char* config_name(uint16_t cfg_id) {
    switch (cfg_id) {
        case CFG_AUTO_ANSWER_PHONE: return "AUTO_ANSWER";
        case CFG_MULTIPOINT: return "MULTIPOINT";
        case CFG_EQ_MODEL: return "EQ_MODEL";
        case CFG_FIND_DEVICE: return "FIND_DEVICE";
        case CFG_NOISE_MODE_CHOOSE: return "NOISE_MODE_CHOOSE";
        case CFG_NOISE_LEVEL_CHOOSE: return "NOISE_LEVEL";
        case CFG_SWITCH_SPATIAL_AUDIO: return "SWITCH_SPATIAL";
        case CFG_SPATIAL_AUDIO_CONFIG: return "SPATIAL_CONFIG";
        case CFG_VIRTUAL_SURROUND: return "VIRTUAL_SURROUND";
        case CFG_AUTO_NOISE: return "AUTO_NOISE";
        case CFG_EARPHONE_SN: return "EARPHONE_SN";
        case CFG_LOW_LATENCY: return "LOW_LATENCY";
        case CFG_SCENE_RENDERING: return "SCENE_RENDERING";
        case CFG_CUSTOM_EQ: return "CUSTOM_EQ";
        case CFG_PERSONALIZED_ANC: return "PERSONALIZED_ANC";
        case CFG_ADAPTIVE_VOLUME: return "ADAPTIVE_VOLUME";
        case CFG_DUAL_CONNECTION_SYNC: return "DUAL_CONN_SYNC";
        default: return "CFG";
    }
}

const char* channel_kind_name(ChannelKind k) {
    switch (k) {
        case ChannelKind::MiuiSppFd2d: return "MIUI SPP (FD2D)";
        case ChannelKind::XiaoAiSpp: return "XiaoAi SPP (1101/008584D01810)";
        case ChannelKind::StandardSpp: return "Standard SPP (1101)";
        default: return "Unknown";
    }
}

const char* auth_mode_name(AuthMode m) {
    switch (m) {
        case AuthMode::Framed: return "Framed RCSP";
        case AuthMode::RawStream: return "Raw stream";
        default: return "None";
    }
}

size_t raw_auth_packet_len(const uint8_t* buf, size_t len) {
    if (!buf || len == 0) return 0;
    // A framed RCSP packet is never a raw auth packet (APK BluetoothAuth.k early-outs on FE DC BA ... EF)
    if (len >= 3 && buf[0] == FRAME_HEAD[0] && buf[1] == FRAME_HEAD[1] && buf[2] == FRAME_HEAD[2]) {
        return 0;
    }
    // 17-byte challenge / response: [0x00 | 0x01][16B body]
    if (len >= 17 && (buf[0] == 0x00 || buf[0] == 0x01)) {
        return 17;
    }
    // 5-byte pass token
    if (len >= 5 && buf[0] == 0x02 &&
        buf[1] == 'p' && buf[2] == 'a' && buf[3] == 's' && buf[4] == 's') {
        return 5;
    }
    return 0;
}

std::vector<uint8_t> make_raw_auth_packet(uint8_t tag, const uint8_t body[16]) {
    std::vector<uint8_t> out;
    out.reserve(17);
    out.push_back(tag);
    if (body) out.insert(out.end(), body, body + 16);
    return out;
}

const char* vendor_display_name(uint16_t vid) {
    switch (vid) {
        case 0x2717: return "Xiaomi";
        case 0x5A4D: return "ZMI";
        case 0x1915: return "Nordic/Airoha";
        default: return "Unknown";
    }
}

// Rebuilt from APK 1.39.0 com.mi.earphone.bluetoothsdk.util.DeviceVidPidTypeUtilsKt.
// Every predicate there compares decimal ints; the values below are the exact
// hex equivalents, so a live GET_TARGET_INFO VID/PID matches directly.
static const DeviceProfile kKnownProfiles[] = {
    // ---- Xiaomi-branded models (小米) ----
    // FlipBuds Pro (J77S)
    {0x2717, 0x5019, "J77S", "小米 FlipBuds Pro", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x5036, "J77S", "小米 FlipBuds Pro", "Xiaomi", 3, 3, true, true, false},
    // Xiaomi Buds 3 Pro (K73 / K73a)
    {0x2717, 0x5025, "K73", "Xiaomi Buds 3 Pro", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x502D, "K73a", "Xiaomi Buds 3 Pro", "Xiaomi", 3, 3, true, true, false},
    // Xiaomi Buds 3 (K75 / K75s 星球大战版)
    {0x2717, 0x5026, "K75", "Xiaomi Buds 3", "Xiaomi", 3, 3, false, true, false},
    {0x2717, 0x502B, "K75", "Xiaomi Buds 3", "Xiaomi", 3, 3, false, true, false},
    {0x2717, 0x505A, "K75s", "Xiaomi Buds 3 (星球大战版)", "Xiaomi", 3, 3, false, true, false},
    // Xiaomi Buds 4 Pro (L76, ZMI VID)
    {0x5A4D, 0xEA0E, "L76", "Xiaomi Buds 4 Pro", "Xiaomi", 3, 3, true, true, false},
    {0x5A4D, 0xEA0F, "L76", "Xiaomi Buds 4 Pro", "Xiaomi", 3, 3, true, true, false},
    {0x5A4D, 0xEA10, "L76", "Xiaomi Buds 4 Pro", "Xiaomi", 3, 3, true, true, false},
    // Xiaomi Buds 4 (M75A, 半入耳自适应降噪)
    {0x2717, 0x5044, "M75A", "Xiaomi Buds 4", "Xiaomi", 1, 1, true, true, false},
    {0x2717, 0x5045, "M75A", "Xiaomi Buds 4", "Xiaomi", 1, 1, true, true, false},
    // Xiaomi 开放式耳机 (N79 / N79A / N79B / N79S)
    {0x2717, 0x5088, "N79", "Xiaomi 开放式耳机", "Xiaomi", 0, 0, false, true, false},
    {0x2717, 0x5089, "N79", "Xiaomi 开放式耳机", "Xiaomi", 0, 0, false, true, false},
    {0x2717, 0x508A, "N79A", "Xiaomi 开放式耳机", "Xiaomi", 0, 0, false, true, false},
    {0x2717, 0x508B, "N79A", "Xiaomi 开放式耳机", "Xiaomi", 0, 0, false, true, false},
    {0x2717, 0x508C, "N79A", "Xiaomi 开放式耳机", "Xiaomi", 0, 0, false, true, false},
    {0x2717, 0x508D, "N79A", "Xiaomi 开放式耳机", "Xiaomi", 0, 0, false, true, false},
    {0x2717, 0x509A, "N79B", "Xiaomi 开放式耳机", "Xiaomi", 0, 0, false, true, false},
    {0x2717, 0x509B, "N79B", "Xiaomi 开放式耳机", "Xiaomi", 0, 0, false, true, false},
    {0x2717, 0x509C, "N79S", "Xiaomi 开放式耳机", "Xiaomi", 0, 0, false, true, false},
    // Xiaomi Buds 5 Pro (O71 / O71 WiFi / O71 BT)
    {0x2717, 0x50AB, "O71", "Xiaomi Buds 5 Pro", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x50AC, "O71", "Xiaomi Buds 5 Pro (WiFi版)", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x50AD, "O71", "Xiaomi Buds 5 Pro", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x50B4, "O71", "Xiaomi Buds 5 Pro (BT版)", "Xiaomi", 3, 3, true, true, false},
    // Xiaomi Buds 5 (O74)
    {0x2717, 0x50B8, "O74", "Xiaomi Buds 5", "Xiaomi", 1, 1, true, true, false},
    {0x2717, 0x50D7, "O74", "Xiaomi Buds 5", "Xiaomi", 1, 1, true, true, false},
    // O70C / O73 / P75 / P76 / P79 / Q74 (Xiaomi 迭代机型)
    {0x2717, 0x50F0, "O70C", "Xiaomi 耳机 (O70C)", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x50FB, "O70C", "Xiaomi 耳机 (O70C)", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x50B5, "O73", "Xiaomi 耳机 (O73)", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x50EA, "P75", "Xiaomi 耳机 (P75)", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x50EB, "P75", "Xiaomi 耳机 (P75)", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x50E3, "P76", "Xiaomi 耳机 (P76)", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x50E5, "P76", "Xiaomi 耳机 (P76)", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x50F4, "P76Bose", "Xiaomi 耳机 (P76 Bose 联名)", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x50E1, "P79", "Xiaomi 耳机 (P79)", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x50E2, "P79", "Xiaomi 耳机 (P79)", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x511E, "Q74C", "Xiaomi 耳机 (Q74C)", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x511F, "Q74C", "Xiaomi 耳机 (Q74C)", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x50DB, "Q74u", "Xiaomi 耳机 (Q74u)", "Xiaomi", 3, 3, true, true, false},
    {0x2717, 0x50DC, "Q74u", "Xiaomi 耳机 (Q74u)", "Xiaomi", 3, 3, true, true, false},

    // ---- Redmi-branded models ----
    {0x2717, 0x5035, "L71", "Redmi Buds 4", "Redmi", 2, 2, false, true, false},
    {0x2717, 0x503B, "L71", "Redmi Buds 4", "Redmi", 2, 2, false, true, false},
    {0x2717, 0x5034, "L77", "Redmi Buds 4 Pro", "Redmi", 3, 3, true, true, false},
    {0x2717, 0x5037, "L77", "Redmi Buds 4 Pro", "Redmi", 3, 3, true, true, false},
    {0x2717, 0x5038, "L77", "Redmi Buds 4 Pro", "Redmi", 3, 3, true, true, false},
    {0x2717, 0x505D, "L77s", "Redmi Buds 4 Pro (哈利波特版)", "Redmi", 3, 3, true, true, false},
    {0x2717, 0x505F, "M79A", "Redmi Buds 4 活力版", "Redmi", 0, 0, false, false, false},
    {0x2717, 0x5060, "M79A", "Redmi Buds 4 青春版", "Redmi", 0, 0, false, false, false},
    {0x2717, 0x5061, "M79A", "Redmi Buds 4 活力版", "Redmi", 0, 0, false, false, false},
    {0x2717, 0x5062, "M79A", "Redmi Buds 4 活力版", "Redmi", 0, 0, false, false, false},
    {0x2717, 0x5069, "M79A", "Redmi Buds 4 活力版", "Redmi", 0, 0, false, false, false},
    {0x2717, 0x508E, "M79I", "Redmi Buds 4 (M79I)", "Redmi", 0, 0, false, false, false},
    {0x2717, 0x507F, "N74A", "Redmi Buds 5A", "Redmi", 1, 1, false, false, false},
    {0x2717, 0x5080, "N74A", "Redmi Buds 5A", "Redmi", 1, 1, false, false, false},
    {0x2717, 0x5081, "N75", "Redmi Buds 5", "Redmi", 3, 3, false, true, false},
    {0x2717, 0x5082, "N75", "Redmi Buds 5", "Redmi", 3, 3, false, true, false},
    {0x2717, 0x506C, "N76", "Redmi Buds 5 Pro", "Redmi", 3, 3, true, true, false},
    {0x2717, 0x506D, "N76", "Redmi Buds 5 Pro", "Redmi", 3, 3, true, true, false},
    {0x2717, 0x506E, "N76GIP", "Redmi Buds 5 Pro (IP联名版)", "Redmi", 3, 3, true, true, false},
    {0x2717, 0x506F, "N76G", "Redmi Buds 5 Pro (电竞版)", "Redmi", 3, 3, true, true, false},
    {0x2717, 0x506A, "N77", "Redmi 耳机 (N77)", "Redmi", 3, 3, true, true, false},
    {0x2717, 0x5075, "N77", "Redmi 耳机 (N77)", "Redmi", 3, 3, true, true, false},
    {0x2717, 0x5087, "N77", "Redmi 耳机 (N77)", "Redmi", 3, 3, true, true, false},
    {0x2717, 0x506B, "N77Ip", "Redmi 耳机 (N77 IP联名版)", "Redmi", 3, 3, true, true, false},
    {0x2717, 0x5095, "N77S", "Redmi 耳机 (N77S)", "Redmi", 3, 3, true, true, false},
    {0x2717, 0x509D, "O76", "Redmi Buds 6 Pro", "Redmi", 3, 3, true, true, false},
    {0x2717, 0x509E, "O76", "Redmi Buds 6 Pro", "Redmi", 3, 3, true, true, false},
    {0x2717, 0x50AF, "O76G", "Redmi Buds 6 Pro (电竞版)", "Redmi", 3, 3, true, true, false},
    {0x2717, 0x509F, "O77", "Redmi Buds 6", "Redmi", 2, 2, false, true, false},
    {0x2717, 0x50A0, "O77", "Redmi Buds 6", "Redmi", 2, 2, false, true, false},
    {0x2717, 0x50A7, "O77", "Redmi Buds 6", "Redmi", 2, 2, false, true, false},
    {0x2717, 0x50B9, "O77S", "REDMI Buds 7S", "Redmi", 2, 2, false, true, false},

    // ---- ZMI legacy (OneMore / raw stream auth) ----
    {0x5A4D, 0xEA01, "TW100", "ZMI PurPods Pro", "ZMI", 1, 1, false, false, true},
    {0x5A4D, 0xEA02, "TW101", "ZMI PurPods Pro", "ZMI", 1, 1, false, false, true},
    {0x5A4D, 0xAC01, "NZBT01", "ZMI PurPods", "ZMI", 0, 0, false, false, true},
    {0x5A4D, 0xBA02, "B508", "ZMI 蓝牙耳机 B508", "ZMI", 0, 0, false, false, true},
    {0x5A4D, 0xBA03, "B608", "ZMI 蓝牙耳机 B608", "ZMI", 0, 0, false, false, true},
    {0x5A4D, 0xBA04, "WBS02", "ZMI 蓝牙耳机 WBS02", "ZMI", 0, 0, false, false, true},
};

std::optional<DeviceProfile> find_device_profile(uint16_t vid, uint16_t pid) {
    if (vid == 0 && pid == 0) return std::nullopt;
    for (const auto& p : kKnownProfiles) {
        if (p.vid == vid && p.pid == pid) return p;
    }
    return std::nullopt;
}

} // namespace miearphone
