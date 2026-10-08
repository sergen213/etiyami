#version 460
layout(set=0,binding=0) uniform sampler2D artwork;
layout(set=0,binding=1,std140) uniform Draw {
    mat4 modelView, projection, normalEye, normalWorld;
    vec4 light, diffuse, color, uvTransform, worldUp, flags, alphaJitter;
    mat4 previousMVP, previousNormalWorld;
    ivec4 temporal;
} u;
layout(location=0) in vec2 vUv;
layout(location=1) in vec4 vColor;
layout(location=2) in vec3 vEye;
layout(location=3) in vec3 vNormal;
layout(location=4) in vec3 vWorldNormal;
layout(location=5) in vec4 vPreviousClip;
layout(location=6) in vec3 vPreviousNormal;
layout(location=7) in vec2 vCurrentDepth;
layout(location=0) out vec4 outColor;
#if !defined(YAMI_INTERFACE_ONLY) && !defined(YAMI_ARTIST_MASK)
layout(location=1) out ivec4 outNormal;
layout(location=2) out ivec4 outMotion;
layout(location=3) out ivec2 outPreviousNormal;
#endif
void main() {
    vec4 value=vColor;
    bool enhanced=u.flags.y>.5;
    if(u.flags.x>.5 && enhanced) {
        vec3 n=normalize(vNormal);
        float d=max(dot(n,normalize(u.light.xyz-vEye*u.light.w)),0);
        float h=dot(n,u.worldUp.xyz)*.5+.5;
        value=vec4(clamp(vec3(.416+.08*h)+.8*u.diffuse.rgb*d,0,1),1);
    }
    // Preserve the exported palette, fog and alpha compositing in the artist's
    // display-authored domain, including MSAA. The floating world attachment
    // retains emissive headroom; effects decodes the entire resolved world once.
    if(u.flags.z>.5) {
        vec4 texel=texture(artwork,vec2(vUv.x,u.worldUp.w>.5 ? 1-vUv.y:vUv.y));
        value*=texel;
    }
    float a=value.a,r=u.alphaJitter.y;
    int f=int(u.alphaJitter.x);
    bool pass=f==0 || f==519 || (f==513&&a<r)||(f==514&&a==r)||
        (f==515&&a<=r)||(f==516&&a>r)||(f==517&&a!=r)||(f==518&&a>=r);
    if(!pass) discard;
#ifdef YAMI_ARTIST_MASK
    // Alpha controls replacement of the clean bit, not opacity: any actual
    // untracked contribution dirties it; a full tracked overwrite restores it.
    // Zero-alpha blending must still execute the original depth write.
    float influence=u.temporal.w==0?0:float(u.temporal.w!=2 || a!=0);
    outColor=vec4(vec3(float(u.temporal.z)),influence);
#else
    if(u.diffuse.w<0) value.rgb=mix(vec3(.2),value.rgb,clamp((2000-abs(vEye.z))/1000,0,1));
    outColor=value;
#ifndef YAMI_INTERFACE_ONLY
    outNormal=ivec4(round(normalize(vWorldNormal+vec3(1e-20))*32767),int(u.flags.w));
    bool valid=u.temporal.y!=0 && vPreviousClip.w>0;
    bool orthographic=length(vec3(u.previousMVP[0][3],u.previousMVP[1][3],u.previousMVP[2][3]))<1e-8;
    float previousDepth=orthographic?1+vPreviousClip.z/vPreviousClip.w:vPreviousClip.w;
    outMotion=ivec4(floatBitsToInt(vec3(vPreviousClip.xy/vPreviousClip.w*.5+.5,previousDepth)),
                    u.flags.w>=0 && (u.temporal.x>0 || u.temporal.y!=0)?u.temporal.x:-1);
    vec3 priorNormal=normalize(vPreviousNormal+vec3(1e-20));
    priorNormal/=abs(priorNormal.x)+abs(priorNormal.y)+abs(priorNormal.z);
    vec2 oct=priorNormal.xy;
    if(priorNormal.z<0) oct=(1-abs(oct.yx))*mix(vec2(-1),vec2(1),greaterThanEqual(oct,vec2(0)));
    bool orthographicCurrent=length(vec3(u.projection[0][3],u.projection[1][3],u.projection[2][3]))<1e-8;
    float currentDepth=orthographicCurrent?1+vCurrentDepth.x/vCurrentDepth.y:vCurrentDepth.y;
    outPreviousNormal=ivec2(valid?int(packSnorm2x16(oct)):int(0x80008000u),
                           floatBitsToInt(currentDepth));
#endif
#endif
}
