#version 460
#ifdef YAMI_RAY_QUERY
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_nonuniform_qualifier : require
#endif
layout(location=0) in vec2 vUv;
layout(location=0) out vec4 gainOut;
layout(location=1) out vec4 reflectionOut;
layout(set=0,binding=0) uniform sampler2D worldColor;
layout(set=0,binding=1) uniform sampler2D worldDepth;
layout(set=0,binding=2) uniform isampler2D worldNormal;
layout(set=0,binding=7) uniform isampler2D surfaceMetadata;
#ifdef YAMI_MSAA_METADATA
layout(set=0,binding=8) uniform isampler2DMS finalMotion;
layout(set=0,binding=9) uniform isampler2DMS finalNormal;
layout(set=0,binding=10) uniform isampler2DMS finalCenterDepth;
layout(set=0,binding=11) uniform sampler2DMS artistMask;
#else
layout(set=0,binding=11) uniform sampler2D artistMask;
#endif
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
#ifdef YAMI_RAY_QUERY
layout(set=0,binding=4) uniform accelerationStructureEXT scene;
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer Vertices { float value[]; };
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer Indices { uint value[]; };
struct Instance {
    uint64_t vertices, indices;
    vec4 uvTransform, color, diffuse;
    uvec4 material;
    vec4 misc;
};
layout(std430,set=0,binding=5) readonly buffer Instances { Instance instance[]; };
layout(set=0,binding=6) uniform sampler2D sourceTextures[];
#endif
// Raster transforms this world light into eye space; ray normals are world-space.
const vec3 lightDirection = normalize(vec3(.5,1,.3));
float noise(vec2 s) { return fract(sin(dot(s,vec2(12.9898,78.233))+p.temporal.z*19.73)*43758.5453); }
vec4 normalAt(ivec2 texel) {
    ivec4 value=texelFetch(worldNormal,texel,0);
    return vec4(vec3(value.xyz)/32767.0,float(value.w));
}
bool orthographic(mat4 vp) {
    return length(vec3(vp[0][3],vp[1][3],vp[2][3]))<1e-8;
}
vec3 centerPosition(vec2 uv,float encodedDepth) {
    vec2 coordinate=(uv-p.jitter.xy)*2-1;
    if(p.temporal.w<=0 || orthographic(p.currentVP)) {
        float z=p.temporal.w>0?encodedDepth-1:encodedDepth;
        vec4 h=p.inverseVP*vec4(coordinate,z,1);
        return h.xyz/h.w;
    }
    // CPU-qualified camera-relative rays avoid inverse-VP translation cancellation.
    if(p.camera.w>.5)
        return p.camera.xyz+encodedDepth*(coordinate.x*p.rayBasisX.xyz+
            coordinate.y*p.rayBasisY.xyz+p.rayBasisZ.xyz);
    vec4 h=p.inverseVP*vec4(coordinate,0,1);
    vec4 depthColumn=p.inverseVP[2];
    float z=(1/encodedDepth-h.w)/depthColumn.w;
    return (h.xyz+depthColumn.xyz*z)*encodedDepth;
}
bool sourceComplete(ivec2 texel) {
#ifdef YAMI_MSAA_METADATA
    ivec4 selected=texelFetch(finalNormal,texel,0);
    int token=texelFetch(finalMotion,texel,0).w;
    vec3 normal=vec3(selected.xyz)/32767.0;
    float depth=intBitsToFloat(texelFetch(finalCenterDepth,texel,0).y);
    if(token<0 || selected.w<0 || dot(normal,normal)<=.01 ||
       isnan(depth) || isinf(depth) || depth<=0) return false;
    normal=normalize(normal);
    vec2 uv=(vec2(texel)+.5)/p.extent.xy;
    vec3 at=centerPosition(uv,depth);
    float tolerance=abs(orthographic(p.currentVP)?1:depth)*.001;
    if(any(isnan(at)) || any(isinf(at)) || isnan(tolerance) || isinf(tolerance)) return false;
    // Compare FINAL retained samples, not a primitive's coverage mask. Every
    // sample's interpolants describe the same pixel center, even on a slope.
    for(int i=1;i<textureSamples(finalNormal);++i) {
        ivec4 adjacent=texelFetch(finalNormal,texel,i);
        if(adjacent.w!=selected.w || texelFetch(finalMotion,texel,i).w!=token) return false;
        vec3 n=vec3(adjacent.xyz)/32767.0;
        if(dot(n,n)<=.01) return false;
        n=normalize(n);
        if(dot(n,normal)<(token>0?.8:.9)) return false;
        float z=intBitsToFloat(texelFetch(finalCenterDepth,texel,i).y);
        if(isnan(z) || isinf(z) || z<=0) return false;
        vec3 delta=centerPosition(uv,z)-at;
        if(any(isnan(delta)) || any(isinf(delta)) ||
           abs(dot(delta,normalize(n+normal)))>tolerance) return false;
    }
#endif
    return true;
}
bool artistClean(ivec2 texel) {
    if(p.post.z<=0) return true;
#ifdef YAMI_MSAA_METADATA
    // Averaged artwork includes every final colour sample, not only sample zero.
    for(int i=0;i<textureSamples(artistMask);++i)
        if(texelFetch(artistMask,texel,i).r<.5) return false;
    return true;
#else
    return texelFetch(artistMask,texel,0).r>.5;
#endif
}
vec3 worldPosition(vec2 uv,float depth) {
    // Visibility depth resolves sample zero; interpolation metadata is at the
    // primitive's pixel center. Keep ray origins and normals at that center.
    if(p.temporal.w>0) {
        ivec2 t=clamp(ivec2(floor(uv*p.extent.xy)),ivec2(0),ivec2(p.extent.xy)-1);
        depth=intBitsToFloat(texelFetch(surfaceMetadata,t,0).y);
    }
    return centerPosition(uv,depth);
}
vec3 hemisphere(vec3 n,vec2 random) {
    float phi=6.2831853*random.x, r=sqrt(random.y);
    vec3 t=normalize(cross(abs(n.y)<.95?vec3(0,1,0):vec3(1,0,0),n));
    return normalize(t*(cos(phi)*r)+cross(n,t)*(sin(phi)*r)+n*sqrt(1-r*r));
}
#ifdef YAMI_RAY_QUERY
void triangle(Instance m,uint primitive,vec2 bary,out vec3 normal,out vec2 uv) {
    Indices ids=Indices(m.indices); Vertices vs=Vertices(m.vertices);
    vec3 weights=vec3(1-bary.x-bary.y,bary);
    normal=vec3(0); uv=vec2(0);
    for(int k=0;k<3;++k) {
        uint at=ids.value[primitive*3+k]*8;
        uv+=vec2(vs.value[at+3],vs.value[at+4])*weights[k];
        normal+=vec3(vs.value[at+5],vs.value[at+6],vs.value[at+7])*weights[k];
    }
    uv=uv*m.uvTransform.xy+m.uvTransform.zw;
    if(m.material.w!=0) uv.y=1-uv.y;
}
bool alphaAccepted(uint fn,float a,float reference) {
    if(fn==0x200u) return false;
    if(fn==0x201u) return a<reference;
    if(fn==0x202u) return a==reference;
    if(fn==0x203u) return a<=reference;
    if(fn==0x204u) return a>reference;
    if(fn==0x205u) return a!=reference;
    if(fn==0x206u) return a>=reference;
    return true;
}
bool trace(vec3 origin,vec3 direction,float maximum,bool shade,out vec3 radiance,out float distance) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q,scene,gl_RayFlagsNoneEXT,255,origin,.035,direction,maximum);
    while(rayQueryProceedEXT(q)) {
        if(rayQueryGetIntersectionTypeEXT(q,false)!=gl_RayQueryCandidateIntersectionTriangleEXT) continue;
        uint object=rayQueryGetIntersectionInstanceCustomIndexEXT(q,false);
        Instance m=instance[object];
        bool front=rayQueryGetIntersectionFrontFaceEXT(q,false);
        // Preserve exported front/back culling rather than assuming all foliage opaque.
        if(m.misc.y!=0 && ((uint(m.misc.z)==0x405u && !front) ||
            (uint(m.misc.z)==0x404u && front) || uint(m.misc.z)==0x408u)) continue;
        if(m.material.z==0) rayQueryConfirmIntersectionEXT(q);
        else {
            vec3 n; vec2 uv;
            triangle(m,rayQueryGetIntersectionPrimitiveIndexEXT(q,false),rayQueryGetIntersectionBarycentricsEXT(q,false),n,uv);
            float alpha=textureLod(sourceTextures[nonuniformEXT(m.material.x)],uv,0).a*(m.misc.w>.5?1:m.color.a);
            if(alphaAccepted(m.material.y,alpha,m.misc.x)) rayQueryConfirmIntersectionEXT(q);
        }
    }
    if(rayQueryGetIntersectionTypeEXT(q,true)==gl_RayQueryCommittedIntersectionNoneEXT) {
        radiance=vec3(0); distance=maximum; return false;
    }
    distance=rayQueryGetIntersectionTEXT(q,true);
    if(!shade) { radiance=vec3(0); return true; }
    Instance m=instance[rayQueryGetIntersectionInstanceCustomIndexEXT(q,true)];
    vec3 n; vec2 uv;
    triangle(m,rayQueryGetIntersectionPrimitiveIndexEXT(q,true),rayQueryGetIntersectionBarycentricsEXT(q,true),n,uv);
    n=normalize(transpose(mat3(rayQueryGetIntersectionWorldToObjectEXT(q,true)))*n);
    vec3 artwork=pow(max(textureLod(sourceTextures[nonuniformEXT(m.material.x)],uv,0).rgb,vec3(0)),vec3(2.2));
    if(m.misc.w>.5) {
        float hemisphereLight=dot(n,vec3(0,1,0))*.5+.5;
        vec3 lighting=clamp(vec3(.416+.08*hemisphereLight)+.8*m.diffuse.rgb*max(dot(n,lightDirection),0),0,1);
        radiance=artwork*pow(lighting,vec3(2.2));
    } else radiance=artwork*pow(max(m.color.rgb,vec3(0)),vec3(2.2));
    return true;
}
#endif
float rasterAO(vec3 position,vec3 normal) {
    float obscured=0, valid=0;
    for(int i=0;i<12;++i) {
        float a=(float(i)+noise(gl_FragCoord.xy))*2.399963;
        vec2 uv=vUv+vec2(cos(a),sin(a))*(2+float(i)*1.8)/p.extent.xy;
        if(any(lessThan(uv,vec2(0)))||any(greaterThanEqual(uv,vec2(1)))) continue;
        ivec2 t=ivec2(floor(uv*p.extent.xy));
        float z=texelFetch(worldDepth,t,0).r; if(z>=.999999) continue;
        vec3 d=worldPosition((vec2(t)+.5)/p.extent.xy,z)-position;
        float len=length(d);
        obscured+=step(.05,dot(normal,d))*max(0,1-len/12.0); valid+=1;
    }
    return valid>0?1-obscured/valid:1;
}
vec3 screenReflection(vec3 origin,vec3 direction) {
    // Only an actual depth intersection contributes; screen misses are black.
    float travel=.35;
    for(int i=0;i<28;++i) {
        travel+=.3+float(i)*.12;
        vec3 at=origin+direction*travel;
        vec4 clip=p.currentVP*vec4(at,1); if(clip.w<=0) break;
        vec2 uv=clip.xy/clip.w*.5+.5+p.jitter.xy;
        if(any(lessThan(uv,vec2(0)))||any(greaterThanEqual(uv,vec2(1)))) break;
        ivec2 size=textureSize(worldDepth,0);
        ivec2 texel=ivec2(floor(uv*vec2(size)));
        float depth=texelFetch(worldDepth,texel,0).r;
        vec4 hitNormal=normalAt(texel);
        if(depth>=1 || hitNormal.w<0 || length(hitNormal.xyz)<.1) continue;
        vec3 target=worldPosition((vec2(texel)+.5)/vec2(size),depth);
        float gap=length(target-p.camera.xyz)-length(at-p.camera.xyz);
        if(gap<0 && gap>-(.3+travel*.025) && dot(hitNormal.xyz,direction)<-.05)
            return pow(max(texelFetch(worldColor,texel,0).rgb,vec3(0)),vec3(2.2));
    }
    return vec3(0);
}
void main() {
    ivec2 texel=ivec2(gl_FragCoord.xy);
    float depth=texelFetch(worldDepth,texel,0).r;
    vec4 validity=normalAt(texel);
    float complete=sourceComplete(texel)?(artistClean(texel)?1:2):0;
    gainOut=vec4(1,1,1,1); reflectionOut=vec4(0,0,0,complete);
    if(depth>=.999999 || validity.w<0 || length(validity.xyz)<.1) return;
    vec3 position=worldPosition(vUv,depth), n=normalize(validity.xyz);
    vec3 view=normalize(p.camera.xyz-position);
    float ao=1, visibility=1; vec3 reflected=vec3(0), indirect=vec3(0);
    vec2 random=vec2(noise(gl_FragCoord.xy),noise(gl_FragCoord.yx+17));
    vec3 reflectedDirection=normalize(mix(reflect(-view,n),hemisphere(n,random),p.post.w*p.post.w));
#ifdef YAMI_RAY_QUERY
    vec3 bounce; float hitDistance;
    if(p.strengths.z>0 && trace(position+n*.06,lightDirection,2000,false,bounce,hitDistance)) visibility=0;
    if(p.strengths.x>0 || p.strengths.w>0) {
        vec3 direction=hemisphere(n,random);
        bool hit=trace(position+n*.06,direction,60,p.strengths.w>0,bounce,hitDistance);
        if(hit) { ao=1-max(0,1-hitDistance/10); indirect=bounce*.32; }
    }
    if(p.strengths.y>0) trace(position+n*.06,reflectedDirection,2000,true,reflected,hitDistance);
#else
    if(p.strengths.x>0) ao=rasterAO(position,n);
    if(p.strengths.y>0) reflected=screenReflection(position+n*.1,reflectedDirection);
#endif
    float fresnel=.04+.96*pow(1-max(dot(n,view),0),5);
    // Keep multiplicative illumination independent of authored texture colors.
    // This is the exact lighting equation, not a radiance/base division (which
    // loses information on black texels and starves textured actor kernels).
    vec3 gain=vec3(mix(1,ao,p.strengths.x)*mix(1,.3+.7*visibility,p.strengths.z));
    gain+=indirect*p.strengths.w;
    gainOut=vec4(max(gain,vec3(0)),visibility);
    reflectionOut=vec4(max(reflected*p.strengths.y*fresnel*(1-.65*p.post.w),vec3(0)),complete);
}
