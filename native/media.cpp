#include "media.hpp"
#include <SDL3/SDL_audio.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace yami {
namespace {
[[noreturn]] void fail(const std::string& what, int error) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, text, sizeof(text));
    throw std::runtime_error(what + ": " + text);
}
void checked(int result, const std::string& what) { if (result < 0) fail(what, result); }
void sdl_checked(bool result, const char* what) {
    if (!result) throw std::runtime_error(std::string(what) + ": " + SDL_GetError());
}
void finite_position(Vec3 p) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
        throw std::runtime_error("Nonfinite audio position");
}
struct Decode {
    AVFormatContext* format{};
    AVCodecContext* codec{};
    AVPacket* packet{};
    AVFrame* frame{};
    int stream{};
    bool drained{}, finished{};
    std::string path;
    Decode(const std::filesystem::path& file, AVMediaType type) {
        const auto utf8 = file.u8string();
        path.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        try {
            checked(avformat_open_input(&format, path.c_str(), nullptr, nullptr), "Opening " + path);
            checked(avformat_find_stream_info(format, nullptr), "Reading streams " + path);
            stream = av_find_best_stream(format, type, -1, -1, nullptr, 0);
            checked(stream, "Finding stream " + path);
            const auto* decoder = avcodec_find_decoder(format->streams[stream]->codecpar->codec_id);
            if (!decoder) throw std::runtime_error("Unsupported codec in " + path);
            codec = avcodec_alloc_context3(decoder);
            packet = av_packet_alloc();
            frame = av_frame_alloc();
            if (!codec || !packet || !frame) throw std::bad_alloc();
            checked(avcodec_parameters_to_context(codec, format->streams[stream]->codecpar), "Codec parameters " + path);
            checked(avcodec_open2(codec, decoder, nullptr), "Opening codec " + path);
        } catch (...) { release(); throw; }
    }
    ~Decode() { release(); }
    void release() {
        av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&codec);
        avformat_close_input(&format);
    }
    bool next() {
        if (finished) return false;
        for (;;) {
            const int received = avcodec_receive_frame(codec, frame);
            if (received == 0) return true;
            if (received == AVERROR_EOF) { finished = true; return false; }
            if (received != AVERROR(EAGAIN)) fail("Decoding " + path, received);
            if (drained) throw std::runtime_error("Codec failed to finish after drain: " + path);
            for (;;) {
                const int read = av_read_frame(format, packet);
                if (read == AVERROR_EOF) {
                    checked(avcodec_send_packet(codec, nullptr), "Draining " + path);
                    drained = true;
                    break;
                }
                if (read < 0) fail("Reading packet " + path, read);
                if (packet->stream_index != stream) { av_packet_unref(packet); continue; }
                const int sent = avcodec_send_packet(codec, packet);
                av_packet_unref(packet);
                if (sent < 0) fail("Submitting packet " + path, sent);
                break;
            }
        }
    }
    double origin() const {
        const auto* s = format->streams[stream];
        return s->start_time == AV_NOPTS_VALUE ? 0 : s->start_time * av_q2d(s->time_base);
    }
    double pts(double fallback) const {
        const auto timestamp = frame->best_effort_timestamp;
        return timestamp == AV_NOPTS_VALUE ? fallback : timestamp * av_q2d(format->streams[stream]->time_base) - origin();
    }
    double duration() const {
        const auto* s = format->streams[stream];
        if (s->duration != AV_NOPTS_VALUE) return s->duration * av_q2d(s->time_base);
        return format->duration == AV_NOPTS_VALUE ? 0 : double(format->duration) / AV_TIME_BASE;
    }
    void seek(double seconds) {
        if (!std::isfinite(seconds) || seconds < 0 || seconds > (double(std::numeric_limits<std::int64_t>::max()) / AV_TIME_BASE) - std::abs(origin()))
            throw std::runtime_error("Invalid seek time for " + path);
        const double timestamp = (seconds + origin()) / av_q2d(format->streams[stream]->time_base);
        if (timestamp < double(std::numeric_limits<std::int64_t>::min()) || timestamp >= double(std::numeric_limits<std::int64_t>::max()))
            throw std::runtime_error("Seek time overflow for " + path);
        checked(av_seek_frame(format, stream, static_cast<std::int64_t>(timestamp), AVSEEK_FLAG_BACKWARD), "Seeking " + path);
        avcodec_flush_buffers(codec);
        av_packet_unref(packet); av_frame_unref(frame);
        drained = finished = false;
    }
};
constexpr std::size_t block_frames = 2048;
// Exact FMOD 3.7.5 software panning table at unpacked DLL VA 100425a0.
// Its entries are floor(sqrt(index * 256)); calculate only at compile time.
constexpr auto pan_gain = [] {
    std::array<int,256> table{};
    for (int i=0;i<256;++i) {
        int value=0;
        while ((value+1)*(value+1)<=i*256) ++value;
        table[i]=value;
    }
    return table;
}();
}

struct VideoDecoder::Impl {
    Decode decode;
    SwsContext* scaler{};
    std::vector<std::uint8_t> pixels;
    int width{}, height{}, pixel_format{};
    double rate{}, length{}, previous{}, target = -1;
    std::int64_t frame_count{}, sample{};
    bool pending = true, ended{}, avi_samples{}, pixels_ready{}, decode_finished{};
    explicit Impl(const std::filesystem::path& path) : decode(path, AVMEDIA_TYPE_VIDEO) {
        if (!decode.next()) throw std::runtime_error("No decoded video frames in " + decode.path);
        width = decode.frame->width; height = decode.frame->height;
        pixel_format=decode.frame->format;
        if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() / 4 ||
            std::size_t(height) > std::numeric_limits<std::size_t>::max() / (std::size_t(width) * 4))
            throw std::runtime_error("Invalid video dimensions in " + decode.path);
        pixels.resize(std::size_t(width) * height * 4);
        rate = av_q2d(av_guess_frame_rate(decode.format, decode.format->streams[decode.stream], decode.frame));
        if (!std::isfinite(rate) || rate < 0) rate = 0;
        length = decode.duration();
        frame_count=decode.format->streams[decode.stream]->nb_frames;
        const std::string_view container=decode.format->iformat->name;
        if (frame_count==0 && (container=="image2" || container=="image2pipe" || container.ends_with("_pipe")))
            frame_count=1; // A filesystem still-image input, not a rounded duration*fps estimate.
        avi_samples = container == "avi" && frame_count > 0 && rate > 0;
        scaler = sws_getContext(width, height, static_cast<AVPixelFormat>(decode.frame->format), width, height,
                                AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler) throw std::runtime_error("Cannot convert video to RGBA: " + decode.path);
    }
    void convert() {
        if (decode.frame->width != width || decode.frame->height != height || decode.frame->format != pixel_format)
            throw std::runtime_error("Video format changed: " + decode.path);
        std::uint8_t* destination[] = {pixels.data()};
        const int stride[] = {width * 4};
        const int rows = sws_scale(scaler, decode.frame->data, decode.frame->linesize, 0, height, destination, stride);
        if (rows != height) throw std::runtime_error("Incomplete RGBA conversion: " + decode.path);
        pixels_ready = true;
    }
    ~Impl() { sws_freeContext(scaler); }
};
VideoDecoder::VideoDecoder(const std::filesystem::path& p) : impl_(std::make_unique<Impl>(p)) {}
VideoDecoder::~VideoDecoder() = default;
VideoDecoder::VideoDecoder(VideoDecoder&&) noexcept = default;
VideoDecoder& VideoDecoder::operator=(VideoDecoder&&) noexcept = default;
int VideoDecoder::width() const noexcept { return impl_->width; }
int VideoDecoder::height() const noexcept { return impl_->height; }
double VideoDecoder::fps() const noexcept { return impl_->rate; }
double VideoDecoder::duration() const noexcept { return impl_->length; }
std::int64_t VideoDecoder::frame_count() const noexcept { return impl_->frame_count; }
bool VideoDecoder::eof() const noexcept { return impl_->ended; }
bool VideoDecoder::next(VideoFrame& output) {
    auto& s = *impl_;
    if (s.avi_samples) {
        if (s.sample >= s.frame_count) { s.ended = true; output = {}; return false; }
        const double pts = double(s.sample) / s.rate;
        // AVIStreamGetFrame addresses samples, including Indeo's no-image packets.
        // Keep the preceding pixels for those slots; FFmpeg emits only changed images.
        while (!s.decode_finished) {
            if (!s.pending) {
                if (!s.decode.next()) { s.decode_finished = true; break; }
                s.pending = true;
            }
            const double decoded_pts = s.decode.pts(s.previous);
            if (decoded_pts > pts + 1e-9) break;
            s.convert();
            s.previous = decoded_pts + 1 / s.rate;
            s.pending = false;
        }
        if (!s.pixels_ready) throw std::runtime_error("AVI sample precedes its first image: " + s.decode.path);
        output = {s.width, s.height, s.width * 4, pts, s.pixels};
        ++s.sample;
        return true;
    }
    for (;;) {
        if (!s.pending && !s.decode.next()) { s.ended = true; output = {}; return false; }
        s.pending = false;
        const double pts = s.decode.pts(s.previous);
        s.previous = pts + (s.rate > 0 ? 1 / s.rate : 0);
        if (s.target >= 0 && pts + 1e-9 < s.target) continue;
        s.target = -1;
        s.convert();
        output = {s.width, s.height, s.width * 4, pts, s.pixels};
        return true;
    }
}
void VideoDecoder::seek(double seconds) {
    auto& s = *impl_; s.decode.seek(seconds); s.target = seconds;
    s.previous = seconds; s.pending = false; s.ended = false;
    s.decode_finished = s.pixels_ready = false;
    if (s.avi_samples)
        s.sample = seconds >= double(s.frame_count) / s.rate ? s.frame_count :
                   static_cast<std::int64_t>(std::ceil(seconds * s.rate - 1e-9));
}

struct AudioDecoder::Impl {
    Decode decode;
    SwrContext* resampler{};
    AudioInfo info;
    int rate{}, channels{};
    std::vector<float> buffer;
    std::size_t offset{}, available{};
    double next_pts{}, target = -1;
    bool ended{};
    explicit Impl(const std::filesystem::path& p, int r, int c) : decode(p, AVMEDIA_TYPE_AUDIO), rate(r), channels(c) {
        if (r <= 0 || r > 384000 || (c != 1 && c != 2)) throw std::runtime_error("Invalid audio output format");
        if (decode.codec->sample_rate <= 0 || decode.codec->ch_layout.nb_channels <= 0)
            throw std::runtime_error("Invalid source audio format: " + decode.path);
        info = {decode.codec->sample_rate, decode.codec->ch_layout.nb_channels, decode.duration()};
        AVChannelLayout layout{};
        av_channel_layout_default(&layout, c);
        const int result = swr_alloc_set_opts2(&resampler, &layout, AV_SAMPLE_FMT_FLT, r,
            &decode.codec->ch_layout, decode.codec->sample_fmt, decode.codec->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&layout);
        try { checked(result, "Creating resampler " + decode.path); checked(swr_init(resampler), "Initializing resampler " + decode.path); buffer.resize(65536u * c); }
        catch (...) { swr_free(&resampler); throw; }
    }
    ~Impl() { swr_free(&resampler); }
    bool refill() {
        for (;;) {
            if (ended) return false;
            const bool frame = decode.next();
            const auto delay = swr_get_delay(resampler, info.sample_rate);
            const int need = swr_get_out_samples(resampler, frame ? decode.frame->nb_samples : 0);
            if (need < 0) fail("Sizing resampled audio " + decode.path, need);
            if (need > int(buffer.size() / channels)) throw std::runtime_error("Audio frame exceeds fixed conversion buffer: " + decode.path);
            const double pts = frame ? decode.pts(next_pts) - double(delay) / info.sample_rate : next_pts;
            std::uint8_t* out = reinterpret_cast<std::uint8_t*>(buffer.data());
            const int count = swr_convert(resampler, &out, int(buffer.size() / channels),
                frame ? const_cast<const std::uint8_t**>(decode.frame->extended_data) : nullptr,
                frame ? decode.frame->nb_samples : 0);
            if (count < 0) fail("Resampling " + decode.path, count);
            next_pts = pts + double(count) / rate;
            offset = 0; available = std::size_t(count);
            if (!frame && count == 0) { ended = true; return false; }
            if (target >= 0) {
                const double discard = std::ceil((target - pts) * rate - 1e-7);
                offset = discard <= 0 ? 0 : discard >= count ? std::size_t(count) : std::size_t(discard);
                if (offset == available) continue;
                target = -1;
            }
            if (available) return true;
        }
    }
};
AudioDecoder::AudioDecoder(const std::filesystem::path& p, int r, int c) : impl_(std::make_unique<Impl>(p,r,c)) {}
AudioDecoder::~AudioDecoder() = default;
AudioDecoder::AudioDecoder(AudioDecoder&&) noexcept = default;
AudioDecoder& AudioDecoder::operator=(AudioDecoder&&) noexcept = default;
const AudioInfo& AudioDecoder::source_info() const noexcept { return impl_->info; }
std::size_t AudioDecoder::read(std::span<float> output) {
    auto& s = *impl_;
    if (output.size() % s.channels) throw std::runtime_error("Audio destination is not a whole number of frames");
    std::size_t done = 0, frames = output.size() / s.channels;
    while (done < frames) {
        if (s.offset == s.available && !s.refill()) break;
        const auto n = std::min(frames - done, s.available - s.offset);
        std::copy_n(s.buffer.data() + s.offset * s.channels, n * s.channels, output.data() + done * s.channels);
        done += n; s.offset += n;
    }
    return done;
}
void AudioDecoder::seek(double seconds) {
    auto& s = *impl_; s.decode.seek(seconds); swr_close(s.resampler);
    checked(swr_init(s.resampler), "Resetting resampler " + s.decode.path);
    s.offset = s.available = 0; s.ended = false; s.target = seconds; s.next_pts = seconds;
}

struct AudioSystem::Impl {
    struct Sound {
        SoundDefinition definition;
        std::vector<float> pcm;
        int channels{}, last_channel = -1;
        bool available{};
    };
    struct Voice {
        Sound* sound{};
        std::unique_ptr<AudioDecoder> decoder;
        std::uint64_t cursor{};
        std::uint8_t volume{};
        int percentage = -1;
        bool paused{};
        Vec3 position{};
    };
    int rate;
    SDL_AudioStream* stream{};
    std::vector<std::unique_ptr<Sound>> sounds;
    std::array<Voice,48> voices{};
    std::array<float,block_frames*2> scratch{}, mixed{};
    Vec3 listener{};
    float music = 0.5f, effects = 0.5f;
    explicit Impl(int r) : rate(r) { if (r <= 0 || r > 384000) throw std::runtime_error("Invalid mixer rate"); }
    ~Impl() { if (stream) SDL_DestroyAudioStream(stream); }
    void apply_master(Voice& v) const {
        if (v.sound && v.percentage >= 0) {
            const int percent = static_cast<int>(double(v.percentage) * (v.sound->definition.streaming ? music : effects));
            // Original x87 retains precision across float-constant multiplies; 100 maps to 254.
            const int raw=static_cast<int>(double(percent) * double(0.01f) * 255.0);
            v.volume = static_cast<std::uint8_t>(std::min(raw,255)); // FSOUND_SetVolume clamps; scripts use 250/255.
        }
    }
    Voice& voice(int channel) {
        if (channel < 0 || channel >= int(voices.size())) throw std::runtime_error("Invalid audio channel");
        return voices[channel];
    }
    Sound* lookup(std::string_view name) const noexcept {
        for (auto& s : sounds) if (s->definition.name == name) return s.get();
        return nullptr;
    }
    Sound& find(std::string_view name) {
        if (auto* sound=lookup(name)) return *sound;
        throw std::runtime_error("Unknown sound: " + std::string(name));
    }
};
AudioSystem::AudioSystem(int rate) : impl_(std::make_unique<Impl>(rate)) {}
AudioSystem::~AudioSystem() = default;
void AudioSystem::open_device() {
    auto& s = *impl_;
    if (s.stream) throw std::runtime_error("Audio device already opened");
    SDL_AudioSpec spec{SDL_AUDIO_F32,2,s.rate};
    s.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s.stream) throw std::runtime_error(std::string("Opening audio device: ") + SDL_GetError());
    sdl_checked(SDL_ResumeAudioStreamDevice(s.stream), "Starting audio device");
}
void AudioSystem::register_sound(SoundDefinition def) {
    auto& s = *impl_;
    if (def.name.empty() || def.pool < 0 || def.pool > 2 || (def.positional && def.streaming))
        throw std::runtime_error("Invalid sound definition");
    for (const auto& sound : s.sounds) if (sound->definition.name == def.name) throw std::runtime_error("Duplicate sound: " + def.name);
    auto sound = std::make_unique<Impl::Sound>(); sound->definition = std::move(def);
    // 0042c1f0 retains a registered object when FSOUND_Sample_Load/Stream_Open returns null.
    if (!std::filesystem::exists(sound->definition.path)) {
        std::cerr << "Audio: unavailable sound '" << sound->definition.name << "': "
                  << sound->definition.path.string() << '\n';
        s.sounds.push_back(std::move(sound));
        return;
    }
    AudioDecoder decoder(sound->definition.path, s.rate, 1);
    sound->channels=decoder.source_info().channels;
    if (sound->channels!=1 && sound->channels!=2)
        throw std::runtime_error("Original sound must be mono or stereo: " + sound->definition.path.string());
    if (sound->definition.positional && decoder.source_info().channels != 1)
        throw std::runtime_error("Original 3D effects must be mono: " + sound->definition.path.string());
    if (sound->channels==2) decoder=AudioDecoder(sound->definition.path,s.rate,2);
    if (!sound->definition.streaming) {
        const int channels = sound->channels;
        for (;;) {
            const auto n = decoder.read(std::span(s.scratch.data(), block_frames * channels));
            if (!n) break;
            sound->pcm.insert(sound->pcm.end(), s.scratch.begin(), s.scratch.begin() + n * channels);
        }
        if (sound->pcm.empty()) throw std::runtime_error("Empty sound: " + sound->definition.path.string());
    }
    sound->available = true;
    s.sounds.push_back(std::move(sound));
}
bool AudioSystem::registered(std::string_view name) const noexcept { return impl_->lookup(name) != nullptr; }
int AudioSystem::play(std::string_view name, Vec3 pos, int percentage) {
    finite_position(pos);
    auto& s = *impl_;
    if (percentage < -1 || percentage > 255) throw std::runtime_error("Invalid sound percentage");
    auto* found=s.lookup(name);
    if(!found || !found->available) return -1; // Unmatched names or original null FMOD samples cannot play.
    auto& sound=*found;
    // 0042cdf0: music names toggle off rather than create a duplicate voice.
    if (name == "menumusic" || name == "level1" || name == "level2" || name == "level3" || name == "level4")
        for (int i=0; i<48; ++i) if (s.voices[i].sound == &sound) { stop(i); return -1; }
    const int first = sound.definition.pool * 16;
    int chosen = first;
    for (int i=first; i<first+16; ++i) {
        if (!s.voices[i].sound) { chosen = i; break; }
        if (s.voices[i].cursor > s.voices[chosen].cursor) chosen = i;
    }
    // Prepare stream before replacing a live voice, so failed opens preserve playback.
    std::unique_ptr<AudioDecoder> decoder;
    if (sound.definition.streaming) decoder = std::make_unique<AudioDecoder>(sound.definition.path, s.rate, sound.channels);
    s.voices[chosen] = {};
    auto& v = s.voices[chosen]; v.sound = &sound; v.decoder = std::move(decoder);
    v.percentage = percentage < 0 ? sound.definition.volume : percentage;
    s.apply_master(v); v.position = pos;
    sound.last_channel = chosen;
    return chosen;
}
void AudioSystem::stop(int channel) { impl_->voice(channel) = {}; }
void AudioSystem::stop(std::string_view name) { auto& s=*impl_; auto& sound=s.find(name); for(auto& v:s.voices) if(v.sound==&sound) v={}; }
void AudioSystem::pause(int channel, bool paused) { impl_->voice(channel).paused=paused; }
void AudioSystem::volume(int channel, std::uint8_t volume) {
    auto& v=impl_->voice(channel); v.percentage=-1; v.volume=volume;
}
void AudioSystem::set_volume(std::string_view name, std::uint32_t percentage, bool effects) {
    auto& s = *impl_;
    auto* sound = s.lookup(name);
    if (!sound || sound->last_channel < 0) return; // Original unmatched name/no live channel.
    auto& voice = s.voices[sound->last_channel];
    if (voice.sound != sound) return; // A reused channel no longer belongs to this sound.
    // 00428c90's third word selects the master, not a fade duration. 0042c4fd
    // then consumes only the low BYTE after unsigned percentage*master truncation.
    const auto scaled = static_cast<std::uint32_t>(double(percentage)*(effects ? s.effects : s.music));
    const auto percent = static_cast<std::uint8_t>(scaled);
    const auto raw = static_cast<int>(double(percent)*double(0.01f)*255.0);
    voice.percentage = -1;
    voice.volume = static_cast<std::uint8_t>(std::min(raw, 255));
}
void AudioSystem::position(int channel, Vec3 pos) { finite_position(pos); impl_->voice(channel).position=pos; }
bool AudioSystem::playing(int channel) const { return impl_->voice(channel).sound != nullptr; }
void AudioSystem::listener(Vec3 pos) { finite_position(pos); impl_->listener=pos; }
void AudioSystem::master_volume(float music, float effects) {
    if (!std::isfinite(music) || !std::isfinite(effects) || music<0 || music>1 || effects<0 || effects>1)
        throw std::runtime_error("Invalid master volume");
    impl_->music=music; impl_->effects=effects;
    for (auto& v:impl_->voices) impl_->apply_master(v);
}
void AudioSystem::rewind(std::string_view name) {
    auto& s=*impl_; auto& sound=s.find(name);
    for(auto& v:s.voices) if(v.sound==&sound) { if(v.decoder) v.decoder->seek(0); v.cursor=0; }
}
void AudioSystem::render(std::span<float> output) {
    if(output.size()%2) throw std::runtime_error("Mixer destination must be stereo frames");
    auto& s=*impl_; std::fill(output.begin(),output.end(),0.f);
    for (std::size_t start=0; start<output.size()/2; start+=block_frames) {
        const auto frames=std::min(block_frames,output.size()/2-start);
        for(auto& v:s.voices) {
            if(!v.sound || v.paused) continue;
            const auto& def=v.sound->definition;
            const int channels=v.sound->channels;
            int pan=128,attenuation=255;
            if(def.positional) {
                const double dx=double(v.position.x)-s.listener.x,dy=double(v.position.y)-s.listener.y,dz=double(v.position.z)-s.listener.z;
                const double distance=std::sqrt(dx*dx+dy*dy+dz*dz);
                // DLL 10029594/100145a8: min/max 1/1000000; 1001468a/10014707 quantize rolloff/pan.
                // Engine DistanceFactor=25 world units/metre; null velocities mean no Doppler.
                attenuation=static_cast<int>(255.0/std::max(1.0,std::min(distance,1000000.0)));
                pan=std::clamp(128+static_cast<int>((distance>0?dx/distance:0)*128),0,255);
            }
            // DLL 10013a54: two integer divisions, not a continuous equal-power curve.
            const int level=int(v.volume)*attenuation/255;
            const float left=float(level*pan_gain[255-pan]/255)/255.f;
            const float right=float(level*pan_gain[pan]/255)/255.f;
            std::size_t done=0;
            bool restarted_empty=false;
            while(done<frames) {
                const float* pcm;
                std::size_t n;
                if(v.decoder) {
                    n=v.decoder->read(std::span(s.scratch.data(),(frames-done)*channels));
                    pcm=s.scratch.data();
                } else {
                    const auto total=v.sound->pcm.size()/channels;
                    n=std::min(frames-done,total-std::size_t(v.cursor));
                    pcm=v.sound->pcm.data()+v.cursor*channels; // Mix cached effects without a scratch copy.
                }
                for(std::size_t i=0;i<n;++i) {
                    output[(start+done+i)*2]+=pcm[i*channels]*left;
                    output[(start+done+i)*2+1]+=pcm[i*channels+(channels==2?1:0)]*right;
                }
                done+=n; v.cursor+=n;
                if(done==frames) break;
                if(!def.loop) { v={}; break; }
                if(n==0 && restarted_empty)
                    throw std::runtime_error("Empty looping stream: "+def.path.string());
                restarted_empty=n==0;
                if(v.decoder) v.decoder->seek(0);
                v.cursor=0;
            }
        }
    }
    for(auto& sample:output) sample=std::clamp(sample,-1.f,1.f);
}
void AudioSystem::update() {
    auto& s=*impl_;
    if(!s.stream) throw std::runtime_error("Audio update requires an opened device");
    int queued=SDL_GetAudioStreamQueued(s.stream);
    if(queued<0) throw std::runtime_error(std::string("Reading audio queue: ")+SDL_GetError());
    const int block_bytes=int(s.mixed.size()*sizeof(float));
    while(queued<block_bytes*2) {
        render(s.mixed);
        sdl_checked(SDL_PutAudioStreamData(s.stream,s.mixed.data(),block_bytes),"Queueing audio");
        queued+=block_bytes;
    }
}

void AudioSystem::register_original_sounds(const std::filesystem::path& root) {
    // 00414ca0, 0042cc30, 0042c1f0. Registration volume is a percentage,
    // converted to FMOD's integer 0..255 channel volume by 0042c4b0/0042cdf0.
    struct Entry { const char* name; const char* path; int volume; bool music; };
    static constexpr Entry table[] = {
        {"yaman_vurulma1","vurulma/yaman_vurulma1.mp3",100,false},
        {"yaman_vurulma2","vurulma/yaman_vurulma2.mp3",100,false},
        {"yaman_vurulma3","vurulma/yaman_vurulma3.mp3",100,false},
        {"yaman_vurulma4","vurulma/yaman_vurulma4.mp3",100,false},
        {"yaman_vurulma5","vurulma/yaman_vurulma5.mp3",100,false},
        {"yaman_ol","vurulma/yaman_ol.mp3",100,false},
        {"yaman_melee_vur1","yaman_melee1.mp3",50,false}, {"yaman_melee_vur2","yaman_melee2.mp3",50,false},
        {"yaman_melee1","tekme1.mp3",100,false}, {"yaman_melee2","tekme2.mp3",100,false},
        {"lazer","lazer.mp3",100,false}, {"foton_topu","foton_topu2.mp3",100,false},
        {"utu_ates1","foton_topu1.mp3",20,false}, {"utu_ates2","foton_topu3.mp3",20,false},
        {"cd_firlat1","CD_ates1.mp3",20,false}, {"cd_firlat2","CD_ates2.mp3",20,false},
        {"utu_ol1","13_robot_vurulma1.mp3",20,false}, {"utu_ol2","13_robot_vurulma2.mp3",20,false},
        {"yaman_ara1","yaman_ara1.mp3",100,false}, {"yaman_ara2","yaman_ara2.mp3",100,false},
        {"cdatan_ates","robot_CD_ates3.mp3",30,false},
        {"bisiklet_kursun1","bisiklet_kursun1.mp3",20,false}, {"bisiklet_kursun2","bisiklet_kursun2.mp3",20,false},
        {"pervane_lazer1","pervane_lazer1.mp3",20,false}, {"pervane_lazer2","pervane_lazer2.mp3",20,false},
        {"sobali_ates1","sobali_ates1.mp3",20,false}, {"sobali_ates2","sobali_ates2.mp3",20,false},
        {"cd_ol1","robot_cd_ol1.mp3",20,false}, {"cd_ol2","robot_cd_ol2.mp3",20,false},
        {"bisiklet_ol","bisiklet_ol.mp3",20,false}, {"pervane_ol","pervane_ol.mp3",30,false}, {"soba_ol","soba_ol.mp3",30,false},
        {"alarm_basla","env/araba_alarm_basla.mp3",20,false}, {"alarm_uzun","env/araba_alarm_uzun.mp3",10,false},
        {"demir_vurma2","env/demir_vurma2.mp3",30,false}, {"demir_vurma3","env/demir_vurma3.mp3",30,false}, {"demir_vurma4","env/demir_vurma4.mp3",30,false},
        {"demir_taslama1","env/demir_taslama1.mp3",30,false}, {"demir_taslama2","env/demir_taslama2.mp3",30,false}, {"demir_taslama3","env/demir_taslama3.mp3",30,false},
        {"kus1","env/hayvan_kus1.mp3",30,false}, {"kus2","env/hayvan_kus2.mp3",30,false}, {"kus3","env/hayvan_kus3.mp3",30,false}, {"kus4","env/hayvan_kus4.mp3",30,false},
        {"kedi5","env/hayvan_kedi5.mp3",30,false}, {"kedi4","env/hayvan_kedi4.mp3",30,false}, {"kedi3","env/hayvan_kedi3.mp3",30,false},
        {"yami","yami.wav",44,false}, {"menumusic","menu.wav",20,true},
        {"level1","level1.wav",10,true}, {"level2","level2.wav",10,true}, {"level3","level3.wav",10,true}, {"level4","level4.wav",10,true}
    };
    for(const auto& e:table) {
        register_sound({e.name,root/"data"/(e.music?"music":"effects")/e.path,e.music,e.music,false,static_cast<std::uint8_t>(e.volume),0});
    }
}
std::span<const CutsceneAssociation> original_cutscenes() noexcept {
    // 004169b0 builds data/avi_sound/<material-name>.wav, streaming, nonlooping,
    // volume 100. Video paths are serialized in the named materials.
    static constexpr CutsceneAssociation table[] = {
#define YAMI_CUTSCENE(n) {n,"data/avi/" n ".avi","data/avi_sound/" n ".wav"}
        YAMI_CUTSCENE("game_intro"), YAMI_CUTSCENE("ismail_usta_ilk_konusma"),
        YAMI_CUTSCENE("kutup_oyun_oncesi"), YAMI_CUTSCENE("robot_dusme"),
        YAMI_CUTSCENE("otobusten_inme"), YAMI_CUTSCENE("otobuse_binme"),
        {"sahne_01","data/avi/Sahne_01.avi","data/avi_sound/sahne_01.wav"},
        YAMI_CUTSCENE("eczane_ilk"), YAMI_CUTSCENE("ismail_usta_robotu_yapmaya_baslar"),
        YAMI_CUTSCENE("peyami_kurtarildi"), YAMI_CUTSCENE("annane_konusma"),
        YAMI_CUTSCENE("sahaf_ikinci_konusma"), YAMI_CUTSCENE("sahaf_baba_konusma"), YAMI_CUTSCENE("sahaf_ilk_konusma"),
        YAMI_CUTSCENE("level1_son"), YAMI_CUTSCENE("sifa_eczane"),
        YAMI_CUTSCENE("bekciyi_ikinci_gorme"), YAMI_CUTSCENE("bekciyi_ilk_gorme"), YAMI_CUTSCENE("son"),
        // Preserve even stale serialized references; missing media is an explicit open error.
        YAMI_CUTSCENE("galata_ucus"), YAMI_CUTSCENE("sahaf_kitap_alma"),
        {"eczane_ikinci","data/avi/eczane_ilk.avi","data/avi_sound/eczane_ikinci.wav"},
        YAMI_CUTSCENE("ismail_usta_ile_ilk_konusma"), YAMI_CUTSCENE("ismail_usta_robotu_yapar"),
        YAMI_CUTSCENE("yaman_okula_vardi")
#undef YAMI_CUTSCENE
    };
    return table;
}
} // namespace yami
