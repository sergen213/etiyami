#version 460
layout(location=0) in vec2 vUv;
layout(location=0) out vec4 gainOut;
layout(location=1) out vec4 geometryOut;
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
} p;
layout(set=0,binding=4) uniform sampler2D historyGain;
layout(set=0,binding=5) uniform sampler2D historyGeometry;
layout(set=0,binding=6) uniform sampler2D rasterColor;
layout(set=0,binding=7) uniform sampler2D currentReflection;
layout(set=0,binding=8) uniform sampler2D historyBase;
layout(set=0,binding=9) uniform sampler2D historyReflection;
layout(set=0,binding=10) uniform isampler2D worldMotion;
layout(set=0,binding=11) uniform isampler2D previousNormal;
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
bool previousNormalAt(ivec2 t,out vec3 normal) {
    int encoded=texelFetch(previousNormal,t,0).x;
    if(encoded==int(0x80008000u)) return false;
    vec2 oct=unpackSnorm2x16(uint(encoded));
    normal=vec3(oct,1-abs(oct.x)-abs(oct.y));
    if(normal.z<0) normal.xy=(1-abs(normal.yx))*
        mix(vec2(-1),vec2(1),greaterThanEqual(normal.xy,vec2(0)));
    normal=normalize(normal); return true;
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
bool sourceAgrees(ivec2 t,vec3 at,vec3 normal,int token,float tolerance) {
    vec4 n=normalAt(t); float z=texelFetch(worldDepth,t,0).r;
    if(!worldAt(n,z) || tokenAt(t)!=token) return false;
    vec3 adjacent=normalize(n.xyz);
    if(dot(adjacent,normal)<(token>0?.8:.9)) return false;
    vec3 delta=position((vec2(t)+.5)/p.extent.xy,centerDepthAt(t))-at;
    // A smooth curved surface's chord lies in the symmetric tangent plane.
    // Keep the strict depth tolerance instead of admitting a depth-gradient
    // band that could join overlapping limbs or a revealed background.
    return abs(dot(delta,normalize(adjacent+normal)))<=tolerance;
}
bool currentSurface(ivec2 reference,vec3 referenceAt,vec2 uv,
                    vec3 normal,int token,float tolerance,out vec3 at,
                    out ivec2 neighbor[2],out vec3 weights,out vec3 depths,
                    out float nativeDepth) {
    // Validate the physical surface before interpolating its projective plane.
    // Authored shading normals remain guidance, not the reconstruction plane.
    vec3 axis[2];
    depths.x=centerDepthAt(reference);
    for(int component=0;component<2;++component) {
        bool found=false;
        for(int side=0;side<2;++side) {
            ivec2 offset=component==0?ivec2(side==0?1:-1,0):ivec2(0,side==0?1:-1);
            ivec2 t=reference+offset;
            if(any(lessThan(t,ivec2(0))) || any(greaterThanEqual(t,ivec2(p.extent.xy)))) continue;
            if(!sourceAgrees(t,referenceAt,normal,token,tolerance)) continue;
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
                     out vec3 oldCoordinate[3]) {
    if(token<0 || !previousNormalAt(reference,previousN)) return false;
    oldCoordinate[0]=intBitsToFloat(texelFetch(worldMotion,reference,0).xyz);
    for(int component=0;component<2;++component) {
        vec3 oldN;
        if(!previousNormalAt(neighbor[component],oldN) || dot(oldN,previousN)<.8) return false;
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
bool currentArtistMapping(vec3 currentDepths,vec3 oldCoordinate[3],
                          out vec4 inverseAxes,out vec3 wRatio) {
    // Invert the relative old screen basis once for the native fragment.
    // Screen barycentrics carry clip W in opposite directions in each pose.
    vec2 axisX=oldCoordinate[1].xy-oldCoordinate[0].xy;
    vec2 axisY=oldCoordinate[2].xy-oldCoordinate[0].xy;
    float xx=dot(axisX,axisX),yy=dot(axisY,axisY);
    float determinant=axisX.x*axisY.y-axisX.y*axisY.x;
    if(isnan(xx) || isinf(xx) || isnan(yy) || isinf(yy) ||
       isnan(determinant) || isinf(determinant) ||
       determinant*determinant<=xx*yy*.000001) return false;
    inverseAxes=vec4(axisY.y,-axisY.x,-axisX.y,axisX.x)/determinant;
    vec3 currentW=orthographic(p.currentVP)?vec3(1):currentDepths;
    vec3 oldW=orthographic(p.previousVP)?vec3(1):
        vec3(oldCoordinate[0].z,oldCoordinate[1].z,oldCoordinate[2].z);
    wRatio=currentW/oldW;
    return !any(isnan(inverseAxes)) && !any(isinf(inverseAxes)) &&
        !any(isnan(wRatio)) && !any(isinf(wRatio)) && all(greaterThan(wRatio,vec3(0)));
}
vec3 cachedRasterAt(ivec2 t,inout ivec2 coordinates[8],inout vec3 colors[8],
                    inout int count,inout int next) {
    for(int i=0;i<count;++i) if(all(equal(t,coordinates[i]))) return colors[i];
    vec3 color=rasterAt(t);
    coordinates[next]=t; colors[next]=color;
    count=min(count+1,8); next=(next+1)%8;
    return color;
}
bool currentArtistAt(ivec2 reference,ivec2 neighbor[2],vec2 oldOrigin,
                     vec4 inverseAxes,vec3 wRatio,vec2 oldTapUV,ivec2 size,
                     vec3 referenceAt,vec3 normal,int token,float tolerance,
                     inout ivec2 coordinates[8],inout vec3 colors[8],
                     inout int count,inout int next,out vec3 color) {
    vec2 delta=oldTapUV-oldOrigin;
    vec2 amount=vec2(dot(inverseAxes.xy,delta),dot(inverseAxes.zw,delta));
    vec3 weights=vec3(1-amount.x-amount.y,amount)*wRatio;
    float total=dot(weights,vec3(1));
    if(total<=0 || isnan(total) || isinf(total) ||
       any(isnan(weights)) || any(isinf(weights))) return false;
    weights/=total;
    // SOURCE pixel centres already contain the current raster jitter.
    vec2 pixel=vec2(reference)+.5+weights.y*vec2(neighbor[0]-reference)+
        weights.z*vec2(neighbor[1]-reference);
    if(any(isnan(pixel)) || any(isinf(pixel)) ||
       any(lessThan(pixel,vec2(.5))) || any(greaterThan(pixel,vec2(size)-.5))) return false;
    ivec2 corner=ivec2(floor(pixel-.5)); vec2 phase=fract(pixel-.5);
    color=vec3(0); float acceptedWeight=0;
    for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
        float w=(x==0?1-phase.x:phase.x)*(y==0?1-phase.y:phase.y);
        if(w<=.00001) continue;
        ivec2 t=corner+ivec2(x,y);
        if(any(lessThan(t,ivec2(0))) || any(greaterThanEqual(t,size)) ||
           !sourceAgrees(t,referenceAt,normal,token,tolerance) ||
           texelFetch(currentReflection,t,0).a!=1) return false;
        // Decode each resolved display-authored tap before interpolation,
        // exactly as for the current native artwork reconstruction.
        color+=cachedRasterAt(t,coordinates,colors,count,next)*w; acceptedWeight+=w;
    }
    if(acceptedWeight<=.00001) return false;
    color/=acceptedWeight;
    return true;
}
bool historyBaseAt(ivec2 t,ivec2 size,int token,vec3 at,vec3 normal,
                   float clipW,out vec4 base) {
    if(any(lessThan(t,ivec2(0))) || any(greaterThanEqual(t,size))) return false;
    base=texelFetch(historyBase,t,0);
    vec4 geometry=texelFetch(historyGeometry,t,0);
    if(base.a!=float(token) || geometry.w<=0) return false;
    if(dot(geometry.xyz,normal)<(token>0?.8:.9)) return false;
    vec3 delta=oldPosition((vec2(t)+.5)/vec2(size),geometry.w)-at;
    return abs(dot(delta,normalize(geometry.xyz+normal)))<=clipW*.001;
}
void main() {
    ivec2 size=textureSize(currentGain,0);
    bool taa=p.temporal.y>0;
    vec2 sampleUV=vUv+(taa?p.jitter.xy:vec2(0));
    vec2 pixel=sampleUV*vec2(size);
    ivec2 reference=clamp(ivec2(floor(pixel)),ivec2(0),size-1);
    vec2 referenceUV=(vec2(reference)+.5)/vec2(size);
    vec4 n=normalAt(reference);
    float z=texelFetch(worldDepth,reference,0).r;
    int token=tokenAt(reference);
    vec3 authored=rasterAt(reference),baseLo=authored,baseHi=authored;
    ivec2 rasterCoordinates[8]; vec3 rasterColors[8];
    rasterCoordinates[0]=reference; rasterColors[0]=authored;
    int rasterCount=1,rasterNext=1;
    bool world=worldAt(n,z),eligible=world && token>=0 && (n.w>.5 || p.temporal.w>0);
    vec3 normal=world?normalize(n.xyz):vec3(0);
    float centerDepth=centerDepthAt(reference);
    vec3 referenceAt=world?position(referenceUV,centerDepth):vec3(0),at=referenceAt;
    float geometryDepth=0,tolerance=0;
    bool surfaceValid=world;
    vec3 screenWeights,currentDepths;
    ivec2 neighbor[2];
    if(world) {
        bool ortho=orthographic(p.currentVP);
        tolerance=abs(p.temporal.w>0?(ortho?1:centerDepth):
            (p.currentVP*vec4(referenceAt,1)).w)*.001;
        if(p.temporal.w>0)
            surfaceValid=currentSurface(reference,referenceAt,sampleUV,normal,token,tolerance,
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
    eligible=eligible && rawReflectionSample.a>.5;
    bool artistValid=eligible && rawReflectionSample.a<1.5;
    vec3 currentLit=authored*rawGain.rgb,currentReflected=rawReflection;
    vec3 surfaceBase=world?authored:vec3(0),surfaceLit=world?currentLit:vec3(0);
    vec3 surfaceReflection=world?rawReflection:vec3(0);
    float surfaceCoverage=world?1:0,visibility=rawGain.a;
    // Reconstruct deterministic artwork separately. Its local footprint bounds
    // are not illumination bounds and must not collapse to the current color.
    if(taa || any(notEqual(p.extent.xy,p.extent.zw))) {
        ivec2 corner=ivec2(floor(pixel-.5)); vec2 phase=fract(pixel-.5);
        authored=vec3(0); currentLit=vec3(0); currentReflected=vec3(0);
        surfaceBase=vec3(0); surfaceLit=vec3(0); surfaceReflection=vec3(0);
        surfaceCoverage=0; visibility=0; float acceptedWeight=0;
        for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
            ivec2 t=clamp(corner+ivec2(x,y),ivec2(0),size-1);
            float w=(x==0?1-phase.x:phase.x)*(y==0?1-phase.y:phase.y);
            if(w<=.00001) continue;
            vec4 adjacent=normalAt(t);
            bool belongs=world && sourceAgrees(t,referenceAt,normal,token,tolerance);
            bool sameSky=!world && z>=1 && texelFetch(worldDepth,t,0).r>=1;
            // Upscaling without TAA must not invent opposite-surface coverage.
            // The selected raster sample already contains authored MSAA
            // coverage; gate taps BEFORE mixing any base or lighting signal.
            if(!taa && !belongs && !sameSky && any(notEqual(t,reference))) continue;
            vec4 reflectionSample=texelFetch(currentReflection,t,0);
            vec3 b=taa && token>0 && p.temporal.x>0 && p.temporal.w>0 && artistValid?
                cachedRasterAt(t,rasterCoordinates,rasterColors,rasterCount,rasterNext):rasterAt(t);
            vec3 r=reflectionSample.rgb;
            vec4 g=texelFetch(currentGain,t,0);
            authored+=b*w; currentLit+=b*g.rgb*w; currentReflected+=r*w;
            visibility+=g.a*w; acceptedWeight+=w;
            if(!belongs || reflectionSample.a<=.5 || (adjacent.w<=.5 && p.temporal.w<=0)) eligible=false;
            if(!belongs || reflectionSample.a<.5 || reflectionSample.a>1.5) artistValid=false;
            if(belongs && abs(g.a-rawGain.a)<=.01) {
                surfaceBase+=b*w; surfaceLit+=b*g.rgb*w;
                surfaceReflection+=r*w; surfaceCoverage+=w;
            }
        }
        if(acceptedWeight>.00001) {
            authored/=acceptedWeight; currentLit/=acceptedWeight; currentReflected/=acceptedWeight;
            surfaceBase/=acceptedWeight; surfaceLit/=acceptedWeight; surfaceReflection/=acceptedWeight;
            surfaceCoverage/=acceptedWeight; visibility/=acceptedWeight;
        } else {
            authored=rasterAt(reference); currentLit=authored*rawGain.rgb; currentReflected=rawReflection;
            surfaceBase=world?authored:vec3(0); surfaceLit=world?currentLit:vec3(0);
            surfaceReflection=world?rawReflection:vec3(0); surfaceCoverage=world?1:0; visibility=rawGain.a;
        }
    }
    vec3 currentBase=authored;
    gainOut=vec4(coverageGain(currentLit,currentBase),0);
    artistValid=artistValid && eligible;
    reflectionOut=vec4(currentReflected,artistValid?1:0);
    baseOut=vec4(authored,-1); geometryOut=vec4(0);
    if(!world) return;
    // Mixed visibility is composited for display below, not a selected-surface
    // lighting signal: carrying it as such would composite its shadow twice.
    // Artwork history stays independent of this lighting-only restriction.
    bool lightingHistoryEligible=eligible && surfaceCoverage>=.99999;
    vec3 gainSum=vec3(0),reflectionSum=vec3(0);
    float weight=0;
    for(int y=-2;y<=2;++y) for(int x=-2;x<=2;++x) {
        ivec2 t=reference+ivec2(x,y);
        if(any(lessThan(t,ivec2(0))) || any(greaterThanEqual(t,size))) continue;
        if(!sourceAgrees(t,referenceAt,normal,token,tolerance)) continue;
        if(taa && abs(x)<=1 && abs(y)<=1) {
            baseLo=min(baseLo,rasterAt(t)); baseHi=max(baseHi,rasterAt(t));
        }
        vec4 g=texelFetch(currentGain,t,0);
        // Hard visibility never diffuses across a blocker boundary; artwork
        // history remains independent of this lighting-only gate.
        if(abs(g.a-rawGain.a)>.01) continue;
        float similarity=dot(normal,normalize(normalAt(t).xyz));
        vec2 offset=vec2(t)+.5-pixel;
        float w=exp(-dot(offset,offset)*.45)*pow(max(similarity,0),8);
        vec3 r=texelFetch(currentReflection,t,0).rgb;
        gainSum+=g.rgb*w; reflectionSum+=r*w; weight+=w;
    }
    vec3 gain=weight>.00001?gainSum/weight:rawGain.rgb;
    vec3 reflected=weight>.00001?reflectionSum/weight:rawReflection;
    // Physical signal bounds retain Monte Carlo history even if this frame's
    // entire local ray kernel misses. Local extrema alone freeze actor noise.
    float minimumGain=mix(1,0,p.strengths.x)*mix(1,.3,p.strengths.z);
    vec3 previousAt=at,previousN=normal;
    vec2 oldUV=vec2(0); float oldDepth=0,oldClipW=0;
    vec3 oldCoordinate[3];
    bool correspondence=false;
    if(p.temporal.x>0 && eligible && surfaceValid) {
        if(p.temporal.w>0) {
            correspondence=dynamicPrevious(reference,token,neighbor,screenWeights,currentDepths,
                                           oldUV,oldDepth,previousAt,previousN,oldCoordinate);
            oldClipW=orthographic(p.previousVP)?1:oldDepth;
        } else if(token==0) {
            vec4 clip=p.previousVP*vec4(previousAt,1);
            correspondence=clip.w>0;
            oldUV=clip.xy/clip.w*.5+.5; oldClipW=clip.w;
        }
    }
    if(p.temporal.x>0 && eligible && correspondence && oldClipW>0) {
        ivec2 oldSize=textureSize(historyBase,0);
        vec2 oldPixel=oldUV*vec2(oldSize)-.5;
        ivec2 corner=ivec2(floor(oldPixel)); vec2 phase=fract(oldPixel);
        vec3 oldGain=vec3(0),oldReflection=vec3(0);
        float lightingWeight=0;
        bool artistEnabled=taa && artistValid && token>0 && p.temporal.w>0;
        vec4 inverseAxes; vec3 wRatio;
        bool artistComplete=false;
        if(artistEnabled)
            artistComplete=currentArtistMapping(currentDepths,oldCoordinate,inverseAxes,wRatio);
        vec3 artistResidual=vec3(0),oldBaseLo=authored,oldBaseHi=authored;
        for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
            ivec2 t=corner+ivec2(x,y);
            float w=(x==0?1-phase.x:phase.x)*(y==0?1-phase.y:phase.y);
            if(w<=.00001 || (!lightingHistoryEligible && !artistComplete)) continue;
            vec4 b;
            if(!historyBaseAt(t,oldSize,token,previousAt,previousN,oldClipW,b)) {
                artistComplete=false;
                continue;
            }
            // Both signals reuse positive taps and the same physical owner;
            // artwork is independent of the lighting gain and shadow gates.
            vec4 oldRef=texelFetch(historyReflection,t,0);
            if(artistComplete) {
                vec3 counterpart;
                if(oldRef.a!=1 ||
                   !currentArtistAt(reference,neighbor,oldCoordinate[0].xy,inverseAxes,wRatio,
                                    (vec2(t)+.5)/vec2(oldSize),size,referenceAt,
                                    normal,token,tolerance,rasterCoordinates,rasterColors,
                                    rasterCount,rasterNext,counterpart))
                    artistComplete=false;
                else {
                    artistResidual+=(b.rgb-counterpart)*w;
                    oldBaseLo=min(oldBaseLo,b.rgb); oldBaseHi=max(oldBaseHi,b.rgb);
                }
            }
            if(lightingHistoryEligible) {
                vec4 g=texelFetch(historyGain,t,0);
                if(g.a>.5 && abs(g.a-(.75+.25*visibility))<.01) {
                    oldGain+=g.rgb*w; oldReflection+=oldRef.rgb*w;
                    lightingWeight+=w;
                }
            }
        }
        if(artistComplete) {
            // Carry AA error at each old native-grid physical centre, never
            // recursively filter whole artwork or renormalize a partial owner.
            vec3 candidate=authored+.88*artistResidual;
            baseLo=min(baseLo,oldBaseLo); baseHi=max(baseHi,oldBaseHi);
            authored=clamp(candidate,baseLo,baseHi);
        }
        if(lightingWeight>.00001) {
            // Indirect gain and reflections are unbounded HDR; only their
            // physical lower bounds are known. Single-frame misses must not
            // erase samples collected from valid earlier rays.
            gain=mix(gain,max(oldGain/lightingWeight,vec3(minimumGain)),.9);
            reflected=mix(reflected,max(oldReflection/lightingWeight,vec3(0)),.9);
        }
    }
    if(surfaceCoverage<.99999) {
        // Preserve exact current sky/opposite-surface/shadow coverage. Only
        // this surface's contribution is replaced by its denoised lighting.
        gain=coverageGain(max(currentLit-surfaceLit,vec3(0))+surfaceBase*gain,currentBase);
        reflected=max(currentReflected-surfaceReflection,vec3(0))+reflected*surfaceCoverage;
    }
    gainOut=vec4(gain,lightingHistoryEligible?.75+.25*visibility:0);
    reflectionOut=vec4(reflected,artistValid?1:0);
    baseOut=vec4(authored,eligible?float(token):-1);
    geometryOut=surfaceValid?vec4(normal,geometryDepth):vec4(0);
}
