#version 460
layout(set=0,binding=0) uniform sampler2D artwork;
layout(set=1,binding=2) uniform sampler2D relief;
layout(set=0,binding=1,std140) uniform Draw {
    mat4 modelView, projection;
    mat3 normalEye;
    vec4 rasterGrid;
    mat4 normalWorld;
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
layout(location=7) in vec4 vCurrentClip;
layout(location=8) in vec3 vPosition;
layout(location=9) in vec3 vPreviousPosition;
layout(location=0) out vec4 outColor;
#ifndef YAMI_INTERFACE_ONLY
layout(location=1) out ivec4 outNormal;
layout(location=2) out ivec4 outMotion;
layout(location=3) out ivec4 outPreviousNormal;
int packFacet(vec3 facet,mat3 normalWorld,vec3 authored) {
    vec3 n=normalWorld*facet;
    if(any(isnan(n)) || any(isinf(n)) || any(isnan(authored)) || any(isinf(authored)))
        return int(0x80008000u);
    float magnitude=max(max(abs(n.x),abs(n.y)),abs(n.z));
    float authoredMagnitude=max(max(abs(authored.x),abs(authored.y)),abs(authored.z));
    if(!(magnitude>0) || !(authoredMagnitude>0)) return int(0x80008000u);
    n/=magnitude;
    if(dot(n,authored/authoredMagnitude)<0) n=-n;
    n/=abs(n.x)+abs(n.y)+abs(n.z);
    vec2 oct=n.xy;
    if(n.z<0) oct=(1-abs(oct.yx))*mix(vec2(-1),vec2(1),greaterThanEqual(oct,vec2(0)));
    return int(packSnorm2x16(oct));
}
#endif
// IKV relief: 8-24 occlusion steps, one secant refinement, flat depth/motion/RT geometry.
vec2 reliefUV(vec2 uv,out vec3 gradient) {
    gradient=vec3(0);
    vec3 dp1=dFdx(vEye),dp2=dFdy(vEye),nf=cross(dp1,dp2),view=-vEye;
    vec2 du1=dFdx(uv),du2=dFdy(uv);float det=du1.x*du2.y-du2.x*du1.y,dist=length(view);
    if(abs(det)<1e-12||dot(nf,nf)<1e-20||dist<1e-4)return uv;
    nf=normalize(nf);view/=dist;if(dot(nf,view)<0.0)nf=-nf;
    vec3 pu=(dp1*du2.y-dp2*du1.y)/det,pv=(dp2*du1.x-dp1*du2.x)/det;
    vec3 gu=cross(pv,nf),gv=cross(nf,pu);gu/=dot(pu,gu);gv/=dot(pv,gv);
    float c=dot(view,nf),depth=smoothstep(.05,.18,c)*(1.0-smoothstep(50.,90.,dist))*
        min(12.0*min(length(pu),length(pv))/float(textureSize(relief,0).x),.6);
    if(!(depth>1e-3))return uv;
    vec2 shift=-vec2(dot(view,gu),dot(view,gv))/c*depth;
    int steps=int(mix(24.,8.,c));float t=0.0,h=textureGrad(relief,uv,du1,du2).r,lastT=0.0,lastF=h-1.0;
    for(int i=0;i<steps&&t<1.0-h;++i){lastT=t;lastF=t-(1.0-h);t+=1.0/float(steps);h=textureGrad(relief,uv+shift*t,du1,du2).r;}
    float f=t-(1.0-h);if(f>0.0&&lastF<0.0)t=mix(lastT,t,lastF/(lastF-f));
    vec2 surface=uv+shift*t;
    vec2 slope=(textureGrad(relief,surface,du1,du2).gb-.5)*.5*vec2(textureSize(relief,0));
    gradient=.7*depth*(slope.x*gu+slope.y*gv);
    return surface;
}

void main() {
#ifndef YAMI_INTERFACE_ONLY
    // Local derivatives avoid translated-world cancellation; evaluate before discard.
    int currentFacet=packFacet(cross(dFdxFine(vPosition),dFdyFine(vPosition)),mat3(u.normalWorld),vWorldNormal);
    int previousFacet=u.temporal.w!=0?packFacet(cross(dFdxFine(vPreviousPosition),dFdyFine(vPreviousPosition)),mat3(u.previousNormalWorld),vPreviousNormal):int(0x80008000u);
    // Hardware snaps vertex coverage; these affine fields recover the physical
    // point at the ideal pixel centre without changing artwork or actual depth.
    float inverseW=1/vCurrentClip.w;
    vec2 q=vCurrentClip.xy*inverseW;
    vec2 depthFields=vec2(inverseW,vCurrentClip.z*inverseW);
    vec4 previousOverW=vPreviousClip*inverseW;
    vec2 qDx=dFdxFine(q),qDy=dFdyFine(q);
    vec2 depthDx=dFdxFine(depthFields),depthDy=dFdyFine(depthFields);
    vec4 previousDx=dFdxFine(previousOverW),previousDy=dFdyFine(previousOverW);
    vec2 delta=gl_FragCoord.xy*u.rasterGrid.xy+u.rasterGrid.zw-q;
    float determinant=qDx.x*qDy.y-qDy.x*qDx.y;
    float currentDepth=0;
    vec3 previousCoordinate=vec3(0);
    bool valid=false;
    bool currentValid=false;
    vec4 jacobian=vec4(qDx,qDy);
    if(vCurrentClip.w!=0 && !any(isnan(vCurrentClip)) && !any(isinf(vCurrentClip)) &&
       u.rasterGrid.x!=0 && u.rasterGrid.y!=0 &&
       !any(isnan(u.rasterGrid)) && !any(isinf(u.rasterGrid)) &&
       !any(isnan(jacobian)) && !any(isinf(jacobian)) &&
       !any(isnan(delta)) && !any(isinf(delta)) &&
       determinant!=0 && !isnan(determinant) && !isinf(determinant)) {
        vec2 lambda=vec2(delta.x*qDy.y-delta.y*qDy.x,
                         qDx.x*delta.y-qDx.y*delta.x)/determinant;
        vec2 corrected=depthFields+depthDx*lambda.x+depthDy*lambda.y;
        if(corrected.x!=0 && !any(isnan(corrected)) && !any(isinf(corrected))) {
            float currentW=1/corrected.x;
            bool orthographicCurrent=length(vec3(u.projection[0][3],u.projection[1][3],u.projection[2][3]))<1e-8;
            float depth=orthographicCurrent?1+corrected.y:currentW;
            if(currentW!=0 && !isnan(currentW) && !isinf(currentW) && !isnan(depth) && !isinf(depth)) {
                currentDepth=depth;
                currentValid=true;
                if(u.temporal.w!=0) {
                    vec4 previous=previousOverW+previousDx*lambda.x+previousDy*lambda.y;
                    float previousW=previous.w*currentW;
                    if(previous.w!=0 && previousW>0 && !isnan(previousW) && !isinf(previousW) &&
                       !any(isnan(previous)) && !any(isinf(previous))) {
                        bool orthographic=length(vec3(u.previousMVP[0][3],u.previousMVP[1][3],u.previousMVP[2][3]))<1e-8;
                        float previousDepth=orthographic?1+previous.z/previous.w:previousW;
                        vec3 coordinate=vec3(previous.xy/previous.w*.5+.5,previousDepth);
                        if(!any(isnan(coordinate)) && !any(isinf(coordinate))) {
                            previousCoordinate=coordinate;
                            valid=u.temporal.y!=0;
                        }
                    }
                }
            }
        }
    }
#endif
    vec4 value=vColor;
    bool enhanced=u.flags.y>.5;
    vec2 surface=vec2(vUv.x,u.worldUp.w>.5 ? 1-vUv.y:vUv.y);
    vec3 gradient=vec3(0);
    if(u.temporal.z!=0)surface=reliefUV(surface,gradient);
    if(u.flags.x>.5 && enhanced) {
        vec3 n=normalize(vNormal-gradient);
        float d=max(dot(n,normalize(u.light.xyz-vEye*u.light.w)),0);
        float h=dot(n,u.worldUp.xyz)*.5+.5;
        value=vec4(clamp(vec3(.416+.08*h)+.8*u.diffuse.rgb*d,0,1),1);
    }
    // Preserve the exported palette, fog and alpha compositing in the artist's
    // display-authored domain, including MSAA. The floating world attachment
    // retains emissive headroom; effects decodes the entire resolved world once.
    if(u.flags.z>.5) {
        vec4 texel=texture(artwork,surface);
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
    vec3 worldNormal=vWorldNormal-transpose(mat3(u.modelView))*gradient;
    outNormal=ivec4(round(normalize(worldNormal+vec3(1e-20))*32767),int(u.flags.w));
    outMotion=ivec4(floatBitsToInt(previousCoordinate),
                    u.flags.w>=0 && (u.temporal.x>0 || u.temporal.y!=0)?u.temporal.x:-1);
    vec3 priorNormal=normalize(vPreviousNormal+vec3(1e-20));
    priorNormal/=abs(priorNormal.x)+abs(priorNormal.y)+abs(priorNormal.z);
    vec2 oct=priorNormal.xy;
    if(priorNormal.z<0) oct=(1-abs(oct.yx))*mix(vec2(-1),vec2(1),greaterThanEqual(oct,vec2(0)));
    outPreviousNormal=ivec4(valid?int(packSnorm2x16(oct)):int(0x80008000u),
                           floatBitsToInt(currentDepth),currentValid?currentFacet:int(0x80008000u),
                           valid?previousFacet:int(0x80008000u));
#endif
}
