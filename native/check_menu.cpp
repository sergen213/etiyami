#include "menu.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <limits>

using namespace yami::menu;
namespace {
bool near(float a, float b) { return std::fabs(a-b) < 0.0001f; }
bool has(const Menu& menu, CommandKind kind) {
    return std::any_of(menu.commands().begin(), menu.commands().end(),
                       [kind](const Command& c) { return c.kind == kind; });
}
void click(Menu& menu, float x, float hotY, const Context& context = {}) {
    menu.set_cursor({x,hotY-45});
    (void)menu.draw(context);
    const auto page=menu.page();
    menu.update({{},true},context);
    assert(menu.page()==page); // Discrete callbacks run on release, not press.
    assert(!has(menu,CommandKind::InitializeLevel));
    assert(!has(menu,CommandKind::ResumeGame) && !has(menu,CommandKind::RunScript));
    menu.clear_commands();
    menu.update({{},false},context);
    (void)menu.draw(context);
}
void click_button(Menu& menu, std::size_t index) {
    const auto rect=menu.pages()[std::size_t(menu.page())].buttons[index].rect;
    // 0041be00 clamps x>979 to 1024. Page14's last answer spans 969..998,
    // so its centre is unreachable; click inside the reachable left edge.
    click(menu,rect.x+1,rect.y+rect.height/2);
    assert(contains(rect,{menu.cursor().x,menu.cursor().y+45}));
}
std::size_t graphics_button(const Menu& menu, std::string_view name, bool increase) {
    const auto& page=menu.pages()[std::size_t(menu.page())];
    const auto label=std::find_if(page.labels.begin(),page.labels.end(),[&](const Label& item) {
        return item.text==name;
    });
    assert(label!=page.labels.end());
    std::size_t result=page.buttons.size();
    float highest=-1;
    for (std::size_t i=0;i<page.buttons.size();++i) {
        const auto& button=page.buttons[i];
        if (button.style!=Style::Normal || button.rect.x<label->position.x ||
            button.rect.y+button.rect.height>label->position.y ||
            button.overMaterial!=(increase?"menu/ayarlar_p_a":"menu/ayarlar_p_e")) continue;
        if (button.rect.y>highest) { highest=button.rect.y; result=i; }
    }
    assert(result<page.buttons.size());
    return result;
}
void changed_graphics(const Menu& menu) {
    const auto apply=std::find_if(menu.commands().begin(),menu.commands().end(),[](const Command& c) {
        return c.kind==CommandKind::ApplySettings;
    });
    assert(apply!=menu.commands().end());
    assert(apply+1!=menu.commands().end() && (apply+1)->kind==CommandKind::PersistSettings);
}
void check_graphics(Menu& menu) {
    menu.show(1); menu.clear_commands();
    click_button(menu,6); // Enter the original main-menu Ayarlar callback.
    assert(menu.page()==3);
    menu.set_graphics_limits(6,3);
    auto settings=menu.settings();
    settings.brightness=1.25f; settings.sensitivity=63;
    settings.graphics={};
    settings.graphics.backend=yami::GraphicsBackend::Vulkan;
    settings.graphics.ray_tracing=false; settings.graphics.temporal_aa=false;
    settings.graphics.shadows=.713f; settings.graphics.indirect_lighting=.413f;
    settings.graphics.exposure=1.713f; settings.graphics.render_scale=.713f;
    settings.graphics.roughness=.613f;
    settings.graphics.ambient_occlusion=.973f;
    settings.graphics.reflections=.237f;
    settings.graphics.bloom=.113f;
    settings.graphics.sharpen=.187f;
    menu.set_settings(settings); menu.clear_commands();
    assert(menu.settings().graphics==settings.graphics);
    assert(menu.settings_effect().values.graphics==settings.graphics);
    const auto sharpenPlus=graphics_button(menu,"Keskinlik",true);
    const auto sharpenMinus=graphics_button(menu,"Keskinlik",false);
    menu.set_launcher_backend(yami::GraphicsBackend::OpenGL);
    menu.set_launcher_backend(yami::GraphicsBackend::Vulkan);
    for (const bool gameplay:{false,true}) {
        menu.set_graphics_device(gameplay,gameplay); // Launcher preview remains GL even with Vulkan selected.
        const auto draws=menu.draw({});
        assert(std::any_of(draws.begin(),draws.end(),[](const Draw& draw) {
            return draw.glyph && draw.text=="OpenGL/GLES için";
        }));
        const auto before=menu.settings().graphics;
        std::ostringstream savedBefore; write_settings(savedBefore,menu.settings());
        for (const auto button:{sharpenMinus,sharpenPlus}) {
            menu.clear_commands(); click_button(menu,button);
            assert(menu.settings().graphics==before);
            assert(!has(menu,CommandKind::ApplySettings) && !has(menu,CommandKind::PersistSettings));
        }
        std::ostringstream savedAfter; write_settings(savedAfter,menu.settings());
        assert(savedAfter.str()==savedBefore.str());
        menu.clear_commands(); click_button(menu,32);
        menu.clear_commands(); click_button(menu,25); // Advanced row 4 is exposure, not sharpening.
        auto exposed=before; exposed.exposure+=.05f;
        assert(menu.settings().graphics==exposed); changed_graphics(menu);
        menu.clear_commands(); click_button(menu,24);
        assert(near(menu.settings().graphics.exposure,before.exposure));
        assert(menu.settings().graphics.sharpen==before.sharpen); changed_graphics(menu);
        menu.clear_commands(); click_button(menu,32);
    }
    menu.set_launcher_backend(yami::GraphicsBackend::OpenGL);
    menu.set_graphics_device(false,false);
    assert(menu.settings().graphics.sharpen==settings.graphics.sharpen);
    const auto glDraws=menu.draw({});
    assert(std::none_of(glDraws.begin(),glDraws.end(),[](const Draw& draw) {
        return draw.glyph && draw.text=="OpenGL/GLES için";
    }));
    menu.clear_commands(); click_button(menu,sharpenPlus);
    assert(near(menu.settings().graphics.sharpen,.237f)); changed_graphics(menu);
    menu.clear_commands(); click_button(menu,sharpenMinus);
    assert(near(menu.settings().graphics.sharpen,.187f)); changed_graphics(menu);
    settings.graphics.backend=yami::GraphicsBackend::OpenGL;
    menu.set_settings(settings); menu.clear_commands();
    const auto original=menu.settings();
    const auto plus=graphics_button(menu,"Ortam gölgesi",true);
    const auto rect=menu.pages()[3].buttons[plus].rect;
    menu.set_cursor({rect.x+1,rect.y+rect.height/2-45});
    menu.update({{},false},{});
    assert(!has(menu,CommandKind::ApplySettings) && !has(menu,CommandKind::PersistSettings));
    menu.clear_commands(); menu.update({{},true},{});
    for (int frame=0;frame<10;++frame) menu.update({{},true},{});
    assert(menu.settings().graphics==original.graphics);
    assert(!has(menu,CommandKind::ApplySettings) && !has(menu,CommandKind::PersistSettings));
    menu.clear_commands(); menu.update({{},false},{});
    assert(menu.settings().graphics.ambient_occlusion==1);
    changed_graphics(menu);
    menu.clear_commands(); click_button(menu,plus);
    assert(!has(menu,CommandKind::ApplySettings) && !has(menu,CommandKind::PersistSettings));
    assert(menu.settings().graphics.reflections==original.graphics.reflections);
    assert(menu.settings().graphics.bloom==original.graphics.bloom);
    assert(menu.settings().graphics.sharpen==original.graphics.sharpen);
    menu.clear_commands(); click_button(menu,graphics_button(menu,"Yansımalar",true));
    assert(near(menu.settings().graphics.reflections,.287f)); changed_graphics(menu);
    menu.clear_commands(); click_button(menu,graphics_button(menu,"Yansımalar",false));
    assert(near(menu.settings().graphics.reflections,.237f)); changed_graphics(menu);
    for (const auto name:{"Ortam gölgesi","Yansımalar","Parlama","Keskinlik"}) {
        const auto minus=graphics_button(menu,name,false);
        for (int step=0;step<25;++step) { menu.clear_commands(); click_button(menu,minus); }
        assert(!has(menu,CommandKind::ApplySettings) && !has(menu,CommandKind::PersistSettings));
        const auto increase=graphics_button(menu,name,true);
        menu.clear_commands(); click_button(menu,increase); changed_graphics(menu);
    }
    const auto effects=menu.settings().graphics;
    assert(near(effects.ambient_occlusion,.05f) && near(effects.reflections,.05f));
    assert(near(effects.bloom,.05f) && near(effects.sharpen,.05f));
    auto retained=original.graphics;
    retained.ambient_occlusion=effects.ambient_occlusion; retained.reflections=effects.reflections;
    retained.bloom=effects.bloom; retained.sharpen=effects.sharpen;
    assert(effects==retained);
    assert(near(menu.settings().brightness,original.brightness));
    assert(near(menu.settings().sensitivity,original.sensitivity));
    for (const auto name:{"Işıklandırma","Tam ekran"}) {
        auto expected=menu.settings().graphics;
        if (name==std::string_view("Işıklandırma")) expected.enhanced=false;
        else expected.fullscreen=false;
        menu.clear_commands(); click_button(menu,graphics_button(menu,name,false));
        assert(name==std::string_view("Işıklandırma") ? !menu.settings().graphics.enhanced :
                                                     !menu.settings().graphics.fullscreen);
        assert(menu.settings().graphics==expected);
        if (name==std::string_view("Işıklandırma")) {
            changed_graphics(menu);
            const auto apply=std::find_if(menu.commands().begin(),menu.commands().end(),[](const Command& c) {
                return c.kind==CommandKind::ApplySettings;
            });
            assert(apply->settings.values.graphics==expected);
        }
        menu.clear_commands(); click_button(menu,graphics_button(menu,name,false));
        assert(!has(menu,CommandKind::ApplySettings) && !has(menu,CommandKind::PersistSettings));
        if (name==std::string_view("Işıklandırma")) expected.enhanced=true;
        else expected.fullscreen=true;
        menu.clear_commands(); click_button(menu,graphics_button(menu,name,true)); changed_graphics(menu);
        assert(menu.settings().graphics==expected);
        menu.clear_commands(); click_button(menu,graphics_button(menu,name,true));
        assert(!has(menu,CommandKind::ApplySettings) && !has(menu,CommandKind::PersistSettings));
    }
    menu.clear_commands(); click_button(menu,graphics_button(menu,"Yumuşatma",false));
    assert(menu.settings().graphics.samples==0); changed_graphics(menu);
    menu.clear_commands(); click_button(menu,graphics_button(menu,"Yumuşatma",true));
    assert(menu.settings().graphics.samples==4); changed_graphics(menu);
    menu.clear_commands(); click_button(menu,graphics_button(menu,"Yumuşatma",true));
    assert(menu.settings().graphics.samples==4 && !has(menu,CommandKind::PersistSettings));
    menu.clear_commands(); click_button(menu,graphics_button(menu,"Doku filtresi",false));
    assert(menu.settings().graphics.anisotropy==2); changed_graphics(menu);
    menu.clear_commands(); click_button(menu,graphics_button(menu,"Doku filtresi",true));
    assert(menu.settings().graphics.anisotropy==2 && !has(menu,CommandKind::PersistSettings));
    menu.clear_commands(); click_button(menu,graphics_button(menu,"Doku filtresi",false));
    assert(menu.settings().graphics.anisotropy==1); changed_graphics(menu);
    menu.clear_commands(); click_button(menu,graphics_button(menu,"Doku filtresi",false));
    assert(!has(menu,CommandKind::ApplySettings) && !has(menu,CommandKind::PersistSettings));
    menu.set_graphics_limits(16,16);
    menu.clear_commands(); click_button(menu,graphics_button(menu,"Yumuşatma",false));
    for (const int samples:{4,8,16}) {
        menu.clear_commands(); click_button(menu,graphics_button(menu,"Yumuşatma",true));
        assert(menu.settings().graphics.samples==samples); changed_graphics(menu);
    }
    for (const float filtering:{2.0f,4.0f,8.0f,16.0f}) {
        menu.clear_commands(); click_button(menu,graphics_button(menu,"Doku filtresi",true));
        assert(menu.settings().graphics.anisotropy==filtering); changed_graphics(menu);
    }
    menu.set_launcher_backend(yami::GraphicsBackend::OpenGL);
    menu.set_graphics_device(false,false);
    menu.clear_commands(); click_button(menu,32);
    const auto beforeShadow=menu.settings().graphics;
    menu.clear_commands(); click_button(menu,21); // Existing advanced shadow increase index.
    auto afterShadow=beforeShadow; afterShadow.shadows+=.05f;
    assert(menu.settings().graphics==afterShadow && !afterShadow.ray_tracing);
    changed_graphics(menu);
    menu.clear_commands(); click_button(menu,32);
    std::ostringstream output; write_settings(output,menu.settings());
    std::istringstream input(output.str()); const auto restored=read_settings(input);
    assert(restored.graphics==menu.settings().graphics);
    menu.set_settings(restored);
    const std::string legacy="parlaklik=1.250000\nhassasiyet=63.000000\nmuzikses=50.000000\n"
                             "efektses=50.000000\nhedefleme=otomatik\n";
    std::istringstream old(legacy);
    const auto legacySettings=read_settings(old);
    const yami::GraphicsSettings defaults;
    assert(defaults.backend==yami::GraphicsBackend::OpenGL);
    assert(defaults.ray_tracing && defaults.temporal_aa);
    assert(near(defaults.shadows,.75f) && near(defaults.indirect_lighting,.35f));
    assert(near(defaults.exposure,1) && near(defaults.render_scale,1) && near(defaults.roughness,.85f));
    assert(legacySettings.graphics==defaults);
    for (const auto backend:{yami::GraphicsBackend::OpenGL,yami::GraphicsBackend::Vulkan}) {
        auto selected=restored; selected.graphics.backend=backend;
        selected.graphics.ray_tracing=true; selected.graphics.temporal_aa=true;
        std::ostringstream saved; write_settings(saved,selected);
        std::istringstream loaded(saved.str());
        assert(read_settings(loaded).graphics==selected.graphics);
        menu.set_settings(selected);
        assert(menu.settings().graphics==selected.graphics);
    }
    menu.set_settings(restored);
    std::ostringstream prefix; write_settings(prefix,legacySettings);
    assert(prefix.str().starts_with(legacy));
    for (const auto extension:{"grafik_ao=nan\n","grafik_ao=1.01\n","grafik_ao=-.01\n",
                              "grafik_ao=0,5\n","grafik_msaa=2.5\n","grafik_msaa=-1\n",
                              "grafik_msaa=2147483648\n","grafik_anizotropi=0\n",
                              "grafik_anizotropi=inf\n","grafik_tamekran=2\n",
                              "grafik_gelistirilmis=true\n","grafik_yansima=\n",
                              "grafik_ao=.5\n grafık_ao=.3\n","grafik_ao=.5\ngrafik_ao=.3\n",
                              "grafik_bilinmeyen=1\n","grafik_parlama=.2junk\n",
                              "grafik_backend=\n","grafik_backend=Vulkan\n","grafik_backend=metal\n",
                              "grafik_backend=opengl\ngrafik_backend=vulkan\n",
                              "grafik_rt=2\n","grafik_rt=-1\n","grafik_rt=true\n","grafik_rt=\n",
                              "grafik_taa=2\n","grafik_taa=-1\n","grafik_taa=true\n","grafik_taa=\n",
                              "grafik_rt=0\ngrafik_rt=1\n","grafik_taa=0\ngrafik_taa=1\n"}) {
        std::istringstream malformed(legacy+extension);
        bool rejected=false;
        try { (void)read_settings(malformed); } catch (const std::exception&) { rejected=true; }
        assert(rejected);
    }
    struct FloatSetting {
        const char* key;
        float yami::GraphicsSettings::*field;
        float minimum,maximum;
    };
    for (const auto setting:{
        FloatSetting{"grafik_golge",&yami::GraphicsSettings::shadows,0,1},
        FloatSetting{"grafik_gi",&yami::GraphicsSettings::indirect_lighting,0,1},
        FloatSetting{"grafik_pozlama",&yami::GraphicsSettings::exposure,.1f,4},
        FloatSetting{"grafik_olcek",&yami::GraphicsSettings::render_scale,.5f,1},
        FloatSetting{"grafik_puruz",&yami::GraphicsSettings::roughness,.05f,1}}) {
        for (const float boundary:{setting.minimum,setting.maximum}) {
            std::ostringstream extension; extension<<setting.key<<'='<<boundary<<'\n';
            std::istringstream input(legacy+extension.str());
            const auto parsed=read_settings(input);
            assert(parsed.graphics.*setting.field==boundary);
            std::ostringstream saved; write_settings(saved,parsed);
            std::istringstream loaded(saved.str());
            assert(read_settings(loaded).graphics==parsed.graphics);
        }
        for (const auto value:{"","nan","inf","-inf","0,5","1junk","1e1000"}) {
            std::istringstream input(legacy+setting.key+"="+value+"\n");
            bool rejected=false;
            try { (void)read_settings(input); } catch (const std::exception&) { rejected=true; }
            assert(rejected);
        }
        for (const float value:{setting.minimum-.01f,setting.maximum+.01f,
                              std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity()}) {
            std::ostringstream extension; extension<<setting.key<<'='<<value<<'\n';
            std::istringstream input(legacy+extension.str());
            bool rejected=false;
            try { (void)read_settings(input); } catch (const std::exception&) { rejected=true; }
            assert(rejected);
            auto invalid=restored; invalid.graphics.*setting.field=value;
            std::ostringstream output; rejected=false;
            try { write_settings(output,invalid); } catch (const std::exception&) { rejected=true; }
            assert(rejected);
        }
        std::ostringstream duplicate;
        duplicate<<setting.key<<'='<<setting.maximum<<'\n'<<setting.key<<'='<<setting.maximum<<'\n';
        std::istringstream input(legacy+duplicate.str());
        bool rejected=false;
        try { (void)read_settings(input); } catch (const std::exception&) { rejected=true; }
        assert(rejected);
    }
    auto invalidBackend=restored;
    invalidBackend.graphics.backend=static_cast<yami::GraphicsBackend>(-1);
    std::ostringstream invalidOutput;
    bool rejected=false;
    try { write_settings(invalidOutput,invalidBackend); } catch (const std::exception&) { rejected=true; }
    assert(rejected);
    settings=menu.settings();
    settings.graphics.anisotropy=std::numeric_limits<float>::max();
    settings.graphics.reflections=.237123f;
    std::ostringstream large; write_settings(large,settings);
    std::istringstream largeInput(large.str());
    assert(read_settings(largeInput).graphics==settings.graphics);
    const auto knob=menu.pages()[3].buttons[1].rect;
    menu.set_cursor({knob.x+1,knob.y+knob.height/2-45});
    menu.clear_commands(); menu.update({{},true},{});
    assert(has(menu,CommandKind::ApplySettings) && !has(menu,CommandKind::PersistSettings));
    menu.clear_commands(); menu.update({{},true},{});
    assert(has(menu,CommandKind::ApplySettings) && !has(menu,CommandKind::PersistSettings));
    menu.set_cursor({0,0}); menu.clear_commands(); menu.update({{},false},{});
    assert(has(menu,CommandKind::PersistSettings) && !has(menu,CommandKind::ApplySettings));
    menu.clear_commands(); menu.set_fullscreen(false);
    assert(has(menu,CommandKind::PersistSettings) && !has(menu,CommandKind::ApplySettings));
    assert(!has(menu,CommandKind::SetAimingMode) && !menu.settings().graphics.fullscreen);
    menu.clear_commands(); menu.set_fullscreen(false);
    assert(menu.commands().empty());
    click_button(menu,0);
    assert(menu.page()==1 && has(menu,CommandKind::PersistSettings));
}
}
void check_settings_file() {
    namespace fs=std::filesystem;
    auto directory=fs::temp_directory_path()/fs::path(u8"yami-settings-ışık-");
    directory+=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    assert(fs::create_directory(directory));
    struct Cleanup {
        fs::path directory;
        ~Cleanup() { std::error_code ignored; fs::remove_all(directory,ignored); }
    } cleanup{directory};
    const auto path=directory/fs::path(u8"ayarlar-ş.ini");
    Settings first; first.graphics.samples=0; first.graphics.ambient_occlusion=.37123f;
    first.graphics.backend=yami::GraphicsBackend::Vulkan;
    first.graphics.ray_tracing=false; first.graphics.temporal_aa=false;
    first.graphics.shadows=.71234f; first.graphics.indirect_lighting=.41234f;
    first.graphics.exposure=1.71234f; first.graphics.render_scale=.71234f;
    first.graphics.roughness=.61234f;
    save_settings(path,first);
    auto second=first; second.graphics.samples=4; second.graphics.fullscreen=true;
    save_settings(path,second); // Existing files must be replaced, including Windows.
    {
        std::ifstream file(path); const auto restored=read_settings(file);
        assert(restored.graphics==second.graphics);
    }
    for (const auto field:{&yami::GraphicsSettings::bloom,&yami::GraphicsSettings::shadows,
                          &yami::GraphicsSettings::indirect_lighting,&yami::GraphicsSettings::exposure,
                          &yami::GraphicsSettings::render_scale,&yami::GraphicsSettings::roughness}) {
        auto invalid=second; invalid.graphics.*field=std::numeric_limits<float>::quiet_NaN();
        bool rejected=false;
        try { save_settings(path,invalid); } catch (const std::exception&) { rejected=true; }
        assert(rejected);
        std::ifstream unchanged(path);
        assert(read_settings(unchanged).graphics==second.graphics);
    }
    for (const auto& entry:fs::directory_iterator(directory))
        assert(entry.path()==path); // Failed writes cannot leave temporary files behind.
}
int main(int argc, char** argv) {
    Menu menu(argc > 1 ? argv[1] : "game");
    assert(menu.pages().size()==24 && menu.page()==1);
    assert(menu.fonts().size()==1 && menu.fonts()[0].height==22);
    assert(menu.fonts()[0].atlasWidth==256 && menu.fonts()[0].atlasHeight==128);
    assert(near(text_width(menu.fonts()[0],"İıŞşĞğÜüÖöÇç"),135.2f));
    const auto& newGame=menu.pages()[1].buttons[0];
    assert(newGame.rect.x==378 && newGame.rect.y==313);
    assert(newGame.rect.width==58 && newGame.rect.height==55);
    assert(contains(newGame.rect,{378,313}));
    assert(contains(newGame.rect,{436,340}));
    assert(contains(newGame.rect,{400,368}));
    assert(!contains(newGame.rect,{436.01f,340}));
    assert(!contains(newGame.rect,{400,368.01f}));
    // Painted label/decorative circle is not the click target.
    click(menu,200,200);
    assert(menu.page()==1 && !has(menu,CommandKind::InitializeLevel));
    menu.clear_commands();
    menu.set_cursor({400,295});
    menu.update({{},false},{});
    assert(menu.pages()[1].buttons[0].over);
    assert(menu.pages()[1].buttons[1].over && menu.pages()[1].buttons[2].over);
    const auto draws=menu.draw({});
    const auto button=std::find_if(draws.begin(),draws.end(),[](const Draw& d) {
        return d.button==0 && !d.glyph;
    });
    assert(button!=draws.end());
    assert(near(button->positions[0].x,378.58f/1024));
    assert(near(button->positions[1].x,435.42f/1024));
    assert(near(button->uv[0].x,0.01f) && near(button->uv[1].x,0.99f));
    menu.clear_commands();
    // Pointer motion is applied after hit testing, not before it.
    menu.set_cursor({300,295});
    menu.update({{100,0},true},{});
    assert(!menu.pages()[1].buttons[0].activated);
    menu.clear_commands();
    menu.update({{},false},{});
    assert(menu.page()==7 && has(menu,CommandKind::InitializeLevel));
    assert(has(menu,CommandKind::ResumeGame));
    menu.show(1); menu.clear_commands();
    // Existing living player: red New Game button resumes, not reinitializes.
    Context living; living.playerExists=true; living.previousLevel=2;
    click(menu,400,340,living);
    assert(!has(menu,CommandKind::InitializeLevel) && has(menu,CommandKind::ResumeGame));
    menu.show(1); menu.clear_commands();
    living.playerDead=true;
    click(menu,400,340,living);
    assert(has(menu,CommandKind::ResetLevel) && has(menu,CommandKind::InitializeLevel));
    menu.show(2); menu.clear_commands();
    click(menu,160,550);
    assert(menu.page()==8);
    menu.clear_commands(); click(menu,150,600);
    assert(!has(menu,CommandKind::LoadCheckpoint)); // missing saves are disabled
    Context saved; saved.saveCounts={{2,2,2,1}};
    menu.clear_commands(); click(menu,150,600,saved);
    const auto load=std::find_if(menu.commands().begin(),menu.commands().end(),[](const Command& c) {
        return c.kind==CommandKind::LoadCheckpoint;
    });
    assert(load!=menu.commands().end() && load->name=="data/save/save11.eti" && load->level==1);
    menu.show(3); menu.clear_commands();
    const auto before=menu.settings();
    menu.set_cursor({220,480});
    menu.update({{},true},{}); // held combo repeats every original frame
    assert(menu.settings().brightness!=before.brightness);
    assert(has(menu,CommandKind::ApplySettings));
    const auto after=menu.settings();
    menu.clear_commands(); menu.update({{},true},{});
    assert(menu.settings().brightness!=after.brightness);
    assert(!has(menu,CommandKind::PersistSettings)); // Held knobs save on release, not every frame.
    std::ostringstream output; write_settings(output,menu.settings());
    std::istringstream input(output.str());
    const auto restored=read_settings(input);
    assert(near(restored.brightness,menu.settings().brightness));
    assert(near(restored.sensitivity,menu.settings().sensitivity));
    assert(restored.automaticAim==menu.settings().automaticAim);
    std::istringstream malformed("parlaklik=nan\nhassasiyet=50\nmuzikses=50\nefektses=50\nhedefleme=elile\n");
    bool rejected=false;
    try { (void)read_settings(malformed); } catch (const std::exception&) { rejected=true; }
    assert(rejected);
    // show() (0041be80) changes only the page; it does not consume a held click.
    // The options input above is still down at (220,480), so releasing after
    // show(12) hits answer 0 at hotspot (220,525), before the next click.
    menu.show(12); menu.clear_commands(); menu.update({{},false},{});
    assert(menu.page()==15 && menu.commands().empty());
    click_button(menu,0); // Original wrong-answer screen returns to its question.
    assert(menu.page()==12 && menu.commands().empty());
    // 0041e1a0 cases 12..14: correct zero-based answers are 2, 5 and 12.
    // Exercise every other XML answer and its real retry button, not injected
    // activation flags. All failures must leave the gameplay callback untouched.
    for (const int question:{12,13,14}) {
        const std::size_t correct=question==12?2:question==13?5:12;
        click(menu,0,100);
        assert(menu.page()==question && menu.commands().empty());
        const auto answers=menu.pages()[std::size_t(question)].buttons.size();
        for (std::size_t answer=0;answer<answers;++answer) {
            if (answer==correct) continue;
            click_button(menu,answer);
            assert(menu.page()==question+3 && menu.commands().empty());
            click_button(menu,0);
            assert(menu.page()==question && menu.commands().empty());
        }
        // These hotspots lie inside original XML answers 2, 5 and 12.
        if (question==12) click(menu,600,500);
        else if (question==13) click(menu,750,550);
        else click(menu,770,400);
        if (question<14) {
            assert(menu.page()==question+1 && menu.commands().empty());
        } else {
            // 0041eb10..0041eb57 resumes (true,false), then invokes exactly this
            // script; the menu itself does not switch to an invented next page.
            const auto commands=menu.commands();
            assert(commands.size()==2);
            assert(commands[0].kind==CommandKind::ResumeGame);
            assert(commands[0].first && !commands[0].second);
            assert(commands[1].kind==CommandKind::RunScript);
            assert(commands[1].name=="librarygame_on_end");
        }
    }
    menu.show(0); menu.set_button_state(5,true); menu.clear_commands();
    const auto hud=menu.draw(living);
    const auto health=std::find_if(hud.begin(),hud.end(),[](const Draw& d) { return d.button==0; });
    assert(health!=hud.end() && near(health->uv[1].x,living.health));
    menu.set_cursor({1000,1000});
    // Compare real artwork/glyph geometry, including the baked background
    // labels and battery: only empty background strips may grow wider.
    const std::array<Objective,1> objectives{{{"Find Yami",false}}};
    const Hud layout{123,2,true,objectives};
    const auto originalView=menu.draw(living,layout);
    const std::vector<Draw> original(originalView.begin(),originalView.end());
    for (float logicalWidth:{768.0f*16/9,768.0f*21/9}) {
        const auto wide=menu.draw(living,layout,logicalWidth);
        const float extra=(logicalWidth-1024)/1024;
        const auto background=menu.pages()[0].material;
        float backgroundLeft=logicalWidth/1024,backgroundRight=0;
        for (const auto& d:wide) {
            if (d.glyph || d.material!=background) continue;
            backgroundLeft=std::min(backgroundLeft,d.positions[0].x);
            backgroundRight=std::max(backgroundRight,d.positions[1].x);
            // The original atlas is empty between energy and PUAN, and
            // between PUAN and the battery. Everything else retains scale.
            const bool blank=(d.uv[0].x>=288.0f/1024 && d.uv[1].x<=480.0f/1024) ||
                             (d.uv[0].x>=650.0f/1024 && d.uv[1].x<=800.0f/1024);
            if (!blank)
                assert(near(d.positions[1].x-d.positions[0].x,d.uv[1].x-d.uv[0].x));
        }
        assert(near(backgroundLeft,0) && near(backgroundRight,logicalWidth/1024));
        for (float landmark:{64.0f,540.0f,920.0f}) {
            const float source=landmark/1024;
            const auto artwork=std::find_if(wide.begin(),wide.end(),[&](const Draw& d) {
                return !d.glyph && d.material==background && d.uv[0].x<=source && source<d.uv[1].x;
            });
            assert(artwork!=wide.end());
            const float offset=landmark<288?0:landmark<800?extra/2:extra;
            assert(near(artwork->positions[0].x+source-artwork->uv[0].x,source+offset));
        }
        for (const auto& before:original) {
            if (!before.glyph && before.material==background) continue;
            float offset=0;
            if (before.button==1 || (before.glyph && before.text.starts_with("Yüksek Zıplama: "))) offset=extra;
            else if (before.button==2 || before.button>=4 || (before.glyph && before.text=="123")) offset=extra/2;
            const auto match=std::find_if(wide.begin(),wide.end(),[&](const Draw& d) {
                return d.button==before.button && d.glyph==before.glyph &&
                       d.text==before.text && d.material==before.material &&
                       near(d.uv[0].x,before.uv[0].x) && near(d.uv[0].y,before.uv[0].y) &&
                       near(d.rect.x,before.rect.x+offset) && near(d.rect.y,before.rect.y);
            });
            assert(match!=wide.end());
            const auto& after=*match;
            assert(near(after.rect.x,before.rect.x+offset));
            assert(near(after.rect.width,before.rect.width));
            assert(near(after.rect.height,before.rect.height));
            for (std::size_t vertex=0;vertex<4;++vertex) {
                assert(near(after.positions[vertex].x,before.positions[vertex].x+offset));
                assert(near(after.positions[vertex].y,before.positions[vertex].y));
                assert(near(after.uv[vertex].x,before.uv[vertex].x));
                assert(near(after.uv[vertex].y,before.uv[vertex].y));
            }
        }
    }
    const auto narrow=menu.draw(living,layout,768);
    assert(narrow.size()==original.size());
    for (std::size_t i=0;i<original.size();++i) {
        for (std::size_t vertex=0;vertex<4;++vertex) {
            assert(near(narrow[i].positions[vertex].x,original[i].positions[vertex].x));
            assert(near(narrow[i].positions[vertex].y,original[i].positions[vertex].y));
            assert(near(narrow[i].uv[vertex].x,original[i].uv[vertex].x));
            assert(near(narrow[i].uv[vertex].y,original[i].uv[vertex].y));
        }
    }
    assert(menu.cursor().x==1024 && menu.cursor().y==723); // original clamp bug retained
    check_graphics(menu);
    check_settings_file();
    std::cout << "Original menu XML, bitmap font, hit/release/held timing, transitions and settings checks passed\n";
}
