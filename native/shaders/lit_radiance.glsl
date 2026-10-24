// Composed RT illumination of one native-resolution temporal texel, shared by
// display.frag (FSR off) and fsr_inputs.frag (FSR input). The includer declares
// p, rasterColor, accumulatedGain, accumulatedGeometry, accumulatedReflection and sourceSun.
vec3 currentSunModulation(ivec2 target) {
    vec2 sourceUV=(vec2(target)+.5)/vec2(textureSize(accumulatedGain,0))+
        (p.temporal.y>0?p.jitter.xy:vec2(0));
    ivec2 size=textureSize(rasterColor,0);
    vec2 pixel=sourceUV*vec2(size);
    bool reconstruct=p.temporal.y>0 || any(notEqual(p.extent.xy,p.extent.zw));
    ivec2 corner=ivec2(floor(reconstruct?pixel-.5:pixel));
    vec2 phase=reconstruct?fract(pixel-.5):vec2(0);
    vec3 baseSum=vec3(0),shadowedSum=vec3(0);
    float shadowSum=0,total=0;
    for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
        float w=(x==0?1-phase.x:phase.x)*(y==0?1-phase.y:phase.y);
        if(w<=.00001) continue;
        ivec2 t=clamp(corner+ivec2(x,y),ivec2(0),size-1);
        // The -1 marker guarantees every active current tap has queried A.
        // RGBA8 G is finite [0,1]; its unqueried default is never admitted.
        float visibility=texelFetch(sourceSun,t,0).g;
        vec3 raw=texelFetch(rasterColor,t,0).rgb;
        vec3 b=pow(max(raw,vec3(0)),vec3(2.2));
        float shadow=mix(1,.3+.7*visibility,p.strengths.z);
        baseSum+=b*w; shadowedSum+=b*shadow*w;
        shadowSum+=shadow*w; total+=w;
    }
    vec3 modulation;
    for(int channel=0;channel<3;++channel)
        modulation[channel]=baseSum[channel]==0?shadowSum/total:
            shadowedSum[channel]/baseSum[channel];
    return clamp(modulation,vec3(.3),vec3(1));
}
// Linear artwork `base` lit by temporal texel t: base*composed + reflection,
// where composed is gain plus AO*Sun. The raster already applied the original
// display-domain distance fog mix(.2,c,f), f=clamp((2000-W)/1000,0,1), to every
// world draw when p.rayBasisX.w>0; lighting the fogged colour would modulate the
// fog, so a partly fogged surface is unfogged, lit, then fogged again. `depth` is the view
// depth (perspective clip W, raster abs(vEye.z)) of the surface `base` was sampled from, or
// <=0 for no ray surface (sky, non-ray draws, grazing facets): those keep the plain composition.
// It must belong to `base` itself, not to texel t: at silhouettes against the sky or a nearer
// surface a jittered sample and its lighting texel differ, and mixing them flickers under FSR.
// `sampleSun` >= 0 is the Sun visibility traced from `base`'s own surface point (the FSR input's
// per-sample ray); < 0 uses texel t's reconstructed Sun.
vec3 litRadiance(vec3 base,ivec2 t,float depth,float sampleSun,out vec3 composed,out vec3 reflection) {
    vec4 gain=texelFetch(accumulatedGain,t,0);
    float ao=intBitsToFloat(texelFetch(accumulatedGeometry,t,0).z);
    composed=gain.rgb;
    if(ao>=0) {
        // AO's fixed 10-unit rays draw dark outlines along every ledge and window frame of buildings
        // down the street, which read as artefacts and alias at render resolution: keep it in the
        // near street (up to 200 units, about 5 m) and fade it to none at 500.
        if(depth>0) ao=mix(ao,1,smoothstep(200.,500.,depth));
        if(p.strengths.z<=0) composed+=ao;
        else {
            vec3 sun=sampleSun>=0?vec3(mix(1,.3+.7*sampleSun,p.strengths.z)):
                gain.a>=0?vec3(mix(1,.3+.7*gain.a,p.strengths.z)):currentSunModulation(t);
            // The same modulation scales retained artwork AA; GI and reflection
            // remain independent of the deterministic current-frame Sun.
            composed+=ao*sun;
        }
    }
    reflection=texelFetch(accumulatedReflection,t,0).rgb;
    vec3 lit=base*composed+reflection;
    // Orthographic depth is 1+NDC depth (<=2): f is 1 there, as nearer than 1000.
    float f=clamp((2000-depth)/1000,0,1);
    if(p.rayBasisX.w<=0 || !(depth>0) || f>=1) return lit;
    // Display-domain refog without dividing by f: the raster colour is c_f = .2(1-f) + D with D = f*c for a
    // plain surface, so .2(1-f) + f*lit(c) = .2(1-f) + (D^2.2*composed + f^2.2*reflection)^(1/2.2) exactly.
    // Blended effects (pickups, glows) drawn over a fogged surface carry no depth of their own and only add
    // to D, so they stay visible and lit like before; unfogging by 1/f erased them where f reached 0.
    vec3 deviation=pow(max(base,vec3(0)),vec3(1/2.2))-.2*(1-f);
    vec3 scaled=pow(abs(deviation),vec3(2.2))*composed+pow(f,2.2)*reflection;
    return pow(max(.2*(1-f)+sign(deviation)*pow(scaled,vec3(1/2.2)),vec3(0)),vec3(2.2));
}
// Temporal texel t's own accumulated base: its geometry W is that surface's depth.
vec3 litRadiance(vec3 base,ivec2 t) {
    vec3 composed,reflection;
    return litRadiance(base,t,intBitsToFloat(texelFetch(accumulatedGeometry,t,0).w),-1,composed,reflection);
}
