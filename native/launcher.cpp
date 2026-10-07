#include "launcher.hpp"

#include "menu.hpp"
#include "renderer.hpp"
#include "scene.hpp"
#include "update.hpp"
#include "update_install.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace yami {
namespace {
constexpr std::array<std::size_t,5> originalPlus{{1,4,13,7,10}};
constexpr std::size_t advancedFocus=13, backendFocus=14, playFocus=15, closeFocus=16, focusCount=17;
constexpr menu::Rect backendRect{504,220,224,44}, playRect{504,116,224,52}, closeRect{504,54,224,48};
constexpr std::array<float,4> ink{{.16f,.12f,.08f,1}};
constexpr std::array<float,4> focusColor{{.95f,.83f,.18f,1}};

void sdl_check(bool result) {
    if (!result) throw std::runtime_error(SDL_GetError());
}
menu::Draw quad(menu::Rect rect, std::array<float,4> color={{1,1,1,1}}) {
    menu::Draw draw;
    draw.rect={rect.x/1024,rect.y/768,rect.width/1024,rect.height/768};
    draw.positions={{{rect.x/1024,(rect.y+rect.height)/768},
                     {(rect.x+rect.width)/1024,(rect.y+rect.height)/768},
                     {(rect.x+rect.width)/1024,rect.y/768},{rect.x/1024,rect.y/768}}};
    draw.uv={{{0,1},{1,1},{1,0},{0,0}}};
    draw.color=color;
    return draw;
}
// Source coordinates are top-down pixels in the original 1024-square artwork.
menu::Draw crop(menu::Rect destination, std::string_view texture, menu::Rect source) {
    auto draw=quad(destination);
    draw.texture=texture;
    const float left=source.x/1024, right=(source.x+source.width)/1024;
    const float top=1-source.y/1024, bottom=1-(source.y+source.height)/1024;
    draw.uv={{{left,top},{right,top},{right,bottom},{left,bottom}}};
    return draw;
}
void outline(std::vector<menu::Draw>& draws, menu::Rect rect) {
    constexpr float width=3;
    draws.push_back(quad({rect.x-width,rect.y-width,rect.width+2*width,width},focusColor));
    draws.push_back(quad({rect.x-width,rect.y+rect.height,rect.width+2*width,width},focusColor));
    draws.push_back(quad({rect.x-width,rect.y,width,rect.height},focusColor));
    draws.push_back(quad({rect.x+rect.width,rect.y,width,rect.height},focusColor));
}
Vec2 menu_point(Renderer& renderer, Vec2 logical) {
    int width=0,height=0;
    sdl_check(SDL_GetWindowSize(renderer.window(),&width,&height));
    if (width<=0 || height<=0) throw std::runtime_error("Invalid launcher window size");
    const auto viewport=fit_original_interface(renderer.pixel_width(),renderer.pixel_height());
    return {(logical.x*renderer.pixel_width()/width-viewport.x)*1024/viewport.width,
            (renderer.pixel_height()-logical.y*renderer.pixel_height()/height-viewport.y)*768/viewport.height};
}
void drain_input() {
    SDL_ResetKeyboard();
    SDL_FlushEvent(SDL_EVENT_KEY_DOWN);
    SDL_FlushEvent(SDL_EVENT_KEY_UP);
    SDL_FlushEvent(SDL_EVENT_MOUSE_MOTION);
    SDL_FlushEvent(SDL_EVENT_MOUSE_BUTTON_DOWN);
    SDL_FlushEvent(SDL_EVENT_MOUSE_BUTTON_UP);
    SDL_FlushEvent(SDL_EVENT_MOUSE_WHEEL);
}
} // namespace

bool run_launcher(Renderer& renderer, SceneCache& scene, menu::Menu& menu,
                  const std::filesystem::path& saveDirectory, bool checkUpdates,
                  std::string_view recoveryMessage) {
    SDL_Window* window=renderer.window();
    const auto windowID=SDL_GetWindowID(window);
    sdl_check(SDL_SetWindowTitle(window,"ETI Yami - Başlatıcı / Launcher"));
    sdl_check(SDL_SetWindowRelativeMouseMode(window,false));
    sdl_check(SDL_SetWindowMouseGrab(window,false));
    sdl_check(SDL_ShowCursor());
    struct Drain { ~Drain() { drain_input(); SDL_ShowCursor(); } } drain;
    updates::cleanup_completed_updates(updates::installation_root());

    menu.clear_commands(); // Initial settings restoration must not start audio or gameplay.
    menu.show(3);
    menu.set_graphics_limits(renderer.max_samples(),renderer.max_anisotropy());
    menu.set_graphics_device(false,false);
    const menu::Context context{};
    const auto& page=menu.pages()[3];
    if (menu.fonts().empty()) throw std::runtime_error("Launcher requires original bitmap font");
    const auto& font=menu.fonts().front();
    const auto apply_preview=[&] {
        auto graphics=menu.settings().graphics;
        graphics.fullscreen=false; // Desired fullscreen belongs to the game handoff, not this window.
        graphics.backend=GraphicsBackend::OpenGL; // Preview never creates or switches to Vulkan.
        renderer.set_graphics(graphics);
        renderer.set_brightness(menu.settings_effect().brightnessRamp);
    };
    const auto commands=[&] {
        for (const auto& command:menu.commands()) {
            switch (command.kind) {
            case menu::CommandKind::ApplySettings: apply_preview(); break;
            case menu::CommandKind::PersistSettings:
                menu::save_settings(saveDirectory/"game.ini",menu.settings()); break;
            case menu::CommandKind::SetMusicVolume:
            case menu::CommandKind::SetAimingMode: break; // Values stay in Menu; audio/world do not exist yet.
            default: throw std::runtime_error("Unexpected gameplay command in launcher");
            }
        }
        menu.clear_commands();
    };
    apply_preview();

    std::array<menu::Rect,focusCount> focusRects{};
    for (std::size_t row=0;row<originalPlus.size();++row) {
        const auto first=originalPlus[row];
        const auto& plus=page.buttons[first].rect;
        const auto& minus=page.buttons[first+1].rect;
        const auto& needle=page.buttons[first+2].rect;
        const float left=std::min({plus.x,minus.x,needle.x});
        const float bottom=std::min({plus.y,minus.y,needle.y});
        focusRects[row]={left,bottom,
            std::max({plus.x+plus.width,minus.x+minus.width,needle.x+needle.width})-left,
            std::max({plus.y+plus.height,minus.y+minus.height,needle.y+needle.height})-bottom};
    }
    for (std::size_t row=0;row<8;++row) {
        const auto& minus=page.buttons[16+row*2].rect;
        const auto& plus=page.buttons[17+row*2].rect;
        focusRects[5+row]={minus.x,minus.y,plus.x+plus.width-minus.x,plus.height};
    }
    focusRects[playFocus]=playRect; focusRects[closeFocus]=closeRect;
    focusRects[advancedFocus]=page.buttons[32].rect;
    focusRects[backendFocus]=backendRect;
    std::size_t focus=playFocus;
    int pressedAction=-1, pressedMenu=-1;
    bool mouseDown=false,focused=(SDL_GetWindowFlags(window)&SDL_WINDOW_INPUT_FOCUS)!=0;
    bool finished=false,start=false;
    updates::Snapshot updateStatus{updates::State::Unavailable,0,0,"Offline"};
    std::unique_ptr<updates::Job> updateJob;
    bool updateHandled=!checkUpdates;
    if (checkUpdates) {
        try {
            updateJob=std::make_unique<updates::Job>(updates::installation_root());
            updateStatus.state=updates::State::Checking;
        } catch (const std::exception& error) {
            updateStatus.state=updates::State::Failed; updateHandled=true;
            std::cerr << "Yami update: " << error.what() << '\n';
        }
    }
    const auto updateBlocksPlay=[&] {
        return updateStatus.state==updates::State::Checking ||
               updateStatus.state==updates::State::Downloading ||
               updateStatus.state==updates::State::Ready;
    };
    std::uint64_t lastUpdatePoll=0;
    std::array<char,192> updateText{};
    std::size_t updateTextLength=0;
    const auto update_line=[&] {
        std::string_view message;
        switch (updateStatus.state) {
        case updates::State::Checking: message="GitHub güncellemeleri kontrol ediliyor..."; break;
        case updates::State::Downloading: message="Yeni sürüm indiriliyor; doğrulanıyor..."; break;
        case updates::State::Ready: message="Güncelleme kuruluyor; yeniden açılacak..."; break;
        case updates::State::Current: message="En yeni sürüm kullanılıyor."; break;
        case updates::State::Unavailable:
            message=checkUpdates?"GitHub sürümü yok veya erişim gerekiyor.":"Güncelleme kontrolü kapalı."; break;
        case updates::State::Failed: message="Güncelleme yapılamadı; mevcut sürüm kullanılabilir."; break;
        case updates::State::Cancelled: message="Güncelleme iptal edildi."; break;
        }
        const auto count=std::snprintf(updateText.data(),updateText.size(),"v%.*s | %.*s",
            int(updates::version().size()),updates::version().data(),int(message.size()),message.data());
        if (count<0 || std::size_t(count)>=updateText.size())
            throw std::runtime_error("Launcher update label overflow");
        updateTextLength=std::size_t(count);
    };
    update_line();
    float mouseX=0,mouseY=0;
    SDL_GetMouseState(&mouseX,&mouseY);
    Vec2 pointer=menu_point(renderer,{mouseX,mouseY});
    const auto action_at=[&](Vec2 point) {
        if (menu::contains(playRect,point)) return int(playFocus);
        if (menu::contains(closeRect,point)) return int(closeFocus);
        if (menu::contains(backendRect,point)) return int(backendFocus);
        return -1;
    };
    const auto position_cursor=[&](Vec2 point) {
        // Menu clamps its cursor. Park instead of clamping outside-content clicks onto controls.
        if (!focused || point.x<0 || point.x>1024 || point.y<0 || point.y>768 ||
            menu::contains(page.buttons[0].rect,point) || action_at(point)>=0)
            menu.set_cursor({0,723});
        else menu.set_cursor({point.x,point.y-45});
    };
    const auto release_mouse=[&] {
        mouseDown=false; pressedAction=-1; pressedMenu=-1;
        menu.set_cursor({0,723}); // Focus loss/keyboard handoff cannot activate a release-only button.
        menu.update({{},false},context);
        commands();
    };
    const auto adjust=[&](bool increase) {
        release_mouse();
        if (focus==backendFocus) {
            menu.set_launcher_backend(increase?GraphicsBackend::Vulkan:GraphicsBackend::OpenGL);
            commands();
            return;
        }
        const std::size_t button=focus==advancedFocus?32:focus<5 ? originalPlus[focus]+(increase?0:1) :
                                                    16+(focus-5)*2+(increase?1:0);
        const auto& rect=page.buttons[button].rect;
        menu.set_cursor({rect.x+rect.width/2,rect.y+rect.height/2-45});
        menu.update({{},true},context); commands();
        menu.update({{},false},context); commands();
        position_cursor(pointer);
    };

    char caps[96]{};
    const int capsLength=std::snprintf(caps,sizeof(caps),"GPU: MSAA %dx / Anizo %.0fx",
                                      renderer.max_samples(),double(renderer.max_anisotropy()));
    if (capsLength<0 || std::size_t(capsLength)>=sizeof(caps))
        throw std::runtime_error("Launcher hardware label overflow");
    std::vector<menu::Draw> chrome;
    chrome.reserve(1024);
    const auto draw_frame=[&] {
        const auto now=std::uint32_t(SDL_GetTicks());
        renderer.clear();
        renderer.camera(identity_matrix(),interface_projection(),true);
        const auto original=menu.draw(context);
        // Draw the stable span in-place; omit Back and the software cursor in favour of SDL's cursor.
        for (std::size_t i=0;i+1<original.size();++i)
            if (original[i].button!=0) scene.draw_ui(original.subspan(i,1),now);
        chrome.clear();
        chrome.push_back(crop({518,54,152,139},"data/images/menuimages/ayarlar_bg.jpg",{520,580,152,139}));
        chrome.push_back(crop({16,658,650,96},"data/images/menuimages/ayarlar_bg.jpg",{824,275,88,50}));
        chrome.push_back(crop({690,653,320,105},"data/images/menuimages/anamenu.jpg",{510,20,500,260}));
        menu::append_text(chrome,font,"ETI YAMI",{36,711},1.5f,ink);
        menu::append_text(chrome,font,"Ayarlarını seç, maceraya başla.",{36,679},.85f,ink);
        menu::append_text(chrome,font,{updateText.data(),updateTextLength},{36,661},.55f,ink);
        menu::append_text(chrome,font,{caps,std::size_t(capsLength)},{758,624},.6f,ink);
        chrome.push_back(quad(backendRect,{{.35f,.39f,.22f,1}}));
        const auto selected=menu.settings().graphics.backend;
        menu::append_text(chrome,font,selected==GraphicsBackend::Vulkan?"Vulkan":"OpenGL",
                          {backendRect.x+20,backendRect.y+14},.9f,{{1,1,.84f,1}});
        menu::append_text(chrome,font,"< OpenGL / Vulkan >  Next launch",{504,276},.55f,ink);
#if YAMI_HAS_VULKAN
        const std::string_view availability=selected==GraphicsBackend::Vulkan?
            "RT: GPU desteği açılışta kontrol edilir":"Vulkan ayarları OpenGL'de etkisiz";
#else
        const std::string_view availability=selected==GraphicsBackend::Vulkan?
            "Bu sürümde Vulkan yok; OpenGL seçin":"OpenGL hazır / Vulkan bu sürümde yok";
#endif
        menu::append_text(chrome,font,availability,{504,200},.47f,ink);
        if (!recoveryMessage.empty()) {
            chrome.push_back(quad({16,618,716,37},{{.25f,.08f,.04f,.95f}}));
            menu::append_text(chrome,font,"Vulkan açılamadı. OpenGL seçin veya tekrar deneyin.",
                              {27,631},.55f,{{1,1,.84f,1}});
        }
        for (const auto row:{playFocus,closeFocus}) {
            const auto rect=focusRects[row];
            const bool disabled=row==playFocus && updateBlocksPlay();
            const bool hover=focused && !disabled && action_at(pointer)==int(row);
            chrome.push_back(quad(rect,disabled?std::array<float,4>{{.4f,.4f,.4f,1}}:
                                 hover?focusColor:std::array<float,4>{{.35f,.39f,.22f,1}}));
            chrome.push_back(crop({rect.x+4,rect.y+4,rect.width-8,rect.height-8},
                                  "data/images/menuimages/ayarlar_bg.jpg",{824,275,88,50}));
            const std::string_view label=row==playFocus?"OYNA / PLAY":"KAPAT / CLOSE";
            const float scale=.95f;
            const float textWidth=menu::text_width(font,label)*scale;
            menu::append_text(chrome,font,label,{rect.x+(rect.width-textWidth)/2,rect.y+14},scale,ink);
        }
        if (focused) outline(chrome,focusRects[focus]);
        chrome.push_back(quad({16,8,716,31},{{.10f,.12f,.12f,.92f}}));
        menu::append_text(chrome,font,"Tab: seç  Oklar: ayarla  Enter: uygula  Esc: kapat",
                          {27,18},.6f,{{1,1,.84f,1}});
        scene.draw_ui(chrome,now);
        renderer.present();
    };
    position_cursor(pointer);
    draw_frame(); // Present before waiting for focus: Wayland must first map the surface.
    while (!finished) {
        const auto updateNow=SDL_GetTicks();
        if (updateJob && !updateHandled && updateNow-lastUpdatePoll>=250) {
            lastUpdatePoll=updateNow;
            updateStatus=updateJob->snapshot();
            if (updateStatus.state==updates::State::Ready) {
                updateHandled=true;
                try {
                    menu.set_cursor({0,723}); menu.update({{},false},context); menu.clear_commands();
                    menu::save_settings(saveDirectory/"game.ini",menu.settings());
                    update_line(); draw_frame();
                    const auto prepared=updateJob->prepared();
                    updates::launch_installer(prepared.stage,updates::installation_root(),scene.root(),saveDirectory);
                    updateJob->release_stage();
                    return false;
                } catch (const std::exception& error) {
                    updateStatus.state=updates::State::Failed;
                    std::cerr << "Yami update: " << error.what() << '\n';
                }
            } else if (updateStatus.state!=updates::State::Checking &&
                       updateStatus.state!=updates::State::Downloading) {
                updateHandled=true;
                if (updateStatus.state==updates::State::Failed)
                    std::cerr << "Yami update: " << updateStatus.message << '\n';
            }
            update_line();
        }
        SDL_Event event;
        while (!finished && SDL_PollEvent(&event)) {
            if (event.type==SDL_EVENT_QUIT) { finished=true; break; }
            if (event.type>=SDL_EVENT_WINDOW_FIRST && event.type<=SDL_EVENT_WINDOW_LAST) {
                if (event.window.windowID!=windowID) continue;
                switch (event.type) {
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED: finished=true; break;
                case SDL_EVENT_WINDOW_FOCUS_LOST: focused=false; release_mouse(); break;
                case SDL_EVENT_WINDOW_FOCUS_GAINED:
                    focused=true; mouseDown=false; pressedAction=-1;
                    SDL_GetMouseState(&mouseX,&mouseY);
                    pointer=menu_point(renderer,{mouseX,mouseY}); position_cursor(pointer); break;
                case SDL_EVENT_WINDOW_RESIZED:
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
                case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                    renderer.resize();
                    SDL_GetMouseState(&mouseX,&mouseY);
                    pointer=menu_point(renderer,{mouseX,mouseY}); position_cursor(pointer); break;
                default: break;
                }
            } else if (event.type==SDL_EVENT_MOUSE_MOTION) {
                if (event.motion.windowID!=windowID || !focused) continue;
                pointer=menu_point(renderer,{event.motion.x,event.motion.y});
                position_cursor(pointer);
                for (std::size_t row=0;row<focusCount;++row)
                    if (menu::contains(focusRects[row],pointer)) { focus=row; break; }
            } else if (event.type==SDL_EVENT_MOUSE_BUTTON_DOWN || event.type==SDL_EVENT_MOUSE_BUTTON_UP) {
                if (event.button.windowID!=windowID || !focused || event.button.button!=SDL_BUTTON_LEFT) continue;
                pointer=menu_point(renderer,{event.button.x,event.button.y}); position_cursor(pointer);
                const int action=action_at(pointer);
                if (event.button.down) {
                    for (std::size_t row=0;row<focusCount;++row)
                        if (menu::contains(focusRects[row],pointer)) { focus=row; break; }
                    mouseDown=true; pressedAction=action;
                    pressedMenu=-1;
                    for (std::size_t i=1;i<page.buttons.size();++i)
                        if (menu::contains(page.buttons[i].rect,pointer)) { pressedMenu=int(i); break; }
                } else {
                    if (mouseDown && pressedAction>=0 && pressedAction==action) {
                        if (action==int(backendFocus)) {
                            const auto current=menu.settings().graphics.backend;
                            menu.set_launcher_backend(current==GraphicsBackend::OpenGL?
                                                      GraphicsBackend::Vulkan:GraphicsBackend::OpenGL);
                            commands();
                        } else if (action!=int(playFocus) || !updateBlocksPlay()) {
                            start=action==int(playFocus); finished=true;
                        }
                    }
                    if (pressedMenu<0 || !menu::contains(page.buttons[std::size_t(pressedMenu)].rect,pointer))
                        menu.set_cursor({0,723});
                    mouseDown=false; pressedAction=-1; pressedMenu=-1;
                }
                // Consume release before any subsequent motion event changes the tested hot point.
                menu.update({{},mouseDown},context); commands();
            } else if (event.type==SDL_EVENT_KEY_DOWN) {
                if (event.key.windowID!=windowID || !focused || event.key.repeat) continue;
                switch (event.key.scancode) {
                case SDL_SCANCODE_ESCAPE: finished=true; break;
                case SDL_SCANCODE_TAB:
                    release_mouse();
                    focus=(focus+((event.key.mod&SDL_KMOD_SHIFT)?focusCount-1:1))%focusCount; break;
                case SDL_SCANCODE_LEFT:
                case SDL_SCANCODE_DOWN: if (focus<playFocus) adjust(false); break;
                case SDL_SCANCODE_RIGHT:
                case SDL_SCANCODE_UP: if (focus<playFocus) adjust(true); break;
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER:
                case SDL_SCANCODE_SPACE:
                    if (focus<playFocus) adjust(true);
                    else if (focus==closeFocus || !updateBlocksPlay()) {
                        start=focus==playFocus; finished=true;
                    }
                    break;
                default: break;
                }
            }
        }
        if (finished) break;
        position_cursor(pointer);
        menu.update({{},mouseDown},context); commands();
        draw_frame();
        SDL_Delay(16);
    }
    // Flush held dials without hit-testing a release, then save the authoritative latest values.
    menu.set_cursor({0,723}); menu.update({{},false},context); menu.clear_commands();
    menu::save_settings(saveDirectory/"game.ini",menu.settings());
    return start;
}
} // namespace yami
