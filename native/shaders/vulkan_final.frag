#version 460
layout(location=0) in vec2 vUv;
layout(set=0,binding=0) uniform sampler2D frame;
layout(push_constant) uniform Final { float ramp; uint copyOnly; } u;
layout(location=0) out vec4 outColor;
void main() {
    vec4 value=texture(frame,vUv);
    if(u.copyOnly!=0) { outColor=value; return; }
    vec3 indices=floor(clamp(value.rgb,0,1)*255+.5);
    outColor=vec4(min(indices*u.ramp,vec3(65535))/65535,value.a);
}
