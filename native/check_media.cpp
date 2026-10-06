#include "media.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::uint64_t hash(std::span<const std::uint8_t> bytes) {
    std::uint64_t value=14695981039346656037ull;
    for (auto byte:bytes) value=(value^byte)*1099511628211ull;
    return value;
}
float energy(std::span<const float> samples, std::size_t channel) {
    float result=0;
    for(std::size_t i=channel;i<samples.size();i+=2) result+=std::abs(samples[i]);
    return result;
}
}
int main(int argc, char** argv) {
    try {
        const auto root=std::filesystem::path(argc>1?argv[1]:"game");
        yami::VideoDecoder video(root/"data/avi/game_intro.avi");
        require(video.width()==512 && video.height()==384,"Intro dimensions differ from recovered asset");
        require(std::abs(video.fps()-12.5)<1e-9,"Intro framerate must be 25/2");
        require(std::abs(video.duration()-31.52)<1e-7,"Intro duration must be 394/12.5");
        require(video.frame_count()==394,"AVIStreamLength frame count differs");
        yami::VideoFrame frame;
        std::size_t count=0;
        while(video.next(frame)) {
            require(std::abs(frame.pts-double(count)*0.08)<1e-7,"Incorrect decoded frame PTS");
            require(frame.stride==2048 && frame.rgba.size()==786432,"Invalid RGBA layout");
            // Independent reference: ffmpeg -i game/data/avi/game_intro.avi
            // -vf 'fps=25/2,select=eq(n\,0)+eq(n\,100)' -fps_mode passthrough -sws_flags bilinear -pix_fmt rgba -f rawvideo pipe:1
            // Frame 0 is entirely opaque black; assert image semantics rather than an opaque hash.
            if(count==0)
                for(std::size_t i=0;i<frame.rgba.size();i+=4)
                    require(frame.rgba[i]==0 && frame.rgba[i+1]==0 && frame.rgba[i+2]==0 && frame.rgba[i+3]==255,
                            "Intro first decoded pixels must be opaque black");
            if(count==100) {
                require(hash(frame.rgba)==0x1559ef5a0c4e5998ull,"Intro timed sample 100 pixels differ");
                const auto center=(192u*512u+256u)*4u;
                require(frame.rgba[center]==1 && frame.rgba[center+1]==1 && frame.rgba[center+2]==1 && frame.rgba[center+3]==255,"Intro center pixel differs");
            }
            if(count==131) {
                require(hash(frame.rgba)==0x9743d3a45f7e56e6ull,"Intro nonblack source sample pixels differ");
                const auto center=(192u*512u+256u)*4u;
                require(frame.rgba[center]==88 && frame.rgba[center+1]==100 && frame.rgba[center+2]==107 && frame.rgba[center+3]==255,
                        "Intro nonblack center pixel differs");
            }
            if(count==8 || count==9)
                require(hash(frame.rgba)==0xf10c009f78020d80ull,"Indeo no-image sample must retain preceding pixels");
            if(count==199 || count==200)
                require(hash(frame.rgba)==0x70abba763e2c7af1ull,"Late Indeo no-image sample must retain preceding pixels");
            ++count;
        }
        require(count==394 && video.eof(),"Intro EOF/frame count incorrect");
        video.seek(8.0);
        require(video.next(frame) && std::abs(frame.pts-8.0)<1e-7 && hash(frame.rgba)==0x1559ef5a0c4e5998ull,"Video keyframe seek did not decode exact target");
        video.seek(8.001);
        require(video.next(frame) && std::abs(frame.pts-8.08)<1e-7,"Fractional video seek did not choose next frame");
        video.seek(16.0);
        require(video.next(frame) && std::abs(frame.pts-16.0)<1e-7 && hash(frame.rgba)==0x70abba763e2c7af1ull,
                "Seek to an Indeo no-image sample must retain preceding pixels");
        bool rejected=false;
        try { video.seek(-1); } catch(const std::runtime_error&) { rejected=true; }
        require(rejected,"Negative video seek accepted");
        yami::VideoDecoder jpeg(root/"data/images/menuimages/sahaf1yanlissecim_bg.jpg");
        require(jpeg.frame_count()==1 && jpeg.width()==1024 && jpeg.height()==1024 && jpeg.next(frame) &&
                hash(frame.rgba)==0x89d13bb7523d2934ull && !jpeg.next(frame),
                "Actual JPEG canonical RGBA pixels/EOF differ");
        yami::VideoDecoder tga(root/"data/images/tabela_yazi_hotel_2.tga");
        require(tga.frame_count()==1 && tga.width()==64 && tga.height()==128 && tga.next(frame) &&
                hash(frame.rgba)==0x1a572dcb4f35be02ull && !tga.next(frame),
                "Actual TGA canonical RGBA pixels/EOF differ");

        yami::AudioDecoder audio(root/"data/avi_sound/game_intro.wav",44100,1);
        const auto info=audio.source_info();
        require(info.sample_rate==44100 && info.channels==1 && std::abs(info.duration-32.0)<1e-7,"Intro WAV source facts differ");
        std::array<float,4096> pcm{};
        std::size_t sample_count=0,first_nonzero=std::size_t(-1);
        for(;;) {
            const auto n=audio.read(pcm);
            if(!n) break;
            for(std::size_t i=0;i<n;++i) {
                if(pcm[i]!=0 && first_nonzero==std::size_t(-1)) first_nonzero=sample_count+i;
                if(sample_count+i==441000) require(pcm[i]==-1440.f/32768.f,"Actual WAV PCM amplitude differs");
            }
            sample_count+=n;
        }
        require(sample_count==1411200 && first_nonzero==438987,"Actual WAV sample count/silence boundary differs");
        audio.seek(10.0);
        require(audio.read(std::span(pcm.data(),1))==1 && pcm[0]==-1440.f/32768.f,"Audio sample-accurate seek failed");
        yami::AudioDecoder effect(root/"data/effects/yami.wav",44100,1);
        require(effect.source_info().channels==1 && effect.source_info().sample_rate==44100,"Actual effect source format differs");
        sample_count=0;
        for(;;) { const auto n=effect.read(pcm); if(!n) break; sample_count+=n; }
        require(sample_count==35751,"Actual effect WAV sample count differs");

        yami::AudioSystem mixer;
        mixer.master_volume(1,1);
        mixer.register_sound({"effect",root/"data/effects/yami.wav",false,false,false,100,0});
        std::vector<float> mixed(4096*2),reference(4096*2);
        int channel=mixer.play("effect");
        mixer.render(reference);
        require(energy(reference,0)>0,"Real effect mixer output is silent");
        require(std::abs(reference[0]-(-2.f/32768.f)*(179.f/255.f))<1e-12f &&
                std::abs(reference[1]-(-2.f/32768.f)*(180.f/255.f))<1e-12f,
                "Original mono center-pan integer gains differ");
        mixer.rewind("effect");
        mixer.pause(channel,true);
        mixer.render(mixed);
        require(energy(mixed,0)==0,"Paused channel advanced or mixed");
        mixer.pause(channel,false);
        mixer.volume(channel,127);
        mixer.render(mixed);
        for(std::size_t i=0;i<mixed.size();++i) {
            const float ratio=i%2 ? 90.f/180.f : 89.f/179.f;
            require(std::abs(mixed[i]-reference[i]*ratio)<1e-7,"Raw FMOD volume integer gain differs");
        }
        mixer.stop(channel);
        mixer.render(mixed);
        require(!mixer.playing(channel) && energy(mixed,0)==0,"Stopped channel mixed");
        mixer.master_volume(1,0.5f);
        channel=mixer.play("effect",{},250);
        mixer.render(mixed);
        require(std::abs(mixed[0]-(-2.f/32768.f)*(180.f/255.f))<1e-12f &&
                std::abs(mixed[1]-(-2.f/32768.f)*(181.f/255.f))<1e-12f,
                "Original scripted 250-percent/master/clamp ordering differs");
        mixer.stop(channel);
        mixer.master_volume(0.25f,0.75f);
        channel=mixer.play("effect");
        const int newest=mixer.play("effect");
        mixer.set_volume("effect",100,false);
        mixer.render(mixed);
        require(std::abs(mixed[0]-(-2.f/32768.f)*(178.f/255.f))<1e-12f &&
                std::abs(mixed[1]-(-2.f/32768.f)*(179.f/255.f))<1e-12f,
                "Named volume must use selected music master and only the last voice");
        mixer.stop(channel);
        mixer.rewind("effect");
        mixer.set_volume("effect",100,true);
        mixer.render(mixed);
        require(std::abs(mixed[0]-(-2.f/32768.f)*(134.f/255.f))<1e-12f &&
                std::abs(mixed[1]-(-2.f/32768.f)*(135.f/255.f))<1e-12f,
                "Named volume effects-master selector differs");
        mixer.stop(newest);
        mixer.master_volume(1,1);
        mixer.register_sound({"position",root/"data/effects/yami.wav",true,false,true,100,1});
        channel=mixer.play("position",{10,0,0});
        mixer.render(mixed);
        require(energy(mixed,0)==0 && energy(mixed,1)>0,"Mono 3D +X pan incorrect");
        mixer.position(channel,{-10,0,0}); mixer.rewind("position"); mixer.render(mixed);
        require(energy(mixed,1)==0 && energy(mixed,0)>0,"Mono 3D position update incorrect");
        mixer.stop(channel);
        mixer.register_sound({"loop",root/"data/effects/yami.wav",true,true,false,100,0});
        channel=mixer.play("loop");
        std::vector<float> looped(35751*2+reference.size());
        mixer.render(looped);
        for(std::size_t i=0;i<reference.size();++i) require(std::abs(looped[35751*2+i]-reference[i])<1e-7,"Stream loop boundary altered PCM");
        mixer.stop(channel);
        mixer.register_original_sounds(root); // Decode present recovered paths; retain original null samples.
        require(mixer.play("yaman_vurulma6")==-1 && mixer.play("yaman_melee_vur")==-1,
                "Original unregistered combat names must not fabricate playback");
        for(const auto name:{"alarm_basla","kus1","kus2"}) {
            require(mixer.play(name)==-1,"Missing original sample must not fabricate playback");
            mixer.stop(name);
            mixer.rewind(name);
            mixer.set_volume(name,100,true);
        }
        channel=mixer.play("menumusic");
        require(channel>=0 && mixer.play("menumusic")==-1 && !mixer.playing(channel),"Original music toggle semantics differ");
        std::cout<<"Media: actual 394-frame Indeo5 intro, RGBA reference pixels/PTS/seeks, mono PCM16 WAV facts, mixer volume/pause/stop/loop/position and registered paths passed\n";
    } catch(const std::exception& error) { std::cerr<<"Media check: "<<error.what()<<'\n'; return 1; }
}
