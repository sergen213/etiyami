#version 460
layout(set=0,binding=0) uniform sampler2D artwork;
layout(set=0,binding=1,std140) uniform Draw {
    mat4 modelView, projection, normalEye, normalWorld;
    vec4 light, diffuse, color, uvTransform, worldUp, flags, alphaJitter;
} u;
layout(location=0) in vec2 vUv;
layout(location=1) in vec4 vColor;
layout(location=2) in vec3 vEye;
layout(location=3) in vec3 vNormal;
layout(location=4) in vec3 vWorldNormal;
layout(location=0) out vec4 outColor;
#ifndef YAMI_INTERFACE_ONLY
layout(location=1) out ivec4 outNormal;
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
    if(u.diffuse.w<0) value.rgb=mix(vec3(.2),value.rgb,clamp((2000-abs(vEye.z))/1000,0,1));
    outColor=value;
#ifndef YAMI_INTERFACE_ONLY
    outNormal=ivec4(round(normalize(vWorldNormal+vec3(1e-20))*32767),int(u.flags.w));
#endif
}
