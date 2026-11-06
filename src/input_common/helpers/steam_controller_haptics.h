// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "common/common_types.h"
#include "common/input.h"

struct SDL_Gamepad;
struct SDL_hid_device;

namespace InputCommon {

/**
 * Plays Switch HD Rumble on the haptic actuators of a natively driven Steam Controller.
 *
 * Each emulated side (left / right Joy-Con actuator) drives the matching side of the controller
 * with a single tone. The two HD Rumble bands of a side are merged into one tone whose pitch is
 * their energy weighted log frequency and whose loudness is their combined RMS amplitude, so a
 * single dominant band passes through unchanged. Writes run on a worker thread and are paced,
 * so a slow wireless link never stalls emulation.
 *
 * The band merge, the paced latest-value-wins writer and the 2 s hold follow the planned Valve
 * haptics backend in RenHDrum (https://github.com/trzz1214/RenHDrum, docs/research/08,
 * Copyright (c) 2026 Three-Dola, MIT OR Apache-2.0).
 */
class SteamControllerHaptics {
public:
    enum class Model {
        Original, ///< Steam Controller (2015), trackpad LRAs driven by pulse trains
        Triton,   ///< Steam Controller (2026), grip LRAs driven by tone reports
    };

    struct Tone {
        f32 frequency{};
        f32 amplitude{};
    };

    /// One encoded report for the device, ready to write.
    struct Report {
        std::array<u8, 65> data{};
        std::size_t size{};
    };

    enum class Target : u8 {
        Left,
        Right,
        Both,
    };

    /// Returns nullptr when the gamepad is not a Steam Controller driven by SDL's HIDAPI driver.
    static std::unique_ptr<SteamControllerHaptics> Create(SDL_Gamepad* gamepad);

    ~SteamControllerHaptics();

    SteamControllerHaptics(const SteamControllerHaptics&) = delete;
    SteamControllerHaptics& operator=(const SteamControllerHaptics&) = delete;

    /// Amplitudes are linear in [0, 1]. Returns immediately; the device is written later.
    void SetVibration(const Common::Input::VibrationStatus& vibration);

    Model GetModel() const {
        return model;
    }

    /// Merges both HD Rumble bands of one side into a single tone.
    static Tone MergeBands(const Common::Input::VibrationStatus& vibration);

    /// Converts a linear amplitude to the tone report's dB gain, or nullopt when it is silent.
    static std::optional<s8> AmplitudeToGainDb(f32 amplitude);

    /// 2026 model: output report 0x83 (LFO tone) on the grip actuators.
    static Report EncodeTritonTone(Target target, s8 gain_db, u16 frequency, u16 duration_ms);
    static Report EncodeTritonStop(Target target);

    /// 2015 model: feature report 0x8F (haptic pulse train) on one trackpad actuator.
    static Report EncodeOriginalPulse(Target target, Tone tone, u32 train_us);

private:
    using Clock = std::chrono::steady_clock;

    /// Written by SetVibration, read by the worker under the mutex.
    struct Request {
        Tone tone{};
        Clock::time_point time{};
        bool fresh{};
    };

    /// Owned by the worker thread.
    struct Output {
        bool playing{};
        s8 gain{};
        u16 frequency{};
        Tone tone{};
        Clock::time_point sent_at{};
    };

    struct Desired {
        bool on{};
        s8 gain{};
        u16 frequency{};
        Tone tone{};
        bool fresh{};
    };

    SteamControllerHaptics(Model model, SDL_Gamepad* gamepad, SDL_hid_device* device,
                           bool is_bluetooth);

    void WorkerLoop(std::stop_token stop_token);
    Clock::time_point ServiceTriton(Clock::time_point now, const std::array<Desired, 2>& desired);
    Clock::time_point ServiceOriginal(Clock::time_point now,
                                      const std::array<Desired, 2>& desired);
    bool Write(const Report& report);

    const Model model;
    SDL_Gamepad* const gamepad;
    SDL_hid_device* const device;
    const Clock::duration write_interval;

    std::mutex mutex;
    std::condition_variable_any wake;
    std::array<Request, 2> requests{};

    std::array<Output, 2> outputs{};
    Clock::time_point last_write{};
    u32 write_failures{};
    std::jthread worker;
};

} // namespace InputCommon
