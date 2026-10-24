#version 460
#extension GL_GOOGLE_include_directive : require
#define YAMI_PENDING 1
#define YAMI_PENDING_ACCESS readonly
#include "effects_rt.glsl"
layout(location=0) flat out vec4 gain;
layout(location=1) flat out vec4 reflection;
// One point per pixel effects_rt.comp finished. Rasterizing its final values into the effects
// targets (rather than storing them from compute) keeps effects.frag's colour-export conversion.
void main() {
    Pending s=pending[gl_VertexIndex];
    gl_Position=vec4(s.pixel/p.extent.xy*2-1,0,1);
    gl_PointSize=1;
    gain=s.gain; reflection=s.reflection;
}
