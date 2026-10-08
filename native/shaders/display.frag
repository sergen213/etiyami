#version 460
layout(location=0) in vec2 vUv;
layout(location=0) out vec4 displayOut;
layout(set=0,binding=0) uniform sampler2D accumulatedBase;
layout(set=0,binding=1) uniform sampler2D accumulatedGain;
layout(set=0,binding=2) uniform sampler2D accumulatedReflection;
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
} p;
layout(set=0,binding=4) uniform sampler2D rasterColor;
vec3 toneMap(vec3 hdr) {
    hdr=max(hdr*p.post.x,vec3(0));
    // Preserve the authored palette below the neutral HDR shoulder and scale
    // highlights uniformly, rather than bleaching channels independently.
    float peak=max(max(hdr.r,hdr.g),hdr.b);
    if(peak>.8) hdr*=(1-.04/(peak-.6))/peak;
    return pow(hdr,vec3(1.0/2.2));
}
vec3 radianceAt(ivec2 t) {
    return texelFetch(accumulatedBase,t,0).rgb*texelFetch(accumulatedGain,t,0).rgb+
        texelFetch(accumulatedReflection,t,0).rgb;
}
void main() {
    ivec2 size=textureSize(accumulatedBase,0),center=ivec2(gl_FragCoord.xy);
    vec4 base=texelFetch(accumulatedBase,center,0);
    vec3 bloom=vec3(0); float bloomWeight=0;
    for(int y=-2;y<=2 && p.post.y>0;++y) for(int x=-2;x<=2;++x) {
        ivec2 t=clamp(center+ivec2(x,y),ivec2(0),size-1);
        float w=exp(-float(x*x+y*y)*.45);
        vec3 value=radianceAt(t);
        float peak=max(max(value.r,value.g),value.b);
        bloom+=value*max(0,peak-1)/max(peak,.001)*w; bloomWeight+=w;
    }
    // The old neutral filter difference is zero for deterministic artwork.
    // Preserve the base; adding noisy radiance back defeats illumination history.
    vec3 hdr=base.rgb*texelFetch(accumulatedGain,center,0).rgb+
        texelFetch(accumulatedReflection,center,0).rgb;
    if(bloomWeight>0) hdr+=bloom/bloomWeight*p.post.y*.22;
    vec2 sourceUV=vUv+(p.temporal.y>0?p.jitter.xy:vec2(0));
    float alpha;
    if(p.temporal.y>0) alpha=texture(rasterColor,sourceUV).a;
    else {
        ivec2 t=clamp(ivec2(floor(sourceUV*p.extent.xy)),ivec2(0),ivec2(p.extent.xy)-1);
        alpha=texelFetch(rasterColor,t,0).a;
    }
    displayOut=vec4(toneMap(hdr),alpha);
}
