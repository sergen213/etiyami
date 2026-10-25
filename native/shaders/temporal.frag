#version 460
layout(location=0) in vec2 vUv;
layout(location=0) out vec4 gainOut;
layout(location=1) out ivec4 geometryOut;
layout(location=2) out vec4 baseOut;
layout(location=3) out vec4 reflectionOut;
layout(set=0,binding=0) uniform sampler2D currentGain;
layout(set=0,binding=1) uniform sampler2D worldDepth;
layout(set=0,binding=2) uniform isampler2D worldNormal;
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
layout(set=0,binding=4) uniform sampler2D historyGain;
layout(set=0,binding=5) uniform isampler2D historyGeometry;
layout(set=0,binding=6) uniform sampler2D rasterColor;
layout(set=0,binding=7) uniform sampler2D currentReflection;
layout(set=0,binding=8) uniform sampler2D historyBase;
layout(set=0,binding=9) uniform sampler2D historyReflection;
layout(set=0,binding=10) uniform isampler2D worldMotion;
layout(set=0,binding=11) uniform isampler2D previousNormal;
layout(set=0,binding=12) uniform sampler2D sourceSun;
vec2 sourcePixel(vec2 nativeCentre,ivec2 nativeSize,ivec2 sourceSize,bool taa,
                 out vec2 nativeUV,out vec2 sampleUV) {
    // Preserve canonical native-centre division and SOURCE acquisition.
    // Precise intermediates prohibit different multiply/add contraction paths.
    precise vec2 point=nativeCentre/vec2(nativeSize);
    precise vec2 sampled=point+(taa?p.jitter.xy:vec2(0));
    precise vec2 pixel=sampled*vec2(sourceSize);
    nativeUV=point; sampleUV=sampled;
    return pixel;
}
float completeness(float flags) { return mod(flags,4.0); }
// History alpha is an exact normal-half code, not the raw source flags:
// half bits = ((confidence+2)<<9) | RMS-noise-bin.
// The 5-exponent/4-mantissa noise bin decodes at its upper half-float edge.
// Noise bin 495 is the conservative unmeasured marker, not a known model.
const float maximumReflectionSigma=65504.0;
uint historyCode(float alpha) { return packHalf2x16(vec2(abs(alpha),0))&0xffffu; }
bool historyMetadataValid(float alpha) {
    uint code=historyCode(alpha);
    return code>=1024u && (code>>9)<=61u && (code&511u)<=495u;
}
int historyConfidence(float alpha) {
    return historyMetadataValid(alpha)?int(historyCode(alpha)>>9)-2:0;
}
bool historyNoiseModelKnown(float alpha) {
    return historyMetadataValid(alpha) && (historyCode(alpha)&511u)<495u;
}
float historyFrameVariance(float alpha) {
    uint bin=historyCode(alpha)&511u;
    float sigma=bin==0u?0:unpackHalf2x16((bin<<6)|63u).x;
    return 3*sigma*sigma;
}
float historyAlpha(int confidence,float frameVariance) {
    float sigma=sqrt(max(frameVariance/3,0));
    uint halfSigma=packHalf2x16(vec2(sigma,0))&0xffffu;
    if(unpackHalf2x16(halfSigma).x<sigma) ++halfSigma;
    uint bin=sigma==0?0u:max(halfSigma>>6,1u);
    uint code=(uint(confidence+2)<<9)|min(bin,495u);
    return unpackHalf2x16(code).x;
}
vec4 normalAt(ivec2 t) {
    ivec4 n=texelFetch(worldNormal,t,0);
    return vec4(vec3(n.xyz)/32767.0,float(n.w));
}
vec3 rasterAt(ivec2 t) {
    return pow(max(texelFetch(rasterColor,t,0).rgb,vec3(0)),vec3(2.2));
}
float centerDepthAt(ivec2 t) {
    return p.temporal.w>0?intBitsToFloat(texelFetch(previousNormal,t,0).y):
        texelFetch(worldDepth,t,0).r;
}
vec3 depthPosition(vec2 uv,float z) {
    vec4 h=p.inverseVP*vec4((uv-p.jitter.xy)*2-1,z,1);
    return h.xyz/h.w;
}
bool orthographic(mat4 vp) {
    return length(vec3(vp[0][3],vp[1][3],vp[2][3]))<1e-8;
}
vec3 position(vec2 uv,float encodedDepth) {
    if(p.temporal.w<=0) return depthPosition(uv,encodedDepth);
    if(orthographic(p.currentVP)) return depthPosition(uv,encodedDepth-1);
    vec2 coordinate=(uv-p.jitter.xy)*2-1;
    // CPU-qualified camera-relative rays avoid inverse-VP translation cancellation.
    if(p.camera.w>.5)
        return p.camera.xyz+encodedDepth*(coordinate.x*p.rayBasisX.xyz+
            coordinate.y*p.rayBasisY.xyz+p.rayBasisZ.xyz);
    // Centre clip W does not lose the far plane's low bits through z/w.
    vec4 h=p.inverseVP*vec4(coordinate,0,1);
    vec4 depthColumn=p.inverseVP[2];
    float z=(1/encodedDepth-h.w)/depthColumn.w;
    return (h.xyz+depthColumn.xyz*z)*encodedDepth;
}
vec3 oldPosition(vec2 uv,float encodedDepth) {
    // Perspective history stores clip W. Orthographic W is constant, so both
    // motion and history store 1+NDC depth instead (zero stays invalid).
    if(orthographic(p.previousVP)) {
        vec4 h=p.inversePreviousVP*vec4(uv*2-1,encodedDepth-1,1);
        return h.xyz/h.w;
    }
    vec2 coordinate=uv*2-1;
    if(p.previousCamera.w>.5)
        return p.previousCamera.xyz+encodedDepth*(coordinate.x*p.previousRayBasisX.xyz+
            coordinate.y*p.previousRayBasisY.xyz+p.previousRayBasisZ.xyz);
    vec4 h=p.inversePreviousVP*vec4(coordinate,0,1);
    vec4 depthColumn=p.inversePreviousVP[2];
    float z=(1/encodedDepth-h.w)/depthColumn.w;
    return (h.xyz+depthColumn.xyz*z)*encodedDepth;
}
bool decodeNormal(int encoded,out vec3 normal) {
    normal=vec3(0);
    if(encoded==int(0x80008000u)) return false;
    vec2 oct=unpackSnorm2x16(uint(encoded));
    vec3 decoded=vec3(oct,1-abs(oct.x)-abs(oct.y));
    if(decoded.z<0) decoded.xy=(1-abs(decoded.yx))*
        mix(vec2(-1),vec2(1),greaterThanEqual(decoded.xy,vec2(0)));
    float squared=dot(decoded,decoded);
    if(squared<=.01 || isnan(squared) || isinf(squared)) return false;
    normal=decoded*inversesqrt(squared); return true;
}
int encodeNormal(vec3 normal) {
    normal/=abs(normal.x)+abs(normal.y)+abs(normal.z);
    vec2 oct=normal.xy;
    if(normal.z<0) oct=(1-abs(oct.yx))*
        mix(vec2(-1),vec2(1),greaterThanEqual(oct,vec2(0)));
    return int(packSnorm2x16(oct));
}
bool physicalNormalAt(ivec2 t,vec3 authored,out vec3 normal) {
    if(p.temporal.w<=0) {
        normal=authored;
        return dot(normal,normal)>.01 && !any(isnan(normal)) && !any(isinf(normal));
    }
    return decodeNormal(texelFetch(previousNormal,t,0).z,normal);
}
bool previousNormalsAt(ivec2 t,out vec3 normal,out vec3 physical) {
    ivec4 encoded=texelFetch(previousNormal,t,0);
    return decodeNormal(encoded.x,normal) && decodeNormal(encoded.w,physical);
}
vec3 coverageGain(vec3 lit,vec3 base) {
    // Encode mixed display coverage only AFTER filtering the independently
    // computed stochastic signals. Black base carries no multiplicative
    // radiance, so one is a neutral encoding without losing a ray sample.
    return mix(vec3(1),lit/max(base,vec3(1e-30)),greaterThan(base,vec3(0)));
}
int tokenAt(ivec2 t) {
    return p.temporal.w>0?texelFetch(worldMotion,t,0).w:0;
}
bool worldAt(vec4 n,float z) { return n.w>=0 && z<1 && dot(n.xyz,n.xyz)>.01; }
bool sourceAgrees(ivec2 t,vec3 at,vec3 normal,vec3 physical,int token,float tolerance) {
    vec4 n=normalAt(t); float z=texelFetch(worldDepth,t,0).r;
    if(!worldAt(n,z) || tokenAt(t)!=token || dot(physical,physical)<=.01) return false;
    vec3 adjacent=normalize(n.xyz),adjacentPhysical;
    float threshold=token>0?.8:.9;
    if(dot(adjacent,normal)<threshold ||
       !physicalNormalAt(t,adjacent,adjacentPhysical) ||
       dot(adjacentPhysical,physical)<threshold) return false;
    float depth=centerDepthAt(t);
    if((p.temporal.w>0 && depth<=0) || isnan(depth) || isinf(depth)) return false;
    vec3 delta=position((vec2(t)+.5)/p.extent.xy,depth)-at;
    // Facets, not authored lighting normals, define the symmetric tangent plane.
    // Keep the strict depth tolerance instead of admitting a depth-gradient
    // band that could join overlapping limbs or a revealed background.
    return abs(dot(delta,normalize(adjacentPhysical+physical)))<=tolerance;
}
bool currentSurface(ivec2 reference,vec3 referenceAt,vec2 uv,
                    vec3 normal,vec3 physical,int token,float tolerance,out vec3 at,
                    out ivec2 neighbor[2],out vec3 weights,out vec3 depths,
                    out float nativeDepth) {
    // Validate the physical surface before interpolating its projective plane.
    // Authored shading normals remain guidance, not the reconstruction plane.
    vec3 axis[2];
    depths.x=centerDepthAt(reference);
    if(depths.x<=0 || isnan(depths.x) || isinf(depths.x)) return false;
    for(int component=0;component<2;++component) {
        bool found=false;
        for(int side=0;side<2;++side) {
            ivec2 offset=component==0?ivec2(side==0?1:-1,0):ivec2(0,side==0?1:-1);
            ivec2 t=reference+offset;
            if(any(lessThan(t,ivec2(0))) || any(greaterThanEqual(t,ivec2(p.extent.xy)))) continue;
            if(!sourceAgrees(t,referenceAt,normal,physical,token,tolerance)) continue;
            float depth=centerDepthAt(t);
            if(depth<=0 || isnan(depth) || isinf(depth)) continue;
            axis[component]=position((vec2(t)+.5)/p.extent.xy,depth)-referenceAt;
            neighbor[component]=t; depths[component+1]=depth; found=true; break;
        }
        if(!found) return false;
    }
    float xx=dot(axis[0],axis[0]),xy=dot(axis[0],axis[1]),yy=dot(axis[1],axis[1]);
    if(xx*yy-xy*xy<=xx*yy*.000001) return false;
    vec2 delta=uv*p.extent.xy-(vec2(reference)+.5);
    vec2 amount=delta/vec2(neighbor[0].x-reference.x,neighbor[1].y-reference.y);
    weights=vec3(1-amount.x-amount.y,amount);
    bool ortho=orthographic(p.currentVP);
    float inverseW=dot(weights,ortho?vec3(1):1/depths);
    if(inverseW<=0 || isnan(inverseW) || isinf(inverseW)) return false;
    nativeDepth=ortho?dot(weights,depths):1/inverseW;
    if(nativeDepth<=0 || isnan(nativeDepth) || isinf(nativeDepth)) return false;
    at=position(uv,nativeDepth);
    return !any(isnan(at)) && !any(isinf(at));
}
bool dynamicPrevious(ivec2 reference,int token,ivec2 neighbor[2],
                     vec3 weights,vec3 currentDepths,out vec2 previousUV,
                     out float previousDepth,out vec3 previousAt,out vec3 previousN,
                     out vec3 previousPhysical) {
    if(token<0 || !previousNormalsAt(reference,previousN,previousPhysical)) return false;
    vec3 oldCoordinate[3];
    oldCoordinate[0]=intBitsToFloat(texelFetch(worldMotion,reference,0).xyz);
    for(int component=0;component<2;++component) {
        vec3 oldN,oldPhysical;
        if(!previousNormalsAt(neighbor[component],oldN,oldPhysical) ||
           dot(oldN,previousN)<.8 || dot(oldPhysical,previousPhysical)<.8) return false;
        oldCoordinate[component+1]=intBitsToFloat(texelFetch(worldMotion,neighbor[component],0).xyz);
    }
    bool currentOrtho=orthographic(p.currentVP),previousOrtho=orthographic(p.previousVP);
    vec3 q=weights*(currentOrtho?vec3(1):1/currentDepths);
    float total=dot(q,vec3(1)),projectedTotal=0;
    vec2 numerator=vec2(0); float depthNumerator=0;
    for(int i=0;i<3;++i) {
        vec3 c=oldCoordinate[i];
        if(c.z<=0 || any(isnan(c)) || any(isinf(c))) return false;
        float projectedW=previousOrtho?1:c.z;
        projectedTotal+=q[i]*projectedW;
        numerator+=q[i]*projectedW*c.xy;
        depthNumerator+=q[i]*c.z;
    }
    if(total<=0 || projectedTotal<=0 || isnan(total) || isinf(total) ||
       isnan(projectedTotal) || isinf(projectedTotal)) return false;
    // Transport actual prior vertex/model clip coordinates directly. A far
    // world-space round trip here introduces a recursive subpixel artwork drift.
    previousUV=numerator/projectedTotal;
    previousDepth=previousOrtho?depthNumerator/total:projectedTotal/total;
    if(previousDepth<=0 || any(isnan(previousUV)) || any(isinf(previousUV)) ||
       isnan(previousDepth) || isinf(previousDepth)) return false;
    previousAt=oldPosition(previousUV,previousDepth);
    return !any(isnan(previousAt)) && !any(isinf(previousAt));
}
bool historyBaseAt(ivec2 t,ivec2 size,int token,vec3 at,vec3 normal,vec3 physical,
                   float clipW) {
    if(any(lessThan(t,ivec2(0))) || any(greaterThanEqual(t,size))) return false;
    float owner=texelFetch(historyBase,t,0).a;
    ivec4 geometry=texelFetch(historyGeometry,t,0);
    float depth=intBitsToFloat(geometry.w);
    vec3 oldN,oldPhysical;
    if(owner!=float(token) || depth<=0 || isnan(depth) || isinf(depth) ||
       !decodeNormal(geometry.x,oldN) || !decodeNormal(geometry.y,oldPhysical)) return false;
    float threshold=token>0?.8:.9;
    if(dot(oldN,normal)<threshold || dot(oldPhysical,physical)<threshold) return false;
    vec3 delta=oldPosition((vec2(t)+.5)/vec2(size),depth)-at;
    return abs(dot(delta,normalize(oldPhysical+physical)))<=clipW*.001;
}
float legacyVisibilityAt(ivec2 t) {
    return p.strengths.z>0?texelFetch(sourceSun,t,0).g:1;
}
vec3 legacyGain(vec4 gain,float visibility) {
    return vec3(gain.a*mix(1,.3+.7*visibility,p.strengths.z))+gain.rgb;
}
float currentSunState(float visibility) {
    // Alpha is half-float: publish a binary state only if its two modulation
    // endpoints remain distinct. A weak Sun must not manufacture a lit state.
    float dark=unpackHalf2x16(packHalf2x16(vec2(mix(1,.3,p.strengths.z),0))).x;
    if(!(dark>0 && dark<1)) return -2;
    return visibility==1?-1:(visibility==0?-dark:-2);
}
float sunStateVisibility(float state) {
    if(state>=0 && state<=1) return state;
    float dark=unpackHalf2x16(packHalf2x16(vec2(mix(1,.3,p.strengths.z),0))).x;
    if(!(dark>0 && dark<1)) return -1;
    return state==-1?1:(state==-dark?0:-1);
}
bool sunStateChanged(float previous,float current) {
    // Fixed/fixed keeps the original raw-visibility tolerance; negatives
    // decode queried binary G, not a modulation factor in visibility units.
    float previousVisibility=sunStateVisibility(previous);
    float currentVisibility=sunStateVisibility(current);
    return previousVisibility>=0 && currentVisibility>=0 &&
        abs(previousVisibility-currentVisibility)>.01;
}
bool independentSource(ivec2 t,vec3 at,vec3 normal,vec3 physical,
                       int token,float owner,float tolerance) {
    return sourceAgrees(t,at,normal,physical,token,tolerance) &&
        normalAt(t).w==owner && completeness(texelFetch(currentReflection,t,0).a)>.5;
}
bool fixedSunAt(vec2 nativeUV,vec3 at,vec3 normal,vec3 physical,int token,float owner,
                float tolerance,out float visibility) {
    visibility=1;
    if(p.strengths.z<=0) return true;
    // Sun samples live at unjittered source centres, unlike raster/AO/GI.
    vec2 pixel=nativeUV*p.extent.xy-.5;
    ivec2 corner=ivec2(floor(pixel)); vec2 phase=fract(pixel);
    visibility=0; float total=0;
    for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
        float w=(x==0?1-phase.x:phase.x)*(y==0?1-phase.y:phase.y);
        if(w<=.00001) continue;
        ivec2 t=corner+ivec2(x,y);
        if(any(lessThan(t,ivec2(0))) || any(greaterThanEqual(t,ivec2(p.extent.xy))) ||
           !independentSource(t,at,normal,physical,token,owner,tolerance)) return false;
        vec4 sun=texelFetch(sourceSun,t,0);
        if(sun.b<.5) return false;
        visibility+=sun.r*w; total+=w;
    }
    if(total<=.00001) return false;
    visibility/=total; return true;
}
void main() {
    ivec2 size=textureSize(currentGain,0);
    bool taa=p.temporal.y>0;
    ivec2 nativeSize=textureSize(historyBase,0);
    precise vec2 nativeUV,sampleUV;
    vec2 pixel=sourcePixel(gl_FragCoord.xy,nativeSize,size,taa,nativeUV,sampleUV);
    ivec2 reference=clamp(ivec2(floor(pixel)),ivec2(0),size-1);
    vec2 referenceUV=(vec2(reference)+.5)/vec2(size);
    vec4 n=normalAt(reference);
    float z=texelFetch(worldDepth,reference,0).r;
    int token=tokenAt(reference);
    vec3 authored=rasterAt(reference);
    bool world=worldAt(n,z),eligible=world && token>=0 && (n.w>.5 || p.temporal.w>0);
    vec3 normal=world?normalize(n.xyz):vec3(0);
    vec3 physical;
    bool physicalValid=world && physicalNormalAt(reference,normal,physical);
    if(!physicalValid) physical=vec3(0);
    eligible=eligible && physicalValid;
    float centerDepth=centerDepthAt(reference);
    vec3 referenceAt=world?position(referenceUV,centerDepth):vec3(0),at=referenceAt;
    float geometryDepth=0,tolerance=0;
    bool surfaceValid=world && physicalValid;
    vec3 screenWeights,currentDepths;
    ivec2 neighbor[2];
    if(world) {
        bool ortho=orthographic(p.currentVP);
        tolerance=abs(p.temporal.w>0?(ortho?1:centerDepth):
            (p.currentVP*vec4(referenceAt,1)).w)*.001;
        if(p.temporal.w>0)
            surfaceValid=physicalValid &&
                currentSurface(reference,referenceAt,sampleUV,normal,physical,token,tolerance,
                               at,neighbor,screenWeights,currentDepths,geometryDepth);
        else {
            vec3 origin=ortho?depthPosition(sampleUV,0):p.camera.xyz;
            vec3 ray=ortho?depthPosition(sampleUV,1)-origin:depthPosition(sampleUV,centerDepth)-origin;
            float denominator=dot(ray,normal);
            surfaceValid=abs(denominator)>1e-6;
            if(surfaceValid) {
                at=origin+ray*(dot(referenceAt-origin,normal)/denominator);
                vec4 currentClip=p.currentVP*vec4(at,1);
                geometryDepth=ortho?1+currentClip.z/currentClip.w:currentClip.w;
            }
        }
        if(surfaceValid) tolerance=abs(ortho?1:geometryDepth)*.001;
        else eligible=false;
    }
    vec4 rawGain=texelFetch(currentGain,reference,0);
    vec4 rawReflectionSample=texelFetch(currentReflection,reference,0);
    vec3 rawReflection=rawReflectionSample.rgb;
    eligible=eligible && completeness(rawReflectionSample.a)>.5;
    float fixedVisibility=1;
    bool typed=eligible && surfaceValid;
    bool fixedSunReady=typed &&
        fixedSunAt(nativeUV,referenceAt,normal,physical,token,n.w,tolerance,fixedVisibility);
    float rawVisibility=legacyVisibilityAt(reference),uniformVisibility=rawVisibility;
    bool currentSunReady=p.strengths.z<=0 || texelFetch(sourceSun,reference,0).a>=.5;
    bool currentSunUniform=true;
    if(typed && (taa || any(notEqual(p.extent.xy,p.extent.zw)))) {
        ivec2 corner=ivec2(floor(pixel-.5)); vec2 phase=fract(pixel-.5);
        currentSunReady=true;
        uniformVisibility=-1;
        for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
            float w=(x==0?1-phase.x:phase.x)*(y==0?1-phase.y:phase.y);
            if(w<=.00001) continue;
            ivec2 t=clamp(corner+ivec2(x,y),ivec2(0),size-1);
            if(!independentSource(t,referenceAt,normal,physical,token,n.w,tolerance)) typed=false;
            if(p.strengths.z>0) {
                vec4 sun=texelFetch(sourceSun,t,0);
                if(sun.a<.5) currentSunReady=false;
                if(uniformVisibility<0) uniformVisibility=sun.g;
                else if(sun.g!=uniformVisibility) currentSunUniform=false;
            }
        }
    }
    typed=typed && (fixedSunReady || currentSunReady);
    vec3 rawComposedGain=legacyGain(rawGain,rawVisibility);
    vec3 currentLit=authored*rawComposedGain,currentReflected=rawReflection;
    vec3 surfaceBase=world?authored:vec3(0),surfaceLit=world?currentLit:vec3(0);
    vec3 surfaceReflection=world?rawReflection:vec3(0);
    float surfaceCoverage=world?1:0;
    if(taa || any(notEqual(p.extent.xy,p.extent.zw))) {
        ivec2 corner=ivec2(floor(pixel-.5)); vec2 phase=fract(pixel-.5);
        authored=vec3(0);
        currentLit=vec3(0); currentReflected=vec3(0);
        surfaceBase=vec3(0); surfaceLit=vec3(0); surfaceReflection=vec3(0);
        surfaceCoverage=0;
        float acceptedWeight=0;
        for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
            ivec2 t=clamp(corner+ivec2(x,y),ivec2(0),size-1);
            float w=(x==0?1-phase.x:phase.x)*(y==0?1-phase.y:phase.y);
            if(w<=.00001) continue;
            vec4 adjacent=normalAt(t);
            bool belongs=world && sourceAgrees(t,referenceAt,normal,physical,token,tolerance);
            bool sameSky=!world && z>=1 && texelFetch(worldDepth,t,0).r>=1;
            // Upscaling without TAA must not invent opposite-surface coverage.
            // The selected raster sample already contains authored MSAA
            // coverage; gate taps BEFORE mixing any base or lighting signal.
            if(!taa && !belongs && !sameSky && any(notEqual(t,reference))) continue;
            vec4 reflectionSample=texelFetch(currentReflection,t,0);
            vec3 b=rasterAt(t);
            vec3 r=reflectionSample.rgb;
            vec4 g=texelFetch(currentGain,t,0);
            float adjacentVisibility=legacyVisibilityAt(t);
            vec3 composed=legacyGain(g,adjacentVisibility);
            currentLit+=b*composed*w; currentReflected+=r*w;
            authored+=b*w;
            acceptedWeight+=w;
            if(!belongs || completeness(reflectionSample.a)<=.5 ||
               (adjacent.w<=.5 && p.temporal.w<=0)) eligible=false;
            if(belongs && (typed || abs(adjacentVisibility-rawVisibility)<=.01)) {
                surfaceBase+=b*w; surfaceLit+=b*composed*w;
                surfaceReflection+=r*w; surfaceCoverage+=w;
            }
        }
        if(acceptedWeight>.00001) {
            authored/=acceptedWeight;
            currentLit/=acceptedWeight; currentReflected/=acceptedWeight;
            surfaceBase/=acceptedWeight; surfaceLit/=acceptedWeight; surfaceReflection/=acceptedWeight;
            surfaceCoverage/=acceptedWeight;
        } else {
            authored=rasterAt(reference);
            currentLit=authored*rawComposedGain; currentReflected=rawReflection;
            surfaceBase=world?authored:vec3(0); surfaceLit=world?currentLit:vec3(0);
            surfaceReflection=world?rawReflection:vec3(0); surfaceCoverage=world?1:0;
        }
    }
    vec3 previousAt=at,previousN=normal,previousPhysical=physical;
    vec2 oldUV=vec2(0); float oldDepth=0,oldClipW=0;
    bool correspondence=false;
    if(p.temporal.x>0 && eligible && surfaceValid) {
        if(p.temporal.w>0) {
            correspondence=dynamicPrevious(reference,token,neighbor,screenWeights,currentDepths,
                                           oldUV,oldDepth,previousAt,previousN,previousPhysical);
            oldClipW=orthographic(p.previousVP)?1:oldDepth;
        } else if(token==0) {
            vec4 clip=p.previousVP*vec4(previousAt,1);
            correspondence=clip.w>0;
            oldUV=clip.xy/clip.w*.5+.5; oldClipW=clip.w;
        }
    }
    vec3 currentBase=authored;
    typed=typed && eligible && surfaceCoverage>=.99999;
    float sunState=fixedSunReady?fixedVisibility:
        (currentSunReady && currentSunUniform?currentSunState(uniformVisibility):-2);
    gainOut=vec4(coverageGain(currentLit,currentBase),0);
    reflectionOut=vec4(currentReflected,historyAlpha(0,0));
    baseOut=vec4(authored,-1);
    geometryOut=ivec4(0,0,floatBitsToInt(-1.0),0);
    if(!world) return;
    // Only a complete current physical owner carries independent stochastic
    // history. Sun stays current: fixed-grid visibility when available, else
    // display reconstructs queried source visibility with raster covariance.
    bool lightingHistoryEligible=typed;
    vec3 gainSum=vec3(0),reflectionSum=vec3(0),reflectionSquares=vec3(0);
    float weight=0,weightSquares=0,aoSum=0;
    for(int y=-2;y<=2;++y) for(int x=-2;x<=2;++x) {
        ivec2 t=reference+ivec2(x,y);
        if(any(lessThan(t,ivec2(0))) || any(greaterThanEqual(t,size))) continue;
        if(!sourceAgrees(t,referenceAt,normal,physical,token,tolerance)) continue;
        vec4 g=texelFetch(currentGain,t,0);
        // Unsafe legacy composition still respects hard visibility boundaries.
        // Independent AO/GI/reflection never partition samples by current Sun.
        if(!typed && abs(legacyVisibilityAt(t)-rawVisibility)>.01) continue;
        if(typed && !independentSource(t,referenceAt,normal,physical,token,n.w,tolerance)) continue;
        float similarity=dot(normal,normalize(normalAt(t).xyz));
        vec2 offset=vec2(t)+.5-pixel;
        float w=exp(-dot(offset,offset)*.45)*pow(max(similarity,0),8);
        vec3 r=texelFetch(currentReflection,t,0).rgb;
        gainSum+=(typed?g.rgb:legacyGain(g,legacyVisibilityAt(t)))*w;
        aoSum+=g.a*w; reflectionSum+=r*w; weight+=w;
        reflectionSquares+=r*r*w; weightSquares+=w*w;
    }
    vec3 gain=weight>.00001?gainSum/weight:(typed?rawGain.rgb:rawComposedGain);
    float ao=weight>.00001?aoSum/weight:rawGain.a;
    vec3 reflected=weight>.00001?reflectionSum/weight:rawReflection;
    bool reflectionFinite=!any(isnan(reflected)) && !any(isinf(reflected)) &&
        !any(isnan(reflectionSquares)) && !any(isinf(reflectionSquares));
    float reflectionFrameVariance=0;
    bool reflectionObservationValid=false,reflectionNoiseKnown=false;
    if(reflectionFinite) {
        float denominator=weight*weight-weightSquares;
        // A finite multiple-ray zero is a valid observation, but cannot measure
        // population noise. Ownership validity and unknown variance differ.
        reflectionFrameVariance=3*maximumReflectionSigma*maximumReflectionSigma;
        reflectionObservationValid=weight>.00001 && denominator>weight*weight*.000001;
        if(reflectionObservationValid && any(greaterThan(reflectionSum,vec3(0)))) {
            vec3 variance=max(reflectionSquares/weight-reflected*reflected,vec3(0));
            reflectionFrameVariance=dot(variance,vec3(1))*weightSquares/denominator;
            reflectionNoiseKnown=!isnan(reflectionFrameVariance) && !isinf(reflectionFrameVariance);
            reflectionObservationValid=reflectionNoiseKnown;
        } else reflectionObservationValid=reflectionObservationValid &&
            all(equal(reflectionSum,vec3(0)));
    }
    int reflectionConfidence=lightingHistoryEligible && reflectionNoiseKnown && p.strengths.y>0?1:0;
    // Physical signal bounds retain Monte Carlo history even if this frame's
    // entire local ray kernel misses. Local extrema alone freeze actor noise.
    float minimumAO=mix(1,0,p.strengths.x);
    if(p.temporal.x>0 && eligible && correspondence && oldClipW>0) {
        ivec2 oldSize=textureSize(historyBase,0);
        vec2 oldPixel=oldUV*vec2(oldSize)-.5;
        ivec2 corner=ivec2(floor(oldPixel)); vec2 phase=fract(oldPixel);
        vec3 oldGain=vec3(0),oldReflection=vec3(0);
        float lightingWeight=0,oldAO=0,oldKnownFrameVariance=0;
        int oldConfidence=59;
        bool lightingComplete=lightingHistoryEligible,oldNoiseKnown=false;
        for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
            ivec2 t=corner+ivec2(x,y);
            float w=(x==0?1-phase.x:phase.x)*(y==0?1-phase.y:phase.y);
            if(w<=0 || !lightingHistoryEligible) continue;
            bool acceptedTap=w>.00001;
            if(!historyBaseAt(t,oldSize,token,previousAt,previousN,previousPhysical,oldClipW)) {
                lightingComplete=false;
                continue;
            }
            vec4 oldRef=texelFetch(historyReflection,t,0);
            if(lightingHistoryEligible) {
                vec4 g=texelFetch(historyGain,t,0);
                float storedAO=intBitsToFloat(texelFetch(historyGeometry,t,0).z);
                // A known current hard-Sun transition rejects stochastic
                // history only; unknown Sun never disables a physical owner.
                if(acceptedTap && p.strengths.z>0 && sunStateChanged(g.a,sunState)) {
                    lightingComplete=false;
                    continue;
                }
                if(storedAO>=0 && !isnan(storedAO) && !isinf(storedAO)) {
                    if(acceptedTap) {
                        oldGain+=g.rgb*w; oldAO+=storedAO*w; oldReflection+=oldRef.rgb*w;
                        lightingWeight+=w;
                    }
                    if(historyMetadataValid(oldRef.a) &&
                       !any(isnan(oldRef.rgb)) && !any(isinf(oldRef.rgb))) {
                        if(acceptedTap) {
                            oldConfidence=min(oldConfidence,historyConfidence(oldRef.a));
                            if(historyNoiseModelKnown(oldRef.a)) {
                                oldKnownFrameVariance=max(oldKnownFrameVariance,historyFrameVariance(oldRef.a));
                                oldNoiseKnown=true;
                            }
                        }
                    } else lightingComplete=false;
                } else lightingComplete=false;
            }
        }
        if(lightingWeight>.00001) {
            // Indirect gain and reflections are unbounded HDR; only their
            // physical lower bounds are known. Single-frame misses must not
            // erase samples collected from valid earlier rays.
            gain=mix(gain,max(oldGain/lightingWeight,vec3(0)),.9);
            ao=mix(ao,max(oldAO/lightingWeight,minimumAO),.9);
            vec3 previousReflection=max(oldReflection/lightingWeight,vec3(0));
            if(lightingComplete && reflectionObservationValid &&
               (reflectionNoiseKnown || (oldNoiseKnown && oldKnownFrameVariance>0)) &&
               p.strengths.y>0) {
                // A zero MC observation borrows only positive measured prior
                // population noise, never zero variance or an unknown maximum.
                if(!reflectionNoiseKnown) reflectionFrameVariance=oldKnownFrameVariance;
                float previousFrameVariance=oldNoiseKnown?oldKnownFrameVariance:reflectionFrameVariance;
                vec3 innovation=reflected-previousReflection;
                float noise=max(reflectionFrameVariance,previousFrameVariance)+
                    previousFrameVariance/float(max(oldConfidence,1));
                reflectionConfidence=1;
                // Compare source innovation to FRAME noise, not accumulated
                // mean noise: a legitimate local MC miss must retain history.
                if(dot(innovation,innovation)<=9*noise) {
                    float retained=oldConfidence>0?float(oldConfidence)/float(oldConfidence+1):.9;
                    reflected=mix(reflected,previousReflection,retained);
                    // A physically valid cold mean keeps exact legacy retention,
                    // but bootstraps only one conservative effective estimate.
                    reflectionFrameVariance=oldConfidence>0?
                        mix(reflectionFrameVariance,previousFrameVariance,retained):
                        max(reflectionFrameVariance,previousFrameVariance);
                    reflectionConfidence=oldConfidence>0?min(oldConfidence+1,59):1;
                }
                // A real source change keeps the fresh color/count one.
            } else {
                reflected=mix(reflected,previousReflection,.9);
                reflectionConfidence=0;
            }
        }
    }
    if(!typed && surfaceCoverage<.99999) {
        // Preserve exact current sky/opposite-surface/shadow coverage. Only
        // this surface's contribution is replaced by its denoised lighting.
        gain=coverageGain(max(currentLit-surfaceLit,vec3(0))+surfaceBase*gain,currentBase);
        reflected=max(currentReflected-surfaceReflection,vec3(0))+reflected*surfaceCoverage;
    }
    gainOut=vec4(gain,typed?sunState:0);
    reflectionOut=vec4(reflected,historyAlpha(reflectionConfidence,reflectionFrameVariance));
    baseOut=vec4(authored,eligible?float(token):-1);
    geometryOut=surfaceValid?
        ivec4(encodeNormal(normal),encodeNormal(physical),floatBitsToInt(typed?max(ao,0):-1.0),
              floatBitsToInt(geometryDepth)):ivec4(0,0,floatBitsToInt(-1.0),0);
}
