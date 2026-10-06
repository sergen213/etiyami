#include "menu.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <istream>
#include <limits>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <utility>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_error.h>

namespace yami::menu {
namespace {
constexpr float angleLimit = 110, angleStep = 2;
constexpr float gainScale = std::bit_cast<float>(0x3b94f209u);
constexpr float rampScale = std::bit_cast<float>(0x3f945d17u);
constexpr float inverseScale = std::bit_cast<float>(0x3c14f209u);
constexpr float inputScale = std::bit_cast<float>(0xbca3d70au);
constexpr std::array<float,4> hudColor{{
    std::bit_cast<float>(0x3f39fbe7u), std::bit_cast<float>(0x3f79db23u),
    std::bit_cast<float>(0x3c75c28fu), 1}};
constexpr std::array<float,4> white{{1,1,1,1}};
constexpr std::array<float,4> parchmentInk{{.16f,.12f,.08f,1}};
constexpr std::array<std::string_view,8> graphicsLabels{{
    "Işıklandırma","Ortam gölgesi","Yansımalar","Parlama","Keskinlik",
    "Yumuşatma","Doku filtresi","Tam ekran"}};
constexpr std::array<std::string_view,8> graphicsFields{{
    "grafik_gelistirilmis","grafik_ao","grafik_yansima","grafik_parlama",
    "grafik_keskinlik","grafik_msaa","grafik_anizotropi","grafik_tamekran"}};
constexpr auto sampleChoices=[] {
    std::array<int,30> choices{};
    int value=4; // 2 samples is rounded up to 4 by some drivers.
    for (std::size_t i=1;i<choices.size();++i) {
        choices[i]=value;
        if (i+1<choices.size()) value*=2;
    }
    return choices;
}();
constexpr std::array<int,5> filterChoices{{1,2,4,8,16}};
template<class T>
T quality_step(T current, T cap, std::span<const int> choices, bool increase) {
    T result=current;
    for (int choice:choices) {
        const T value=T(choice);
        if (value>cap) break;
        if (increase && value>current) return value;
        if (!increase && value<current) result=value;
    }
    return result;
}
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
using Document = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
Document xml(const std::filesystem::path& path, const char* rootName) {
    Document doc(xmlReadFile(path.string().c_str(), "WINDOWS-1254", XML_PARSE_NONET), xmlFreeDoc);
    require(bool(doc), "Cannot parse original menu/font XML");
    require(!doc->intSubset && !doc->extSubset, "DTDs are not allowed in menu/font XML");
    const auto root = xmlDocGetRootElement(doc.get());
    require(root && xmlStrEqual(root->name,BAD_CAST rootName), "Unexpected menu/font XML root");
    return doc;
}
bool named(xmlNode* node, const char* name) {
    return node && node->type == XML_ELEMENT_NODE && xmlStrEqual(node->name,BAD_CAST name);
}
xmlNode* child(xmlNode* node, const char* name) {
    require(node != nullptr, "Missing menu/font XML parent");
    for (auto* item=node->children;item;item=item->next) if (named(item,name)) return item;
    throw std::runtime_error(std::string("Missing menu/font XML element: ")+name);
}
std::string attribute(xmlNode* node, const char* name) {
    require(node != nullptr, "Missing menu/font XML element");
    auto* value=xmlGetProp(node,BAD_CAST name);
    require(value != nullptr, "Missing menu/font XML attribute");
    std::string result(reinterpret_cast<const char*>(value));
    xmlFree(value);
    return result;
}
std::string value(xmlNode* node, const char* name) { return attribute(child(node,name),"value"); }
float number(std::string_view text) {
    float result=0;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
    require(parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size() && std::isfinite(result),
            "Invalid original menu/font/settings number");
    return result;
}
float attr_number(xmlNode* node, const char* name) { return number(attribute(node,name)); }
float field_number(xmlNode* node, const char* name) { return number(value(node,name)); }
int integer(double value) {
    require(value >= double(std::numeric_limits<int>::min()) && value < 2147483648.0,
            "Original menu/font integer overflow");
    return static_cast<int>(value); // Original _ftol truncates, not round-to-nearest.
}
std::size_t count(xmlNode* node, const char* name, std::size_t maximum) {
    const int result=integer(attr_number(node,name));
    require(result>=0 && std::size_t(result)<=maximum, "Original menu control count exceeds layout");
    return std::size_t(result);
}
Alignment alignment(std::string_view name) {
    if (name=="center") return Alignment::Center;
    if (name=="left") return Alignment::Left;
    if (name=="right") return Alignment::Right;
    throw std::runtime_error("Unknown original menu text alignment");
}
Style style(std::string_view name) {
    if (name=="normal") return Style::Normal;
    if (name=="noHitCheck") return Style::NoHitCheck;
    if (name=="AVI") return Style::Video;
    if (name=="combo") return Style::Repeat;
    if (name=="ibre") return Style::Needle;
    throw std::runtime_error("Unknown original menu button style");
}
Rect control_rect(xmlNode* node) {
    Rect result{field_number(node,"xPos"),field_number(node,"yPos"),
                float(integer(field_number(node,"width"))),float(integer(field_number(node,"height")))};
    require(result.width>0 && result.height>0, "Invalid original menu control dimensions");
    return result;
}
// libxml2 converts WINDOWS-1254 to UTF-8. Original font keys are bytes, including
// signed XML codes; convert Unicode back to those keys without allocating.
std::uint8_t next_byte(std::string_view& text) {
    require(!text.empty(), "Empty text decode");
    const auto lead=std::uint8_t(text.front());
    std::uint32_t code=lead;
    std::size_t length=1;
    if (lead>=0xc2 && lead<=0xdf) { code=lead&31; length=2; }
    else if (lead>=0xe0 && lead<=0xef) { code=lead&15; length=3; }
    else if (lead>=0xf0 && lead<=0xf4) { code=lead&7; length=4; }
    else require(lead<0x80, "Invalid UTF-8 menu/HUD text");
    require(text.size()>=length, "Truncated UTF-8 menu/HUD text");
    for (std::size_t i=1;i<length;++i) {
        const auto c=std::uint8_t(text[i]);
        require((c&0xc0)==0x80, "Invalid UTF-8 menu/HUD continuation");
        code=(code<<6)|(c&63);
    }
    require((length==1 || (length==2 && code>=0x80) || (length==3 && code>=0x800) ||
             (length==4 && code>=0x10000)) && code<=0x10ffff && !(code>=0xd800 && code<=0xdfff),
            "Invalid UTF-8 menu/HUD code point");
    text.remove_prefix(length);
    if (code<0x80) return std::uint8_t(code);
    switch (code) {
    case 0x11e: return 0xd0; case 0x130: return 0xdd; case 0x15e: return 0xde;
    case 0x11f: return 0xf0; case 0x131: return 0xfd; case 0x15f: return 0xfe;
    default: break;
    }
    if (code>=0xa0 && code<=0xff && code!=0xd0 && code!=0xdd && code!=0xde &&
        code!=0xf0 && code!=0xfd && code!=0xfe) return std::uint8_t(code);
    constexpr std::array<std::uint16_t,32> upper{{
        0x20ac,0,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,0x2c6,0x2030,0x160,0x2039,
        0x152,0,0,0,0,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,0x2dc,
        0x2122,0x161,0x203a,0x153,0,0,0x178}};
    for (std::size_t i=0;i<upper.size();++i) if (upper[i]==code) return std::uint8_t(i+128);
    throw std::runtime_error("HUD text is not representable in original WINDOWS-1254 font");
}
const BitmapFont& find_font(std::span<const BitmapFont> fonts, std::string_view name) {
    for (const auto& font:fonts) if (font.name==name) return font;
    throw std::runtime_error("Unknown original bitmap font");
}
void valid_settings(const Settings& settings) {
    require(std::isfinite(settings.brightness) && settings.brightness>=-5 && settings.brightness<=5,
            "Original brightness must be finite and in [-5,5]");
    for (float item:{settings.sensitivity,settings.musicVolume,settings.effectVolume})
        require(std::isfinite(item) && item>=0 && item<=100, "Original option must be finite and in [0,100]");
    require(valid_graphics(settings.graphics), "Invalid native graphics settings");
}
Rect normalized(Rect rect) { return {rect.x/1024,rect.y/768,rect.width/1024,rect.height/768}; }
}

bool contains(Rect rect, Vec2 point) noexcept {
    return point.x>=rect.x && point.x<=rect.x+rect.width && point.y>=rect.y && point.y<=rect.y+rect.height;
}
std::vector<BitmapFont> read_fonts(const std::filesystem::path& root) {
    const auto doc=xml(resolve_asset(root,"data/fonts/fontlist.xml"),"FONT_LIST");
    std::vector<BitmapFont> result;
    for (auto* item=xmlDocGetRootElement(doc.get())->children;item;item=item->next) {
        if (!named(item,"FONT")) continue;
        BitmapFont font;
        font.name=attribute(item,"name");
        font.texture=attribute(item,"texturefile");
        font.spacing=attr_number(item,"character_spacing");
        require(font.spacing>=0, "Negative original character spacing");
        require(std::none_of(result.begin(),result.end(),[&](const BitmapFont& existing) {
            return existing.name==font.name;
        }), "Duplicate original bitmap font");
        std::ifstream atlas(resolve_asset(root,font.texture),std::ios::binary);
        std::array<unsigned char,18> header{};
        require(bool(atlas.read(reinterpret_cast<char*>(header.data()),header.size())), "Truncated font atlas TGA header");
        font.atlasWidth=header[12]|(int(header[13])<<8);
        font.atlasHeight=header[14]|(int(header[15])<<8);
        require(font.atlasWidth>0 && font.atlasHeight>0, "Invalid original font atlas dimensions");
        const auto glyphDoc=xml(resolve_asset(root,attribute(item,"fontfile")),"FONT");
        const auto glyphRoot=xmlDocGetRootElement(glyphDoc.get());
        font.height=integer(attr_number(glyphRoot,"height"));
        require(font.height>0 && font.height<=font.atlasHeight, "Invalid original font height");
        for (auto* letter=glyphRoot->children;letter;letter=letter->next) {
            if (!named(letter,"char")) continue;
            const int code=integer(attr_number(letter,"code"));
            require(code>=-128 && code<=255, "Original glyph code outside byte range");
            const auto index=std::uint8_t(code);
            auto& glyph=font.glyphs[index];
            require(!glyph.present, "Duplicate original glyph byte");
            const int x=integer(attr_number(letter,"x")),y=integer(attr_number(letter,"y"));
            const int width=integer(attr_number(letter,"width"));
            require(x>=0 && y>=0 && width>0 && width<=font.atlasWidth-x && font.height<=font.atlasHeight-y,
                    "Original glyph outside font atlas");
            glyph={true,float(width),float(font.height),float(x)/font.atlasWidth,float(y)/font.atlasHeight,
                   float(x+width)/font.atlasWidth,float(y+font.height)/font.atlasHeight};
        }
        result.push_back(std::move(font));
    }
    require(!result.empty(), "Original font list is empty");
    return result;
}
float text_width(const BitmapFont& font, std::string_view text) {
    float result=0;
    while (!text.empty()) {
        const auto byte=next_byte(text);
        const auto& glyph=font.glyphs[byte];
        result=float(double(glyph.width)+double(font.spacing)+double(result)); // final spacing included
    }
    return result;
}
Settings read_settings(std::istream& input) {
    Settings result;
    std::string line;
    const auto field=[&](std::string_view name) {
        require(bool(std::getline(input,line)), "Truncated original game.ini");
        if (!line.empty() && line.back()=='\r') line.pop_back();
        require(line.starts_with(name), "Unexpected original game.ini field order");
        return std::string_view(line).substr(name.size());
    };
    result.brightness=number(field("parlaklik="));
    result.sensitivity=number(field("hassasiyet="));
    result.musicVolume=number(field("muzikses="));
    result.effectVolume=number(field("efektses="));
    const auto aim=field("hedefleme=");
    require(aim=="elile" || aim=="otomatik", "Invalid original aiming mode");
    result.automaticAim=aim!="elile";
    std::array<bool,8> seen{};
    while (std::getline(input,line)) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        if (line.empty()) continue;
        const auto separator=line.find('=');
        require(separator!=std::string::npos, "Malformed native graphics field");
        const std::string_view name(line.data(),separator);
        const auto field=std::find(graphicsFields.begin(),graphicsFields.end(),name);
        require(field!=graphicsFields.end(), "Unknown native graphics field");
        const auto index=std::size_t(field-graphicsFields.begin());
        require(!seen[index], "Duplicate native graphics field");
        seen[index]=true;
        const auto value=std::string_view(line).substr(separator+1);
        auto& graphics=result.graphics;
        if (index==0 || index==7) {
            require(value=="0" || value=="1", "Invalid native graphics boolean");
            (index==0?graphics.enhanced:graphics.fullscreen)=value=="1";
        } else if (index==5) {
            const auto parsed=std::from_chars(value.data(),value.data()+value.size(),graphics.samples);
            require(parsed.ec==std::errc{} && parsed.ptr==value.data()+value.size() &&
                    graphics.samples>=0, "Invalid native graphics sample count");
        } else {
            const float parsed=number(value);
            switch (index) {
            case 1: graphics.ambient_occlusion=parsed; break;
            case 2: graphics.reflections=parsed; break;
            case 3: graphics.bloom=parsed; break;
            case 4: graphics.sharpen=parsed; break;
            case 6: graphics.anisotropy=parsed; break;
            default: throw std::runtime_error("Unmapped native graphics field");
            }
        }
    }
    require(!input.bad(), "Cannot read original settings stream");
    valid_settings(result);
    return result;
}
void write_settings(std::ostream& output, const Settings& settings) {
    valid_settings(settings);
    // Preserve original six-decimal prefix, but round-trip native floats exactly.
    // Each numeric conversion has its own bounded buffer; keys never share it.
    std::array<char,64> buffer{};
    const auto append=[&](std::string_view key,float value,bool original=false) {
        output.write(key.data(),std::streamsize(key.size()));
        output.put('=');
        const auto converted=original ?
            std::to_chars(buffer.data(),buffer.data()+buffer.size(),value,std::chars_format::fixed,6) :
            std::to_chars(buffer.data(),buffer.data()+buffer.size(),value);
        require(converted.ec==std::errc{}, "Settings serialization overflow");
        output.write(buffer.data(),converted.ptr-buffer.data());
        output.put('\n');
    };
    append("parlaklik",settings.brightness,true);
    append("hassasiyet",settings.sensitivity,true);
    append("muzikses",settings.musicVolume,true);
    append("efektses",settings.effectVolume,true);
    output << (settings.automaticAim?"hedefleme=otomatik\n":"hedefleme=elile\n");
    const auto& graphics=settings.graphics;
    append(graphicsFields[0],graphics.enhanced?1:0);
    append(graphicsFields[1],graphics.ambient_occlusion);
    append(graphicsFields[2],graphics.reflections);
    append(graphicsFields[3],graphics.bloom);
    append(graphicsFields[4],graphics.sharpen);
    output << "grafik_msaa=";
    const auto samples=std::to_chars(buffer.data(),buffer.data()+buffer.size(),graphics.samples);
    require(samples.ec==std::errc{}, "Settings serialization overflow");
    output.write(buffer.data(),samples.ptr-buffer.data()); output.put('\n');
    append(graphicsFields[6],graphics.anisotropy);
    append(graphicsFields[7],graphics.fullscreen?1:0);
    require(bool(output), "Cannot write original settings stream");
}
void save_settings(const std::filesystem::path& path, const Settings& settings) {
    require(!std::filesystem::is_symlink(std::filesystem::symlink_status(path)),
            "Native settings may not be a symlink");
    auto serial=std::chrono::steady_clock::now().time_since_epoch().count();
    auto temporary=path;
    do { temporary=path; temporary+=".tmp-"+std::to_string(serial++); }
    while (std::filesystem::exists(std::filesystem::symlink_status(temporary)));
    try {
        std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
        require(bool(file), "Cannot write native settings");
        write_settings(file,settings); file.flush(); file.close();
        require(bool(file), "Cannot finish native settings");
        // SDL replaces existing destinations on Windows as well as POSIX.
        const auto source=temporary.u8string(), destination=path.u8string();
        if (!SDL_RenamePath(reinterpret_cast<const char*>(source.c_str()),
                            reinterpret_cast<const char*>(destination.c_str())))
            throw std::runtime_error(SDL_GetError());
    } catch (...) {
        std::error_code ignored; std::filesystem::remove(temporary,ignored); throw;
    }
}
std::array<std::uint8_t,4> checkpoint_counts(const std::filesystem::path& directory) {
    std::array<std::uint8_t,4> result{};
    for (std::size_t level=0;level<result.size();++level) {
        std::array<char,11> name{{'s','a','v','e',char('1'+level),'1','.','e','t','i','\0'}};
        for (std::uint8_t checkpoint=1;checkpoint<=4;++checkpoint) {
            name[5]=char('0'+checkpoint);
            std::ifstream file(directory/name.data(),std::ios::binary);
            if (!file) break; // 0041c520..0041c910 stop at the FIRST missing file.
            ++result[level];
        }
    }
    return result;
}

Menu::Menu(const std::filesystem::path& root) : fonts_(read_fonts(root)) {
    const auto doc=xml(resolve_asset(root,"data/menu/menulist.xml"),"MENU");
    const auto menuRoot=xmlDocGetRootElement(doc.get());
    const auto mouse=child(menuRoot,"MOUSE");
    cursor_={float(integer(field_number(mouse,"xinit"))),float(integer(field_number(mouse,"yinit")))};
    cursorWidth_=float(integer(field_number(mouse,"width")));
    cursorHeight_=float(integer(field_number(mouse,"height")));
    mouseMaterial_=value(mouse,"matfile");
    require(cursorWidth_>0 && cursorWidth_<=1024 && cursorHeight_>0 && cursorHeight_<=768,
            "Invalid original mouse quad dimensions");
    for (auto* item=menuRoot->children;item;item=item->next) {
        if (!named(item,"PAGE")) continue;
        Page page;
        page.id=integer(attr_number(item,"id"));
        require(page.id==int(pages_.size()), "Original menu pages must be contiguous and ordered");
        require(page.id<24, "Menu page exceeds original 24-page dispatcher");
        page.rect={attr_number(item,"xPos"),attr_number(item,"yPos"),
                   float(integer(attr_number(item,"width"))),float(integer(attr_number(item,"height")))};
        require(page.rect.width>0 && page.rect.height>0, "Invalid original menu page dimensions");
        page.material=attribute(item,"filename");
        const auto buttons=count(item,"noButton",32),labels=count(item,"noLabel",32),combos=count(item,"noCombo",32);
        if (buttons) {
            for (auto* node=child(item,"BUTTONS")->children;node;node=node->next) {
                if (!named(node,"BUTTON")) continue;
                Button button;
                button.style=style(value(node,"type")); button.rect=control_rect(node);
                button.material=value(node,"matfile"); button.overMaterial=value(node,"overmatfile");
                button.textMaterial=value(node,"textmatfile"); button.text=value(node,"label");
                button.font=value(node,"font"); button.alignment=alignment(value(node,"orientation"));
                (void)find_font(fonts_,button.font);
                page.buttons.push_back(std::move(button));
            }
        }
        require(page.buttons.size()==buttons, "Original noButton count does not match XML");
        if (combos) {
            for (auto* node=child(item,"COMBOS")->children;node;node=node->next) {
                if (!named(node,"COMBO")) continue;
                Combo combo;
                const float width=float(integer(field_number(node,"bwidth")));
                const float height=float(integer(field_number(node,"height")));
                const float y=field_number(node,"yPos");
                require(width>0 && height>0, "Invalid original combo dimensions");
                combo.previous={field_number(node,"xPos1"),y,width,height};
                combo.next={field_number(node,"xPos2"),y,width,height};
                combo.textX=field_number(node,"xText"); combo.labelWidth=float(integer(field_number(node,"lwidth")));
                combo.material=value(node,"matfile"); combo.overMaterial=value(node,"overmatfile");
                combo.textMaterial=value(node,"textmatfile"); combo.font=value(node,"font");
                combo.alignment=alignment(value(node,"orientation"));
                (void)find_font(fonts_,combo.font);
                const int labelCount=integer(field_number(node,"labelno"));
                require(labelCount>0 && labelCount<=5, "Original combo label count exceeds layout");
                for (auto* label=node->children;label;label=label->next)
                    if (named(label,"label")) combo.labels.push_back(attribute(label,"value"));
                require(combo.labels.size()==std::size_t(labelCount), "Original combo label count mismatch");
                page.combos.push_back(std::move(combo));
            }
        }
        require(page.combos.size()==combos, "Original noCombo count does not match XML");
        if (labels) {
            for (auto* node=child(item,"LABELS")->children;node;node=node->next) {
                if (!named(node,"LABEL")) continue;
                Label label;
                label.position={field_number(node,"xPos"),field_number(node,"yPos")};
                label.material=value(node,"matfile"); label.text=value(node,"label"); label.font=value(node,"font");
                (void)find_font(fonts_,label.font); page.labels.push_back(std::move(label));
            }
        }
        require(page.labels.size()==labels, "Original noLabel count does not match XML");
        pages_.push_back(std::move(page));
    }
    require(pages_.size()==24, "Original menu requires all 24 pages");
    constexpr std::array<std::size_t,24> expected{{8,22,5,16,4,2,4,1,3,3,3,2,4,18,17,1,1,1,1,4,4,4,4,4}};
    for (std::size_t i=0;i<expected.size();++i)
        require(pages_[i].buttons.size()==expected[i], "Menu controls do not match original command table");
    activeCounts_=expected;
    pages_[3].buttons[15].angle=60; // 0041cbb0/00415360: automatic aiming default
    auto& settingsPage=pages_[3];
    for (std::size_t row=0;row<graphicsLabels.size();++row) {
        const float labelY=574-float(row)*62;
        settingsPage.labels.push_back({{764,labelY},{},std::string(graphicsLabels[row]),"title"});
        for (bool increase:{false,true}) {
            Button button;
            button.rect={increase?948.0f:764.0f,labelY-40,36,36};
            button.material="menu/fill";
            button.overMaterial=increase?"menu/ayarlar_p_a":"menu/ayarlar_p_e";
            button.font="title";
            settingsPage.buttons.push_back(std::move(button));
        }
    }
    activeCounts_[3]=settingsPage.buttons.size();
    commands_.reserve(64);
    draws_.reserve(4096); // Original HUD has bounded strings; retained for every frame.
    set_cursor(cursor_);
}
void Menu::show(int page) {
    require(page>=0 && std::size_t(page)<pages_.size(), "Unknown original menu page");
    current_=page;
}
void Menu::set_cursor(Vec2 point) {
    require(std::isfinite(point.x) && std::isfinite(point.y), "Nonfinite menu cursor");
    cursor_.y=std::clamp(float(integer(point.y)),0.0f,768-cursorHeight_);
    cursor_.x=std::max(float(integer(point.x)),0.0f);
    // 0041bd80/0041be00 assign 1024, not 1024-width. Keep this original edge case.
    if (cursor_.x>1024-cursorWidth_) cursor_.x=1024;
}
void Menu::set_button_state(std::size_t button, bool over, bool activated) {
    require(button<pages_[std::size_t(current_)].buttons.size(), "Original menu button index out of bounds");
    auto& state=pages_[std::size_t(current_)].buttons[button];
    state.over=over; state.activated=activated;
}
void Menu::show_notification(std::size_t button) {
    require(button==2 || button==4, "Original timed HUD notification uses button 2 or 4");
    pages_[0].buttons[button].over=true; saveFrames_=45;
}
void Menu::set_game_started(bool value) noexcept { gameStarted_=value; }
void Menu::emit(CommandKind kind, int level, bool first, bool second, std::string_view name) {
    commands_.push_back({kind,level,first,second,name,settings_effect()});
}
void Menu::video_reset(int page, std::size_t button) {
    emit(CommandKind::ResetVideo,0,false,false,pages_[std::size_t(page)].buttons[button].overMaterial);
}
Settings Menu::settings() const noexcept {
    const auto& buttons=pages_[3].buttons;
    return {float(double(buttons[3].angle)*inverseScale*-5),
            float(50-double(buttons[6].angle)*50*inverseScale),
            float(50-double(buttons[9].angle)*50*inverseScale),
            float(50-double(buttons[12].angle)*50*inverseScale),buttons[15].angle!=-60,graphics_};
}
SettingsEffect Menu::settings_effect() const noexcept {
    const auto& buttons=pages_[3].buttons;
    return {settings(),std::uint16_t((double(angleLimit)-buttons[3].angle)*rampScale),
            float((double(angleLimit)-buttons[6].angle)*gainScale),
            float((double(angleLimit)-buttons[9].angle)*gainScale),
            float((double(angleLimit)-buttons[12].angle)*gainScale)};
}
void Menu::set_graphics_limits(int samples, float anisotropy) {
    require(samples>=0 && std::isfinite(anisotropy) && anisotropy>=1, "Invalid graphics hardware limits");
    maxSamples_=samples; maxAnisotropy_=anisotropy;
}
void Menu::set_fullscreen(bool fullscreen) {
    if (graphics_.fullscreen==fullscreen) return;
    graphics_.fullscreen=fullscreen;
    originalSettingsDirty_=false;
    emit(CommandKind::PersistSettings);
}
void Menu::set_settings(const Settings& settings) {
    valid_settings(settings);
    graphics_=settings.graphics;
    originalSettingsDirty_=false;
    auto& buttons=pages_[3].buttons;
    buttons[3].angle=float(double(settings.brightness)*0.2f*-110);
    buttons[6].angle=float((double(settings.sensitivity)-50)*angleLimit*inputScale);
    buttons[9].angle=float((double(settings.musicVolume)-50)*angleLimit*inputScale);
    buttons[12].angle=float((double(settings.effectVolume)-50)*angleLimit*inputScale);
    buttons[15].angle=settings.automaticAim?60:-60;
    emit(CommandKind::SetAimingMode,0,settings.automaticAim);
    emit(CommandKind::ApplySettings);
    emit(CommandKind::SetMusicVolume,0,false,false,"menumusic");
}
void Menu::update(const Input& input, const Context& context) {
    const Vec2 hot{cursor_.x,cursor_.y+cursorHeight_};
    auto& page=pages_[std::size_t(current_)];
    const bool released=previousLeft_ && !input.leftDown;
    // 0041c130: discrete COMBOS change on release only, no wraparound.
    for (auto& combo:page.combos) {
        combo.overPrevious=contains(combo.previous,hot); combo.overNext=contains(combo.next,hot);
        if (released && combo.overPrevious && combo.selected) --combo.selected;
        if (released && combo.overNext && combo.selected+1<combo.labels.size()) ++combo.selected;
    }
    for (std::size_t i=0;i<activeCounts_[std::size_t(current_)];++i) {
        auto& button=page.buttons[i];
        const bool hit=contains(button.rect,hot);
        button.activated=hit && (button.style==Style::Repeat && input.leftDown ? true : released);
        // 0041c2f0's exclusion is a contradictory AND of "noHitCheck", "AVI",
        // and "ibre", so all types are tested. Only HUD noHitCheck over-state
        // is preserved (its comparison accidentally uses PAGE 0's type array).
        if (current_!=0 || pages_[0].buttons[i].style!=Style::NoHitCheck) button.over=hit;
    }
    dispatch(context,released);
    previousLeft_=input.leftDown;
    // Original menu command processing precedes cursor motion in the render stage.
    if (current_!=0 && current_!=7) set_cursor({cursor_.x+input.motion.x,cursor_.y+input.motion.y});
}
void Menu::dispatch(const Context& context, bool released) {
    auto& buttons=pages_[std::size_t(current_)].buttons;
    const auto active=[&](std::size_t i) {
        return i<activeCounts_[std::size_t(current_)] && buttons[i].activated;
    };
    const auto any=[&](std::size_t first,std::size_t last) {
        for (std::size_t i=first;i<last;++i) if (active(i)) return true;
        return false;
    };
    const auto resume=[&](bool first,bool second) { emit(CommandKind::ResumeGame,0,first,second); };
    switch (current_) { // Complete 0041e1a0 switch, including robot pages 19..23.
    case 0:
        if (active(6)) {
            buttons[5].over=buttons[6].over=buttons[7].over=false;
            emit(CommandKind::RestartCheckpoint,context.level); resume(false,false);
        } else if (active(7)) {
            if (context.level>=1 && context.level<=4) {
                constexpr std::array<std::string_view,4> music{{"level1","level2","level3","level4"}};
                emit(CommandKind::StopMusic,context.level,false,false,music[std::size_t(context.level-1)]);
            }
            emit(CommandKind::PlayMusic,0,false,false,"menumusic");
            buttons[5].over=buttons[6].over=buttons[7].over=false;
            resume(false,false); emit(CommandKind::RestoreCamera);
            current_=1; gameStarted_=false; emit(CommandKind::ShowMainMenu,1,true,false);
        }
        break;
    case 1:
        for (std::size_t i=0;i<18;i+=3) {
            if (!buttons[i].over) {
                if (i==0 && gameStarted_) buttons[21].over=false;
                buttons[i+1].over=buttons[i+2].over=false;
                video_reset(1,i+2); // Original resets every nonhover frame, not just exit.
            } else if (i==0 && gameStarted_ && context.playerExists && !context.playerDead) {
                buttons[1].over=buttons[2].over=false;
            } else buttons[i+1].over=buttons[i+2].over=true;
        }
        buttons[20].over=buttons[19].over;
        if (!buttons[19].over) video_reset(1,20);
        if (active(19)) { current_=18; break; }
        if (active(0)) {
            if (context.playerExists && context.playerDead) {
                emit(CommandKind::ResetLevel,0,true);
                emit(CommandKind::InitializeLevel,1,true,true); gameStarted_=true;
            } else if (context.previousLevel!=-1) resume(true,false);
            else {
                current_=7; emit(CommandKind::InitializeLevel,1,true,true);
                gameStarted_=true; resume(true,true);
            }
        } else if (active(3)) current_=2;
        else if (active(6)) current_=3;
        else if (active(9)) { current_=4; video_reset(4,3); }
        else if (active(12)) { current_=5; video_reset(5,1); }
        else if (active(15)) current_=6;
        break;
    case 2:
        if (active(0)) current_=1;
        else if (active(1)) current_=8;
        else if (active(2)) current_=9;
        else if (active(3)) current_=10;
        else if (active(4)) current_=11;
        break;
    case 3: {
        if (active(0)) {
            emit(CommandKind::PersistSettings); originalSettingsDirty_=false; current_=1; break;
        }
        bool changed=false;
        for (std::size_t i=0;i<4;++i) {
            auto& angle=buttons[3+i*3].angle;
            const float before=angle;
            if (active(1+i*3) && -angleLimit<angle-angleStep) angle-=angleStep;
            if (active(2+i*3) && angle+angleStep<angleLimit) angle+=angleStep;
            changed|=angle!=before;
        }
        const float previousAim=buttons[15].angle;
        if (active(13)) buttons[15].angle=60;
        if (active(14)) buttons[15].angle=-60;
        if (buttons[15].angle!=previousAim) {
            emit(CommandKind::SetAimingMode,0,buttons[15].angle==60);
            changed=true;
        }
        originalSettingsDirty_|=changed;
        const auto previousGraphics=graphics_;
        for (std::size_t row=0;row<graphicsLabels.size();++row) {
            const bool decrease=active(16+row*2),increase=active(17+row*2);
            if (!increase && !decrease) continue;
            if (row==0) graphics_.enhanced=increase;
            else if (row==7) graphics_.fullscreen=increase;
            else if (row==5)
                graphics_.samples=quality_step(graphics_.samples,maxSamples_,sampleChoices,increase);
            else if (row==6)
                graphics_.anisotropy=quality_step(graphics_.anisotropy,maxAnisotropy_,filterChoices,increase);
            else {
                float* value=row==1?&graphics_.ambient_occlusion:row==2?&graphics_.reflections:
                             row==3?&graphics_.bloom:&graphics_.sharpen;
                *value=std::clamp(*value+(increase?.05f:-.05f),0.0f,1.0f);
            }
        }
        const bool graphicsChanged=graphics_!=previousGraphics;
        if (changed || graphicsChanged) emit(CommandKind::ApplySettings);
        if (graphicsChanged || (released && originalSettingsDirty_)) {
            emit(CommandKind::PersistSettings); originalSettingsDirty_=false;
        }
        // Original applies music/effect gains and volumes each options frame.
        if (context.previousLevel==-1) emit(CommandKind::SetMusicVolume,0,false,false,"menumusic");
        for (const auto name:{"level1","level2","level3","level4"})
            emit(CommandKind::SetMusicVolume,0,false,false,name);
        break;
    }
    case 4: case 19: case 20: case 21: case 22: case 23: {
        constexpr std::array<int,6> robots{{4,19,20,21,22,23}};
        const auto position=std::find(robots.begin(),robots.end(),current_)-robots.begin();
        if (active(0) && position>0) { current_=robots[std::size_t(position-1)]; video_reset(current_,3); }
        else if (active(1)) {
            if (position<5) { current_=robots[std::size_t(position+1)]; video_reset(current_,3); }
            // Last-page next is deliberately inert in original case 23.
        } else if (active(2)) current_=1;
        break;
    }
    case 5:
        if (active(0)) current_=1;
        break;
    case 6: case 18:
        if (current_==18 && active(0)) { current_=1; break; }
        // Case 18 deliberately falls through the exit-page command logic.
        // Unused original static array entries are zero; no invented exit button.
        if (current_==6) {
            buttons[1].over=buttons[0].over; buttons[3].over=buttons[2].over;
            if (active(0)) current_=1;
            else if (active(2)) { emit(CommandKind::PersistSettings); emit(CommandKind::Quit); }
        }
        break;
    case 7: break; // Loading page has no command handler in original switch.
    case 8: case 9: case 10: case 11:
        if (active(0)) current_=2;
        else if (active(1) || (current_!=11 && active(2))) {
            constexpr std::array<std::array<std::string_view,2>,4> saves{{
                {{"data/save/save11.eti","data/save/save12.eti"}},
                {{"data/save/save21.eti","data/save/save22.eti"}},
                {{"data/save/save31.eti","data/save/save32.eti"}},
                {{"data/save/save41.eti",""}}}};
            const int level=current_-7;
            emit(CommandKind::LoadCheckpoint,level,false,false,saves[std::size_t(level-1)][active(1)?0:1]);
            resume(true,false);
        }
        break;
    case 12:
        if (active(0) || active(1)) current_=15;
        else if (active(2)) current_=13;
        else if (active(3)) current_=15;
        break;
    case 13:
        if (any(0,5)) current_=16;
        else if (active(5)) current_=14;
        else if (any(6,18)) current_=16;
        break;
    case 14:
        if (any(0,12)) current_=17;
        else if (active(12)) { resume(true,false); emit(CommandKind::RunScript,0,true,false,"librarygame_on_end"); }
        else if (any(13,17)) current_=17;
        break;
    case 15: if (active(0)) current_=12; break;
    case 16: if (active(0)) current_=13; break;
    case 17: if (active(0)) current_=14; break;
    default: throw std::runtime_error("Unmapped original menu page");
    }
}

void Menu::quad(Rect rect, std::string_view material, Style style, int button,
                bool over, float left, float right, float inset) {
    Draw draw;
    draw.rect=normalized(rect); draw.material=material; draw.style=style;
    draw.page=current_; draw.button=button; draw.hovered=over;
    const float x0=float(double(rect.width)*left+rect.x);
    const float x1=float(double(rect.width)*right+rect.x);
    const float top=float(double(rect.y)+rect.height);
    draw.positions={{{x0/1024,top/768},{x1/1024,top/768},{x1/1024,rect.y/768},{x0/1024,rect.y/768}}};
    draw.uv={{{left,1-inset},{right,1-inset},{right,inset},{left,inset}}};
    draws_.push_back(draw);
}
void append_text(std::vector<Draw>& draws, const BitmapFont& font, std::string_view value,
                 Vec2 origin, float scale, std::array<float,4> color) {
    require(std::isfinite(scale) && scale>0, "Invalid original bitmap text scale");
    int x=integer(origin.x),y=integer(origin.y);
    auto remaining=value;
    while (!remaining.empty()) {
        const auto& glyph=font.glyphs[next_byte(remaining)];
        Draw draw;
        // 0042f860 uses height-1, and truncates X after EACH glyph advance.
        const float right=float(double(glyph.width)*scale+x);
        const float top=float((double(glyph.height)-1)*scale+y);
        draw.rect=normalized({float(x),float(y),right-x,top-y});
        draw.positions={{{float(x)/1024,top/768},{right/1024,top/768},
                         {right/1024,float(y)/768},{float(x)/1024,float(y)/768}}};
        draw.uv={{{glyph.u0,glyph.v0},{glyph.u1,glyph.v0},{glyph.u1,glyph.v1},{glyph.u0,glyph.v1}}};
        draw.color=color; draw.texture=font.texture;
        draw.text=value; draw.font=font.name; draw.glyph=true;
        draws.push_back(draw);
        x=integer((double(glyph.width)+font.spacing)*scale+x);
    }
}
void Menu::text(std::string_view value, std::string_view name, std::string_view material,
                int x, int y, float scale, std::array<float,4> color, int button) {
    if (value.empty()) return;
    const auto begin=draws_.size();
    append_text(draws_,find_font(fonts_,name),value,{float(x),float(y)},scale,color);
    for (std::size_t i=begin;i<draws_.size();++i) {
        draws_[i].material=material; draws_[i].page=current_; draws_[i].button=button;
    }
}
std::span<const Draw> Menu::draw(const Context& context, const Hud& hud, float logicalWidth) {
    draws_.clear();
    const auto& page=pages_[std::size_t(current_)];
    float extra=0;
    if (current_==0) {
        require(std::isfinite(logicalWidth) && logicalWidth>0, "Invalid HUD logical width");
        extra=std::max(1024.0f,logicalWidth)-1024;
    }
    if (extra==0) quad(page.rect,page.material,Style::Normal,-1,false);
    else {
        // arayuz_bg bakes energy at the left, PUAN in the centre, and the
        // battery at the right. Stretch only its empty translucent columns.
        const auto strip=[&](float left,float right,float x,float width) {
            quad({x,page.rect.y,width,page.rect.height},page.material,Style::Normal,-1,false);
            auto& draw=draws_.back();
            draw.uv={{{left/1024,1},{right/1024,1},{right/1024,0},{left/1024,0}}};
        };
        strip(0,288,0,288);
        strip(320,400,288,extra/2);
        strip(288,800,288+extra/2,512);
        strip(320,400,800+extra/2,extra/2);
        strip(800,1024,800+extra,224);
    }
    if (current_==3) {
        // Reuse the original unprinted parchment, leaving the chrome and knobs untouched.
        quad({752,94,264,522},{},Style::Normal,-1,false);
        auto& paper=draws_.back();
        paper.texture="data/images/menuimages/ayarlar_bg.jpg";
        paper.uv={{{824.0f/1024,1-275.0f/1024},{912.0f/1024,1-275.0f/1024},
                   {912.0f/1024,1-325.0f/1024},{824.0f/1024,1-325.0f/1024}}};
    }
    const auto controlsBegin=draws_.size();
    if (current_>=8 && current_<=11) {
        const auto saves=context.saveCounts[std::size_t(current_-8)];
        require(saves<=4, "Save count exceeds original four-file scan");
        // 00420430 overwrites noButton with 1+contiguous save count. Native code
        // bounds it by actual XML controls instead of reading uninitialized slots.
        activeCounts_[std::size_t(current_)]=std::min(page.buttons.size(),std::size_t(saves)+1);
    }
    const std::size_t count=activeCounts_[std::size_t(current_)]-(current_==1 && !gameStarted_ ? 1 : 0);
    for (std::size_t i=0;i<count;++i) {
        const auto& button=page.buttons[i];
        float left=0.01f,right=0.99f;
        if (current_==0 && i==0) right=context.health;
        if (current_==0 && i==1) left=1-context.energy;
        if (current_==7 && i==0) { left=0; right=context.loadingProgress; }
        quad(button.rect,button.over?button.overMaterial:button.material,button.style,int(i),
             button.over,left,right,current_==6?0.0015f:0);
        if (current_==3 && i>=16 && !button.over) {
            auto& native=draws_.back();
            native.material={};
            native.texture=(i-16)%2==0?"data/images/menuimages/dugme/ayarlar_eksi.tga":
                                       "data/images/menuimages/dugme/ayarlar_arti.tga";
        }
        if (button.style==Style::Needle && button.angle!=0) {
            auto& quad=draws_.back();
            // 0041f612..0041f6b1: pivot=(x+floor(width/2),y+14), not quad centre.
            const Vec2 pivot{button.rect.x+std::floor(button.rect.width/2),button.rect.y+14};
            constexpr float radians=0.017453292519943295f;
            const float cosine=std::cos(button.angle*radians),sine=std::sin(button.angle*radians);
            for (auto& point:quad.positions) {
                const float dx=point.x*1024-pivot.x,dy=point.y*768-pivot.y;
                point={(pivot.x+dx*cosine-dy*sine)/1024,(pivot.y+dx*sine+dy*cosine)/768};
            }
        }
    }
    for (std::size_t i=0;i<page.buttons.size();++i) {
        const auto& button=page.buttons[i];
        if (button.text.empty()) continue;
        const auto& font=find_font(fonts_,button.font);
        const float width=text_width(font,button.text);
        float x=button.rect.x+5;
        if (button.alignment==Alignment::Center) x=button.rect.x+std::floor(button.rect.width/2)-width*0.5f;
        if (button.alignment==Alignment::Right) x=button.rect.x+button.rect.width-width-5;
        const int y=integer(button.rect.y+std::floor(button.rect.height/2)-float(font.height/2));
        text(button.text,button.font,button.textMaterial,integer(x),y,1,white,int(i));
    }
    for (const auto& combo:page.combos) {
        quad(combo.previous,combo.overPrevious?combo.overMaterial:combo.material,Style::Normal,-1,combo.overPrevious);
        quad(combo.next,combo.overNext?combo.overMaterial:combo.material,Style::Normal,-1,combo.overNext);
        // 0041f820 flips the next arrow horizontally.
        auto& next=draws_.back();
        next.uv={{{1,1},{0,1},{0,0},{1,0}}};
    }
    for (const auto& combo:page.combos) {
        const auto& font=find_font(fonts_,combo.font);
        const auto& selected=combo.labels[combo.selected];
        const float width=text_width(font,selected);
        float x=combo.textX+5;
        if (combo.alignment==Alignment::Center) x=combo.textX+combo.labelWidth*0.5f-width*0.5f;
        if (combo.alignment==Alignment::Right) x=combo.textX+combo.labelWidth-width-5;
        const int y=integer(combo.previous.y+std::floor(combo.previous.height/2)-float(font.height/2));
        text(selected,combo.font,combo.textMaterial,integer(x),y,1,white);
    }
    for (const auto& label:page.labels)
        text(label.text,label.font,label.material,integer(label.position.x),integer(label.position.y),1,
             current_==3?parchmentInk:white);
    if (current_==3) {
        // Labels and values use the retained original bitmap font, not host UI text.
        for (std::size_t row=0;row<graphicsLabels.size();++row) {
            const auto& label=page.labels[row];
            std::string_view value;
            if (row==0) value=graphics_.enhanced?"Geliştirilmiş":"Klasik";
            else if (row==7) value=graphics_.fullscreen?"Açık":"Kapalı";
            else {
                auto& buffer=graphicsText_[row];
                char* end=buffer.data()+buffer.size();
                std::to_chars_result converted;
                if (row==5) converted=std::to_chars(buffer.data(),end,graphics_.samples);
                else if (row==6) converted=std::to_chars(buffer.data(),end,graphics_.anisotropy);
                else {
                    const float strength=row==1?graphics_.ambient_occlusion:row==2?graphics_.reflections:
                                         row==3?graphics_.bloom:graphics_.sharpen;
                    converted=std::to_chars(buffer.data(),end,int(std::lround(strength*100)));
                }
                require(converted.ec==std::errc{} && converted.ptr<end, "Graphics value text overflow");
                char* finish=converted.ptr;
                if (row>=1 && row<=4) *finish++='%';
                else if ((row==5 && graphics_.samples!=0) || row==6) *finish++='x';
                value={buffer.data(),std::size_t(finish-buffer.data())};
                if (row==5 && graphics_.samples==0) value="Kapalı";
            }
            const float scale=.85f;
            const float width=text_width(find_font(fonts_,"title"),value)*scale;
            text(value,"title",{},integer(874-width*.5f),integer(label.position.y-31),scale,parchmentInk);
        }
    }
    if (current_!=0 && current_!=7) quad({cursor_.x,cursor_.y,cursorWidth_,cursorHeight_},mouseMaterial_,Style::Normal,-1,false);
    if (current_==0) {
        const auto score=std::to_chars(scoreText_.data(),scoreText_.data()+scoreText_.size(),hud.score);
        require(score.ec==std::errc{}, "HUD score text overflow");
        text({scoreText_.data(),std::size_t(score.ptr-scoreText_.data())},"title",{},590,13,1.5f,hudColor);
        if (hud.highJumps!=0 && context.level!=4) {
            constexpr std::string_view prefix="Yüksek Zıplama: ";
            std::copy(prefix.begin(),prefix.end(),jumpText_.begin());
            const auto jumps=std::to_chars(jumpText_.data()+prefix.size(),jumpText_.data()+jumpText_.size(),hud.highJumps);
            require(jumps.ec==std::errc{}, "HUD jump text overflow");
            text({jumpText_.data(),std::size_t(jumps.ptr-jumpText_.data())},"title",{},800,709,0.8f,hudColor);
        }
        if (hud.objectivesVisible) {
            require(hud.objectives.size()<=4, "Objective count exceeds original four-entry HUD layout");
            int y=709;
            for (const auto& objective:hud.objectives) {
                text(objective.text,"title",{},5,y,0.75f,objective.failed?std::array<float,4>{{1,0,0,1}}:hudColor);
                y-=15;
            }
        }
    }
    if (extra>0) {
        for (std::size_t i=controlsBegin;i<draws_.size();++i) {
            auto& draw=draws_[i];
            float offset=0;
            if (draw.button==1 || (draw.glyph && draw.text.data()==jumpText_.data())) offset=extra;
            else if (draw.button==2 || draw.button>=4 ||
                     (draw.glyph && draw.text.data()==scoreText_.data())) offset=extra/2;
            draw.rect.x+=offset/1024;
            for (auto& point:draw.positions) point.x+=offset/1024;
        }
    }
    // 00420430 clears timed notification over-states after drawing, not before.
    if (saveFrames_) --saveFrames_;
    else {
        if (page.buttons.size()>2) pages_[std::size_t(current_)].buttons[2].over=false;
        if (page.buttons.size()>4) pages_[std::size_t(current_)].buttons[4].over=false;
    }
    return draws_;
}
} // namespace yami::menu
