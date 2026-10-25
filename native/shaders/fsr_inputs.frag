#version 460
#extension GL_GOOGLE_include_directive : require
layout(location=0) in vec2 vUv;
layout(location=0) out vec4 colorOut;
layout(location=1) out vec4 motionOut;
// Reactive-mask history: log2 illumination factor, linear view depth (clip W) and log2 artwork luminance.
layout(location=2) out vec4 historyOut;
layout(location=3) out float reactiveOut;
layout(set=0,binding=0) uniform sampler2D rasterColor;
layout(set=0,binding=1) uniform sampler2D worldDepth;
layout(set=0,binding=2) uniform isampler2D worldMotion;
layout(std140,set=0,binding=3) uniform Parameters {
    mat4 inverseVP, currentVP, previousVP;
    vec4 camera, extent, jitter, strengths, post, temporal;
    mat4 inversePreviousVP;
    vec4 rayBasisX;
    vec4 rayBasisY;
    vec4 rayBasisZ;
    vec4 previousRayBasisX;
    vec4 previousRayBasisY;
    vec4 previousRayBasisZ;
    vec4 previousCamera;
    vec4 environment,fog;
} p;
// The current frame's denoised temporal outputs (display.frag bindings 1, 5, 2, 6), at render size under FSR.
layout(set=0,binding=4) uniform sampler2D accumulatedGain;
layout(set=0,binding=5) uniform isampler2D accumulatedGeometry;
layout(set=0,binding=6) uniform sampler2D accumulatedReflection;
layout(set=0,binding=7) uniform sampler2D sourceSun;
// The previous FSR frame's historyOut; p.jitter.z>0 when it is valid for this frame.
layout(set=0,binding=8) uniform sampler2D previousHistory;
// sun_taa.comp's temporally filtered per-sample Sun visibility (.r), at the same render texel.
layout(set=0,binding=9) uniform sampler2D filteredSun;
#include "lit_radiance.glsl"
float luminance(vec3 c) { return dot(c,vec3(.2126,.7152,.0722)); }
void main() {
    ivec2 t=ivec2(gl_FragCoord.xy);
    // UNJITTERED position of this render sample.
    vec2 current=gl_FragCoord.xy/p.extent.xy-p.jitter.xy;
    // FSR accumulates linear lit HDR: this sample's linear artwork (temporal.frag
    // rasterAt decode) lit exactly as display.frag radianceAt composes it, by the
    // native temporal texel nearest the unjittered position, so lit edges are resolved by FSR.
    ivec2 size=textureSize(accumulatedGain,0);
    ivec2 native=clamp(ivec2(floor(current*vec2(size))),ivec2(0),size-1);
    vec3 artwork=pow(max(texelFetch(rasterColor,t,0).rgb,vec3(0)),vec3(2.2));
    vec4 h=p.inverseVP*vec4(current*2-1,texelFetch(worldDepth,t,0).r,1);
    // Fog depth of this sample's own surface (inverseVP*(ndc,1) is world/clipW): raster metadata
    // token >=0 marks a ray-geometry world surface; anything else (sky, non-ray draws) is unfogged here.
    float fogDepth=texelFetch(worldMotion,t,0).w>=0?1/h.w:0;
    // This sample's own Sun ray (Sun pass gainOut.g/.a at the same render texel, traced from its jittered
    // surface point under FSR) so FSR resolves shadow edges at their true sub-pixel position; its value is the
    // temporally filtered visibility (sun_taa.comp), which keeps the edge stable where the reactive mask drops
    // FSR's history.
    vec4 sampleSun=texelFetch(sourceSun,t,0);
    vec3 composed,reflection,sampleComposed,sampleReflection;
    colorOut=vec4(litRadiance(artwork,native,fogDepth,p.strengths.z>0 && sampleSun.a>.5?texelFetch(filteredSun,t,0).r:-1,
                              sampleComposed,sampleReflection),1);
    litRadiance(artwork,native,fogDepth,-1,composed,reflection);
    // Illumination factor lit/artwork: exact for grey composed light, and taken from the
    // denoised unjittered terms (texel Sun, not this sample's) so jitter cannot fake a shading change.
    // Reflection is relative to the artwork, floored so black artwork keeps the composed term.
    float illumination=log2(max(luminance(composed)+luminance(reflection)/max(luminance(artwork),.05),1e-4));
    if(!(abs(illumination)<64)) illumination=0;
    // Raster artwork luminance (log2), for blended effects (pickup glows, smoke) that change the artwork but
    // not the lighting. Floored at .02 linear (~.17 display): below it 8-bit texture/vertex-colour steps are
    // >=5% each and grow to huge log ratios near black, while the original fog colour (.2 display, ~.029
    // linear) stays above the floor, so effects over the darkest fogged street still register.
    float art=log2(max(luminance(artwork),.02));
    if(!(abs(art)<64)) art=0;
    // UV motion from the unjittered current centre to its previous position, and the
    // previous clip W (FSR is perspective-only: clip W is linear view depth) of this surface.
    vec3 previous=intBitsToFloat(texelFetch(worldMotion,t,0).xyz);
    vec2 motion=vec2(0);
    float previousDepth=0;
    // Raster metadata stores previous UV plus clip-W (1+depth if orthographic);
    // zero marks no depth-writing surface, which reprojects camera motion instead.
    if(previous.z>0 && !isinf(previous.z) && !any(isnan(previous.xy)) && !any(isinf(previous.xy))) {
        motion=previous.xy-current; previousDepth=previous.z;
    } else {
        vec4 clip=p.previousVP*vec4(h.xyz/h.w,1);
        if(clip.w>0) { motion=clip.xy/clip.w*.5+.5-current; previousDepth=clip.w; }
    }
    if(any(isnan(motion)) || any(isinf(motion))) { motion=vec2(0); previousDepth=0; }
    motionOut=vec4(motion,0,0);
    // inverseVP*(ndc,1) is world/clipW, so 1/h.w is this sample's clip W.
    historyOut=vec4(illumination,1/h.w,art,0);
    // Reactive: shading that changed with no matching motion (a moving shadow on static
    // ground) must not accumulate. Only a valid same-surface previous sample can say so;
    // disocclusion and invalid history stay 0 (FSR handles those itself).
    reactiveOut=0;
    vec2 previousUV=current+motion;
    if(p.jitter.z<=0 || !(previousDepth>0) || isinf(previousDepth) ||
       any(lessThan(previousUV,vec2(0))) || any(greaterThan(previousUV,vec2(1)))) return;
    // Nearest texels, not bilinear: filtering would blend illumination and depth across
    // shadow and silhouette edges. The 3x3 previous range of the same surface absorbs
    // sub-pixel jitter at static shadow/texture edges; only change beyond it is reactive.
    ivec2 historySize=textureSize(previousHistory,0);
    ivec2 centre=ivec2(floor(previousUV*vec2(historySize)));
    vec2 low=vec2(1e30),high=vec2(-1e30);
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        vec4 old=texelFetch(previousHistory,clamp(centre+ivec2(x,y),ivec2(0),historySize-1),0);
        // Same surface: previous depth within 2% of where this surface was (NaN/inf fail).
        if(!(abs(old.y-previousDepth)<=previousDepth*.02)) continue;
        low=min(low,old.xz); high=max(high,old.xz);
    }
    if(low.x>high.x) return;
    // Lighting, log2 units: a full-strength Sun shadow scales illumination by .3..~.5 (|change| 1..1.74),
    // denoised AO/GI/reflection move a few % per frame (<.14 at 10%). Changes under 19%
    // stay accumulated; a 41% change (ratio .59) and beyond is fully reactive.
    vec2 change=max(max(vec2(illumination,art)-high,low-vec2(illumination,art)),0);
    // Artwork (blended effects over a surface that did not move): stricter, because static texture under
    // sub-pixel jitter is only bracketed by the 3x3 range up to its own aliasing. Beyond the range by 41%
    // (log2 .5) it starts, by 2x (log2 1) it is fully reactive; an additive glow over fog or a dark wall
    // exceeds that at its core. Without it FI's optical flow (8x8 blocks over featureless fog) tears the
    // effect, and FSR ghosts it.
    reactiveOut=.9*max(smoothstep(.25,.75,change.x),smoothstep(.5,1,change.y));
}
