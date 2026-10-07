#version 460
layout(location=0) in vec2 vUv;
layout(location=0) out vec4 historyOut;
layout(location=1) out vec4 geometryOut;
layout(location=2) out vec4 displayOut;
layout(set=0,binding=0) uniform sampler2D currentColor;
layout(set=0,binding=1) uniform sampler2D worldDepth;
layout(set=0,binding=2) uniform isampler2D worldNormal;
layout(std140,set=0,binding=3) uniform Parameters {
    mat4 inverseVP, currentVP, previousVP;
    vec4 camera, extent, jitter, strengths, post, temporal;
} p;
layout(set=0,binding=4) uniform sampler2D historyColor;
layout(set=0,binding=5) uniform sampler2D historyGeometry;
layout(set=0,binding=6) uniform sampler2D rasterColor;
vec4 normalAt(ivec2 texel) {
    ivec4 value=texelFetch(worldNormal,texel,0);
    return vec4(vec3(value.xyz)/32767.0,float(value.w));
}
vec3 rasterAt(ivec2 texel) {
    return pow(max(texelFetch(rasterColor,texel,0).rgb,vec3(0)),vec3(2.2));
}
vec3 position(vec2 uv,float z) {
    vec4 h=p.inverseVP*vec4((uv-p.jitter.xy)*2-1,z,1);
    return h.xyz/h.w;
}
vec3 toneMap(vec3 hdr) {
    hdr=max(hdr*p.post.x,vec3(0));
    // Keep ordinary display-authored artwork neutral, with a smooth HDR
    // shoulder only above .8 linear. One scale preserves highlight hue;
    // channelwise filmic curves instead lift and bleach the legacy palette.
    float peak=max(max(hdr.r,hdr.g),hdr.b);
    if(peak>.8) hdr*=(1-.04/(peak-.6))/peak;
    return pow(hdr,vec3(1.0/2.2));
}
void main() {
    ivec2 size=textureSize(currentColor,0);
    bool taa=p.temporal.y>0;
    // Resolve raster jitter into a fixed native pixel grid. Coverage belongs to
    // this current-frame footprint, not to the denoiser's selected surface.
    vec2 sampleUV=vUv+(taa?p.jitter.xy:vec2(0));
    vec2 sourcePixel=sampleUV*vec2(size);
    ivec2 reference=clamp(ivec2(floor(sourcePixel)),ivec2(0),size-1);
    vec2 referenceUV=(vec2(reference)+.5)/vec2(size);
    vec4 n=normalAt(reference);
    float z=texelFetch(worldDepth,reference,0).r;
    vec4 original=texelFetch(currentColor,reference,0);
    vec4 current=original;
    float authoredAlpha=texelFetch(rasterColor,reference,0).a;
    vec3 coverageLo=original.rgb, coverageHi=original.rgb;
    bool historyEligible=n.w>.5;
    float worldCoverage=0;
    if(taa) {
        ivec2 base=ivec2(floor(sourcePixel-.5));
        vec2 phase=fract(sourcePixel-.5);
        current=vec4(0);
        authoredAlpha=0;
        for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
            ivec2 texel=clamp(base+ivec2(x,y),ivec2(0),size-1);
            float w=(x==0?1-phase.x:phase.x)*(y==0?1-phase.y:phase.y);
            vec4 value=texelFetch(currentColor,texel,0);
            current+=value*w;
            authoredAlpha+=texelFetch(rasterColor,texel,0).a*w;
            if(w<=.00001) continue;
            coverageLo=min(coverageLo,value.rgb); coverageHi=max(coverageHi,value.rgb);
            vec4 adjacent=normalAt(texel);
            float depth=texelFetch(worldDepth,texel,0).r;
            if(depth<1 && length(adjacent.xyz)>.1) {
                worldCoverage+=w;
                // A moving/excluded surface may contribute current coverage,
                // but cannot be carried by the static reference's history.
                if(adjacent.w<=.5) historyEligible=false;
            }
        }
    }
    bool world=n.w>=0 && z<1 && length(n.xyz)>.1;
    bool neutral=!taa && !any(greaterThan(p.strengths,vec4(0))) && p.post.y<=0;
    if(!world || neutral) {
        // All enhanced-world input is linear, including excluded sky; UI is
        // composited later. Exclusion affects effects/history, not tone mapping.
        historyOut=vec4(current.rgb,0); geometryOut=vec4(0);
        displayOut=vec4(toneMap(current.rgb),authoredAlpha); return;
    }
    vec3 baseColor=rasterAt(reference);
    vec3 normal=normalize(n.xyz), referenceAt=position(referenceUV,z);
    // Reconstruct the native pixel on the selected source surface, not at the
    // source texel's center: otherwise upscaling loses the native history phase.
    vec3 ray=position(sampleUV,z)-p.camera.xyz;
    float denominator=dot(ray,normal);
    vec3 at=abs(denominator)>1e-6?
        p.camera.xyz+ray*(dot(referenceAt-p.camera.xyz,normal)/denominator):referenceAt;
    float linearDepth=(p.currentVP*vec4(at,1)).w;
    float surfaceTolerance=linearDepth*.001;
    vec3 sum=vec3(0);
    vec3 illuminationLo=original.rgb-baseColor, illuminationHi=illuminationLo;
    float weight=0, surfaceCoverage=0;
    vec3 surfaceColor=vec3(0), surfaceBase=vec3(0);
    vec3 bloom=vec3(0); float bloomWeight=0;
    // Fetch and gate each source texel BEFORE interpolating any color.
    // Excluded sky and unrelated surfaces cannot share the world filter kernel.
    for(int y=-2;y<=2;++y) for(int x=-2;x<=2;++x) {
        ivec2 texel=reference+ivec2(x,y);
        if(any(lessThan(texel,ivec2(0))) || any(greaterThanEqual(texel,size))) continue;
        vec4 adjacent=normalAt(texel);
        float depth=texelFetch(worldDepth,texel,0).r;
        if(adjacent.w<0 || depth>=1 || length(adjacent.xyz)<.1) continue;
        float similarity=dot(normal,normalize(adjacent.xyz));
        if(similarity<.9) continue;
        vec2 uv=(vec2(texel)+.5)/vec2(size);
        float separation=abs(dot(position(uv,depth)-referenceAt,normal));
        if(separation>surfaceTolerance) continue;
        vec2 offset=vec2(texel)+.5-sourcePixel;
        float w=exp(-dot(offset,offset)*.45)*pow(similarity,32)*
            exp(-separation/max(surfaceTolerance,1e-6));
        vec3 value=texelFetch(currentColor,texel,0).rgb;
        vec3 adjacentBase=rasterAt(texel);
        // Only illumination is noisy. Keep the resolved texture/tint/fog base
        // untouched, and stop illumination from crossing its authored edges.
        vec3 difference=adjacentBase-baseColor;
        float illuminationWeight=w*exp(-64*dot(difference,difference)/
            max(max(dot(baseColor,baseColor),dot(adjacentBase,adjacentBase)),.0001));
        if(taa) {
            vec2 tent=max(vec2(0),1-abs(offset));
            float coverage=tent.x*tent.y;
            surfaceColor+=value*coverage; surfaceCoverage+=coverage;
            surfaceBase+=adjacentBase*coverage;
        }
        sum+=(value-adjacentBase)*illuminationWeight; weight+=illuminationWeight;
        float bright=max(max(value.r,value.g),value.b);
        bloom+=value*max(0,bright-1)/max(bright,.001)*w; bloomWeight+=w;
        if(illuminationWeight>.1) {
            illuminationLo=min(illuminationLo,value-adjacentBase);
            illuminationHi=max(illuminationHi,value-adjacentBase);
        }
    }
    vec3 filtered=weight>.0001?max(vec3(0),baseColor+sum/weight):original.rgb;
    if(taa) {
        // Replace only this surface's current radiance with its denoised value.
        // Sky/opposite-surface coverage is reconstructed from current HDR,
        // never admitted to the surface denoiser or fetched from old neighbors.
        filtered=max(vec3(0),current.rgb-surfaceColor+surfaceBase+(filtered-baseColor)*surfaceCoverage);
        historyEligible=historyEligible && surfaceCoverage>=worldCoverage-.00001;
    }
    // History must not turn camera motion into lagging coplanar artwork.
    // Its allowable variation is noisy illumination around THIS frame's
    // reconstructed raster base, not neighboring deterministic texture colors.
    float coverage=taa?surfaceCoverage:1;
    vec3 resolvedBase=taa?current.rgb-surfaceColor+surfaceBase:baseColor;
    vec3 lo=min(filtered,max(vec3(0),resolvedBase+illuminationLo*coverage));
    vec3 hi=max(filtered,max(vec3(0),resolvedBase+illuminationHi*coverage));
    if(taa && surfaceCoverage<.99999) {
        // Actual mixed geometry/sky footprints still accumulate their raster
        // silhouette coverage. Interior material edges do not borrow that range.
        lo=min(lo,coverageLo); hi=max(hi,coverageHi);
    }
    vec3 accumulated=filtered;
    // Effects alpha carries deterministic hard-shadow visibility, not artwork
    // opacity. Reject stale lighting independently of stationary geometry.
    float historyTag=.75+.25*current.a;
    if(p.temporal.x>0 && historyEligible) {
        vec4 clip=p.previousVP*vec4(at,1);
        // History is the resolved, unjittered native grid. Color and geometry
        // remain a single nearest sample with strict linear-depth validation.
        vec2 uv=clip.xy/clip.w*.5+.5;
        if(clip.w>0 && all(greaterThanEqual(uv,vec2(0))) && all(lessThan(uv,vec2(1)))) {
            ivec2 oldTexel=ivec2(floor(uv*vec2(textureSize(historyColor,0))));
            vec4 oldGeometry=texelFetch(historyGeometry,oldTexel,0);
            vec4 oldColor=texelFetch(historyColor,oldTexel,0);
            bool agrees=oldColor.a>.5 && abs(oldColor.a-historyTag)<.01 &&
                dot(oldGeometry.xyz,normal)>.9 && oldGeometry.w>0 &&
                abs(oldGeometry.w-clip.w)<min(oldGeometry.w,clip.w)*.001;
            if(agrees) {
                vec3 clipped=clamp(oldColor.rgb,lo,hi);
                // TAA uses a longer accumulation; RT denoising retains a short
                // history without forcing temporal antialiasing on the user.
                float retain=p.temporal.y>0?.88:.65;
                accumulated=mix(filtered,clipped,retain);
            }
        }
    }
    historyOut=vec4(accumulated,historyEligible?historyTag:0);
    // RGBA32F preserves linear eye depth at far distances; there is no nonlinear
    // clip-depth floor or silhouette-gradient relaxation in history validation.
    geometryOut=vec4(normal,linearDepth);
    vec3 sharpened=max(vec3(0),accumulated+(current.rgb-filtered)*p.post.z*.35);
    if(bloomWeight>0) sharpened+=bloom/bloomWeight*p.post.y*.22;
    displayOut=vec4(toneMap(sharpened),authoredAlpha);
}
