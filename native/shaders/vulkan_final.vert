#version 460
layout(location=0) out vec2 vUv;
void main() {
    vec2 p=vec2(gl_VertexIndex==1?3:-1,gl_VertexIndex==2?3:-1);
    gl_Position=vec4(p,0,1);
    vUv=p*.5+.5;
}
