#include "auth_cipher.hpp"
#include "bt_session.hpp"
#include "rcsp.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace miearphone;

static std::vector<uint8_t> from_hex(const char* s) {
    std::vector<uint8_t> out;
    while (*s) {
        if (*s == ' ') { ++s; continue; }
        unsigned v = 0;
        std::sscanf(s, "%2x", &v);
        out.push_back(static_cast<uint8_t>(v));
        s += 2;
    }
    return out;
}

int main(int argc, char** argv) {
    // 1. Auth cipher 4/4 real captured pairs
    struct Pair { const char* rnd; const char* ans; };
    static const Pair kPairs[4] = {
        {"74a3be2b9b5579c1be7d8e2104baeb0d", "33623ae7a9915ed6d95577c063d3fd11"},
        {"35cfe002ca52cc1c268d7848552385f0", "d7d20f717ef84987847d4bf030d8ccb0"},
        {"3e911f7b7fb05fd5beaac4ce1461db65", "ad7418ab8d7d5cc6c5ae835dc1e2c4ba"},
        {"5afaaf15be6b32e163d6c23445067de5", "30ddb0e38fa248d47f3663c11437db5c"},
    };
    for (const auto& p : kPairs) {
        auto r = from_hex(p.rnd);
        auto e = from_hex(p.ans);
        auto got = auth_response(r.data());
        assert(std::memcmp(got.data(), e.data(), 16) == 0);
    }
    std::printf("[PASS] auth_cipher 4/4 real-device capture vectors\n");

    // 2. Frame build & parse & sticky stream extract
    auto cmd = build_command(CMD_SET_DEVICE_CONFIG, make_noise_config(1, 2).encode(), 4, APP_CONFIG);
    assert(to_hex(cmd, false) == "FEDCBAC0F200060404000B0102EF");
    auto rsp = build_response(CMD_NOTIFY_DEVICE_CONFIG, 0x04);
    assert(to_hex(rsp, false) == "FEDCBA01F400020004EF");

    std::vector<uint8_t> stream = {0x99, 0xAA};
    stream.insert(stream.end(), cmd.begin(), cmd.end());
    stream.insert(stream.end(), rsp.begin(), rsp.begin() + 5);
    auto ex1 = extract_frames(stream);
    assert(ex1.size() == 1 && ex1[0] == cmd);
    stream.insert(stream.end(), rsp.begin() + 5, rsp.end());
    auto ex2 = extract_frames(stream);
    assert(ex2.size() == 1 && ex2[0] == rsp && stream.empty());
    std::printf("[PASS] RCSP framing & sticky stream extraction\n");

    // 3. Multi-battery + charging bit + offline byte
    BatteryStatus bs;
    update_battery_from_mul(bs, {0x50, static_cast<uint8_t>(0x80 | 85), 0xFF});
    assert(bs.left.online() && bs.left.percent == 80 && !bs.left.charging);
    assert(bs.right.online() && bs.right.percent == 85 && bs.right.charging);
    assert(!bs.box.online());
    assert(bs.overall == 80);
    std::printf("[PASS] Multi-battery (Left/Right/Case + charging flag)\n");

    // 4. Custom 10-band EQ codec (live captured bytes + negative gain roundtrip)
    auto live_eq = from_hex("01 06 06 06 00 00 0A 00 3E 06 00 7D 04 00 FA 06 01 F4 02 03 E8 02 07 D0 02 0F A0 03 1F 40 03 2E E0 03 3E 80 03");
    auto peq = parse_custom_eq(live_eq);
    assert(peq.has_value() && peq->mode == 6 && peq->gain_bound == 6 && peq->bands.size() == 10);
    assert(peq->bands[0].freq == 62 && peq->bands[0].gain == 6);
    assert(peq->bands[9].freq == 16000 && peq->bands[9].gain == 3);

    auto bands = default_10band_eq();
    bands[0].gain = -5;
    bands[9].gain = 4;
    auto saved_cfg = make_custom_eq_save(bands);
    std::vector<uint8_t> sim_rep = saved_cfg.value;
    sim_rep[2] = 6; sim_rep[3] = 6;
    auto rep_parsed = parse_custom_eq(sim_rep);
    assert(rep_parsed && rep_parsed->bands[0].gain == -5 && rep_parsed->bands[9].gain == 4);
    std::printf("[PASS] 10-band Custom EQ codec\n");

    // 5. Spatial Audio & Scene Rendering codecs
    SpatialState sp_in{true, 1, true, false};
    auto sp_cfg = make_spatial_audio_config(sp_in);
    assert(sp_cfg.value.size() == 1 && sp_cfg.value[0] == 0x0B);
    auto sp_out = parse_spatial_audio_config(sp_cfg.value);
    assert(sp_out.open && sp_out.preference == 1 && sp_out.head_tracking && !sp_out.virtual_surround);
    std::printf("[PASS] Spatial Audio & Scene Rendering codecs\n");

    // 6. Optional live hardware smoke & regression test
    if (argc > 1 && std::string(argv[1]) == "--live") {
        auto devs = enumerate_paired_devices();
        std::printf("[LIVE] Paired Bluetooth devices (%zu):\n", devs.size());
        for (const auto& d : devs) {
            std::printf("  - %s [%s] connected=%d hint=%d\n",
                        d.name.c_str(), d.mac.c_str(), d.system_connected, d.is_earphone_hint);
        }
        BtSession sess;
        sess.set_callbacks(
            [](const std::string& l) { std::printf("  %s\n", l.c_str()); },
            nullptr, nullptr);
        int wsa = 0;
        bool ok = sess.connect_and_auth("00:BB:43:7B:CD:0B", 6000, &wsa);
        assert(ok && "Live connect_and_auth failed");

        auto orig = sess.snapshot();
        std::printf("[LIVE SNAPSHOT] name=%s fw=%s vid=%04X pid=%04X sn=%s auth=%d\n",
                    orig.name.c_str(), orig.fw_version.c_str(), orig.vid, orig.pid,
                    orig.sn.c_str(), orig.authenticated);
        std::printf("[LIVE BATTERY] L=%d%%(chg=%d) R=%d%%(chg=%d) Box=%d%%(chg=%d) Overall=%d%%\n",
                    orig.battery.left.percent, orig.battery.left.charging,
                    orig.battery.right.percent, orig.battery.right.charging,
                    orig.battery.box.percent, orig.battery.box.charging,
                    orig.battery.overall);

        // Test 1: Multipoint toggle roundtrip
        bool mp_orig = orig.multipoint;
        assert(sess.set_multipoint(!mp_orig));
        assert(sess.snapshot().multipoint == !mp_orig);
        assert(sess.set_multipoint(mp_orig));
        assert(sess.snapshot().multipoint == mp_orig);
        std::printf("[PASS] Live Multipoint (CFG 4) toggle & readback verified!\n");

        // Test 2: ANC 3 manual gears + Adaptive ANC (CFG 37) without dropping ANC
        assert(sess.set_anc(ANC_MODE_NOISE, ANC_LEVEL_DEEP, false));
        std::this_thread::sleep_for(std::chrono::milliseconds(450));
        assert(sess.snapshot().anc_mode == ANC_MODE_NOISE && sess.snapshot().anc_level == ANC_LEVEL_DEEP);

        assert(sess.set_auto_noise(true));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        assert(sess.snapshot().anc_mode == ANC_MODE_NOISE && sess.snapshot().auto_noise == true);

        // Switching back to manual Mild (1) automatically clears auto_noise and keeps ANC ON
        assert(sess.set_anc(ANC_MODE_NOISE, ANC_LEVEL_MILD, false));
        std::this_thread::sleep_for(std::chrono::milliseconds(450));
        assert(sess.snapshot().anc_mode == ANC_MODE_NOISE && sess.snapshot().anc_level == ANC_LEVEL_MILD && !sess.snapshot().auto_noise);
        std::printf("[PASS] Live ANC 3-gear + Adaptive ANC (CFG 37) verified (ANC never drops)!\n");

        sess.close();
        std::printf("[PASS] Live C++ RFCOMM session verified!\n");
    }

    return 0;
}
