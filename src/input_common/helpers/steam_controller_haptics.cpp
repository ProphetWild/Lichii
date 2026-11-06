// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstring>

#include <SDL3/SDL.h>

#include "common/logging.h"
#include "common/thread.h"
#include "input_common/helpers/steam_controller_haptics.h"

namespace InputCommon {

namespace {
using namespace std::chrono_literals;

constexpr u16 VALVE_VENDOR_ID = 0x28DE;

// Below about -48 dB a side is treated as silent.
constexpr f32 SILENT_AMPLITUDE = 0.004f;
// 0 dB is the actuator's unity level; positive gains drive the firmware limiter into clipping.
constexpr int MIN_GAIN_DB = -40;
constexpr int MAX_GAIN_DB = 0;
// HD Rumble spans 40-1252 Hz.
constexpr f32 MIN_FREQUENCY = 40.0f;
constexpr f32 MAX_FREQUENCY = 1300.0f;
constexpr f32 NEUTRAL_FREQUENCY = 160.0f;

// Resend only when the tone changes by at least this much.
constexpr int GAIN_STEP_DB = 1;
constexpr f32 FREQUENCY_STEP = 0.03f;

// A side the game stops updating goes quiet after this long, matching the plain SDL rumble path.
constexpr auto HOLD_TIME = 2000ms;
// While the game keeps updating a side with values below the resend steps, refresh the tone
// before the device's own timeout ends it.
constexpr auto REFRESH_TIME = 1000ms;

constexpr auto WIRED_WRITE_INTERVAL = 8ms;
constexpr auto BLUETOOTH_WRITE_INTERVAL = 16ms;
constexpr auto ORIGINAL_WRITE_INTERVAL = 15ms;
constexpr u32 MAX_WRITE_FAILURES = 100;

// 2026 model output report 0x83, MsgHapticLfoTone in SDL's steam/controller_structs.h. Side codes
// 0/1 are the trackpads, 3/4 the grips and 5 both grips.
constexpr u8 TRITON_REPORT_LFO_TONE = 0x83;
constexpr u8 TRITON_LEFT_GRIP = 3;
constexpr u8 TRITON_RIGHT_GRIP = 4;
constexpr u8 TRITON_BOTH_GRIPS = 5;
constexpr std::size_t TRITON_TONE_REPORT_SIZE = 10;

// 2015 model feature report 0x8F, MsgFireHapticPulse. which_pad 0 is right, 1 is left. The pulse
// train is re-armed before it runs out, since there is no sustained tone command.
constexpr u8 ORIGINAL_REPORT_HAPTIC_PULSE = 0x8F;
constexpr u8 ORIGINAL_LEFT_PAD = 1;
constexpr u8 ORIGINAL_RIGHT_PAD = 0;
constexpr std::size_t ORIGINAL_REPORT_SIZE = 65;
constexpr auto ORIGINAL_TRAIN_TIME = 60ms;
constexpr auto ORIGINAL_KEEP_ALIVE = 40ms;

std::optional<SteamControllerHaptics::Model> ModelFromProduct(u16 product_id) {
    switch (product_id) {
    case 0x1302: // Wired
    case 0x1303: // Bluetooth
    case 0x1304: // Steam Controller Puck
    case 0x1305: // Steam Machine receiver
        return SteamControllerHaptics::Model::Triton;
    case 0x1101: // Legacy
    case 0x1102: // Wired
    case 0x1105: // Bluetooth
    case 0x1106: // Bluetooth
    case 0x1142: // Wireless receiver
    case 0x1201: // Wired (HEADCRAB)
    case 0x1202: // Bluetooth (HEADCRAB)
        return SteamControllerHaptics::Model::Original;
    default:
        return std::nullopt;
    }
}

bool IsBluetooth(u16 product_id) {
    return product_id == 0x1303 || product_id == 0x1105 || product_id == 0x1106 ||
           product_id == 0x1202;
}

void PutU16(u8* data, u16 value) {
    data[0] = static_cast<u8>(value & 0xFF);
    data[1] = static_cast<u8>(value >> 8);
}

u8 TritonSide(SteamControllerHaptics::Target target) {
    switch (target) {
    case SteamControllerHaptics::Target::Left:
        return TRITON_LEFT_GRIP;
    case SteamControllerHaptics::Target::Right:
        return TRITON_RIGHT_GRIP;
    case SteamControllerHaptics::Target::Both:
    default:
        return TRITON_BOTH_GRIPS;
    }
}

SteamControllerHaptics::Target SideTarget(std::size_t side) {
    return side == 0 ? SteamControllerHaptics::Target::Left
                     : SteamControllerHaptics::Target::Right;
}

const char* ModelName(SteamControllerHaptics::Model model) {
    return model == SteamControllerHaptics::Model::Triton ? "2026" : "2015";
}
} // Anonymous namespace

std::unique_ptr<SteamControllerHaptics> SteamControllerHaptics::Create(SDL_Gamepad* gamepad) {
    if (gamepad == nullptr || SDL_GetGamepadVendor(gamepad) != VALVE_VENDOR_ID) {
        return nullptr;
    }
    const u16 product_id = SDL_GetGamepadProduct(gamepad);
    const auto model = ModelFromProduct(product_id);
    if (!model) {
        return nullptr;
    }
    // Only SDL's HIDAPI driver talks to the hardware. Steam's virtual gamepad has its own ids.
    const SDL_GUID guid = SDL_GetJoystickGUID(SDL_GetGamepadJoystick(gamepad));
    if (guid.data[14] != 'h') {
        return nullptr;
    }

    SDL_hid_device* device = nullptr;
    if (*model == Model::Triton) {
        // SDL has no call for the tone reports, so they go through a second, shared handle.
        const char* path = SDL_GetGamepadPath(gamepad);
        device = path != nullptr ? SDL_hid_open_path(path) : nullptr;
        if (device == nullptr) {
            LOG_WARNING(Input, "Steam Controller (2026): could not open {} for haptics: {}",
                        path != nullptr ? path : "(no path)", SDL_GetError());
            return nullptr;
        }
    }

    LOG_INFO(Input, "Steam Controller ({}) PID {:04X}: translating HD Rumble to its haptics",
             ModelName(*model), product_id);
    return std::unique_ptr<SteamControllerHaptics>(
        new SteamControllerHaptics(*model, gamepad, device, IsBluetooth(product_id)));
}

SteamControllerHaptics::SteamControllerHaptics(Model model_, SDL_Gamepad* gamepad_,
                                               SDL_hid_device* device_, bool is_bluetooth)
    : model{model_}, gamepad{gamepad_}, device{device_},
      write_interval{model_ == Model::Original
                         ? Clock::duration{ORIGINAL_WRITE_INTERVAL}
                         : Clock::duration{is_bluetooth ? BLUETOOTH_WRITE_INTERVAL
                                                        : WIRED_WRITE_INTERVAL}} {
    worker = std::jthread([this](std::stop_token stop_token) { WorkerLoop(stop_token); });
}

SteamControllerHaptics::~SteamControllerHaptics() {
    worker.request_stop();
    if (worker.joinable()) {
        worker.join();
    }
    if (device != nullptr) {
        Write(EncodeTritonStop(Target::Both));
        SDL_hid_close(device);
    }
}

void SteamControllerHaptics::SetVibration(const Common::Input::VibrationStatus& vibration) {
    const Request request{
        .tone = MergeBands(vibration),
        .time = Clock::now(),
        .fresh = true,
    };
    {
        std::scoped_lock lock{mutex};
        switch (vibration.side) {
        case Common::Input::VibrationSide::Left:
            requests[0] = request;
            break;
        case Common::Input::VibrationSide::Right:
            requests[1] = request;
            break;
        case Common::Input::VibrationSide::Unknown:
        default:
            requests[0] = request;
            requests[1] = request;
            break;
        }
    }
    wake.notify_one();
}

SteamControllerHaptics::Tone SteamControllerHaptics::MergeBands(
    const Common::Input::VibrationStatus& vibration) {
    const f32 low = std::clamp(vibration.low_amplitude, 0.0f, 1.0f);
    const f32 high = std::clamp(vibration.high_amplitude, 0.0f, 1.0f);
    const f32 low_power = low * low;
    const f32 high_power = high * high;
    const f32 total_power = low_power + high_power;
    if (!(total_power > 0.0f)) {
        return {NEUTRAL_FREQUENCY, 0.0f};
    }
    const f32 low_frequency = std::clamp(vibration.low_frequency, MIN_FREQUENCY, MAX_FREQUENCY);
    const f32 high_frequency = std::clamp(vibration.high_frequency, MIN_FREQUENCY, MAX_FREQUENCY);
    const f32 log_frequency =
        (low_power * std::log(low_frequency) + high_power * std::log(high_frequency)) /
        total_power;
    return {
        .frequency = std::exp(log_frequency),
        .amplitude = (std::min)(std::sqrt(total_power), 1.0f),
    };
}

std::optional<s8> SteamControllerHaptics::AmplitudeToGainDb(f32 amplitude) {
    if (!(amplitude >= SILENT_AMPLITUDE)) {
        return std::nullopt;
    }
    const f32 gain = std::round(20.0f * std::log10((std::min)(amplitude, 1.0f)));
    return static_cast<s8>(std::clamp(static_cast<int>(gain), MIN_GAIN_DB, MAX_GAIN_DB));
}

SteamControllerHaptics::Report SteamControllerHaptics::EncodeTritonTone(Target target, s8 gain_db,
                                                                        u16 frequency,
                                                                        u16 duration_ms) {
    Report report{};
    report.size = TRITON_TONE_REPORT_SIZE;
    report.data[0] = TRITON_REPORT_LFO_TONE;
    report.data[1] = TritonSide(target);
    report.data[2] = static_cast<u8>(gain_db);
    PutU16(&report.data[3], frequency);
    PutU16(&report.data[5], duration_ms);
    // lfo_freq and lfo_depth stay 0 for a plain tone.
    return report;
}

SteamControllerHaptics::Report SteamControllerHaptics::EncodeTritonStop(Target target) {
    // -128 dB, the stop form used by tools that drive these reports on hardware.
    Report report{};
    report.size = TRITON_TONE_REPORT_SIZE;
    report.data[0] = TRITON_REPORT_LFO_TONE;
    report.data[1] = TritonSide(target);
    report.data[2] = 0x80;
    report.data[6] = 0x80;
    return report;
}

SteamControllerHaptics::Report SteamControllerHaptics::EncodeOriginalPulse(Target target, Tone tone,
                                                                           u32 train_us) {
    // One pulse per period of the tone. The on-time carries the loudness, like a PWM duty cycle.
    const f32 frequency = std::clamp(tone.frequency, MIN_FREQUENCY, MAX_FREQUENCY);
    const u32 period_us = static_cast<u32>(std::lround(1'000'000.0f / frequency));
    const u32 on_us = std::clamp<u32>(
        static_cast<u32>(std::lround(static_cast<f32>(period_us) * 0.5f *
                                     std::clamp(tone.amplitude, 0.0f, 1.0f))),
        10, period_us / 2);
    const u32 off_us = period_us - on_us;
    const u32 count = (std::max)(1u, train_us / period_us);

    Report report{};
    report.size = ORIGINAL_REPORT_SIZE;
    report.data[0] = 0; // Report ID
    report.data[1] = ORIGINAL_REPORT_HAPTIC_PULSE;
    report.data[2] = 7; // Payload length
    report.data[3] = target == Target::Left ? ORIGINAL_LEFT_PAD : ORIGINAL_RIGHT_PAD;
    PutU16(&report.data[4], static_cast<u16>(on_us));
    PutU16(&report.data[6], static_cast<u16>(off_us));
    PutU16(&report.data[8], static_cast<u16>((std::min)(count, 0xFFFFu)));
    return report;
}

void SteamControllerHaptics::WorkerLoop(std::stop_token stop_token) {
    Common::SetCurrentThreadName("SteamHaptics");
    auto deadline = Clock::time_point::max();
    while (!stop_token.stop_requested()) {
        std::array<Desired, 2> desired{};
        {
            std::unique_lock lock{mutex};
            const auto has_request = [this] { return requests[0].fresh || requests[1].fresh; };
            if (deadline == Clock::time_point::max()) {
                wake.wait(lock, stop_token, has_request);
            } else {
                wake.wait_until(lock, stop_token, deadline, has_request);
            }
            if (stop_token.stop_requested()) {
                return;
            }
            const auto now = Clock::now();
            for (std::size_t side = 0; side < requests.size(); ++side) {
                Request& request = requests[side];
                request.fresh = false;
                Desired& wanted = desired[side];
                wanted.tone = request.tone;
                wanted.fresh = request.time > outputs[side].sent_at;
                if (now - request.time >= HOLD_TIME) {
                    continue;
                }
                const auto gain = AmplitudeToGainDb(request.tone.amplitude);
                if (!gain) {
                    continue;
                }
                wanted.on = true;
                wanted.gain = *gain;
                wanted.frequency = static_cast<u16>(std::lround(
                    std::clamp(request.tone.frequency, MIN_FREQUENCY, MAX_FREQUENCY)));
            }
            // Wake again when a held request runs out.
            deadline = Clock::time_point::max();
            for (std::size_t side = 0; side < requests.size(); ++side) {
                if (desired[side].on) {
                    deadline = (std::min)(deadline, requests[side].time + HOLD_TIME);
                }
            }
        }
        if (write_failures >= MAX_WRITE_FAILURES) {
            deadline = Clock::time_point::max();
            continue;
        }
        const auto now = Clock::now();
        const auto next = model == Model::Triton ? ServiceTriton(now, desired)
                                                 : ServiceOriginal(now, desired);
        deadline = (std::min)(deadline, next);
    }
}

SteamControllerHaptics::Clock::time_point SteamControllerHaptics::ServiceTriton(
    Clock::time_point now, const std::array<Desired, 2>& desired) {
    for (auto& output : outputs) {
        // The tone was sent with HOLD_TIME as its duration, so the device has stopped it.
        if (output.playing && now - output.sent_at >= HOLD_TIME) {
            output.playing = false;
        }
    }

    const auto needs_write = [&](std::size_t side) {
        const Desired& wanted = desired[side];
        const Output& output = outputs[side];
        if (!wanted.on) {
            return output.playing;
        }
        if (!output.playing) {
            return true;
        }
        if (std::abs(wanted.gain - output.gain) >= GAIN_STEP_DB) {
            return true;
        }
        if (std::abs(static_cast<f32>(wanted.frequency) - static_cast<f32>(output.frequency)) >=
            static_cast<f32>(output.frequency) * FREQUENCY_STEP) {
            return true;
        }
        return wanted.fresh && now - output.sent_at >= REFRESH_TIME;
    };

    std::array<bool, 2> pending{needs_write(0), needs_write(1)};
    if (pending[0] || pending[1]) {
        if (now - last_write < write_interval) {
            return last_write + write_interval;
        }
        const auto commit = [&](std::size_t side) {
            Output& output = outputs[side];
            output.playing = desired[side].on;
            output.gain = desired[side].gain;
            output.frequency = desired[side].frequency;
            output.tone = desired[side].tone;
            output.sent_at = now;
            pending[side] = false;
        };
        const auto encode = [&](Target target, const Desired& wanted) {
            return wanted.on ? EncodeTritonTone(target, wanted.gain, wanted.frequency,
                                                static_cast<u16>(HOLD_TIME.count()))
                             : EncodeTritonStop(target);
        };
        const bool same = desired[0].on == desired[1].on &&
                          (!desired[0].on || (desired[0].gain == desired[1].gain &&
                                              desired[0].frequency == desired[1].frequency));
        if (pending[0] && pending[1] && same) {
            Write(encode(Target::Both, desired[0]));
            commit(0);
            commit(1);
        } else {
            const std::size_t side = pending[0] ? 0 : 1;
            Write(encode(SideTarget(side), desired[side]));
            commit(side);
        }
        last_write = now;
    }

    auto next = Clock::time_point::max();
    if (pending[0] || pending[1]) {
        next = last_write + write_interval;
    }
    for (std::size_t side = 0; side < outputs.size(); ++side) {
        const Output& output = outputs[side];
        if (output.playing) {
            next = (std::min)(next, output.sent_at + HOLD_TIME);
            if (desired[side].on && desired[side].fresh) {
                next = (std::min)(next, output.sent_at + REFRESH_TIME);
            }
        }
    }
    return next;
}

SteamControllerHaptics::Clock::time_point SteamControllerHaptics::ServiceOriginal(
    Clock::time_point now, const std::array<Desired, 2>& desired) {
    std::array<bool, 2> pending{};
    for (std::size_t side = 0; side < outputs.size(); ++side) {
        Output& output = outputs[side];
        const Desired& wanted = desired[side];
        if (!wanted.on) {
            // The last pulse train runs out by itself.
            output.playing = false;
            continue;
        }
        pending[side] = !output.playing || now - output.sent_at >= ORIGINAL_KEEP_ALIVE ||
                        std::abs(wanted.gain - output.gain) >= GAIN_STEP_DB ||
                        std::abs(static_cast<f32>(wanted.frequency) -
                                 static_cast<f32>(output.frequency)) >=
                            static_cast<f32>(output.frequency) * FREQUENCY_STEP;
    }

    if (pending[0] || pending[1]) {
        if (now - last_write < write_interval) {
            return last_write + write_interval;
        }
        const std::size_t side = pending[0] ? 0 : 1;
        constexpr auto train_us =
            std::chrono::duration_cast<std::chrono::microseconds>(ORIGINAL_TRAIN_TIME).count();
        Write(EncodeOriginalPulse(SideTarget(side), desired[side].tone,
                                  static_cast<u32>(train_us)));
        Output& output = outputs[side];
        output.playing = true;
        output.gain = desired[side].gain;
        output.frequency = desired[side].frequency;
        output.tone = desired[side].tone;
        output.sent_at = now;
        pending[side] = false;
        last_write = now;
    }

    auto next = Clock::time_point::max();
    if (pending[0] || pending[1]) {
        next = last_write + write_interval;
    }
    for (const Output& output : outputs) {
        if (output.playing) {
            next = (std::min)(next, output.sent_at + ORIGINAL_KEEP_ALIVE);
        }
    }
    return next;
}

bool SteamControllerHaptics::Write(const Report& report) {
    bool written = false;
    if (model == Model::Triton) {
        written = device != nullptr && SDL_hid_write(device, report.data.data(), report.size) >= 0;
    } else {
        written = SDL_SendGamepadEffect(gamepad, report.data.data(), static_cast<int>(report.size));
    }
    if (written) {
        write_failures = 0;
        return true;
    }
    if (++write_failures == MAX_WRITE_FAILURES) {
        LOG_WARNING(Input, "Steam Controller ({}) haptics stopped after repeated write errors: {}",
                    ModelName(model), SDL_GetError());
    }
    return false;
}

} // namespace InputCommon
