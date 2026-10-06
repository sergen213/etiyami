#pragma once
#include "assets.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace yami {
// All failures throw std::runtime_error; callers own the SDL subsystem lifecycle.
struct VideoFrame {
    int width{}, height{}, stride{};
    double pts{};
    // Decoder-owned, top-left origin RGBA8. Valid until next next()/seek()/destruction.
    std::span<const std::uint8_t> rgba;
};
class VideoDecoder {
public:
    explicit VideoDecoder(const std::filesystem::path& path);
    ~VideoDecoder();
    VideoDecoder(VideoDecoder&&) noexcept;
    VideoDecoder& operator=(VideoDecoder&&) noexcept;
    int width() const noexcept;
    int height() const noexcept;
    double fps() const noexcept;
    double duration() const noexcept;
    std::int64_t frame_count() const noexcept; // Exact stream nb_frames; 1 for stills, 0 if unknown.
    bool next(VideoFrame& frame); // AVI sample timeline (holds no-image samples) or still image; false at EOF.
    void seek(double seconds); // Next frame is first frame at or after seconds.
    bool eof() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct AudioInfo { int sample_rate{}, channels{}; double duration{}; };
// Bounded-memory decoder. Output is interleaved float PCM at requested rate/channels.
class AudioDecoder {
public:
    explicit AudioDecoder(const std::filesystem::path& path, int rate = 44100, int channels = 2);
    ~AudioDecoder();
    AudioDecoder(AudioDecoder&&) noexcept;
    AudioDecoder& operator=(AudioDecoder&&) noexcept;
    const AudioInfo& source_info() const noexcept;
    std::size_t read(std::span<float> output); // Returns frames, not samples.
    void seek(double seconds);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
using AudioPosition = Vec3;
struct SoundDefinition {
    std::string name;
    std::filesystem::path path;
    bool loop{}, streaming{}, positional{};
    std::uint8_t volume = 100; // Original byte percentage 0..255 (scripts exceed 100); FMOD clamps output.
    int pool{}; // Original 0: one-shot, 1: spatial looping, 2: remaining channels.
};
class AudioSystem {
public:
    explicit AudioSystem(int sample_rate = 44100);
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;
    void open_device(); // Optional; SDL_INIT_AUDIO must already be initialized.
    void register_sound(SoundDefinition definition); // Missing paths retain an unavailable original null sample and report it.
    void register_original_sounds(const std::filesystem::path& game_root);
    bool registered(std::string_view name) const noexcept;
    int play(std::string_view name, AudioPosition position = {}, int percentage = -1); // Default registered percentage; -1 on music toggle-stop, unknown name, or unavailable sample.
    void stop(int channel);
    void stop(std::string_view name);
    void pause(int channel, bool paused);
    void volume(int channel, std::uint8_t volume);
    void set_volume(std::string_view name, std::uint32_t percentage, bool effects);
    void position(int channel, AudioPosition position);
    bool playing(int channel) const;
    void rewind(std::string_view name);
    void listener(AudioPosition position); // Original fixed forward +Z, up +Y, zero velocity.
    void master_volume(float music, float effects); // Original defaults 0.5 each, range 0..1.
    void update(); // Feed bounded SDL queue; call regularly from main loop.
    void render(std::span<float> interleaved_stereo); // Same mixer, deterministic offline use.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct CutsceneAssociation { std::string_view name, video, sound; };
std::span<const CutsceneAssociation> original_cutscenes() noexcept;
} // namespace yami
