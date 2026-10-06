#include "renderer.hpp"
#include "world_effects.hpp"
#include <epoxy/gl.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace yami {
namespace {
void sdl_check(bool success) {
    if (!success) throw std::runtime_error(SDL_GetError());
}
void gl_check(const char* operation) {
    const auto error = glGetError();
    if (error != GL_NO_ERROR)
        throw std::runtime_error(std::string(operation) + ": GL error " + std::to_string(error));
}
unsigned compile_shader(unsigned type, const std::string& source) {
    const unsigned shader = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    int success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        int size = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &size);
        std::string message(std::max(size, 1), '\0');
        glGetShaderInfoLog(shader, size, nullptr, message.data());
        glDeleteShader(shader);
        throw std::runtime_error("Shader compile: " + message);
    }
    return shader;
}
unsigned create_program(const std::string& vertex_source, const std::string& fragment_source) {
    unsigned vertex = 0, fragment = 0, program = 0;
    try {
        vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
        fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
        program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        int success = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &success);
        if (!success) {
            int size = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &size);
            std::string message(std::max(size, 1), '\0');
            glGetProgramInfoLog(program, size, nullptr, message.data());
            throw std::runtime_error("Shader link: "+message);
        }
    } catch (...) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        if (program) glDeleteProgram(program);
        throw;
    }
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return program;
}
std::array<float, 9> normal_matrix(const Mat4& m) {
    const auto& v = m.values;
    const float a = v[0], b = v[4], c = v[8], d = v[1], e = v[5], f = v[9],
                g = v[2], h = v[6], i = v[10];
    std::array<float, 9> result{{e*i-f*h, c*h-b*i, b*f-c*e,
                               f*g-d*i, a*i-c*g, c*d-a*f,
                               d*h-e*g, b*g-a*h, a*e-b*d}};
    const float determinant = a*result[0] + b*result[3] + c*result[6];
    if (determinant == 0 || !std::isfinite(determinant))
        throw std::runtime_error("Singular illuminated model transform");
    for (auto& value : result) value /= determinant;
    return result;
}
constexpr const char* vertex_source = R"(
layout(location=0) in vec3 aPosition;
layout(location=1) in vec2 aUv;
layout(location=2) in vec3 aNormal;
uniform mat4 uModelView, uProjection;
uniform mat3 uNormal;
uniform vec4 uLight, uDiffuse, uColor, uUvTransform;
uniform bool uLighting, uEnhanced;
out vec2 vUv;
out vec4 vColor;
out float vFogDepth;
out vec3 vNormal, vEye;
void main() {
    vec4 eye = uModelView * vec4(aPosition,1.0);
    gl_Position = uProjection * eye;
    vUv = aUv*uUvTransform.xy+uUvTransform.zw;
    vFogDepth = abs(eye.z);
    vNormal = uNormal*aNormal;
    vEye = eye.xyz;
    if (uLighting && !uEnhanced) {
        vec3 light = normalize(uLight.xyz-eye.xyz*uLight.w);
        float diffuse = max(dot(uNormal*aNormal,light),0.0);
        vColor = vec4(clamp(vec3(0.456)+0.8*uDiffuse.rgb*diffuse,0.0,1.0),1.0);
    } else vColor = uColor;
}
)";
constexpr const char* fragment_source = R"(
in vec2 vUv;
in vec4 vColor;
in float vFogDepth;
in vec3 vNormal, vEye;
uniform vec4 uLight, uDiffuse;
uniform vec3 uWorldUp;
uniform bool uLighting, uEnhanced;
uniform sampler2D uTexture;
uniform bool uTextured, uFog, uFlipVertical;
uniform int uAlphaFunction;
uniform float uAlphaReference;
out vec4 outColor;
void main() {
    vec4 value = vColor;
    if (uLighting && uEnhanced) {
        vec3 normal = vNormal*inversesqrt(max(dot(vNormal,vNormal),0.00000001));
        vec3 light = normalize(uLight.xyz-vEye*uLight.w);
        float diffuse = max(dot(normal,light),0.0);
        float hemisphere = dot(normal,uWorldUp)*0.5+0.5;
        value = vec4(clamp(vec3(0.416+0.08*hemisphere)+0.8*uDiffuse.rgb*diffuse,0.0,1.0),1.0);
    }
    if (uTextured) value *= texture(uTexture,vec2(vUv.x,uFlipVertical ? 1.0-vUv.y : vUv.y));
    float a = value.a, r = uAlphaReference;
    bool pass = uAlphaFunction == 0 || uAlphaFunction == 519 ||
        (uAlphaFunction == 513 && a < r) || (uAlphaFunction == 514 && a == r) ||
        (uAlphaFunction == 515 && a <= r) || (uAlphaFunction == 516 && a > r) ||
        (uAlphaFunction == 517 && a != r) || (uAlphaFunction == 518 && a >= r);
    if (!pass) discard;
    if (uFog) value.rgb = mix(vec3(0.2),value.rgb,clamp((2000.0-vFogDepth)/1000.0,0.0,1.0));
    outColor = value;
}
)";
constexpr const char* post_vertex_source = R"(
out vec2 vUv;
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0,gl_VertexID == 2 ? 3.0 : -1.0);
    gl_Position = vec4(p,0.0,1.0);
    vUv = p*0.5+0.5;
}
)";
constexpr const char* post_fragment_source = R"(
in vec2 vUv;
uniform sampler2D uFrame;
uniform float uRampStep;
uniform bool uCopy;
out vec4 outColor;
void main() {
    vec4 value = texture(uFrame,vUv);
    if (uCopy) { outColor = value; return; }
    // 0042e290: ramp[i] = min(i*(128+parameter),65535). Apply after blending,
    // only to this game's framebuffer, never the user's monitor gamma.
    vec3 indices = floor(clamp(value.rgb,0.0,1.0)*255.0+0.5);
    outColor = vec4(min(indices*uRampStep,vec3(65535.0))/65535.0,value.a);
}
)";
}

Viewport fit_original_interface(int width, int height) {
    if (width <= 0 || height <= 0) throw std::runtime_error("Invalid drawable dimensions");
    int w = width, h = height;
    if (static_cast<std::int64_t>(width)*3 > static_cast<std::int64_t>(height)*4)
        w = height*4/3;
    else h = width*3/4;
    return {(width-w)/2, (height-h)/2, w, h};
}
Mat4 frustum_projection(float half_height, float aspect, float near_plane, float far_plane) {
    if (!(half_height > 0 && aspect > 0 && near_plane > 0 && far_plane > near_plane) ||
        !std::isfinite(half_height+aspect+near_plane+far_plane))
        throw std::runtime_error("Invalid camera frustum");
    Mat4 m;
    m.values[0] = near_plane/(half_height*aspect);
    m.values[5] = near_plane/half_height;
    m.values[10] = -(far_plane+near_plane)/(far_plane-near_plane);
    m.values[11] = -1;
    m.values[14] = -2*far_plane*near_plane/(far_plane-near_plane);
    return m;
}
Mat4 interface_projection() {
    auto m = identity_matrix();
    m.values[0] = 2.f/1024;
    m.values[5] = 2.f/768;
    m.values[10] = -1;
    m.values[12] = m.values[13] = -1;
    return m;
}
Renderer::Renderer(const DisplayOptions& options) {
    if (options.width <= 0 || options.height <= 0 || !valid_graphics(options))
        throw std::runtime_error("Invalid display options");
    effects_ = {options.ambient_occlusion, options.reflections, options.bloom, options.sharpen};
    enhanced_ = options.enhanced;
    try {
#ifdef __APPLE__
        constexpr int desktop_major = 4, desktop_minor = 1;
#else
        constexpr int desktop_major = 3, desktop_minor = 3;
#endif
        sdl_check(SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, options.gles ? 3 : desktop_major));
        sdl_check(SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, options.gles ? 0 : desktop_minor));
        sdl_check(SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                    options.gles ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_CORE));
        sdl_check(SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1));
        sdl_check(SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0));
        sdl_check(SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0));
        sdl_check(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0));
        sdl_check(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0));
        window_ = SDL_CreateWindow("ETI Yami — native", options.width, options.height,
            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
            (options.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
        if (!window_) throw std::runtime_error(SDL_GetError());
        context_ = SDL_GL_CreateContext(window_);
        if (!context_) throw std::runtime_error(SDL_GetError());
        sdl_check(SDL_GL_MakeCurrent(window_, context_));
        sdl_check(SDL_GL_SetSwapInterval(1));
        const std::string prefix = options.gles ? "#version 300 es\nprecision highp float;\n" : "#version 330 core\n";
        program_ = create_program(prefix+vertex_source, prefix+fragment_source);
        post_program_ = create_program(prefix+post_vertex_source, prefix+post_fragment_source);
        brightness_uniform_ = glGetUniformLocation(post_program_, "uRampStep");
        copy_uniform_ = glGetUniformLocation(post_program_, "uCopy");
        glUseProgram(post_program_);
        glUniform1i(glGetUniformLocation(post_program_, "uFrame"), 0);
        // Common initialization permits classic/enhanced switches without rebuilding
        // the context, shaders or any scene assets.
        effect_program_ = create_program(prefix+post_vertex_source, prefix+world_effect_fragment_source);
        glUseProgram(effect_program_);
        glUniform1i(glGetUniformLocation(effect_program_, "uFrame"), 0);
        glUniform1i(glGetUniformLocation(effect_program_, "uDepth"), 1);
        constexpr const char* effect_names[] = {"uProjectionInfo", "uTexelSize", "uWorldUp", "uEffects"};
        for (std::size_t n = 0; n < effect_uniforms_.size(); ++n) {
            effect_uniforms_[n] = glGetUniformLocation(effect_program_, effect_names[n]);
            if (effect_uniforms_[n] == -1)
                throw std::runtime_error(std::string("Inactive effect uniform: ")+effect_names[n]);
        }
        glGenFramebuffers(1, &effect_framebuffer_);
        glGenTextures(1, &resolve_depth_);
        glGenTextures(1, &effect_texture_);
        glGenVertexArrays(1, &post_vao_);
        glGenFramebuffers(1, &draw_framebuffer_);
        glGenFramebuffers(1, &resolve_framebuffer_);
        glGenTextures(1, &resolve_texture_);
        glGenRenderbuffers(1, &depth_buffer_);
        glGenRenderbuffers(1, &color_buffer_);
        glUseProgram(program_);
        constexpr const char* names[] = {"uModelView", "uNormal", "uProjection", "uLight",
            "uDiffuse", "uColor", "uLighting", "uFog", "uAlphaFunction", "uAlphaReference", "uTextured", "uFlipVertical", "uUvTransform", "uEnhanced", "uWorldUp"};
        for (std::size_t n = 0; n < uniforms_.size(); ++n) {
            uniforms_[n] = glGetUniformLocation(program_, names[n]);
            if (uniforms_[n] == -1) throw std::runtime_error(std::string("Inactive shader uniform: ")+names[n]);
        }
        glUniform1i(glGetUniformLocation(program_, "uTexture"), 0);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size_);
        glGetIntegerv(GL_MAX_SAMPLES, &max_samples_);
        if (!epoxy_is_desktop_gl() || epoxy_gl_version() >= 42 ||
            epoxy_has_gl_extension("GL_ARB_internalformat_query")) {
            for (const unsigned format : {GL_RGBA8, GL_DEPTH24_STENCIL8}) {
                int maximum = 0;
                glGetInternalformativ(GL_RENDERBUFFER, format, GL_SAMPLES, 1, &maximum);
                max_samples_ = std::min(max_samples_, maximum);
            }
        }
        requested_samples_ = samples_ = std::min(options.samples, max_samples_);
        if (epoxy_has_gl_extension("GL_EXT_texture_filter_anisotropic") ||
            epoxy_has_gl_extension("GL_ARB_texture_filter_anisotropic"))
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_anisotropy_);
        anisotropy_ = std::min(options.anisotropy, max_anisotropy_);
        glFrontFace(GL_CW);
        glDisable(GL_DITHER);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glClearColor(0, 0, 0, 1);
        resize();
        gl_check("Renderer initialization");
    } catch (...) { cleanup(); throw; }
}
void Renderer::cleanup() noexcept {
    if (context_) {
        SDL_GL_MakeCurrent(window_, context_);
        for (const auto& mesh : meshes_) {
            glDeleteVertexArrays(1, &mesh.vao);
            glDeleteBuffers(1, &mesh.vertices);
            glDeleteBuffers(1, &mesh.indices);
        }
        for (const auto& texture : textures_) glDeleteTextures(1, &texture.id);
        if (program_) glDeleteProgram(program_);
        if (post_program_) glDeleteProgram(post_program_);
        if (effect_program_) glDeleteProgram(effect_program_);
        glDeleteVertexArrays(1, &post_vao_);
        glDeleteFramebuffers(1, &draw_framebuffer_);
        glDeleteFramebuffers(1, &resolve_framebuffer_);
        glDeleteFramebuffers(1, &effect_framebuffer_);
        glDeleteTextures(1, &resolve_texture_);
        glDeleteTextures(1, &resolve_depth_);
        glDeleteTextures(1, &effect_texture_);
        glDeleteRenderbuffers(1, &color_buffer_);
        glDeleteRenderbuffers(1, &depth_buffer_);
        SDL_GL_DestroyContext(context_);
        context_ = nullptr;
    }
    if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }
}
Renderer::~Renderer() { cleanup(); }
GraphicsSettings Renderer::graphics_settings() const noexcept {
    GraphicsSettings settings;
    settings.samples = samples_;
    settings.anisotropy = anisotropy_;
    settings.enhanced = enhanced_;
    settings.fullscreen = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
    settings.ambient_occlusion = effects_[0];
    settings.reflections = effects_[1];
    settings.bloom = effects_[2];
    settings.sharpen = effects_[3];
    return settings;
}
void Renderer::set_graphics(const GraphicsSettings& settings) {
    if (!valid_graphics(settings)) throw std::runtime_error("Invalid graphics settings");
    const int samples = std::min(settings.samples, max_samples_);
    const float anisotropy = std::min(settings.anisotropy, max_anisotropy_);
    if (settings.fullscreen != ((SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0))
        sdl_check(SDL_SetWindowFullscreen(window_, settings.fullscreen));
    // Drivers may round a request up; repeating that request is not a quality change.
    const bool changed_samples = samples != samples_ && samples != requested_samples_;
    requested_samples_ = samples;
    if (changed_samples) samples_ = samples;
    int width, height;
    sdl_check(SDL_GetWindowSizeInPixels(window_, &width, &height));
    if (width != width_ || height != height_) resize();
    else if (changed_samples) allocate_targets(false);
    if (anisotropy != anisotropy_) {
        int active_texture, texture;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        for (const auto& uploaded : textures_) {
            if (!uploaded.mipmaps) continue;
            glBindTexture(GL_TEXTURE_2D, uploaded.id);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, anisotropy);
        }
        glBindTexture(GL_TEXTURE_2D, texture);
        glActiveTexture(active_texture);
        anisotropy_ = anisotropy;
    }
    enhanced_ = settings.enhanced;
    effects_ = {settings.ambient_occlusion, settings.reflections, settings.bloom, settings.sharpen};
    model_valid_ = normal_valid_ = false;
    gl_check("Graphics settings");
}
void Renderer::resize() {
    int width, height;
    sdl_check(SDL_GetWindowSizeInPixels(window_, &width, &height));
    if (width == width_ && height == height_) return;
    if (width <= 0 || height <= 0 || width > max_texture_size_ || height > max_texture_size_)
        throw std::runtime_error("Drawable size exceeds GPU framebuffer support");
    width_ = width; height_ = height;
    allocate_targets(true);
}
void Renderer::allocate_targets(bool resizeTextures) {
    int active_texture, texture, renderbuffer, read_framebuffer, draw_framebuffer;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
    if (resizeTextures) {
        const auto allocate_texture = [&](unsigned texture, bool depth) {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, depth ? GL_DEPTH24_STENCIL8 : GL_RGBA8,
                width_, height_, 0, depth ? GL_DEPTH_STENCIL : GL_RGBA,
                depth ? GL_UNSIGNED_INT_24_8 : GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        };
        allocate_texture(resolve_texture_, false);
        glBindFramebuffer(GL_FRAMEBUFFER, resolve_framebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolve_texture_, 0);
        allocate_texture(resolve_depth_, true);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, resolve_depth_, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("Incomplete resolve framebuffer");
        allocate_texture(effect_texture_, false);
        glBindFramebuffer(GL_FRAMEBUFFER, effect_framebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, effect_texture_, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("Incomplete world effect framebuffer");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, draw_framebuffer_);
    for (;;) {
        int color_samples = 0, depth_samples = 0;
        if (samples_) {
            glBindRenderbuffer(GL_RENDERBUFFER, color_buffer_);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples_, GL_RGBA8, width_, height_);
            glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &color_samples);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_buffer_);
        } else {
            // The resolved colour texture is the scene target without MSAA.
            // Release the unused multisample colour storage, retaining its handle.
            glBindRenderbuffer(GL_RENDERBUFFER, color_buffer_);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 1, 1);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolve_texture_, 0);
        }
        glBindRenderbuffer(GL_RENDERBUFFER, depth_buffer_);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples_, GL_DEPTH24_STENCIL8, width_, height_);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &depth_samples);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_buffer_);
        if (color_samples == depth_samples) {
            samples_ = color_samples;
            break;
        }
        // Both formats must choose the same supported count for depth resolves.
        const int common = std::max(color_samples, depth_samples);
        if (common <= samples_ || common > max_samples_)
            throw std::runtime_error("No matching color/depth MSAA count");
        samples_ = common;
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Incomplete scene framebuffer");
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_framebuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    glBindTexture(GL_TEXTURE_2D, texture);
    glActiveTexture(active_texture);
    frame_finished_ = false;
    gl_check("Framebuffer resize");
}
void Renderer::clear() {
    glBindFramebuffer(GL_FRAMEBUFFER, draw_framebuffer_);
    glUseProgram(program_);
    frame_finished_ = false;
    world_finished_ = false;
    world_camera_ = false;
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}
void Renderer::camera(const Mat4& view, const Mat4& projection, bool interface) {
    if (view_.values != view.values) model_valid_ = normal_valid_ = false;
    view_ = view;
    const auto& p = projection.values;
    world_camera_ = !interface && p[11] == -1 && p[15] == 0 && p[0] != 0 && p[5] != 0;
    if (enhanced_ && world_camera_) {
        projection_info_ = {1/p[0], 1/p[5], p[10], p[14]};
        const auto up = transform_direction(view, {0, 1, 0});
        const float length = std::sqrt(up.x*up.x+up.y*up.y+up.z*up.z);
        world_up_ = length > 0 ? std::array<float, 3>{up.x/length, up.y/length, up.z/length}
                               : std::array<float, 3>{0, 1, 0};
        glUniform3fv(uniforms_[14], 1, world_up_.data());
    }
    glUniform1i(uniforms_[13], enhanced_ && world_camera_);
    const auto viewport = interface ? fit_original_interface(width_, height_) : Viewport{0, 0, width_, height_};
    glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
    glScissor(viewport.x, viewport.y, viewport.width, viewport.height);
    glEnable(GL_SCISSOR_TEST);
    glUniformMatrix4fv(uniforms_[2], 1, GL_FALSE, projection.values.data());
    light_[0] = 0.5f; light_[1] = 1; light_[2] = 0.3f; light_[3] = 0;
    if (!interface) {
        const auto transformed = transform_direction(view, {0.5f, 1, 0.3f});
        light_[0] = transformed.x; light_[1] = transformed.y; light_[2] = transformed.z;
    }
    glUniform4fv(uniforms_[3], 1, light_.data());
}
void Renderer::hud_camera() {
    auto projection = interface_projection();
    if (static_cast<std::int64_t>(width_)*3 < static_cast<std::int64_t>(height_)*4) {
        camera(identity_matrix(), projection, true);
    } else {
        projection.values[0] = 2.f/(768.f*width_/height_);
        camera(identity_matrix(), projection);
    }
}
void Renderer::finish_world() {
    if (!enhanced_ || !world_camera_ || world_finished_ ||
        std::all_of(effects_.begin(), effects_.end(), [](float strength) { return strength == 0; })) return;
    // Preserve scene state across both fullscreen draws; their shaders never
    // touch the scene uniforms, and invalidating the model cache keeps later draws safe.
    constexpr unsigned capabilities[] = {GL_DEPTH_TEST, GL_CULL_FACE, GL_BLEND,
                                        GL_SCISSOR_TEST, GL_POLYGON_OFFSET_FILL};
    std::array<bool, 5> enabled{};
    for (std::size_t n = 0; n < enabled.size(); ++n) enabled[n] = glIsEnabled(capabilities[n]);
    std::array<int, 4> viewport{}, scissor{};
    std::array<GLboolean, 4> color_mask{};
    GLboolean depth_mask;
    int texture, vao;
    glGetIntegerv(GL_VIEWPORT, viewport.data());
    glGetIntegerv(GL_SCISSOR_BOX, scissor.data());
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask.data());
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    for (const auto capability : capabilities) glDisable(capability);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glViewport(0, 0, width_, height_);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, draw_framebuffer_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_framebuffer_);
    // Formats and dimensions match, including the packed depth/stencil storage.
    // With zero MSAA both FBOs share color, so resolve only the separate depth.
    glBlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_,
                      GL_DEPTH_BUFFER_BIT | (samples_ ? GL_COLOR_BUFFER_BIT : 0), GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, effect_framebuffer_);
    glUseProgram(effect_program_);
    glUniform4fv(effect_uniforms_[0], 1, projection_info_.data());
    glUniform2f(effect_uniforms_[1], 1.f/width_, 1.f/height_);
    glUniform3fv(effect_uniforms_[2], 1, world_up_.data());
    glUniform4fv(effect_uniforms_[3], 1, effects_.data());
    glBindTexture(GL_TEXTURE_2D, resolve_texture_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, resolve_depth_);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(post_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    // A draw, not a single-to-multisample blit. The sampled effect texture is
    // distinct from the scene color attachment in both the MSAA and zero-MSAA paths.
    glBindFramebuffer(GL_FRAMEBUFFER, draw_framebuffer_);
    glUseProgram(post_program_);
    glUniform1i(copy_uniform_, GL_TRUE);
    glBindTexture(GL_TEXTURE_2D, effect_texture_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glUseProgram(program_);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindVertexArray(vao);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    glDepthMask(depth_mask);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
    for (std::size_t n = 0; n < enabled.size(); ++n)
        if (enabled[n]) glEnable(capabilities[n]);
    model_valid_ = normal_valid_ = false;
    world_finished_ = true;
    gl_check("World composition");
}
unsigned Renderer::upload_mesh(std::span<const Vertex> vertices, std::span<const std::uint16_t> indices) {
    if (vertices.empty() || indices.empty() || indices.size()%3 ||
        indices.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("Invalid triangle mesh");
    for (auto index : indices)
        if (index >= vertices.size()) throw std::runtime_error("Mesh index out of range");
    Geometry mesh{0, 0, 0, vertices.size(), indices.size()};
    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);
    glGenBuffers(1, &mesh.vertices);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vertices);
    glBufferData(GL_ARRAY_BUFFER, vertices.size_bytes(), vertices.data(), GL_DYNAMIC_DRAW);
    glGenBuffers(1, &mesh.indices);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.indices);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size_bytes(), indices.data(), GL_STATIC_DRAW);
    constexpr unsigned counts[] = {3, 2, 3};
    constexpr std::size_t offsets[] = {offsetof(Vertex, position), offsetof(Vertex, uv), offsetof(Vertex, normal)};
    for (unsigned n = 0; n != 3; ++n) {
        glEnableVertexAttribArray(n);
        glVertexAttribPointer(n, counts[n], GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsets[n]));
    }
    unsigned id;
    if (free_mesh_) {
        id = free_mesh_;
        free_mesh_ = meshes_[id-1].next_free;
        meshes_[id-1] = mesh;
    } else {
        meshes_.push_back(mesh);
        id = static_cast<unsigned>(meshes_.size());
    }
    gl_check("Mesh upload");
    return id;
}
void Renderer::release_mesh(unsigned id) noexcept {
    if (!id || id > meshes_.size() || !meshes_[id-1].vao) return;
    auto& mesh = meshes_[id-1];
    glDeleteVertexArrays(1, &mesh.vao);
    glDeleteBuffers(1, &mesh.vertices);
    glDeleteBuffers(1, &mesh.indices);
    mesh = {0, 0, 0, 0, 0, free_mesh_};
    free_mesh_ = id;
}
void Renderer::update_mesh(unsigned id, std::span<const Vertex> vertices) {
    if (!id || id > meshes_.size() || vertices.size() != meshes_[id-1].vertex_count)
        throw std::runtime_error("Invalid animated mesh update");
    glBindBuffer(GL_ARRAY_BUFFER, meshes_[id-1].vertices);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertices.size_bytes(), vertices.data());
}
unsigned Renderer::upload_texture(int width, int height, std::span<const std::uint8_t> rgba,
                                  bool mipmaps, bool flip_vertical) {
    if (width <= 0 || height <= 0 || width > max_texture_size_ || height > max_texture_size_ ||
        rgba.size() != static_cast<std::size_t>(width)*height*4)
        throw std::runtime_error("Invalid texture dimensions or RGBA byte count");
    Texture texture{0, width, height, mipmaps, flip_vertical};
    glGenTextures(1, &texture.id);
    glBindTexture(GL_TEXTURE_2D, texture.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (mipmaps && max_anisotropy_ > 1)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, anisotropy_);
    if (mipmaps) glGenerateMipmap(GL_TEXTURE_2D);
    textures_.push_back(texture);
    gl_check("Texture upload");
    return static_cast<unsigned>(textures_.size());
}
void Renderer::update_texture(unsigned id, int width, int height,
                              std::span<const std::uint8_t> rgba, int x, int y) {
    if (!id || id > textures_.size()) throw std::runtime_error("Unknown texture");
    const auto& texture = textures_[id-1];
    if (width <= 0 || height <= 0 || x < 0 || y < 0 ||
        width > texture.width || height > texture.height ||
        x > texture.width-width || y > texture.height-height ||
        rgba.size() != static_cast<std::size_t>(width)*height*4)
        throw std::runtime_error("Invalid video texture update rectangle");
    glBindTexture(GL_TEXTURE_2D, texture.id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    if (texture.mipmaps) glGenerateMipmap(GL_TEXTURE_2D);
}
void Renderer::draw(unsigned id, unsigned texture, const Material& material, const MaterialPass& pass,
                    const DrawState& state) {
    if (!id || id > meshes_.size() || !meshes_[id-1].vao || texture > textures_.size())
        throw std::runtime_error("Unknown mesh or texture");
    const auto enabled = [](unsigned capability, bool value) { if (value) glEnable(capability); else glDisable(capability); };
    enabled(GL_CULL_FACE, material.cull);
    if (material.cull) glCullFace(material.cull_face);
    enabled(GL_POLYGON_OFFSET_FILL, material.polygon_offset);
    if (material.polygon_offset)
        glPolygonOffset(material.polygon_offset_factor, material.polygon_offset_units);
    enabled(GL_BLEND, pass.blend);
    if (pass.blend) glBlendFunc(pass.blend_source, pass.blend_destination);
    enabled(GL_DEPTH_TEST, pass.depth_test);
    if (pass.depth_test) glDepthFunc(pass.depth_function);
    glDepthMask(pass.depth_write);
    glColorMask(pass.color_write[0], pass.color_write[1], pass.color_write[2], pass.color_write[3]);
    if (!model_valid_ || last_model_.values != state.model.values) {
        last_model_ = state.model;
        last_model_view_ = multiply(view_, state.model);
        model_valid_ = true;
        normal_valid_ = false;
        glUniformMatrix4fv(uniforms_[0], 1, GL_FALSE, last_model_view_.values.data());
    }
    if (state.lighting) {
        if (!normal_valid_) {
            const auto normals = normal_matrix(last_model_view_);
            glUniformMatrix3fv(uniforms_[1], 1, GL_FALSE, normals.data());
            normal_valid_ = true;
        }
        glUniform4fv(uniforms_[4], 1, state.diffuse_light.data());
    } else glUniform4fv(uniforms_[5], 1, state.color.data());
    glUniform1i(uniforms_[6], state.lighting);
    glUniform1i(uniforms_[7], state.fog);
    glUniform1i(uniforms_[8], pass.alpha_test ? pass.alpha_function : 0);
    glUniform1f(uniforms_[9], std::clamp(pass.alpha_reference, 0.f, 1.f));
    glUniform1i(uniforms_[10], texture != 0);
    if (texture) glUniform1i(uniforms_[11], textures_[texture-1].flip_vertical);
    glUniform4fv(uniforms_[12], 1, state.uv_transform.data());
    glBindTexture(GL_TEXTURE_2D, texture ? textures_[texture-1].id : 0);
    glBindVertexArray(meshes_[id-1].vao);
    glDrawElements(GL_TRIANGLES, static_cast<int>(meshes_[id-1].index_count), GL_UNSIGNED_SHORT, nullptr);
}
void Renderer::finish_frame() {
    if (frame_finished_) return;
    gl_check("Scene rendering");
    glDisable(GL_SCISSOR_TEST);
    if (samples_) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, draw_framebuffer_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_framebuffer_);
        glBlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, width_, height_);
    glUseProgram(post_program_);
    glUniform1i(copy_uniform_, GL_FALSE);
    glUniform1f(brightness_uniform_, static_cast<float>(brightness_step_));
    glBindTexture(GL_TEXTURE_2D, resolve_texture_);
    glBindVertexArray(post_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    frame_finished_ = true;
    gl_check("Frame composition");
}
void Renderer::present() { finish_frame(); sdl_check(SDL_GL_SwapWindow(window_)); }
std::vector<std::uint8_t> Renderer::capture_rgba() {
    finish_frame();
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width_)*height_*4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    gl_check("Frame capture");
    return pixels;
}
} // namespace yami
