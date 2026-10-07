#include "setup_artwork.hpp"

#include "media.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <libxml/parser.h>
#include <libxml/tree.h>

namespace yami::setup {
namespace {
constexpr std::array<std::string_view,9> artworkFiles{{
    "data/menu/menulist.xml",
    "data/fonts/fontlist.xml", "data/fonts/title.xml",
    "data/images/fonts/title.tga",
    "data/images/menuimages/anamenu.jpg",
    "data/images/menuimages/ayarlar_bg.jpg",
    "data/images/menuimages/robotlar6_bg.jpg",
    "data/materials/menu/ayarlar_bg.dat",
    "data/materials/menu/robotlar6_bg.dat"
}};
constexpr SDL_Color ink{41,31,20,255}, disabledInk{88,75,55,255};
constexpr SDL_Color gold{242,212,46,255}, olive{83,88,39,255};
constexpr SDL_FRect paperSource{824,275,88,50}; // Same untouched paper as the launcher.
constexpr SDL_FRect buttonSource{103,525,150,72}; // Original SES button's border.
constexpr std::size_t maxTextBytes=4096;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void sdl_check(bool success) {
    if (!success) throw std::runtime_error(SDL_GetError());
}
void bounded_file(const std::filesystem::path& path, std::uintmax_t maximum) {
    const auto size=std::filesystem::file_size(path);
    if (!size || size>maximum)
        throw std::runtime_error("Empty or oversized original artwork: "+path.string());
}
struct ImageInfo { int width{},height{}; bool flip{}; };
// Check the actual shipped still-image headers before VideoDecoder allocates a
// decoded image. Restrict formats to JPEG and uncompressed true-color TGA.
ImageInfo image_info(const std::filesystem::path& path, bool tga) {
    bounded_file(path,8*1024*1024);
    std::ifstream input(path,std::ios::binary);
    require(bool(input),"Cannot open original artwork image");
    ImageInfo result;
    if (tga) {
        std::array<unsigned char,18> header{};
        require(bool(input.read(reinterpret_cast<char*>(header.data()),header.size())),
                "Truncated original font atlas header");
        require(header[1]==0 && header[2]==2 && (header[16]==24 || header[16]==32) &&
                (header[17]&0xc0)==0 && (header[17]&0x10)==0,
                "Original font atlas must be a non-interleaved true-color TGA");
        result.width=header[12]|(int(header[13])<<8);
        result.height=header[14]|(int(header[15])<<8);
        // Original scene loader ignores TGA origin when applying font XML UVs.
        result.flip=(header[17]&0x20)==0;
        const auto expected=18u+header[0]+std::uintmax_t(result.width)*result.height*(header[16]/8);
        require(std::filesystem::file_size(path)>=expected,"Truncated original font atlas pixels");
    } else {
        const auto byte=[&]() {
            const int value=input.get();
            require(value!=std::char_traits<char>::eof(),"Truncated original JPEG header");
            return value;
        };
        require(byte()==0xff && byte()==0xd8,"Original menu artwork is not a JPEG");
        bool dimensions=false;
        for (;;) {
            require(byte()==0xff,"Invalid original JPEG marker");
            int marker=byte();
            while (marker==0xff) marker=byte();
            require(marker!=0 && marker!=0xd8 && marker!=0xd9 && marker!=0x01 &&
                    !(marker>=0xd0 && marker<=0xd7),"Invalid original JPEG header marker");
            const int high=byte(),low=byte();
            int remaining=(high<<8|low)-2;
            require(remaining>=0,"Invalid original JPEG segment length");
            if ((marker>=0xc0 && marker<=0xcf) && marker!=0xc4 && marker!=0xc8 && marker!=0xcc) {
                require(!dimensions && (marker==0xc0 || marker==0xc1 || marker==0xc2) && remaining>=6,
                        "Unsupported or duplicate original JPEG frame header");
                require(byte()==8,"Original JPEG artwork must use 8-bit components");
                const int yHigh=byte(),yLow=byte(),xHigh=byte(),xLow=byte(),components=byte();
                require((components==1 || components==3) && remaining==6+3*components,
                        "Invalid original JPEG frame components");
                result.height=(yHigh<<8)|yLow; result.width=(xHigh<<8)|xLow;
                dimensions=true; remaining-=6;
            }
            if (marker==0xda) {
                require(dimensions && remaining>=4,"Original JPEG has no valid image dimensions");
                break;
            }
            input.ignore(remaining);
            require(bool(input),"Truncated original JPEG header segment");
        }
    }
    require(result.width>0 && result.height>0 && result.width<=4096 && result.height<=4096 &&
            std::uint64_t(result.width)*result.height<=16*1024*1024,
            "Original artwork image exceeds the 4096-pixel / 64-MiB safety limit");
    return result;
}
// The shared font reader parses glyphs; this small manifest gate ensures it can
// only open the bounded files in files(), not arbitrary ISO-controlled paths.
void font_manifest(const std::filesystem::path& path) {
    using Document=std::unique_ptr<xmlDoc,decltype(&xmlFreeDoc)>;
    Document document(xmlReadFile(path.string().c_str(),"WINDOWS-1254",XML_PARSE_NONET),xmlFreeDoc);
    require(bool(document) && !document->intSubset && !document->extSubset,
            "Invalid original font list or forbidden DTD");
    auto* root=xmlDocGetRootElement(document.get());
    require(root && xmlStrEqual(root->name,BAD_CAST "FONT_LIST"),"Invalid original font list root");
    int count=0;
    for (auto* node=root->children;node;node=node->next) {
        if (node->type!=XML_ELEMENT_NODE) continue;
        require(xmlStrEqual(node->name,BAD_CAST "FONT") && ++count==1,
                "Installer artwork requires exactly one original title font");
        for (const auto& [name,value]:std::array<std::pair<const char*,const char*>,3>{{
            {"name","title"},{"fontfile","data/fonts/title.xml"},
            {"texturefile","data/images/fonts/title.tga"}}}) {
            auto* attribute=xmlGetProp(node,BAD_CAST name);
            const bool valid=attribute && xmlStrEqual(attribute,BAD_CAST value);
            xmlFree(attribute);
            require(valid,"Original font list references an unexpected artwork file");
        }
    }
    require(count==1,"Original title font is missing");
}
void valid_text(std::string_view label, float height) {
    require(label.size()<=maxTextBytes,"Installer artwork label exceeds 4096 UTF-8 bytes");
    require(std::isfinite(height) && height>0 && height<=768,"Invalid installer bitmap font height");
}
} // namespace

std::span<const std::string_view> Artwork::files() noexcept { return artworkFiles; }

Artwork::Artwork(SDL_Renderer* renderer, const std::filesystem::path& game) : renderer_(renderer) {
    require(renderer_!=nullptr,"Original installer artwork requires an SDL renderer");
    try {
        std::array<std::filesystem::path,artworkFiles.size()> paths;
        AssetDirectories directories;
        for (std::size_t i=0;i<paths.size();++i) {
            paths[i]=resolve_asset(game,std::string(artworkFiles[i]),&directories);
            bounded_file(paths[i],i>=3 && i<=6?8*1024*1024:64*1024);
        }
        for (const auto& [material,image]:std::array<std::pair<std::size_t,std::size_t>,2>{{{7,5},{8,6}}}) {
            const auto data=read_material(paths[material]);
            require(data.passes.size()==1 && !data.passes[0].video &&
                    data.passes[0].texture_name==artworkFiles[image],
                    "Original menu material references an unexpected installer image");
        }
        font_manifest(paths[1]);
        constexpr std::array<std::size_t,4> images{{4,5,6,3}};
        std::array<ImageInfo,4> info;
        for (std::size_t i=0;i<images.size();++i) {
            info[i]=image_info(paths[images[i]],i==3);
            require(info[i].width==(i==3?256:1024) && info[i].height==(i==3?128:1024),
                    "Original installer artwork has unexpected atlas dimensions");
        }
        auto fonts=menu::read_fonts(game);
        require(fonts.size()==1 && fonts[0].name=="title" && fonts[0].height==22 &&
                fonts[0].atlasWidth==info[3].width && fonts[0].atlasHeight==info[3].height &&
                std::isfinite(fonts[0].spacing) && fonts[0].spacing<=22,
                "Invalid original installer title font metrics");
        font_=std::move(fonts.front());
        require(font_.glyphs['A'].present && font_.glyphs['?'].present && font_.glyphs[' '].present,
                "Original installer title font is missing required glyphs");
        for (std::size_t i=0;i<images.size();++i) {
            VideoDecoder decoder(paths[images[i]]);
            VideoFrame frame;
            require(decoder.width()==info[i].width && decoder.height()==info[i].height &&
                    decoder.next(frame) && frame.width==info[i].width && frame.height==info[i].height &&
                    frame.stride==frame.width*4 &&
                    frame.rgba.size()==std::size_t(frame.width)*frame.height*4,
                    "Original artwork decoded dimensions or RGBA bytes do not match its header");
            textures_[i].reset(SDL_CreateTexture(renderer_,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,
                                                  frame.width,frame.height));
            require(bool(textures_[i]),SDL_GetError());
            sdl_check(SDL_SetTextureBlendMode(textures_[i].get(),SDL_BLENDMODE_BLEND));
            sdl_check(SDL_SetTextureScaleMode(textures_[i].get(),SDL_SCALEMODE_LINEAR));
            if (info[i].flip) {
                for (int y=0;y<frame.height;++y) {
                    const SDL_Rect row{0,y,frame.width,1};
                    sdl_check(SDL_UpdateTexture(textures_[i].get(),&row,
                        frame.rgba.data()+std::size_t(frame.height-1-y)*frame.stride,frame.stride));
                }
            } else sdl_check(SDL_UpdateTexture(textures_[i].get(),nullptr,frame.rgba.data(),frame.stride));
        }
        glyphs_.reserve(maxTextBytes); // Reused across every label/frame, including UTF-8 byte worst case.
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("Cannot load original ETI Yami installer artwork: ")+error.what()+
                                 ". Select an intact original YAMI ISO.");
    }
}

void Artwork::crop(std::size_t texture, SDL_FRect source, SDL_FRect destination, SDL_Color color) {
    auto* image=textures_[texture].get();
    sdl_check(SDL_SetTextureColorMod(image,color.r,color.g,color.b));
    sdl_check(SDL_SetTextureAlphaMod(image,color.a));
    sdl_check(SDL_RenderTexture(renderer_,image,&source,&destination));
}

void Artwork::background() {
    crop(0,{0,0,1024,1024},{0,0,1024,768});
    // Keep the actual menu's ETI chocolate wordmark and the illustrated computer
    // robot in its original metal pod, rather than rendering a 3D UV atlas flat.
    crop(2,{0,0,554,940},{0,176,344,576});
    crop(0,{510,20,500,260},{22,14,296,154});
    crop(1,paperSource,{350,24,646,716});
}

float Artwork::width(std::string_view label, float pixelHeight) const {
    valid_text(label,pixelHeight);
    return menu::text_width(font_,label)*pixelHeight/(font_.height-1);
}

void Artwork::text(float x, float y, std::string_view label, SDL_Color color,
                   float pixelHeight, float maxWidth) {
    valid_text(label,pixelHeight);
    require(std::isfinite(x) && std::isfinite(y) && x>=-4096 && x<=4096 && y>=-4096 && y<=4096 &&
            std::isfinite(maxWidth) && maxWidth>=0,"Invalid installer text coordinates or width");
    if (label.empty()) return;
    float scale=pixelHeight/(font_.height-1);
    if (maxWidth>0) {
        const float natural=menu::text_width(font_,label)*scale;
        if (natural>maxWidth) scale*=maxWidth/natural;
    }
    glyphs_.clear();
    menu::append_text(glyphs_,font_,label,{x,768-y-(font_.height-1)*scale},scale,
                      {color.r/255.f,color.g/255.f,color.b/255.f,color.a/255.f});
    auto* atlas=textures_[3].get();
    sdl_check(SDL_SetTextureColorMod(atlas,color.r,color.g,color.b));
    sdl_check(SDL_SetTextureAlphaMod(atlas,color.a));
    for (const auto& glyph:glyphs_) {
        const SDL_FRect source{glyph.uv[0].x*font_.atlasWidth,glyph.uv[0].y*font_.atlasHeight,
            (glyph.uv[1].x-glyph.uv[0].x)*font_.atlasWidth,
            (glyph.uv[2].y-glyph.uv[0].y)*font_.atlasHeight};
        const SDL_FRect destination{glyph.rect.x*1024,(1-glyph.rect.y-glyph.rect.height)*768,
                                    glyph.rect.width*1024,glyph.rect.height*768};
        sdl_check(SDL_RenderTexture(renderer_,atlas,&source,&destination));
    }
}

void Artwork::button(SDL_FRect rect, std::string_view label, bool enabled, bool focused, bool primary) {
    require(std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.w) &&
            std::isfinite(rect.h) && rect.w>=48 && rect.h>=40 && rect.w<=1024 && rect.h<=768,
            "Invalid original installer button rectangle");
    const SDL_Color tint=enabled?SDL_Color{255,255,255,255}:SDL_Color{216,209,191,255};
    // Eight-slice only the real label-free frame; the authentic paper center
    // replaces the baked SES text and accepts the installer's bitmap labels.
    constexpr std::array<float,4> sx{{0,14,136,150}},sy{{0,12,60,72}};
    const std::array<float,4> dx{{rect.x,rect.x+14,rect.x+rect.w-14,rect.x+rect.w}};
    const std::array<float,4> dy{{rect.y,rect.y+12,rect.y+rect.h-12,rect.y+rect.h}};
    for (std::size_t row=0;row<3;++row) for (std::size_t column=0;column<3;++column) {
        if (row==1 && column==1) continue;
        crop(1,{buttonSource.x+sx[column],buttonSource.y+sy[row],sx[column+1]-sx[column],sy[row+1]-sy[row]},
               {dx[column],dy[row],dx[column+1]-dx[column],dy[row+1]-dy[row]},tint);
    }
    crop(1,paperSource,{rect.x+14,rect.y+12,rect.w-28,rect.h-24},tint);
    if (primary || focused) {
        const auto outline=focused?gold:olive;
        sdl_check(SDL_SetRenderDrawColor(renderer_,outline.r,outline.g,outline.b,outline.a));
        const int thickness=focused?3:1;
        for (int i=1;i<=thickness;++i) {
            const SDL_FRect edge{rect.x-i,rect.y-i,rect.w+2*i,rect.h+2*i};
            sdl_check(SDL_RenderRect(renderer_,&edge));
        }
    }
    const float height=std::min(20.f,rect.h-26);
    const float fit=std::min(width(label,height),rect.w-36);
    text(rect.x+(rect.w-fit)/2,rect.y+(rect.h-height)/2,label,
         enabled?ink:disabledInk,height,rect.w-36);
}
} // namespace yami::setup
