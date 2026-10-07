// Exercise the real GUI, replacing only native dialog delivery at SDL's event-pump boundary.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
namespace probe {
bool SDLCALL poll(SDL_Event*);
void SDLCALL file(SDL_DialogFileCallback,void*,SDL_Window*,const SDL_DialogFileFilter*,int,const char*,bool);
void SDLCALL folder(SDL_DialogFileCallback,void*,SDL_Window*,const char*,bool);
bool SDLCALL present(SDL_Renderer*);
}
#define main setup_entry
#define SDL_PollEvent probe::poll
#define SDL_ShowOpenFileDialog probe::file
#define SDL_ShowOpenFolderDialog probe::folder
#define SDL_RenderPresent probe::present
#include "setup_main.cpp"
#undef SDL_RenderPresent
#undef SDL_ShowOpenFolderDialog
#undef SDL_ShowOpenFileDialog
#undef SDL_PollEvent
#undef main

namespace probe {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
SDL_DialogFileCallback callback=nullptr;
void* opaque=nullptr;
std::shared_ptr<State> state;
fs::path iso,base,frames;
int dialogs=0,stage=0;
bool realIso=false,cancelPreview=false,cancelSent=false,finished=false,errorDialog=false;

void key(SDL_Keycode value) {
    SDL_Event event{}; event.type=SDL_EVENT_KEY_DOWN; event.key.key=value;
    require(SDL_PushEvent(&event),"Cannot queue GUI key");
}
void click(SDL_Renderer* renderer,int index) {
    const auto bounds=Buttons[index];
    SDL_Event event{}; event.type=SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.windowID=SDL_GetWindowID(SDL_GetRenderWindow(renderer));
    event.button.button=SDL_BUTTON_LEFT;
    require(SDL_RenderCoordinatesToWindow(renderer,bounds.x+bounds.w/2,bounds.y+bounds.h/2,
                                         &event.button.x,&event.button.y),"Cannot map GUI button coordinates");
    require(SDL_PushEvent(&event),"Cannot queue GUI button press");
    event.type=SDL_EVENT_MOUSE_BUTTON_UP;
    require(SDL_PushEvent(&event),"Cannot queue GUI button release");
}
void dialog(SDL_DialogFileCallback cb,void* context,bool directory) {
    require(!callback,"GUI opened overlapping dialogs");
    auto* request=static_cast<DialogRequest*>(context);
    require(request->directory==directory,"Wrong native picker kind");
    if (state) require(state==request->state,"Dialog lost shared GUI state");
    state=request->state; callback=cb; opaque=context;
    require(state->dialog,"GUI did not mark its pending picker");
    require(directory==(!errorDialog && (dialogs==1 || dialogs==2 || dialogs==4)),"Unexpected picker sequence");
    if (dialogs%2) {
        SDL_Event event{}; event.type=SDL_EVENT_USER;
        require(SDL_PushEvent(&event),"Cannot queue callback-bearing event");
    }
}
void SDLCALL file(SDL_DialogFileCallback cb,void* context,SDL_Window*,const SDL_DialogFileFilter*,int,const char*,bool) {
    dialog(cb,context,false);
}
void SDLCALL folder(SDL_DialogFileCallback cb,void* context,SDL_Window*,const char*,bool) {
    dialog(cb,context,true);
}
bool SDLCALL poll(SDL_Event* event) {
    const bool result=SDL_PollEvent(event); // Keep the real SDL backend/event queue.
    if (callback && result==bool(dialogs%2)) {
        const auto choice=dialogs==1?base.string():iso.string();
        const char* files[]={dialogs==1 || dialogs==3?choice.c_str():nullptr,nullptr};
        auto cb=callback; callback=nullptr;
        auto* context=opaque; opaque=nullptr;
        std::cout << "Inline picker callback " << dialogs << ", PollEvent returns " << result << std::endl;
        if (errorDialog) SDL_SetError("Injected portal response error");
        cb(context,errorDialog?nullptr:files,0); // The old GUI deadlocks here, including when PollEvent returns false.
        std::lock_guard lock(state->mutex);
        require(!state->dialog,"Native callback did not release picker state");
        if (errorDialog) {
            require(state->error=="Injected portal response error" && state->request.iso==iso &&
                    state->request.paths.root==base/"etiyami" && !state->needsPreview,"Picker error lost selection or scheduled work");
        } else {
            if (dialogs==0) require(state->request.iso.empty() && !state->needsPreview,"ISO cancel changed selection");
            if (dialogs==1 || dialogs==2 || dialogs==4)
                require(state->request.paths.root==base/"etiyami","Folder selection/cancel lost destination");
            if (dialogs==3) require(state->request.iso==iso && state->needsPreview,"ISO choice did not schedule preview");
        }
        ++dialogs;
    }
    return result;
}
void frame(SDL_Renderer* renderer,const char* name) {
    if (frames.empty()) return;
    std::unique_ptr<SDL_Surface,decltype(&SDL_DestroySurface)> pixels(SDL_RenderReadPixels(renderer,nullptr),SDL_DestroySurface);
    require(bool(pixels),"Cannot read actual GUI framebuffer");
    require(SDL_SaveBMP(pixels.get(),(frames/name).c_str()),"Cannot save actual GUI framebuffer");
}
bool SDLCALL present(SDL_Renderer* renderer) {
    // graphical() releases State::mutex before rendering; inspect the consumer after its actual paint.
    if (stage==0) { key(SDLK_RETURN); stage=1; }
    else if (stage==1 && dialogs==1) { click(renderer,1); stage=2; }
    else if (stage==2 && dialogs==2) { click(renderer,1); stage=3; }
    else if (stage==3 && dialogs==3) { click(renderer,0); stage=4; }
    else if (stage==4 && dialogs==4) {
        std::lock_guard lock(state->mutex);
        if (cancelPreview && state->busy && !cancelSent) {
            require(state->previewing,"ISO choice started installation instead of preview");
            frame(renderer,"preview.bmp"); key(SDLK_ESCAPE); cancelSent=true;
        } else if (!state->busy) {
            if (cancelPreview) {
                require(cancelSent && state->cancelled && !state->preview && !state->installed,
                        "GUI preview cancel did not stop safely");
                require(state->phase=="Artwork reading cancelled. Choose the ISO again to retry.","GUI lost cancellation result");
                frame(renderer,"cancelled.bmp"); errorDialog=true; click(renderer,0); stage=6;
            } else if (realIso) {
                require(state->error.empty() && state->preview && !state->previewing,
                        "Original ISO preview failed");
                require(state->phase=="ISO ready. Click Install game to continue.","Artwork was not constructed by GUI");
                require(std::string_view(SDL_GetWindowTitle(SDL_GetRenderWindow(renderer))).starts_with("ETI Yami - Ready to install"),
                        "GUI did not publish artwork-ready title");
                frame(renderer,"ready.bmp"); click(renderer,1); stage=5;
            } else {
                require(!state->error.empty() && !state->preview && !state->installed,"Invalid ISO preview did not finish with an error");
                errorDialog=true; click(renderer,0); stage=6;
            }
        }
    } else if (stage==5 && dialogs==5) {
        std::lock_guard lock(state->mutex);
        require(state->error.empty() && state->preview && !state->busy,"Folder cancel broke artwork-ready GUI");
        frame(renderer,"ready-after-folder-cancel.bmp"); errorDialog=true; click(renderer,0); stage=6;
    } else if (stage==6 && state && !state->dialog && !callback) {
        require(state->error=="Injected portal response error","GUI did not consume native picker error");
        key(SDLK_ESCAPE); finished=true; stage=7;
    }
    return SDL_RenderPresent(renderer);
}
void run(const InstallRequest& request,bool cancel) {
    callback=nullptr; opaque=nullptr; state.reset(); dialogs=stage=0;
    cancelPreview=cancel; cancelSent=finished=errorDialog=false; interrupted=false;
    require(SDL_Init(SDL_INIT_VIDEO),"Cannot initialize real SDL video");
    struct Lifetime { ~Lifetime() { SDL_Quit(); } } lifetime;
    std::cout << "SDL video: " << SDL_GetCurrentVideoDriver() << "; preview cancellation: " << cancel << std::endl;
    require(graphical(request)==0 && finished && !callback,"Actual GUI did not respond and exit");
    require(!state->busy && !state->installed,"GUI left work running or installed a game");
    state.reset();
    require(!fs::exists(request.paths.root) && !fs::exists(base/"etiyami") &&
            !fs::exists(request.paths.applications) && !fs::exists(request.paths.desktop),"GUI smoke wrote installation/shortcuts");
}
}
int main(int argc,char** argv) {
    try {
        using namespace probe;
        require(argc==1 || argc==4,"Usage: check_setup_gui [ORIGINAL_ISO 7ZZ FRAME_DIRECTORY]");
        auto pattern=(fs::temp_directory_path()/"yami-setup-gui-XXXXXX").string();
        require(::mkdtemp(pattern.data())!=nullptr,"Cannot create private GUI fixture"); base=pattern;
        struct Fixture { ~Fixture() { std::error_code error; fs::remove_all(base,error); } } fixture;
        realIso=argc==4;
        iso=realIso?fs::absolute(argv[1]):base/"missing.iso";
        if (realIso) {
            require(fs::is_regular_file(iso),"Original ISO is missing");
            frames=fs::absolute(argv[3]); require(!fs::exists(frames),"Frame directory must not exist");
            fs::create_directories(frames); fs::permissions(frames,fs::perms::owner_all);
        }
        InstallRequest request{{base/"initial-install",base/"applications",base/"Desktop"},{},{},
                               realIso?fs::absolute(argv[2]):fs::path{}, {}};
        run(request,false);
        if (realIso) run(request,true); // A fresh SDL/window/renderer lifecycle, real extraction and GUI Escape cancellation.
        std::cout << "GUI inline ISO/folder callback reentry, cancellation and responsive exit passed\n";
    } catch (const std::exception& error) {
        std::cerr << "setup GUI check: " << error.what() << '\n'; return 1;
    }
}
