#include "setup.hpp"
#include "setup_artwork.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
namespace fs = std::filesystem;
using namespace yami::setup;
constexpr int Width=1024, Height=768;
constexpr SDL_Color Ink{41,31,20,255}, Muted{86,65,42,255}, Olive{83,88,39,255},
    Gold{242,212,46,255}, Paper{232,211,162,255}, Edge{116,87,46,255},
    Backdrop{57,42,28,255}, Error{140,35,26,255};
std::atomic<bool> interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free);
void interrupt(int) { interrupted.store(true,std::memory_order_relaxed); }
struct Options {
    fs::path iso, destination, engine, archiver, remover;
    bool unattended=false;
};
Options arguments(int argc,char** argv) {
    Options options;
    for (int i=1;i<argc;++i) {
        const std::string argument=argv[i];
        if (argument=="--help" || argument=="-h") {
            std::cout << "ETI Yami Linux setup " YAMI_VERSION "\n"
                "Usage: yami-setup [--iso GAME.iso] [--install-dir DIRECTORY]\n"
                "                 [--engine DIRECTORY] [--archiver FILE] [--remover FILE]\n"
                "                 [--unattended]\n"
                "Default: game artwork from your ISO, per-user installation, application-menu entries.\n"
                "Desktop shortcuts are added when the desktop directory is available.\n"
                "--unattended installs the supplied ISO without opening a window.\n";
            std::exit(0);
        }
        if (argument=="--version") { std::cout << YAMI_VERSION << '\n'; std::exit(0); }
        if (argument=="--unattended") { options.unattended=true; continue; }
        if (argument!="--iso" && argument!="--install-dir" && argument!="--engine" &&
            argument!="--archiver" && argument!="--remover")
            throw std::runtime_error("Unknown option: "+argument);
        if (++i==argc) throw std::runtime_error("Missing value for "+argument);
        if (argument=="--iso") options.iso=fs::absolute(argv[i]);
        else if (argument=="--install-dir") options.destination=fs::absolute(argv[i]);
        else if (argument=="--engine") options.engine=fs::absolute(argv[i]);
        else if (argument=="--archiver") options.archiver=fs::absolute(argv[i]);
        else options.remover=fs::absolute(argv[i]);
    }
    if (options.unattended && options.iso.empty()) throw std::runtime_error("--unattended requires --iso GAME.iso");
    return options;
}
struct Preview {
    fs::path path;
    Preview() {
        const auto temporary=fs::temp_directory_path();
        if (fs::space(temporary).available < (640ULL<<20))
            throw std::runtime_error("Reading original artwork needs 640 MiB free temporary space. Choose a larger TMPDIR.");
        auto pattern=(temporary/"yami-setup-art-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) throw std::runtime_error("Cannot create a private artwork preview directory");
        path=pattern;
        try { fs::create_directory(path/"game"); fs::create_directory(path/"scratch"); }
        catch (...) { std::error_code error; fs::remove_all(path,error); throw; }
    }
    ~Preview() { std::error_code error; fs::remove_all(path,error); }
};
struct State {
    std::mutex mutex;
    InstallRequest request;
    InstallResult result;
    std::shared_ptr<Preview> preview;
    std::string phase="Choose your original game ISO to begin.",error;
    std::uint64_t done=0,total=0;
    bool busy=false,previewing=false,needsPreview=false,installed=false,dialog=false,dirty=true;
    std::atomic<bool> cancelled{false},workerDone{false};
};
struct DialogRequest { std::shared_ptr<State> state; bool directory; };
void SDLCALL selected(void* opaque,const char* const* files,int) {
    std::unique_ptr<DialogRequest> dialog(static_cast<DialogRequest*>(opaque));
    std::lock_guard lock(dialog->state->mutex);
    auto& state=*dialog->state; state.dialog=false; state.dirty=true;
    if (!files) { state.error=SDL_GetError(); return; }
    if (!files[0]) return;
    state.error.clear();
    if (dialog->directory) state.request.paths.root=fs::path(files[0])/"etiyami";
    else { state.request.iso=files[0]; state.needsPreview=true; }
}
void color(SDL_Renderer* renderer,SDL_Color value) {
    SDL_SetRenderDrawColor(renderer,value.r,value.g,value.b,value.a);
}
void rectangle(SDL_Renderer* renderer,SDL_FRect bounds,SDL_Color value) {
    color(renderer,value); SDL_RenderFillRect(renderer,&bounds);
}
constexpr std::array<SDL_FRect,4> Buttons{{
    {778,172,186,48},{778,298,186,48},{382,616,196,58},{716,616,248,58}
}};
bool inside(SDL_FRect bounds,float x,float y) {
    return x>=bounds.x && x<bounds.x+bounds.w && y>=bounds.y && y<bounds.y+bounds.h;
}
void compact_label(std::string& value) {
    std::size_t codepoints=0;
    for (const auto byte : value) if ((static_cast<unsigned char>(byte)&0xc0)!=0x80) ++codepoints;
    if (codepoints<=36) return;
    std::size_t begin=value.size();
    for (unsigned count=0;count<33 && begin; ++count) {
        --begin;
        while (begin && (static_cast<unsigned char>(value[begin])&0xc0)==0x80) --begin;
    }
    value.erase(0,begin); value.insert(0,"...");
}
int graphical(const InstallRequest& request) {
    auto state=std::make_shared<State>(); state->request=request; state->needsPreview=!request.iso.empty();
    std::unique_ptr<SDL_Window,decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("ETI Yami - Linux Setup",Width,Height,SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY),SDL_DestroyWindow);
    if (!window) throw std::runtime_error(SDL_GetError());
    std::unique_ptr<SDL_Renderer,decltype(&SDL_DestroyRenderer)> renderer(
        SDL_CreateRenderer(window.get(),"software"),SDL_DestroyRenderer);
    if (!renderer) throw std::runtime_error(SDL_GetError());
    SDL_SetRenderLogicalPresentation(renderer.get(),Width,Height,SDL_LOGICAL_PRESENTATION_LETTERBOX);
    // Commit a Wayland framebuffer before waiting for focus or showing the picker.
    color(renderer.get(),Backdrop); SDL_RenderClear(renderer.get()); SDL_RenderPresent(renderer.get());
    std::unique_ptr<Artwork> artwork;
    std::shared_ptr<Preview> loadedPreview;
    std::thread worker;
    struct JoinWorker {
        std::thread& thread; State& state;
        ~JoinWorker() { state.cancelled=true; if (thread.joinable()) thread.join(); }
    } joinWorker{worker,*state};
    std::unique_ptr<DialogRequest> pendingDialog;
    int focus=0,hover=-1,pressed=-1;
    bool quit=false,closeWhenIdle=false;
    fs::path displayedIso,displayedRoot;
    std::string isoLabel,rootLabel;
    fs::path titledIso;
    int titleMode=-1;
    auto progress=[state](std::string_view phase,std::uint64_t done,std::uint64_t total) {
        if (state->cancelled) return false;
        std::lock_guard lock(state->mutex);
        if (!state->cancelled) state->phase=phase;
        state->done=done; state->total=total; state->dirty=true;
        return !state->cancelled;
    };
    auto enabled=[&](int index) {
        if (state->dialog) return false;
        if (state->busy) return index==2 && !state->cancelled;
        if (state->installed) return index>=2;
        return index<3 || (artwork && !state->request.iso.empty());
    };
    auto begin=[&](bool preview) {
        state->busy=true; state->previewing=preview; state->error.clear(); state->dirty=true;
        state->phase=preview?"Reading the original game's artwork...":"Preparing installation...";
        state->done=state->total=0; state->cancelled=false; state->workerDone=false;
        if (preview) { artwork.reset(); loadedPreview.reset(); state->preview.reset(); }
        const auto input=state->request;
        worker=std::thread([state,input,progress,preview] {
            try {
                if (preview) {
                    auto assets=std::make_shared<Preview>();
                    extract_iso_artwork(input.iso,assets->path/"game",assets->path/"scratch",input.archiver,Artwork::files(),progress);
                    fs::remove_all(assets->path/"scratch");
                    std::lock_guard lock(state->mutex); state->preview=std::move(assets);
                } else {
                    const auto result=install(input,progress);
                    std::lock_guard lock(state->mutex); state->result=result; state->installed=true;
                    state->phase=std::string(result.reused?"Existing game kept. ":"Installation complete. ") +
                        "Use ETI Yami in Apps." + (result.paths.desktop.empty()?" Desktop shortcuts unavailable.":" Desktop files also created; visibility depends on your desktop.");
                }
            } catch (const std::exception& error) {
                std::lock_guard lock(state->mutex);
                if (state->cancelled) state->phase=preview?"Artwork reading cancelled. Choose the ISO again to retry.":"Installation cancelled. No game was replaced.";
                else {
                    state->phase=preview?"Original artwork could not be read.":"Installation could not finish.";
                    state->error=error.what();
                }
            }
            { std::lock_guard lock(state->mutex); state->dirty=true; }
            state->workerDone=true;
        });
        focus=2;
    };
    auto activate=[&](int index) {
        if (!enabled(index)) return;
        state->dirty=true;
        if (index<2) {
            state->dialog=true;
            pendingDialog=std::make_unique<DialogRequest>(DialogRequest{state,index==1});
        } else if (index==2) {
            if (state->busy) { state->cancelled=true; state->phase="Cancelling safely..."; }
            else quit=true;
        } else if (state->installed) {
            try { launch_game(state->result); quit=true; }
            catch (const std::exception& error) { state->error=error.what(); }
        } else begin(false);
    };
    auto paintText=[&](float x,float y,std::string_view value,SDL_Color tint=Ink,float height=20,float maximum=580) {
        if (artwork) { artwork->text(x,y,value,tint,height,maximum); return; }
        color(renderer.get(),tint);
        const float scale=height/8;
        SDL_SetRenderScale(renderer.get(),scale,scale);
        SDL_RenderDebugTextFormat(renderer.get(),x/scale,y/scale,"%.*s",int(std::min<std::size_t>(value.size(),std::size_t(maximum/height*1.5f))),value.data());
        SDL_SetRenderScale(renderer.get(),1,1);
    };
    auto wrapped=[&](float x,float y,std::string_view value,SDL_Color tint) {
        for (int line=0;line<4 && !value.empty();++line) {
            std::size_t length=std::min<std::size_t>(value.size(),58);
            if (artwork) while (length && artwork->width(value.substr(0,length),19)>578) --length;
            while (length && length<value.size() && (static_cast<unsigned char>(value[length])&0xc0)==0x80) --length;
            if (length<value.size()) {
                const auto space=value.rfind(' ',length);
                if (space!=std::string_view::npos && space>length/2) length=space;
            }
            if (!length) break;
            paintText(x,y+line*26,value.substr(0,length),tint,19);
            value.remove_prefix(length);
            while (!value.empty() && value.front()==' ') value.remove_prefix(1);
        }
    };
    auto paintButton=[&](int index,std::string_view label,bool primary=false) {
        const auto bounds=Buttons[index]; const bool on=enabled(index),focused=focus==index || hover==index;
        if (artwork) { artwork->button(bounds,label,on,focused,primary); return; }
        rectangle(renderer.get(),bounds,on?(primary?Gold:Paper):Paper);
        color(renderer.get(),focused?Olive:Edge); SDL_RenderRect(renderer.get(),&bounds);
        paintText(bounds.x+15,bounds.y+15,label,on?Ink:Muted,19,bounds.w-30);
    };
    while (!quit) {
        if (state->workerDone && worker.joinable()) {
            worker.join();
            std::unique_lock lock(state->mutex);
            state->busy=false; state->dirty=true;
            if (state->cancelled && !state->installed) {
                state->phase=state->previewing?"Artwork reading cancelled. Choose the ISO again to retry.":
                                              "Installation cancelled. No game was replaced.";
                state->preview.reset();
            }
            if (state->previewing && state->preview && !state->cancelled) {
                auto preview=state->preview;
                lock.unlock();
                try {
                    auto original=std::make_unique<Artwork>(renderer.get(),preview->path/"game");
                    lock.lock(); artwork=std::move(original); loadedPreview=std::move(preview);
                    state->phase="ISO ready. Click Install game to continue.";
                } catch (const std::exception& error) {
                    if (!lock.owns_lock()) lock.lock();
                    state->error=error.what(); state->phase="Original artwork could not be read.";
                    state->preview.reset();
                }
            }
            state->previewing=false;
            focus=(artwork || state->installed)?3:0;
        }
        std::unique_lock lock(state->mutex);
        if (interrupted) { closeWhenIdle=true; state->cancelled=true; }
        // The asynchronous native picker owns State and no renderer/window references.
        if (closeWhenIdle && !state->busy) quit=true;
        SDL_Event event;
        while (true) {
            // Portal picker callbacks may run inline while SDL pumps events.
            lock.unlock();
            const bool hasEvent=SDL_PollEvent(&event);
            lock.lock();
            if (!hasEvent) break;
            SDL_ConvertEventToRenderCoordinates(renderer.get(),&event);
            if (event.type==SDL_EVENT_MOUSE_MOTION) {
                int next=-1;
                for (int i=0;i<4;++i) if (inside(Buttons[i],event.motion.x,event.motion.y) && enabled(i)) next=i;
                if (next!=hover) { hover=next; state->dirty=true; }
            } else state->dirty=true;
            if (event.type==SDL_EVENT_QUIT || event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                closeWhenIdle=true;
                if (state->busy) { state->cancelled=true; state->phase="Cancelling safely..."; }
                else quit=true;
            } else if (event.type==SDL_EVENT_WINDOW_FOCUS_LOST) pressed=-1;
            else if (event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (event.key.key==SDLK_TAB) {
                    const int step=(event.key.mod&SDL_KMOD_SHIFT)?-1:1;
                    for (int attempt=0;attempt<4;++attempt) { focus=(focus+step+4)%4; if (enabled(focus)) break; }
                } else if (event.key.key==SDLK_RETURN || event.key.key==SDLK_KP_ENTER || event.key.key==SDLK_SPACE) activate(focus);
                else if (event.key.key==SDLK_ESCAPE) activate(2);
            } else if (event.type==SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button==SDL_BUTTON_LEFT) {
                pressed=-1;
                for (int i=0;i<4;++i) if (inside(Buttons[i],event.button.x,event.button.y) && enabled(i)) { focus=i; pressed=i; }
            } else if (event.type==SDL_EVENT_MOUSE_BUTTON_UP && event.button.button==SDL_BUTTON_LEFT) {
                const int action=pressed; pressed=-1;
                if (action>=0 && inside(Buttons[action],event.button.x,event.button.y)) activate(action);
            }
        }
        if (quit) break;
        if (pendingDialog) {
            auto* context=pendingDialog.release();
            static constexpr SDL_DialogFileFilter filters[]={{"Game ISO image","iso;ISO"}};
            lock.unlock(); // SDL may synchronously report a native dialog error.
            if (context->directory) SDL_ShowOpenFolderDialog(selected,context,window.get(),nullptr,false);
            else SDL_ShowOpenFileDialog(selected,context,window.get(),filters,1,nullptr,false);
            lock.lock();
        }
        if (state->needsPreview && !state->busy && !state->dialog) { state->needsPreview=false; begin(true); }
        if (!state->dirty && !(state->busy && !state->total)) { lock.unlock(); SDL_Delay(32); continue; }
        if (displayedIso!=state->request.iso) {
            displayedIso=state->request.iso; isoLabel=displayedIso.filename().string(); compact_label(isoLabel);
        }
        if (displayedRoot!=state->request.paths.root) {
            displayedRoot=state->request.paths.root; rootLabel=displayedRoot.string();
            const char* home=SDL_getenv("HOME");
            if (home && rootLabel.starts_with(std::string(home)+"/")) rootLabel.replace(0,std::strlen(home),"~");
            compact_label(rootLabel);
        }
        const int mode=state->installed?2:(artwork && !state->busy?1:0);
        if (mode!=titleMode || titledIso!=displayedIso) {
            std::string title=mode==2?"ETI Yami - Installation complete":
                              mode==1?"ETI Yami - Ready to install":"ETI Yami - Linux Setup";
            if (!displayedIso.empty() && mode!=2) title+=" - "+displayedIso.string();
            SDL_SetWindowTitle(window.get(),title.c_str());
            titleMode=mode; titledIso=displayedIso;
        }
        state->dirty=false;
        color(renderer.get(),Backdrop); SDL_RenderClear(renderer.get());
        if (artwork) artwork->background();
        else {
            rectangle(renderer.get(),{350,24,646,716},Paper);
            paintText(32,56,"ETI YAMI",Paper,35,285);
            paintText(32,111,"MEKANIK ISTILA",Gold,17,285);
            paintText(32,214,"Original artwork",Paper,18,285);
            paintText(32,244,"loads from your ISO.",Paper,18,285);
        }
        paintText(382,61,state->installed?"READY TO PLAY":"INSTALL ETI YAMI",Ink,31);
        paintText(382,112,"Your original game, ready for Linux.",Muted,20);
        paintText(382,174,"GAME ISO",Olive,22,380);
        paintText(382,223,isoLabel.empty()?"Select your original disc image.":isoLabel,Ink,21);
        paintButton(0,"Choose ISO");
        paintText(382,301,"INSTALL FOLDER",Olive,22,380);
        paintText(382,351,rootLabel,Ink,20);
        paintButton(1,"Change...");
        paintText(382,401,"Apps: play + uninstall. Desktop files when available.",Muted,20);
        paintText(382,450,state->installed?"INSTALLED":state->busy?(state->previewing?"READING ORIGINAL ART":"INSTALLING GAME"):
                  artwork?"READY TO INSTALL":"SELECT YOUR ISO",Olive,20);
        wrapped(382,484,state->error.empty()?state->phase:state->error,state->error.empty()?Ink:Error);
        if (state->busy) {
            rectangle(renderer.get(),{382,594,582,8},Edge);
            const float fraction=state->total?std::clamp(float(double(state->done)/double(state->total)),0.0f,1.0f):0;
            if (state->total) rectangle(renderer.get(),{382,594,582*fraction,8},Olive);
            else rectangle(renderer.get(),{382+float((SDL_GetTicks()/8)%470),594,112,8},Olive);
        }
        paintButton(2,state->busy?(state->cancelled?"Stopping...":"Cancel"):"Close");
        paintButton(3,state->installed?"Launch game":state->busy?(state->previewing?"Reading ISO...":"Installing..."):"Install game",true);
        paintText(382,697,"No sudo. No Windows installers are run.",Muted,18);
        lock.unlock(); // Progress updates never wait for rendering/frame pacing.
        SDL_RenderPresent(renderer.get()); SDL_Delay(16);
    }
    if (worker.joinable()) { state->cancelled=true; worker.join(); }
    return 0;
}
}
int main(int argc,char** argv) {
    try {
        const auto options=arguments(argc,argv);
        if (std::getenv("WAYLAND_DISPLAY")) SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER,"wayland,x11",SDL_HINT_DEFAULT);
        if (!SDL_Init(options.unattended?0:SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
        std::signal(SIGINT,interrupt); std::signal(SIGTERM,interrupt);
        struct Lifetime { ~Lifetime() { SDL_Quit(); } } lifetime;
        const char* base=SDL_GetBasePath();
        if (!base) throw std::runtime_error(SDL_GetError());
        const fs::path directory(base);
        InstallRequest request; request.paths=default_install_paths();
        if (!options.destination.empty()) request.paths.root=options.destination;
        request.iso=options.iso;
        request.engine=options.engine.empty()?(fs::is_directory(directory/"../engine")?directory/"../engine":directory):options.engine;
        request.archiver=options.archiver.empty()?directory/"7zz":options.archiver;
        request.remover=options.remover.empty()?directory/"yami-remove":options.remover;
        if (options.unattended) {
            std::string previous;
            const auto result=install(request,[&](std::string_view phase,std::uint64_t,std::uint64_t) {
                if (previous!=phase) { previous=phase; std::cout << phase << std::endl; }
                return !interrupted;
            });
            std::cout << "Installed: " << result.paths.root << '\n'; return 0;
        }
        return graphical(request);
    } catch (const std::exception& error) { std::cerr << "ETI Yami setup: " << error.what() << '\n'; return 1; }
}
