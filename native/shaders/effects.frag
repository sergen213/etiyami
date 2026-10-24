#version 460
#extension GL_GOOGLE_include_directive : require
#ifdef YAMI_RAY_QUERY
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_nonuniform_qualifier : require
#endif
layout(location=0) in vec2 vUv;
layout(location=0) out vec4 gainOut;
#if !defined(YAMI_SUN_PASS) && !defined(YAMI_SOURCE_CLASSIFY)
layout(location=1) out vec4 reflectionOut;
#endif
#ifdef YAMI_RAY_QUERY
layout(set=0,binding=12) uniform isampler2D sourceMotion;
#if !defined(YAMI_SUN_PASS) && !defined(YAMI_SOURCE_CLASSIFY)
layout(set=0,binding=13) uniform sampler2D classifiedSource;
#endif
#endif
layout(set=0,binding=0) uniform sampler2D worldColor;
layout(set=0,binding=1) uniform sampler2D worldDepth;
layout(set=0,binding=2) uniform isampler2D worldNormal;
layout(set=0,binding=7) uniform isampler2D surfaceMetadata;
#ifdef YAMI_MSAA_METADATA
layout(set=0,binding=8) uniform isampler2DMS finalMotion;
layout(set=0,binding=9) uniform isampler2DMS finalNormal;
layout(set=0,binding=10) uniform isampler2DMS finalCenterDepth;
#endif
#if defined(YAMI_RAY_QUERY) && !defined(YAMI_SUN_PASS) && !defined(YAMI_SOURCE_CLASSIFY)
#define YAMI_PENDING
#endif
#include "effects_rt.glsl"
vec4 normalAt(ivec2 texel) {
    ivec4 value=texelFetch(worldNormal,texel,0);
    return vec4(vec3(value.xyz)/32767.0,float(value.w));
}
bool physicalNormal(int encoded,out vec3 normal) {
    if(encoded==int(0x80008000u)) return false;
    vec2 oct=unpackSnorm2x16(uint(encoded));
    normal=vec3(oct,1-abs(oct.x)-abs(oct.y));
    if(normal.z<0) normal.xy=(1-abs(normal.yx))*
        mix(vec2(-1),vec2(1),greaterThanEqual(normal.xy,vec2(0)));
    normal=normalize(normal);
    return !any(isnan(normal)) && !any(isinf(normal));
}
bool physicalNormalAt(ivec2 texel,out vec3 normal) {
    if(p.temporal.w>0)
        return physicalNormal(texelFetch(surfaceMetadata,texel,0).z,normal);
    // Direct effects callers without motion metadata expose only their normal.
    normal=normalize(normalAt(texel).xyz);
    return !any(isnan(normal)) && !any(isinf(normal));
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
    vec3 physical;
    if(p.temporal.w>0 && !physicalNormalAt(texel,physical)) return false;
#ifdef YAMI_MSAA_METADATA
    ivec4 selected=texelFetch(finalNormal,texel,0);
    int token=texelFetch(finalMotion,texel,0).w;
    vec3 normal=vec3(selected.xyz)/32767.0;
    ivec4 selectedSurface=texelFetch(finalCenterDepth,texel,0);
    if(!physicalNormal(selectedSurface.z,physical)) return false;
    float depth=intBitsToFloat(selectedSurface.y);
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
        ivec4 adjacentSurface=texelFetch(finalCenterDepth,texel,i);
        vec3 adjacentPhysical;
        if(!physicalNormal(adjacentSurface.z,adjacentPhysical) ||
           dot(adjacentPhysical,physical)<(token>0?.8:.9)) return false;
        float z=intBitsToFloat(adjacentSurface.y);
        if(isnan(z) || isinf(z) || z<=0) return false;
        vec3 delta=centerPosition(uv,z)-at;
        if(any(isnan(delta)) || any(isinf(delta)) ||
           abs(dot(delta,normalize(adjacentPhysical+physical)))>tolerance) return false;
    }
#endif
    return true;
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
#if defined(YAMI_RAY_QUERY) && !defined(YAMI_SOURCE_CLASSIFY)
// Hit/miss only (Sun visibility): the same accepted triangles as trace(), but the first one ends traversal.
// On a hit, object/primitive/bary/toWorld name that committed (first accepted, not necessarily nearest) triangle.
bool occluded(vec3 origin,vec3 direction,float maximum,out uint object,out uint primitive,out vec2 bary,out mat4x3 toWorld) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q,scene,gl_RayFlagsTerminateOnFirstHitEXT|gl_RayFlagsCullBackFacingTrianglesEXT,255,origin,.035,direction,maximum);
    while(rayQueryProceedEXT(q))
        if(alphaHit(rayQueryGetIntersectionInstanceCustomIndexEXT(q,false),rayQueryGetIntersectionPrimitiveIndexEXT(q,false),
                    rayQueryGetIntersectionBarycentricsEXT(q,false))) rayQueryConfirmIntersectionEXT(q);
    if(rayQueryGetIntersectionTypeEXT(q,true)==gl_RayQueryCommittedIntersectionNoneEXT) return false;
    object=rayQueryGetIntersectionInstanceCustomIndexEXT(q,true);
    primitive=rayQueryGetIntersectionPrimitiveIndexEXT(q,true);
    bary=rayQueryGetIntersectionBarycentricsEXT(q,true);
    toWorld=rayQueryGetIntersectionObjectToWorldEXT(q,true);
    return true;
}
// Shadow terminator (Hanika, "Hacking the Shadow Terminator", 2021): a point on a flat facet of a
// smooth-shaded low-poly model is lifted onto the curved surface its authored vertex normals imply,
// so Sun rays stop shadowing each facet that merely tilts away from the light. Flat geometry
// (vertex normals equal to the facet) is unchanged. Returns the lift for this pixel's surface point
// `at`; nearby points on the same facet (the fixed-grid Sun point) reuse it.
vec3 smoothSurfaceLift(vec3 at,vec3 authored,vec3 physical) {
    if(dot(authored,physical)>.99999) return vec3(0);
    vec3 toSurface=at-p.camera.xyz; float length_=length(toSurface);
    uint object,primitive; vec2 bary; float distance; mat4x3 toWorld,toObject;
    if(!(length_>0) || !intersect(p.camera.xyz,toSurface/length_,0,length_*1.01+1,object,primitive,bary,distance,toWorld,toObject) ||
       abs(distance-length_)>length_*.005+.5) return vec3(0);
    Instance m=instance[object];
    if(m.misc.w<=.5) return vec3(0);
    Indices ids=Indices(m.indices); Vertices vs=Vertices(m.vertices);
    mat3 normalToWorld=transpose(mat3(toObject));
    vec3 v[3],n[3];
    for(int k=0;k<3;++k) {
        uint index=ids.value[primitive*3+k]*8;
        v[k]=toWorld*vec4(vs.value[index],vs.value[index+1],vs.value[index+2],1);
        n[k]=normalize(normalToWorld*vec3(vs.value[index+5],vs.value[index+6],vs.value[index+7]));
        // Orient like the facet, which raster metadata already aligns with the authored side.
        if(dot(n[k],physical)<0) n[k]=-n[k];
    }
    vec3 hit=p.camera.xyz+toSurface*(distance/length_),weights=vec3(1-bary.x-bary.y,bary),lift=vec3(0);
    for(int k=0;k<3;++k) lift-=weights[k]*min(0,dot(hit-v[k],n[k]))*n[k];
    return any(isnan(lift)) || any(isinf(lift))?vec3(0):lift;
}
#endif
#if defined(YAMI_RAY_QUERY) && !defined(YAMI_SOURCE_CLASSIFY)
bool sunSourceComplete(ivec2 t) {
#ifdef YAMI_SUN_PASS
    return mod(texelFetch(worldColor,t,0).a,4.0)>.5;
#else
    return mod(round(texelFetch(classifiedSource,t,0).r*255),4.0)>.5;
#endif
}
bool sunSourceAgrees(ivec2 t,vec3 at,vec3 authored,vec3 physical,
                     int owner,int token,float tolerance) {
    if(any(lessThan(t,ivec2(0))) || any(greaterThanEqual(t,ivec2(p.extent.xy)))) return false;
    vec4 n=normalAt(t); vec3 adjacentPhysical;
    if(texelFetch(worldDepth,t,0).r>=.999999 || n.w!=float(owner) ||
       dot(n.xyz,n.xyz)<=.01 || !sunSourceComplete(t) ||
       (p.temporal.w>0 && texelFetch(sourceMotion,t,0).w!=token) ||
       !physicalNormalAt(t,adjacentPhysical)) return false;
    float threshold=token>0?.8:.9;
    if(dot(normalize(n.xyz),authored)<threshold || dot(adjacentPhysical,physical)<threshold) return false;
    float depth=p.temporal.w>0?intBitsToFloat(texelFetch(surfaceMetadata,t,0).y):
        texelFetch(worldDepth,t,0).r;
    if(isnan(depth) || isinf(depth) || (p.temporal.w>0 && depth<=0)) return false;
    vec3 delta=centerPosition((vec2(t)+.5)/p.extent.xy,depth)-at;
    return !any(isnan(delta)) && !any(isinf(delta)) &&
        abs(dot(delta,normalize(adjacentPhysical+physical)))<=tolerance;
}
bool fixedSunPoint(vec2 uv,float depth,vec3 physical,out vec3 at) {
    // The selected centre depth is at uv-jitter. Intersect its physical plane
    // with the UNJITTERED source ray; reusing depth at uv tilts sloped receivers.
    if(p.temporal.w>0 && !orthographic(p.currentVP) && p.camera.w>.5) {
        vec2 selected=(uv-p.jitter.xy)*2-1,fixedPosition=uv*2-1;
        vec3 selectedRay=selected.x*p.rayBasisX.xyz+selected.y*p.rayBasisY.xyz+p.rayBasisZ.xyz;
        vec3 fixedRay=fixedPosition.x*p.rayBasisX.xyz+fixedPosition.y*p.rayBasisY.xyz+p.rayBasisZ.xyz;
        float denominator=dot(physical,fixedRay);
        if(abs(denominator)<=1e-6) return false;
        float fixedDepth=depth*dot(physical,selectedRay)/denominator;
        if(fixedDepth<=0 || isnan(fixedDepth) || isinf(fixedDepth)) return false;
        at=p.camera.xyz+fixedDepth*fixedRay;
    } else if(orthographic(p.currentVP)) {
        float z=p.temporal.w>0?depth-1:depth;
        vec4 h=p.inverseVP*vec4((uv-p.jitter.xy)*2-1,z,1);
        vec3 offset=(p.inverseVP*vec4(p.jitter.xy*2,0,0)).xyz/h.w;
        vec3 direction=p.inverseVP[2].xyz/h.w;
        float denominator=dot(physical,direction);
        if(abs(denominator)<=1e-6) return false;
        // Use affine deltas, not subtraction of translated world positions.
        at=h.xyz/h.w+offset-direction*(dot(physical,offset)/denominator);
    } else {
        vec3 selected=centerPosition(uv,depth),origin=p.camera.xyz;
        vec3 direction=centerPosition(uv+p.jitter.xy,depth)-origin;
        float denominator=dot(physical,direction);
        if(abs(denominator)<=1e-6) return false;
        at=origin+direction*(dot(physical,selected-origin)/denominator);
    }
    return !any(isnan(at)) && !any(isinf(at));
}
bool sunPointCertain(ivec2 t) {
    if(p.temporal.w<=0) return true;
    vec4 n=normalAt(t); vec3 physical;
    if(!physicalNormalAt(t,physical)) return false;
    int token=texelFetch(sourceMotion,t,0).w;
    float depth=intBitsToFloat(texelFetch(surfaceMetadata,t,0).y);
    vec2 uv=(vec2(t)+.5)/p.extent.xy;
    vec3 at=centerPosition(uv,depth),fixedPosition;
    if(!fixedSunPoint(uv,depth,physical,fixedPosition)) return false;
    float tolerance=abs(orthographic(p.currentVP)?1:depth)*.001;
    vec3 axis[2];
    // Mirror currentSurface's actual axis conditioning. A homogeneous but
    // degenerate projective footprint still needs the original jittered ray.
    for(int component=0;component<2;++component) {
        bool found=false;
        for(int side=0;side<2;++side) {
            ivec2 offset=component==0?ivec2(side==0?1:-1,0):ivec2(0,side==0?1:-1);
            ivec2 adjacent=t+offset;
            if(!sunSourceAgrees(adjacent,at,normalize(n.xyz),physical,int(n.w),token,tolerance)) continue;
            float adjacentDepth=intBitsToFloat(texelFetch(surfaceMetadata,adjacent,0).y);
            axis[component]=centerPosition((vec2(adjacent)+.5)/p.extent.xy,adjacentDepth)-at;
            found=true; break;
        }
        if(!found) return false;
    }
    float xx=dot(axis[0],axis[0]),xy=dot(axis[0],axis[1]),yy=dot(axis[1],axis[1]);
    return !isnan(xx) && !isinf(xx) && !isnan(xy) && !isinf(xy) &&
        !isnan(yy) && !isinf(yy) && xx*yy-xy*xy>xx*yy*.000001;
}
#if !defined(YAMI_SUN_PASS) && !defined(YAMI_SOURCE_CLASSIFY)
bool currentSourceCertain(ivec2 t,vec3 at,vec3 authored,vec3 physical,int owner) {
    int token=p.temporal.w>0?texelFetch(sourceMotion,t,0).w:0;
    if(token<0) return false;
    float depth=p.temporal.w>0?intBitsToFloat(texelFetch(surfaceMetadata,t,0).y):
        texelFetch(worldDepth,t,0).r;
    float tolerance=abs(orthographic(p.currentVP)?1:depth)*.001;
    if(isnan(tolerance) || isinf(tolerance)) return false;
    // Unlike Sun's one-sided axis support, current-only stochastic estimates
    // require the complete footprint: both sides, every diagonal and centre.
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        ivec2 adjacent=t+ivec2(x,y);
        if(!sunSourceAgrees(adjacent,at,authored,physical,owner,token,tolerance)) return false;
    }
    return true;
}
#endif
#endif
#ifdef YAMI_SOURCE_CLASSIFY
void main() {
    ivec2 t=ivec2(gl_FragCoord.xy);
    float complete=sourceComplete(t)?1:0;
    gainOut=vec4(complete/255,0,0,0);
}
#elif defined(YAMI_SUN_PASS)
// Frame generation's midpoint Sun shading (sun_midpoint.comp): xy = current-frame UV offset from this pixel's
// shadow point to where the shadow was on the same receiver one frame earlier, z = 1 where valid; zero elsewhere.
layout(location=1) out vec4 shadowOffsetOut;
bool conditionedAt(ivec2 t) { return texelFetch(worldColor,t,0).a>=4; }
// Zeng et al., "Temporally Reliable Motion Vectors for Real-time Ray Tracing" (2021) §4.1: move the committed
// blocker point b (the hit triangle's barycentres, D = b' - b with the previous pose) and the receiver plane
// (point s, physical normal n, displacement d) to their previous poses and re-intersect along the Sun
// direction L: the previous shadow point is s' = s + D - L·n·(D - d)/n·L. On the CURRENT receiver it sits at
// q = s' - d = s + (D - d) - L·n·(D - d)/n·L; the result is uv(q) - uv(s) with the unjittered current VP.
vec3 shadowOffset(vec3 s,vec3 n,vec3 d,uint object,uint primitive,vec2 bary,mat4x3 toWorld) {
    Instance m=instance[object];
    Indices ids=Indices(m.indices); Vertices current=Vertices(m.vertices),before=Vertices(m.previousVertices);
    vec3 weights=vec3(1-bary.x-bary.y,bary),b=vec3(0),bPrevious=vec3(0);
    for(int k=0;k<3;++k) {
        uint i=ids.value[primitive*3+k]*8;
        b+=weights[k]*vec3(current.value[i],current.value[i+1],current.value[i+2]);
        bPrevious+=weights[k]*vec3(before.value[i],before.value[i+1],before.value[i+2]);
    }
    mat4x3 previousToWorld=transpose(mat3x4(m.previousModel[0],m.previousModel[1],m.previousModel[2]));
    vec3 relative=previousToWorld*vec4(bPrevious,1)-toWorld*vec4(b,1)-d;
    vec3 delta=relative-lightDirection*(dot(n,relative)/dot(n,lightDirection));
    vec4 now=p.currentVP*vec4(s,1),then=p.currentVP*vec4(s+delta,1);
    vec2 offset=(then.xy/then.w-now.xy/now.w)*.5;
    return now.w>0 && then.w>0 && !any(isnan(offset)) && !any(isinf(offset))?vec3(offset,1):vec3(0);
}
void main() {
    ivec2 t=ivec2(gl_FragCoord.xy);
    gainOut=vec4(1,1,0,0); shadowOffsetOut=vec4(0);
    vec4 n=normalAt(t); float z=texelFetch(worldDepth,t,0).r;
    vec3 physical;
    if(z>=.999999 || n.w<0 || dot(n.xyz,n.xyz)<=.01 || !physicalNormalAt(t,physical)) return;
    int token=p.temporal.w>0?texelFetch(sourceMotion,t,0).w:0;
    float depth=p.temporal.w>0?intBitsToFloat(texelFetch(surfaceMetadata,t,0).y):z;
    if(isnan(depth) || isinf(depth) || (p.temporal.w>0 && depth<=0)) return;
    vec3 at=centerPosition(vUv,depth),fixedAt;
    if(any(isnan(at)) || any(isinf(at))) return;
    float tolerance=abs(orthographic(p.currentVP)?1:
        (p.temporal.w>0?depth:(p.currentVP*vec4(at,1)).w))*.001;
    bool ready=token>=0 && (n.w>.5 || p.temporal.w>0) && sunSourceComplete(t) &&
        !isnan(tolerance) && !isinf(tolerance) && fixedSunPoint(vUv,depth,physical,fixedAt);
    vec2 pixel=(vUv+p.jitter.xy)*p.extent.xy-.5;
    ivec2 corner=ivec2(floor(pixel)); vec2 phase=fract(pixel);
    for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
        float weight=(x==0?1-phase.x:phase.x)*(y==0?1-phase.y:phase.y);
        if(weight<=.00001) continue;
        if(!sunSourceAgrees(corner+ivec2(x,y),at,normalize(n.xyz),physical,int(n.w),token,tolerance))
            ready=false;
    }
    // Under FSR (post.z) every sample also traces from its own jittered surface point `at` (gainOut.g): the
    // FSR input uses that per-sample visibility, so FSR resolves shadow edges at their true sub-pixel
    // position. The fixed-grid point is constant across jitter, which quantised moving edges to whole
    // lighting texels (a shadow sliding ~1 px/frame advanced in 0 / 2-4 px jumps).
    bool boundary=!ready || p.post.z>0;
    // Native/fixed reconstruction reaches two cells; the old spatial kernel
    // reaches another two. Only this conservative owner halo needs legacy rays.
    for(int y=-4;y<=4 && !boundary;++y) for(int x=-4;x<=4;++x)
        if(!sunSourceAgrees(t+ivec2(x,y),at,normalize(n.xyz),physical,int(n.w),token,tolerance) ||
           !conditionedAt(t+ivec2(x,y))) {
            boundary=true; break;
        }
    // The raster material already shades surfaces turning away from the Sun with their smooth
    // authored normal. Fading the ray-traced shadow out over that range avoids darkening them
    // twice and drawing the low-poly facet boundary as a hard shadow edge; cast shadows on
    // surfaces facing the Sun are unchanged.
    float facing=smoothstep(0,.25,dot(normalize(n.xyz),lightDirection));
    if(facing<=0) {
        gainOut.b=ready?1:0; gainOut.a=boundary?1:0;
        return;
    }
    // Both Sun points lie on this pixel's facet; lift them onto the smooth surface once.
    vec3 lift=ready || boundary?smoothSurfaceLift(at,normalize(n.xyz),physical):vec3(0);
    // Shadow offsets only for frame generation (jitter.w), from a perspective camera with valid history.
    bool offsets=p.jitter.w>0 && p.temporal.x>0 && p.temporal.w>0 && p.camera.w>.5 && !orthographic(p.currentVP) &&
        dot(physical,lightDirection)>.001;
    // d: the receiver's own displacement, from its raster metadata (previous unjittered UV and clip W of the
    // sample at `at`, as fsr_inputs.frag reads it) through the previous camera rays; none keeps it camera-static.
    vec3 d=vec3(0);
    if(offsets) {
        vec3 previous=intBitsToFloat(texelFetch(sourceMotion,t,0).xyz);
        if(previous.z>0 && !isinf(previous.z) && !any(isnan(previous.xy)) && !any(isinf(previous.xy))) {
            vec2 ndc=previous.xy*2-1;
            d=p.previousCamera.xyz+previous.z*(ndc.x*p.previousRayBasisX.xyz+ndc.y*p.previousRayBasisY.xyz+
                p.previousRayBasisZ.xyz)-at;
        }
        if(any(isnan(d)) || any(isinf(d))) offsets=false;
    }
    uint object,primitive; vec2 bary; mat4x3 toWorld;
    if(ready) {
        bool hit=occluded(fixedAt+lift+physical*.06,lightDirection,2000,object,primitive,bary,toWorld);
        gainOut.r=hit?1-facing:1;
        gainOut.b=1;
        if(hit && offsets) shadowOffsetOut.xyz=shadowOffset(fixedAt,physical,d,object,primitive,bary,toWorld);
    }
    if(boundary) {
        bool hit=occluded(at+lift+physical*.06,lightDirection,2000,object,primitive,bary,toWorld);
        gainOut.g=hit?1-facing:1;
        gainOut.a=1;
        // The fixed-grid point's offset wins where both rays hit.
        if(hit && offsets && shadowOffsetOut.z==0) shadowOffsetOut.xyz=shadowOffset(at,physical,d,object,primitive,bary,toWorld);
    }
}
#else
float rasterAO(vec3 position,vec3 normal,float random) {
    float obscured=0, valid=0;
    for(int i=0;i<12;++i) {
        float a=(float(i)+random)*2.399963;
        vec2 uv=vUv+vec2(cos(a),sin(a))*(2+float(i)*1.8)/p.extent.xy;
        if(any(lessThan(uv,vec2(0)))||any(greaterThanEqual(uv,vec2(1)))) continue;
        ivec2 t=ivec2(floor(uv*p.extent.xy));
        float z=texelFetch(worldDepth,t,0).r; if(z>=.999999) continue;
        if(p.temporal.w>0) {
            ivec4 metadata=texelFetch(surfaceMetadata,t,0);
            float centerDepth=intBitsToFloat(metadata.y);
            if(centerDepth<=0 || isnan(centerDepth) || isinf(centerDepth) ||
               metadata.z==int(0x80008000u)) continue;
        }
        vec3 d=worldPosition((vec2(t)+.5)/p.extent.xy,z)-position;
        float len=length(d);
        obscured+=step(.05,dot(normal,d))*max(0,1-len/12.0); valid+=1;
    }
    return valid>0?1-obscured/valid:1;
}
vec3 screenReflection(vec3 origin,vec3 direction) {
    // Only an actual depth intersection contributes; screen misses are black.
    float travel=.35;
    ivec2 size=ivec2(p.extent.xy);
    for(int i=0;i<28;++i) {
        travel+=.3+float(i)*.12;
        vec3 at=origin+direction*travel;
        vec4 clip=p.currentVP*vec4(at,1); if(clip.w<=0) break;
        vec2 uv=clip.xy/clip.w*.5+.5+p.jitter.xy;
        if(any(lessThan(uv,vec2(0)))||any(greaterThanEqual(uv,vec2(1)))) break;
        ivec2 texel=ivec2(floor(uv*vec2(size)));
        float depth=texelFetch(worldDepth,texel,0).r;
        vec4 hitNormal=normalAt(texel);
        if(depth>=1 || hitNormal.w<0 || length(hitNormal.xyz)<.1) continue;
        vec3 hitPhysical;
        if(!physicalNormalAt(texel,hitPhysical)) continue;
        vec3 target=worldPosition((vec2(texel)+.5)/vec2(size),depth);
        float gap=length(target-p.camera.xyz)-length(at-p.camera.xyz);
        if(gap<0 && gap>-(.3+travel*.025) && dot(hitPhysical,direction)<-.05)
            return pow(max(texelFetch(worldColor,texel,0).rgb,vec3(0)),vec3(2.2));
    }
    return vec3(0);
}
void main() {
    ivec2 texel=ivec2(gl_FragCoord.xy);
    float depth=texelFetch(worldDepth,texel,0).r;
    vec4 validity=normalAt(texel);
#ifdef YAMI_RAY_QUERY
    float complete=round(texelFetch(classifiedSource,texel,0).r*255);
#else
    float complete=sourceComplete(texel)?1:0;
#endif
    gainOut=vec4(0,0,0,1); reflectionOut=vec4(0,0,0,complete);
    if(p.temporal.w>0) {
        float centerDepth=intBitsToFloat(texelFetch(surfaceMetadata,texel,0).y);
        if(centerDepth<=0 || isnan(centerDepth) || isinf(centerDepth)) return;
    }
    if(depth>=.999999 || validity.w<0 || length(validity.xyz)<.1) return;
    vec3 position=worldPosition(vUv,depth), n=normalize(validity.xyz),physical;
    if(!physicalNormalAt(texel,physical)) return;
    if(any(isnan(position)) || any(isinf(position))) return;
#ifdef YAMI_RAY_QUERY
    // The centre's original conditioning uses cached neighbour completeness,
    // never rereading retained MS samples. Risk stays independent of Sun.
    bool conditioned=complete>.5 && sunPointCertain(texel);
    if(p.strengths.z>0 && conditioned) complete+=4;
#endif
    vec3 view=normalize(p.camera.xyz-position);
    float ao=1; vec3 reflected=vec3(0), indirect=vec3(0);
    vec2 random=vec2(noise(gl_FragCoord.xy),noise(gl_FragCoord.yx+17));
    // Authored normals retain the reflection lobe, but cannot send a receiver
    // ray through its actual facet. Rough diffuse directions use that facet.
    vec3 mirror=reflect(-view,n);
    if(dot(mirror,physical)<0) mirror=reflect(mirror,physical);
    float fresnel=.04+.96*pow(1-max(dot(n,view),0),5);
#ifdef YAMI_RAY_QUERY
    bool diffuse=p.strengths.x>0 || p.strengths.w>0;
    bool reflectionActive=p.strengths.y>0;
    bool roughReflection=reflectionActive && p.post.w!=0;
    bool stochastic=diffuse || roughReflection;
    bool stable=!stochastic || (conditioned && currentSourceCertain(texel,position,n,physical,int(validity.w)));
    int sampleCount=(diffuse||reflectionActive)?(stable?1:16):0;
    int reflectionCount=p.post.w==0?1:sampleCount;
    float aoSum=0; vec3 indirectSum=vec3(0);
    vec3 origin=position+physical*.06;
    for(int i=0;i<sampleCount;++i) {
        effectSample(i,gl_FragCoord.xy,stochastic,stable,diffuse,reflectionActive,reflectionCount,
                     origin,physical,mirror,random,aoSum,indirectSum,reflected);
        if(!stable) {
            // History-unsafe: whole waves would loop 16 times for a few such lanes. effects_rt.comp
            // continues samples 1..15 from this exact state in packed waves; effects_resolve writes it.
            uint slot=atomicAdd(queue.count,1u);
            if(slot%YAMI_PENDING_GROUP==0u && slot/YAMI_PENDING_GROUP<YAMI_PENDING_GROUPS) atomicAdd(queue.groups,1u);
            pending[slot]=Pending(vec4(indirectSum,aoSum),vec4(reflected,complete),vec4(origin,fresnel),
                vec4(physical,random.x),vec4(mirror,random.y),gl_FragCoord.xy,uint(stable),0u);
            return;
        }
    }
    rtOutputs(diffuse,reflectionActive,sampleCount,reflectionCount,aoSum,indirectSum,reflected,fresnel,complete,
              gainOut,reflectionOut);
#else
    vec3 reflectedDirection=normalize(mix(mirror,hemisphere(physical,random),p.post.w*p.post.w));
    if(p.strengths.x>0) ao=rasterAO(position,physical,random.x);
    if(p.strengths.y>0) reflected=screenReflection(position+physical*.1,reflectedDirection);
    gainOut=vec4(max(indirect*p.strengths.w,vec3(0)),mix(1,ao,p.strengths.x));
    reflectionOut=vec4(max(reflected*p.strengths.y*fresnel*(1-.65*p.post.w),vec3(0)),complete);
#endif
}
#endif
