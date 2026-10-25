#include "scene.hpp"
#include "parallax.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace yami {
namespace {
constexpr std::array<std::uint16_t, 6> quad_indices{{0, 1, 2, 0, 2, 3}};
std::atomic<std::uint64_t> next_temporal_id{1};
constexpr std::array<Vertex, 4> quad_vertices{{
    {{0,1,0},{0,1},{0,0,1}}, {{1,1,0},{1,1},{0,0,1}},
    {{1,0,0},{1,0},{0,0,1}}, {{0,0,0},{0,0},{0,0,1}}
}};
std::string model_key(const std::filesystem::path& path) { return path.generic_string(); }
bool image_flip_vertical(const std::filesystem::path& path) {
    bool flip = true;
    auto extension = path.extension().string();
    for (auto& c : extension) if (c >= 'A' && c <= 'Z') c = char(c+'a'-'A');
    if (extension == ".tga") {
        std::array<std::uint8_t, 18> header{};
        std::ifstream input(path, std::ios::binary);
        if (!input.read(reinterpret_cast<char*>(header.data()), header.size()))
            throw std::runtime_error("Truncated TGA header: "+path.string());
        // 00417a30 ignores origin bit. The one original top-first TGA is therefore
        // inverted relative to FFmpeg's canonical top-left image; retain that fact.
        flip = (header[17]&0x20) == 0;
    }
    return flip;
}
ShadowBounds indexed_bounds(std::span<const Vertex> vertices, std::span<const std::uint16_t> indices) {
    ShadowBounds bounds;
    for (const auto index : indices) bounds.include(vertices[index].position);
    return bounds;
}
}
static std::uint32_t material_hash(std::string_view name) noexcept {
    std::uint32_t hash=2166136261;
    for (unsigned char c:name) {
        if (c=='\\') c='/';
        else if (c>='A' && c<='Z') c=unsigned(c+'a'-'A');
        hash=(hash^c)*16777619;
    }
    return hash;
}

TrafficLightPhase traffic_light_phase(std::uint64_t elapsed) noexcept {
    const auto phase=elapsed%9000;
    return phase<4000?TrafficLightPhase::Red:phase<5000?TrafficLightPhase::Yellow:TrafficLightPhase::Green;
}
std::array<float,3> traffic_light_color(TrafficLightPhase phase) noexcept {
    switch(phase) {
    case TrafficLightPhase::Red: return {1,.03,.01};
    case TrafficLightPhase::Yellow: return {1,.65,.02};
    case TrafficLightPhase::Green: return {.02,1,.08};
    }
    return {};
}

float night_lamp_emission(std::string_view name,float night) noexcept {
    return night>0 && (name.ends_with("lamba_001.tga") || name.ends_with("trafik_lambasi.tga")) ? night : 0;
}

bool night_street_lamp(std::string_view model_name) {
    std::string normalized(model_name);
    for(auto& c:normalized) {
        if(c=='\\') c='/';
        else if(c>='A'&&c<='Z') c=char(c+'a'-'A');
    }
    const auto slash=normalized.rfind('/');
    const auto name=std::string_view(normalized).substr(slash==std::string::npos?0:slash+1);
    const auto parent=normalized.substr(0,slash);
    const auto parent_slash=parent.rfind('/');
    const auto directory=std::string_view(parent).substr(parent_slash==std::string::npos?0:parent_slash+1);
    if(name!="model.dat") return false;
    if(directory.starts_with("model_sokak_lambasi") || directory=="model_sahaf_carsi_lamba" || directory=="lamba") return true;
    return directory.starts_with("lamba") && directory.size()>5 &&
        std::all_of(directory.begin()+5,directory.end(),[](unsigned char c){return c>='0'&&c<='9';});
}

float night_window_emission(std::string_view name,float night) noexcept {
    if (night<=0) return 0;
    const bool window=name.find("/pencere/")!=std::string_view::npos || name.find("\\pencere\\")!=std::string_view::npos ||
        name.find("kemerli_pencere")!=std::string_view::npos;
    if (!window) return 0;
    const auto hash=material_hash(name);
    return hash%3==0 ? night*.35f : 0; // Stable pseudo-random occupancy: one third lit, never flickering.
}
float night_facade_emission(std::string_view name,float night) noexcept {
    if (night<=0) return 0;
    const bool facade=name.find("/cephe/")!=std::string_view::npos || name.find("\\cephe\\")!=std::string_view::npos ||
        name.find("/cephe_")!=std::string_view::npos || name.find("\\cephe_")!=std::string_view::npos;
    return facade && material_hash(name)%3==0 ? night*.25f : 0;
}
std::vector<LocalLight> derive_night_lights(const Mesh& mesh,const MaterialPass& pass,int width,int height,
                                             std::span<const std::uint8_t> rgba,bool flip_vertical,bool street_lamp) {
    std::vector<LocalLight> result;
    const float lamp=night_lamp_emission(pass.texture_name,1);
    const bool traffic=pass.texture_name.ends_with("trafik_lambasi.tga");
    const float emission=night_window_emission(pass.texture_name,1)+night_facade_emission(pass.texture_name,1);
    if(mesh.vertices.empty()) return result;
    if(street_lamp) {
        float max_y=mesh.vertices.front().position.y;
        for(const auto& vertex:mesh.vertices) max_y=std::max(max_y,vertex.position.y);
        // ponytail: no authored emitter nodes; the shipped all-metal lamp heads occupy the top 20 units.
        Vec3 sum{}; float head_y=max_y; std::size_t count=0;
        for(const auto& vertex:mesh.vertices) if(vertex.position.y>=max_y-20) {
            sum.x+=vertex.position.x; sum.z+=vertex.position.z; head_y=std::min(head_y,vertex.position.y); ++count;
        }
        LocalLight light; light.position={sum.x/count,head_y-6,sum.z/count};
        light.color={1,.55f,.2f}; light.intensity=60; light.radius=650; light.kind=LocalLightKind::Spot;
        result.push_back(light); return result;
    }
    if(lamp>0&&!traffic) {
        Vec3 min=mesh.vertices.front().position,max=min;
        for(const auto& vertex:mesh.vertices) {
            min.x=std::min(min.x,vertex.position.x); min.y=std::min(min.y,vertex.position.y); min.z=std::min(min.z,vertex.position.z);
            max.x=std::max(max.x,vertex.position.x); max.y=std::max(max.y,vertex.position.y); max.z=std::max(max.z,vertex.position.z);
        }
        LocalLight light; light.position={(min.x+max.x)*.5f,min.y-6,(min.z+max.z)*.5f};
        light.color={1,.55f,.2f}; light.intensity=60*lamp; light.radius=650; light.kind=LocalLightKind::Spot;
        result.push_back(light); return result;
    }
    if(lamp<=0&&emission<=0) return result;
    if(width<=0||height<=0||rgba.size()!=std::size_t(width)*height*4) throw std::invalid_argument("Invalid night emitter image");
    const auto smooth=[](float a,float b,float value) {
        const float t=std::clamp((value-a)/(b-a),0.f,1.f); return t*t*(3-2*t);
    };
    const auto mask_at=[&](Vec2 uv,int& phase) {
        phase=-1;
        float u=uv.x-std::floor(uv.x),v=uv.y-std::floor(uv.y); if(flip_vertical) v=1-v;
        const int x=std::min(width-1,int(u*width)),y=std::min(height-1,int(v*height));
        const auto* pixel=rgba.data()+(std::size_t(y)*width+x)*4;
        const float r=pixel[0]/255.f,g=pixel[1]/255.f,b=pixel[2]/255.f,brightness=std::max({r,g,b});
        const float saturation=brightness-std::min({r,g,b});
        if(traffic) {
            if(u>=.32f||brightness<=.12f||saturation<=.03f) return 0.f;
            float hue=brightness==r?(g-b)/saturation:brightness==g?(b-r)/saturation+2:(r-g)/saturation+4;
            hue=hue/6; if(hue<0) hue+=1;
            if(hue<.07f||hue>=.93f) phase=static_cast<int>(TrafficLightPhase::Red);
            else if(hue<.20f) phase=static_cast<int>(TrafficLightPhase::Yellow);
            else if(hue>.38f&&hue<.58f) phase=static_cast<int>(TrafficLightPhase::Green);
            else return 0.f;
            return smooth(.12f,.35f,brightness)*smooth(.03f,.12f,saturation);
        }
        return (.35f+.65f*brightness)*smooth(.12f,.35f,brightness)*smooth(.005f,.03f,b-std::max(r,g));
    };
    struct Candidate { std::array<std::uint32_t,3> ids; Vec3 center,normal; float area,mask,low; int phase; };
    std::vector<Candidate> candidates;
    for(std::size_t at=0;at+2<mesh.indices.size();at+=3) {
        const std::array<std::uint32_t,3> ids{{mesh.indices[at],mesh.indices[at+1],mesh.indices[at+2]}};
        const auto& a=mesh.vertices[ids[0]],&b=mesh.vertices[ids[1]],&c=mesh.vertices[ids[2]];
        const Vec2 uv{(a.uv.x+b.uv.x+c.uv.x)/3,(a.uv.y+b.uv.y+c.uv.y)/3};
        int phase; const float mask=mask_at(uv,phase); if(mask<=.01f) continue;
        const Vec3 ab{b.position.x-a.position.x,b.position.y-a.position.y,b.position.z-a.position.z};
        const Vec3 ac{c.position.x-a.position.x,c.position.y-a.position.y,c.position.z-a.position.z};
        const Vec3 cross{ab.y*ac.z-ab.z*ac.y,ab.z*ac.x-ab.x*ac.z,ab.x*ac.y-ab.y*ac.x};
        const float area=std::sqrt(cross.x*cross.x+cross.y*cross.y+cross.z*cross.z)*.5f; if(area<=1e-8f) continue;
        Vec3 normal{a.normal.x+b.normal.x+c.normal.x,a.normal.y+b.normal.y+c.normal.y,a.normal.z+b.normal.z+c.normal.z};
        float length=std::sqrt(normal.x*normal.x+normal.y*normal.y+normal.z*normal.z);
        if(length<=1e-8f) { normal=cross; length=area*2; }
        normal={normal.x/length,normal.y/length,normal.z/length};
        candidates.push_back({ids,{(a.position.x+b.position.x+c.position.x)/3,(a.position.y+b.position.y+c.position.y)/3,
            (a.position.z+b.position.z+c.position.z)/3},normal,area,mask,std::min({a.position.y,b.position.y,c.position.y}),phase});
    }
    std::vector<std::size_t> parent(candidates.size()); for(std::size_t i=0;i<parent.size();++i) parent[i]=i;
    const auto root=[&](std::size_t value) { while(parent[value]!=value) { parent[value]=parent[parent[value]]; value=parent[value]; } return value; };
    const auto join=[&](std::size_t a,std::size_t b) { a=root(a); b=root(b); if(a!=b) parent[b]=a; };
    if(traffic) for(std::size_t i=0;i<candidates.size();++i) for(std::size_t j=0;j<i;++j) {
        const auto& a=candidates[i],&b=candidates[j];
        const float x=a.center.x-b.center.x,y=a.center.y-b.center.y,z=a.center.z-b.center.z;
        const float facing=a.normal.x*b.normal.x+a.normal.y*b.normal.y+a.normal.z*b.normal.z;
        if(a.phase==b.phase && x*x+y*y+z*z<60*60 && facing>.2f) join(i,j);
    }
    else {
        std::unordered_map<std::uint64_t,std::size_t> edges;
        for(std::size_t i=0;i<candidates.size();++i) {
            const auto ids=candidates[i].ids;
            const std::array<std::array<std::uint32_t,2>,3> triangle_edges{{{ids[0],ids[1]},{ids[1],ids[2]},{ids[2],ids[0]}}};
            for(auto edge:triangle_edges) {
                if(edge[0]>edge[1]) std::swap(edge[0],edge[1]);
                const auto key=(std::uint64_t(edge[0])<<32)|edge[1];
                const auto [at,inserted]=edges.emplace(key,i); if(!inserted) join(i,at->second);
            }
        }
    }
    struct Aggregate { Vec3 position{},normal{}; float area=0,mask=0,low=std::numeric_limits<float>::max(); int phase=-1; };
    std::vector<Aggregate> groups(candidates.size());
    for(std::size_t i=0;i<candidates.size();++i) {
        auto& group=groups[root(i)]; const auto& candidate=candidates[i];
        group.position.x+=candidate.center.x*candidate.area; group.position.y+=candidate.center.y*candidate.area; group.position.z+=candidate.center.z*candidate.area;
        group.normal.x+=candidate.normal.x*candidate.area; group.normal.y+=candidate.normal.y*candidate.area; group.normal.z+=candidate.normal.z*candidate.area;
        group.mask+=candidate.mask*candidate.area; group.area+=candidate.area; group.low=std::min(group.low,candidate.low); group.phase=candidate.phase;
    }
    std::vector<bool> valid(groups.size(),true);
    if(traffic) {
        const auto center=[&](const Aggregate& group) { return Vec3{group.position.x/group.area,group.position.y/group.area,group.position.z/group.area}; };
        const auto facing=[&](const Aggregate& a,const Aggregate& b) {
            const float al=a.normal.x*a.normal.x+a.normal.y*a.normal.y+a.normal.z*a.normal.z;
            const float bl=b.normal.x*b.normal.x+b.normal.y*b.normal.y+b.normal.z*b.normal.z;
            const float dot=a.normal.x*b.normal.x+a.normal.y*b.normal.y+a.normal.z*b.normal.z;
            return al>0&&bl>0?dot/std::sqrt(al*bl):0.f;
        };
        const auto distance2=[](Vec3 a,Vec3 b) { const float x=a.x-b.x,y=a.y-b.y,z=a.z-b.z; return x*x+y*y+z*z; };
        for(std::size_t i=0;i<groups.size();++i) if(groups[i].phase==static_cast<int>(TrafficLightPhase::Yellow)) valid[i]=false;
        for(std::size_t r=0;r<groups.size();++r) if(groups[r].phase==static_cast<int>(TrafficLightPhase::Red))
            for(std::size_t g=0;g<groups.size();++g) if(groups[g].phase==static_cast<int>(TrafficLightPhase::Green)) {
                const auto rc=center(groups[r]),gc=center(groups[g]);
                if(distance2(rc,gc)>=60*60||facing(groups[r],groups[g])<=.2f) continue;
                const Vec3 middle{(rc.x+gc.x)*.5f,(rc.y+gc.y)*.5f,(rc.z+gc.z)*.5f};
                std::size_t best=groups.size(); float nearest=60*60;
                for(std::size_t y=0;y<groups.size();++y) if(groups[y].phase==static_cast<int>(TrafficLightPhase::Yellow)) {
                    const float distance=distance2(center(groups[y]),middle);
                    if(distance<nearest&&facing(groups[r],groups[y])>.2f) { best=y; nearest=distance; }
                }
                if(best<groups.size()) valid[best]=true;
            }
    }
    for(std::size_t index=0;index<groups.size();++index) if(const auto& group=groups[index];group.area>0&&valid[index]) {
        float length=std::sqrt(group.normal.x*group.normal.x+group.normal.y*group.normal.y+group.normal.z*group.normal.z);
        const Vec3 normal=length>1e-8f?Vec3{group.normal.x/length,group.normal.y/length,group.normal.z/length}:Vec3{};
        LocalLight light; light.position={group.position.x/group.area+normal.x*2,traffic?group.low-2:group.position.y/group.area+normal.y*2,group.position.z/group.area+normal.z*2};
        light.color=traffic?traffic_light_color(static_cast<TrafficLightPhase>(group.phase)):std::array<float,3>{1,.55f,.2f};
        light.intensity=(traffic?8*lamp:24*emission)*group.mask/group.area; light.radius=traffic?240:300;
        light.traffic_phase=traffic?group.phase:-1;
        result.push_back(light);
    }
    return result;
}
ModelInstance::ModelInstance(Renderer& renderer, const ModelResource& model, bool animated)
    : renderer_(renderer), model_(model) {
    parts.reserve(model.parts.size());
    try {
        for (const auto& source : model.parts) {
            parts.emplace_back();
            auto& part = parts.back();
            part.temporal_id = next_temporal_id.fetch_add(1, std::memory_order_relaxed);
            const auto& passes = source.material->material.passes;
            for (std::size_t p = 0; p < passes.size(); ++p)
                if (!passes[p].blend && passes[p].depth_write && !passes[p].video) {
                    part.primary_pass = p;
                    break;
                }
            if (animated && !source.mesh->mesh.bones.empty()) {
                prepare_skinning(source.mesh->mesh, part.skin);
                part.bounds = indexed_bounds(part.skin.vertices, source.mesh->mesh.indices);
                part.gpu = renderer_.upload_mesh(part.skin.vertices, source.mesh->mesh.indices);
                part.owned = true;
            } else {
                if (!source.mesh->static_gpu) {
                    source.mesh->static_bounds = indexed_bounds(source.mesh->mesh.vertices, source.mesh->mesh.indices);
                    source.mesh->static_gpu = renderer_.upload_mesh(source.mesh->mesh.vertices, source.mesh->mesh.indices);
                }
                part.bounds = source.mesh->static_bounds;
                part.gpu = source.mesh->static_gpu;
            }
        }
    } catch (...) {
        for (const auto& part : parts) if (part.owned) renderer_.release_mesh(part.gpu);
        throw;
    }
}
ModelInstance::~ModelInstance() {
    for (const auto& part : parts) if (part.owned) renderer_.release_mesh(part.gpu);
}
ShadowBounds ModelInstance::bounds(const Mat4& transform) const {
    ShadowBounds result;
    for (const auto& part : parts)
        if (part.primary_pass != static_cast<std::size_t>(-1))
            result.include(part.bounds.transformed(transform));
    return result;
}
SceneCache::SceneCache(Renderer& renderer, std::filesystem::path root)
    : renderer_(renderer), root_(std::move(root)) {
    quad_ = renderer_.upload_mesh(quad_vertices, quad_indices);
}
std::filesystem::path SceneCache::resolve(std::string_view name) {
    return resolve_asset(root_, std::string(name), &directories_);
}
MeshResource& SceneCache::mesh(const std::filesystem::path& path) {
    const auto key = model_key(path);
    if (const auto found = meshes_.find(key); found != meshes_.end()) return *found->second;
    auto resource = std::make_unique<MeshResource>();
    resource->mesh = read_mesh(path);
    resource->animations.reserve(resource->mesh.animation_names.size());
    for (const auto& name : resource->mesh.animation_names)
        resource->animations.push_back(read_animation(animation_path(path, name, &directories_), resource->mesh));
    return *meshes_.emplace(key, std::move(resource)).first->second;
}
ModelResource& SceneCache::model(std::string_view descriptor) {
    if (const auto found = models_.find(descriptor); found != models_.end()) return *found->second;
    const auto path = resolve(descriptor);
    auto resource = std::make_unique<ModelResource>();
    resource->model = read_model(path);
    resource->parts.reserve(resource->model.parts.size());
    for (const auto& part : resource->model.parts)
        resource->parts.push_back({&mesh(mesh_path(path, part, &directories_)), &material(part.material_name)});
    if(night_street_lamp(descriptor)) for(std::size_t n=0;n<resource->parts.size();++n) {
        bool separate_head=false;
        for(const auto& pass:resource->parts[n].material->material.passes) {
            if(pass.texture_name.ends_with("cret2.tga")) {
                resource->street_lamp_part=n; separate_head=true; break;
            }
            if(pass.texture_name.ends_with("metal_001.tga")) resource->street_lamp_model=true;
        }
        if(separate_head) break;
    }
    return *models_.emplace(std::string(descriptor), std::move(resource)).first->second;
}
MaterialResource& SceneCache::material(std::string_view name) {
    if (const auto found = materials_.find(name); found != materials_.end()) return *found->second;
    auto resource = std::make_unique<MaterialResource>();
    resource->material = read_material(resolve("data/materials/"+std::string(name)+".dat"));
    resource->textures.resize(resource->material.passes.size());
    resource->relief.resize(resource->material.passes.size());
    return *materials_.emplace(std::string(name), std::move(resource)).first->second;
}
unsigned SceneCache::image(std::string_view name) {
    if (const auto found = images_.find(name); found != images_.end()) return found->second;
    std::filesystem::path path;
    try { path = resolve(name); }
    catch (const std::filesystem::filesystem_error& error) {
        if (error.code() != std::errc::no_such_file_or_directory && error.code() != std::errc::is_a_directory)
            throw;
        // 00417c30's real supported fopen-failure result: opaque 2x2 white RGB.
        if (!white_) {
            constexpr std::array<std::uint8_t, 16> white{{255,255,255,255,255,255,255,255,
                                                        255,255,255,255,255,255,255,255}};
            white_ = renderer_.upload_texture(2, 2, white);
        }
        std::cerr << "Original white-image behavior: " << name << '\n';
        images_.emplace(std::string(name), white_);
        return white_;
    }
    const bool flip_vertical = image_flip_vertical(path);
    VideoDecoder decoder(path);
    VideoFrame frame;
    if (!decoder.next(frame)) throw std::runtime_error("Image contains no frame: "+path.string());
    const auto texture = renderer_.upload_texture(frame.width, frame.height, frame.rgba, true, flip_vertical);
    images_.emplace(std::string(name), texture);
    return texture;
}
unsigned SceneCache::relief(std::string_view name) {
    if (const auto found = relief_images_.find(name); found != relief_images_.end()) return found->second;
    const auto path = resolve(name);
    VideoDecoder decoder(path);
    VideoFrame frame;
    if (!decoder.next(frame)) throw std::runtime_error("Image contains no frame: "+path.string());
    auto generated = derive_relief(frame.rgba, frame.width, frame.height);
    unsigned texture = 0;
    if (!generated.rgba.empty())
        texture = renderer_.upload_texture(generated.width, generated.height, generated.rgba, true, image_flip_vertical(path));
    relief_images_.emplace(std::string(name), texture);
    return texture;
}
bool SceneCache::video_texture(std::string_view name, std::uint32_t time, bool loop, unsigned& texture) {
    auto found = videos_.find(name);
    if (found == videos_.end()) {
        Video video;
        video.decoder = std::make_unique<VideoDecoder>(resolve(name));
        const int width = video.decoder->width(), height = video.decoder->height();
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
            video.decoder->frame_count() <= 0 || !(video.decoder->fps() > 0))
            throw std::runtime_error("AVI lacks valid dimensions/frame-count/rate: "+std::string(name));
        // Original 004087d0 centers the DIB in a power-of-two black canvas.
        const int canvas_width = static_cast<int>(std::bit_ceil(static_cast<unsigned>(width)));
        const int canvas_height = static_cast<int>(std::bit_ceil(static_cast<unsigned>(height)));
        video.padding_x = (canvas_width-width)/2;
        video.padding_y = (canvas_height-height)/2;
        std::vector<std::uint8_t> black(static_cast<std::size_t>(canvas_width)*canvas_height*4);
        for (std::size_t i = 3; i < black.size(); i += 4) black[i] = 255;
        video.texture = renderer_.upload_texture(canvas_width, canvas_height, black, false);
        found = videos_.emplace(std::string(name), std::move(video)).first;
    }
    auto& video = found->second;
    texture = video.texture;
    if (!video.active) { video.started = time; video.active = true; }
    const auto elapsed = loop ? static_cast<std::uint32_t>(time-video.started) : time;
    // AVIStreamSampleToTime(length)/length at00408876 is an integer ms divisor.
    const auto duration_ms = static_cast<std::uint64_t>(std::llround(video.decoder->duration()*1000));
    const auto count = video.decoder->frame_count();
    const auto frame_ms = duration_ms/static_cast<std::uint64_t>(count);
    if (!frame_ms) throw std::runtime_error("Invalid original AVI frame duration: "+std::string(name));
    std::int64_t target = elapsed/frame_ms;
    if (target >= count) {
        if (!loop) return false;
        // Original resets its baseline on wrap rather than retaining overshoot.
        video.started = time;
        target = 0;
    }
    if (target < video.frame) { video.decoder->seek(0); video.frame = -1; }
    VideoFrame frame;
    while (video.frame < target) {
        if (!video.decoder->next(frame)) throw std::runtime_error("AVI ends before its declared frame count: "+std::string(name));
        ++video.frame;
    }
    if (!frame.rgba.empty())
        renderer_.update_texture(texture, frame.width, frame.height, frame.rgba, video.padding_x, video.padding_y);
    return true;
}
void SceneCache::prepare(ModelInstance& instance, std::size_t animation, std::uint32_t elapsed) {
    const auto& model = instance.resource();
    const bool interpolate = renderer_.graphics_settings().enhanced; // smooth 30fps poses (enhanced only)
    for (std::size_t n = 0; n < model.parts.size(); ++n) {
        auto& part = instance.parts[n];
        const auto& source = model.parts[n];
        if (part.owned) {
            if (animation >= source.mesh->animations.size())
                throw std::runtime_error("Entity animation ordinal exceeds model mesh animation list");
            const auto& clip = source.mesh->animations[animation];
            const auto frame = animation_frame(clip, elapsed);
            if (animation != part.animation || frame != part.frame || interpolate != part.interpolated ||
                (interpolate && elapsed != part.elapsed)) {
                skin_mesh(source.mesh->mesh, clip, elapsed, model.model.scale, model.model.offset, part.skin, interpolate);
                renderer_.update_mesh(part.gpu, part.skin.vertices);
                part.bounds = indexed_bounds(part.skin.vertices, source.mesh->mesh.indices);
                part.animation = animation; part.frame = frame;
                part.elapsed = elapsed; part.interpolated = interpolate;
            }
        }
    }
}
void SceneCache::draw_shadow(ModelInstance& instance, DrawState state) {
    const auto& model = instance.resource();
    for (std::size_t n = 0; n < model.parts.size(); ++n) {
        const auto& part = instance.parts[n];
        if (part.primary_pass == static_cast<std::size_t>(-1)) continue;
        auto& material = *model.parts[n].material;
        const auto& pass = material.material.passes[part.primary_pass];
        auto& texture = material.textures[part.primary_pass];
        if (!texture) texture = image(pass.texture_name);
        auto submission = state;
        submission.ray_primary = true;
        submission.temporal_static = state.temporal_static && !part.owned;
        renderer_.draw(part.gpu, texture, material.material, pass, submission);
    }
}
void SceneCache::prepare_night_lights(ModelInstance& instance,std::size_t n) {
    auto& part=instance.parts[n]; if(part.night_lights_ready) return;
    const auto& source=instance.resource().parts[n]; const auto& passes=source.material->material.passes;
    part.night_lights.resize(passes.size());
    if(n==instance.resource().street_lamp_part ||
       (instance.resource().street_lamp_model && instance.resource().street_lamp_part==static_cast<std::size_t>(-1))) {
        part.night_lights[0]=derive_night_lights(source.mesh->mesh,passes[0],0,0,{},false,true);
        part.night_lights_ready=true; return;
    }
    for(std::size_t p=0;p<passes.size();++p) {
        const auto& pass=passes[p];
        const float lamp=night_lamp_emission(pass.texture_name,1);
        if(lamp>0&&!pass.texture_name.ends_with("trafik_lambasi.tga")) {
            part.night_lights[p]=derive_night_lights(source.mesh->mesh,pass,0,0,{},false); continue;
        }
        if(lamp<=0&&night_window_emission(pass.texture_name,1)+night_facade_emission(pass.texture_name,1)<=0) continue;
        try {
            const auto path=resolve(pass.texture_name); VideoDecoder decoder(path); VideoFrame frame;
            if(decoder.next(frame)) part.night_lights[p]=derive_night_lights(source.mesh->mesh,pass,frame.width,frame.height,frame.rgba,image_flip_vertical(path));
        } catch(const std::filesystem::filesystem_error& error) {
            if(error.code()!=std::errc::no_such_file_or_directory&&error.code()!=std::errc::is_a_directory) throw;
        }
    }
    part.night_lights_ready=true;
}

void SceneCache::draw(ModelInstance& instance, DrawState state, std::size_t animation,
                      std::uint32_t elapsed, std::uint32_t wall_clock) {
    prepare(instance, animation, elapsed);
    const auto graphics = renderer_.graphics_settings();
    const bool parallax = graphics.enhanced && graphics.parallax;
    const bool night_lights=graphics.enhanced&&graphics.ray_tracing&&renderer_.ray_tracing_available()&&state.emission>0;
    const int traffic_phase=static_cast<int>(traffic_light_phase(elapsed));
    const auto& model = instance.resource();
    for (std::size_t n = 0; n < model.parts.size(); ++n) {
        const auto& part = instance.parts[n];
        const auto& source = model.parts[n];
        auto& material = *source.material;
        if(night_lights) prepare_night_lights(instance,n);
        for (std::size_t p = 0; p < material.material.passes.size(); ++p) {
            const auto& pass = material.material.passes[p];
            auto& texture = material.textures[p];
            if (pass.video) video_texture(pass.texture_name, wall_clock, true, texture);
            else if (!texture) texture = image(pass.texture_name);
            auto submission = state;
            submission.emission=pass.texture_name.ends_with("trafik_lambasi.tga") ? 0 : night_lamp_emission(pass.texture_name,state.emission);
            submission.window_emission=night_window_emission(pass.texture_name,state.emission)+
                night_facade_emission(pass.texture_name,state.emission);
            if(night_lights) for(auto light:part.night_lights[p]) {
                if(light.traffic_phase>=0&&light.traffic_phase!=traffic_phase) continue;
                const Vec3 local{light.position[0],light.position[1],light.position[2]};
                const Vec3 world=transform_point(state.model,local);
                light.position={world.x,world.y,world.z}; light.intensity*=state.emission;
                renderer_.submit_light(light);
            }
            submission.ray_primary = p == part.primary_pass;
            submission.ray_geometry = state.ray_geometry && submission.ray_primary;
            submission.temporal_static = state.temporal_static && !part.owned && !pass.video;
            submission.temporal_id = submission.ray_geometry && !pass.video ? part.temporal_id : 0;
            if (parallax && submission.temporal_static && submission.ray_geometry && !pass.blend && !pass.alpha_test &&
                pass.depth_test && pass.depth_write && texture != white_)
                submission.relief = relief(pass.texture_name);
            renderer_.draw(part.gpu, texture, material.material, pass, submission);
        }
    }
}
void SceneCache::draw_quad(const menu::Draw& quad, std::uint32_t time, bool loop) {
    if (!quad.visible) return;
    DrawState state;
    state.ray_geometry = false;
    state.color = quad.color;
    const auto& p = quad.positions;
    auto& m = state.model.values;
    m[0] = (p[2].x-p[3].x)*1024; m[1] = (p[2].y-p[3].y)*768;
    m[4] = (p[0].x-p[3].x)*1024; m[5] = (p[0].y-p[3].y)*768;
    m[12] = p[3].x*1024; m[13] = p[3].y*768;
    state.uv_transform = {quad.uv[2].x-quad.uv[3].x, quad.uv[0].y-quad.uv[3].y,
                          quad.uv[3].x, quad.uv[3].y};
    if (quad.material.empty()) {
        Material material;
        material.cull = false;
        MaterialPass pass;
        pass.depth_test = false; pass.depth_write = false;
        pass.blend = true;
        renderer_.draw(quad_, quad.texture.empty() ? 0 : image(quad.texture), material, pass, state);
        return;
    }
    auto& material = this->material(quad.material);
    for (std::size_t p = 0; p < material.material.passes.size(); ++p) {
        const auto& pass = material.material.passes[p];
        unsigned texture;
        if (!quad.texture.empty()) texture = image(quad.texture);
        else {
            auto& cached = material.textures[p];
            if (pass.video) { if (!video_texture(pass.texture_name, time, loop, cached)) continue; }
            else if (!cached) cached = image(pass.texture_name);
            texture = cached;
        }
        renderer_.draw(quad_, texture, material.material, pass, state);
    }
}
void SceneCache::draw_ui(std::span<const menu::Draw> quads, std::uint32_t time) {
    for (const auto& quad : quads) draw_quad(quad, time, true);
}
bool SceneCache::draw_video(std::string_view name, std::uint32_t elapsed) {
    auto& resource = material(name);
    if (resource.material.passes.empty() || !resource.material.passes[0].video)
        throw std::runtime_error("Cutscene material does not contain video: "+std::string(name));
    unsigned texture;
    if (!video_texture(resource.material.passes[0].texture_name, elapsed, false, texture)) return false;
    menu::Draw quad;
    quad.material = name;
    quad.positions = {{{0,1},{1,1},{1,0},{0,0}}};
    // 0041659c..004165e8 exact UVs crop the centered512x384 source in512x512.
    quad.uv = {{{0,-0.125f},{1,-0.125f},{1,-0.875f},{0,-0.875f}}};
    draw_quad(quad, elapsed, false);
    return true;
}
void SceneCache::reset_video(std::string_view name) {
    auto& resource = material(name);
    for (const auto& pass : resource.material.passes) if (pass.video) {
        const auto found = videos_.find(pass.texture_name);
        if (found != videos_.end()) {
            found->second.decoder->seek(0);
            found->second.frame = -1;
            found->second.active = false;
        }
    }
}
} // namespace yami
