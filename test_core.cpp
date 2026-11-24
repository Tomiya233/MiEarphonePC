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
    // Keep logs visible even when a later assert aborts the process.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
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

    // 6. Raw-stream auth packet classifier (APK BluetoothAuth.k)
    {
        uint8_t body[16] = {0};
        auto challenge = make_raw_auth_packet(0x00, body);
        auto response = make_raw_auth_packet(0x01, body);
        assert(challenge.size() == 17 && response.size() == 17);
        assert(raw_auth_packet_len(challenge) == 17);
        assert(raw_auth_packet_len(response) == 17);

        std::vector<uint8_t> pass = {0x02, 'p', 'a', 's', 's'};
        assert(raw_auth_packet_len(pass) == 5);

        // A framed RCSP packet must never be mistaken for a raw one
        auto framed = build_command(CMD_AUTH_CHECK, {0x01}, 1, APP_AUTH);
        assert(raw_auth_packet_len(framed) == 0);

        // Too short / unknown tag
        std::vector<uint8_t> shorty = {0x00, 0x11, 0x22};
        assert(raw_auth_packet_len(shorty) == 0);
        std::vector<uint8_t> junk(20, 0x03);
        assert(raw_auth_packet_len(junk) == 0);
        std::printf("[PASS] Raw-stream auth packet classifier (17B / 5B / framed reject)\n");
    }

    // 7. Model table rebuilt from APK DeviceVidPidTypeUtilsKt (decimal -> hex)
    {
        auto n76 = find_device_profile(0x2717, 0x506C);   // 20588
        assert(n76 && std::string(n76->codename) == "N76");
        assert(std::string(n76->brand) == "Redmi");

        auto o71 = find_device_profile(0x2717, 0x50AB);   // 20651
        assert(o71 && std::string(o71->codename) == "O71");
        assert(std::string(o71->brand) == "Xiaomi");

        auto m75a = find_device_profile(0x2717, 0x5044);  // 20548
        assert(m75a && std::string(m75a->brand) == "Xiaomi");

        auto l76 = find_device_profile(0x5A4D, 0xEA0E);   // 59918
        assert(l76 && std::string(l76->codename) == "L76");

        auto k73 = find_device_profile(0x2717, 0x5025);   // 20517
        assert(k73 && std::string(k73->brand) == "Xiaomi");

        assert(!find_device_profile(0x2717, 0x0001).has_value());
        assert(!find_device_profile(0, 0).has_value());
        assert(std::string(vendor_display_name(0x2717)) == "Xiaomi");
        std::printf("[PASS] Device profile table (Xiaomi/Redmi/ZMI VID:PID)\n");
    }

    // 8. Transport naming for logs / UI
    {
        assert(std::string(channel_kind_name(ChannelKind::MiuiSppFd2d)).find("FD2D") != std::string::npos);
        assert(std::string(channel_kind_name(ChannelKind::XiaoAiSpp)).find("XiaoAi") != std::string::npos);
        assert(std::string(auth_mode_name(AuthMode::Framed)) == "Framed RCSP");
        assert(std::string(auth_mode_name(AuthMode::RawStream)) == "Raw stream");
        std::printf("[PASS] Channel / auth-mode naming\n");
    }

    // 9. Raw-stream handshake judging (offline simulation of the state machine)
    {
        uint8_t rnd[16];
        for (int i = 0; i < 16; ++i) rnd[i] = static_cast<uint8_t>(0x40 + i);
        auto our_enc = auth_response(rnd);
        uint8_t chal[16] = {0};

        // Device echoes our ciphertext -> it holds the shared key
        auto echo = make_raw_auth_packet(0x01, our_enc.data());
        assert(judge_raw_auth_packet(echo, our_enc.data(), chal) == RawAuthVerdict::ProvedKey);

        // Echo with a wrong body must not be accepted
        std::array<uint8_t, 16> wrong = our_enc;
        wrong[15] ^= 0xFF;
        auto bad_echo = make_raw_auth_packet(0x01, wrong.data());
        assert(judge_raw_auth_packet(bad_echo, our_enc.data(), chal) == RawAuthVerdict::Ignore);

        // Device issues its own nonce -> we capture it and answer
        uint8_t devrnd[16];
        for (int i = 0; i < 16; ++i) devrnd[i] = static_cast<uint8_t>(0xA0 + i);
        auto devchal = make_raw_auth_packet(0x00, devrnd);
        assert(judge_raw_auth_packet(devchal, our_enc.data(), chal) == RawAuthVerdict::Challenge);
        assert(std::memcmp(chal, devrnd, 16) == 0);

        // Our answer to that challenge is [0x01][auth_response(devrnd)]
        auto answer = auth_response(devrnd);
        auto answer_pkt = make_raw_auth_packet(0x01, answer.data());
        assert(answer_pkt.size() == 17 && answer_pkt[0] == 0x01);
        assert(std::memcmp(answer_pkt.data() + 1, answer.data(), 16) == 0);

        // Accept token
        std::vector<uint8_t> pass = {0x02, 'p', 'a', 's', 's'};
        assert(judge_raw_auth_packet(pass, our_enc.data(), chal) == RawAuthVerdict::PassToken);

        // Framed RCSP data is never raw auth
        auto framed = build_command(CMD_GET_TARGET_INFO, {0xFF, 0xFF, 0xFF, 0xFF}, 0, APP_AUTH);
        assert(judge_raw_auth_packet(framed, our_enc.data(), chal) == RawAuthVerdict::Ignore);
        std::printf("[PASS] Raw-stream handshake judging (echo / challenge / pass / framed)\n");
    }

    // 10. SPP service records must reproduce the APK UUID literals exactly,
    //    in the APK's probe order, with the auth mode each one implies.
    {
        auto cat = spp_service_catalog();
        assert(cat.size() == 3);
        assert(spp_service_uuid_text(cat[0]) == "0000fd2d-0000-1000-8000-00805f9b34fb");
        assert(spp_service_uuid_text(cat[1]) == "00001101-0000-1000-8000-008584d01810");
        assert(spp_service_uuid_text(cat[2]) == "00001101-0000-1000-8000-00805f9b34fb");
        assert(cat[0].kind == ChannelKind::MiuiSppFd2d && cat[0].mode == AuthMode::Framed);
        assert(cat[1].kind == ChannelKind::XiaoAiSpp && cat[1].mode == AuthMode::RawStream);
        assert(cat[2].kind == ChannelKind::StandardSpp && cat[2].mode == AuthMode::RawStream);
        std::printf("[PASS] SPP service catalog (FD2D / XiaoAi / standard + auth mode)\n");
    }

    // 6. Optional live hardware smoke & regression test
    if (argc > 1 && std::string(argv[1]) == "--live") {
        auto devs = enumerate_paired_devices();
        std::printf("[LIVE] Paired Bluetooth devices (%zu):\n", devs.size());
        for (const auto& d : devs) {
            std::printf("  - %s [%s] connected=%d hint=%d\n",
                        d.name.c_str(), d.mac.c_str(), d.system_connected, d.is_earphone_hint);
        }
        std::string target_mac = (argc > 2) ? argv[2] : "";
        for (const auto& d : devs) {
            if (target_mac.empty() && d.is_earphone_hint) {
                target_mac = d.mac;
                if (d.system_connected) break;
            }
        }
        assert(!target_mac.empty() && "No paired earphone found; pass MAC as argv[2]");

        BtSession sess;
        sess.set_callbacks(
            [](const std::string& l) { std::printf("  %s\n", l.c_str()); },
            nullptr, nullptr);
        int wsa = 0;
        bool ok = sess.connect_and_auth(target_mac, 6000, &wsa);
        if (!ok) {
            std::printf("[LIVE FAIL] connect_and_auth(MAC=%s) failed, WSA=%d\n", target_mac.c_str(), wsa);
            std::printf("            10060=timeout 10064=host down -> earbud asleep/in case;\n"
                        "            otherwise check that no other client holds the SPP channel\n");
        }
        assert(ok && "Live connect_and_auth failed");

        auto orig = sess.snapshot();
        std::printf("[LIVE SNAPSHOT] name=%s fw=%s vid=%04X pid=%04X sn=%s auth=%d\n",
                    orig.name.c_str(), orig.fw_version.c_str(), orig.vid, orig.pid,
                    orig.sn.c_str(), orig.authenticated);
        std::printf("[LIVE TRANSPORT] channel=%s auth_mode=%s brand=%s model=%s\n",
                    channel_kind_name(orig.channel), auth_mode_name(orig.auth_mode),
                    orig.brand.c_str(), orig.model_name.c_str());
        if (auto prof = sess.profile()) {
            std::printf("[LIVE PROFILE] codename=%s market=%s brand=%s anc_gears=%u trans_gears=%u "
                        "spatial=%d multipoint=%d\n",
                        prof->codename, prof->market_name, prof->brand,
                        prof->anc_gears, prof->trans_gears,
                        prof->spatial ? 1 : 0, prof->multipoint ? 1 : 0);
        } else if (orig.vid && orig.pid) {
            std::printf("[LIVE PROFILE] VID:PID %04X:%04X not in the adaptation table\n",
                        orig.vid, orig.pid);
        }
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
