// Shared by effects.frag, effects_rt.comp and effects_resolve.vert. effects.frag queues each
// history-unsafe pixel after its first RT sample; effects_rt.comp continues samples 1..15 from the
// stored partial sums with this same code, and effects_resolve writes its final values through the
// same colour export. One source keeps every evaluated expression and its order identical.
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
#ifdef YAMI_RAY_QUERY
layout(set=0,binding=4) uniform accelerationStructureEXT scene;
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer Vertices { float value[]; };
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer Indices { uint value[]; };
struct Instance {
    uint64_t vertices, indices;
    vec4 uvTransform, color, diffuse;
    uvec4 material;
    vec4 misc;
    // Last frame's pose (frame-generation shadow motion): vertex buffer and object-to-world rows.
    uint64_t previousVertices, unused;
    vec4 previousModel[3];
};
layout(std430,set=0,binding=5) readonly buffer Instances { Instance instance[]; };
layout(set=0,binding=6) uniform sampler2D sourceTextures[];
layout(std430,set=0,binding=14) readonly buffer LocalLights { vec4 localLight[]; };
layout(std430,set=0,binding=15) readonly buffer LocalLightTiles { uint localTile[]; };
#endif
#ifdef YAMI_PENDING
#ifndef YAMI_PENDING_ACCESS
#define YAMI_PENDING_ACCESS
#endif
#define YAMI_PENDING_GROUP 64u
#define YAMI_PENDING_GROUPS 65535u // maxComputeWorkGroupCount[0] minimum
struct Pending {
    vec4 gain;       // indirectSum, aoSum
    vec4 reflection; // reflected sum, complete
    vec4 origin;     // ray origin, fresnel
    vec4 physical;   // physical normal, random.x
    vec4 mirror;     // mirror direction, random.y
    vec2 pixel;      // gl_FragCoord.xy
    uint stable, unused;
};
// groups: VkDispatchIndirectCommand (offset 0); count..: VkDrawIndirectCommand (offset 12).
layout(std430,set=1,binding=0) YAMI_PENDING_ACCESS buffer PendingQueue {
    uint groups, groupsY, groupsZ, count, instanceCount, firstVertex, firstInstance;
} queue;
layout(std430,set=1,binding=1) YAMI_PENDING_ACCESS buffer PendingPixels { Pending pending[]; };
#endif
// Raster transforms this world light into eye space; ray normals are world-space.
const vec3 lightDirection = normalize(vec3(.5,1,.3));
float noise(vec2 s) { return fract(sin(dot(s,vec2(12.9898,78.233))+p.temporal.z*19.73)*43758.5453); }
vec3 hemisphere(vec3 n,vec2 random) {
    float phi=6.2831853*random.x, r=sqrt(random.y);
    vec3 t=normalize(cross(abs(n.y)<.95?vec3(0,1,0):vec3(1,0,0),n));
    return normalize(t*(cos(phi)*r)+cross(n,t)*(sin(phi)*r)+n*sqrt(1-r*r));
}
#if defined(YAMI_RAY_QUERY) && !defined(YAMI_SOURCE_CLASSIFY)
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
// Only alpha-tested instances are non-opaque, so every candidate needs this test. Exported face culling is traversal
// culling: build_scene encodes it in instance facing/cull flags/mask for gl_RayFlagsCullBackFacingTrianglesEXT.
bool alphaHit(uint object,uint primitive,vec2 bary) {
    Instance m=instance[object];
    vec3 n; vec2 uv;
    triangle(m,primitive,bary,n,uv);
    float alpha=textureLod(sourceTextures[nonuniformEXT(m.material.x)],uv,0).a*(m.misc.w>.5?1:m.color.a);
    return alphaAccepted(m.material.y,alpha,m.misc.x);
}
// The committed opaque/alpha-accepted triangle, shared by every RT query.
bool intersect(vec3 origin,vec3 direction,float minimum,float maximum,out uint object,out uint primitive,
               out vec2 bary,out float distance,out mat4x3 toWorld,out mat4x3 toObject) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q,scene,gl_RayFlagsCullBackFacingTrianglesEXT,255,origin,minimum,direction,maximum);
    while(rayQueryProceedEXT(q))
        if(alphaHit(rayQueryGetIntersectionInstanceCustomIndexEXT(q,false),rayQueryGetIntersectionPrimitiveIndexEXT(q,false),
                    rayQueryGetIntersectionBarycentricsEXT(q,false))) rayQueryConfirmIntersectionEXT(q);
    if(rayQueryGetIntersectionTypeEXT(q,true)==gl_RayQueryCommittedIntersectionNoneEXT) return false;
    object=rayQueryGetIntersectionInstanceCustomIndexEXT(q,true);
    primitive=rayQueryGetIntersectionPrimitiveIndexEXT(q,true);
    bary=rayQueryGetIntersectionBarycentricsEXT(q,true);
    distance=rayQueryGetIntersectionTEXT(q,true);
    toWorld=rayQueryGetIntersectionObjectToWorldEXT(q,true);
    toObject=rayQueryGetIntersectionWorldToObjectEXT(q,true);
    return true;
}
bool trace(vec3 origin,vec3 direction,float maximum,bool shade,out vec3 radiance,out float distance) {
    uint object,primitive; vec2 bary; mat4x3 toWorld,toObject;
    radiance=vec3(0);
    if(!intersect(origin,direction,.035,maximum,object,primitive,bary,distance,toWorld,toObject)) {
        distance=maximum; return false;
    }
    if(!shade) return true;
    Instance m=instance[object];
    vec3 n; vec2 uv;
    triangle(m,primitive,bary,n,uv);
    n=normalize(transpose(mat3(toObject))*n);
    vec3 displayArt=max(textureLod(sourceTextures[nonuniformEXT(m.material.x)],uv,0).rgb,vec3(0));
    vec3 artwork=pow(displayArt,vec3(2.2));
    vec3 windows=m.misc.z*vec3(1,.55,.2)*(.35+.65*max(displayArt.r,max(displayArt.g,displayArt.b)))*
        smoothstep(.12,.35,max(displayArt.r,max(displayArt.g,displayArt.b)))*smoothstep(.005,.03,displayArt.b-max(displayArt.r,displayArt.g));
    if(m.misc.w>.5) {
        float hemisphereLight=dot(n,vec3(0,1,0))*.5+.5;
        vec3 lighting=clamp(vec3(.416+.08*hemisphereLight)*p.environment.x+.8*m.diffuse.rgb*max(dot(n,lightDirection),0)*p.environment.y,0,1);
        radiance=artwork*pow(lighting,vec3(2.2))+artwork*m.misc.y*vec3(1,.55,.2)+windows;
    } else radiance=artwork*pow(max(m.color.rgb,vec3(0)),vec3(2.2))+artwork*m.misc.y*vec3(1,.55,.2)+windows;
    return true;
}
#endif
#if defined(YAMI_RAY_QUERY) && !defined(YAMI_SUN_PASS) && !defined(YAMI_SOURCE_CLASSIFY)
vec2 stratifiedRandom(int index,vec2 random,vec2 pixel) {
    if(index>0) random=vec2(noise(pixel+vec2(index*23,index*47)),
        noise(pixel.yx+17+vec2(index*61,index*13)));
    return (vec2(index%4,index/4)+random)*.25;
}
// Sample i of a pixel's diffuse/reflection estimate; pixel is its gl_FragCoord.xy.
void effectSample(int i,vec2 pixel,bool stochastic,bool stable,bool diffuse,bool reflectionActive,int reflectionCount,
                  vec3 origin,vec3 physical,vec3 mirror,vec2 random,inout float aoSum,inout vec3 indirectSum,inout vec3 reflected) {
    vec3 bounce; float hitDistance;
    vec3 direction=physical;
    if(stochastic) {
        vec2 u=stable?random:stratifiedRandom(i,random,pixel);
        direction=hemisphere(physical,u);
    }
    if(diffuse) {
        bool hit=trace(origin,direction,60,p.strengths.w>0,bounce,hitDistance);
        aoSum+=hit?1-max(0,1-hitDistance/10):1;
        if(hit) indirectSum+=bounce*.32;
    }
    if(reflectionActive && i<reflectionCount) {
        vec3 reflectedDirection=normalize(mix(mirror,direction,p.post.w*p.post.w));
        vec3 radiance;
        trace(origin,reflectedDirection,2000,true,radiance,hitDistance);
        reflected+=radiance;
    }
}
void rtOutputs(bool diffuse,bool reflectionActive,int sampleCount,int reflectionCount,float aoSum,vec3 indirectSum,
               vec3 reflected,float fresnel,float complete,out vec4 gain,out vec4 reflection) {
    float ao=1; vec3 indirect=vec3(0);
    if(diffuse) {
        ao=aoSum/float(sampleCount); indirect=indirectSum/float(sampleCount);
    }
    if(reflectionActive) reflected/=float(reflectionCount);
    // Independent stochastic signals; deterministic Sun is evaluated on its
    // own fixed source grid and never multiplied into their history.
    gain=vec4(max(indirect*p.strengths.w,vec3(0)),mix(1,ao,p.strengths.x));
    reflection=vec4(max(reflected*p.strengths.y*fresnel*(1-.65*p.post.w),vec3(0)),complete);
}
#endif
