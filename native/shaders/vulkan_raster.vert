#version 460
layout(location=0) in vec3 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec3 normal;
layout(set=0,binding=1,std140) uniform Draw {
    mat4 modelView, projection, normalEye, normalWorld;
    vec4 light, diffuse, color, uvTransform, worldUp, flags, alphaJitter;
} u;
layout(location=0) out vec2 vUv;
layout(location=1) out vec4 vColor;
layout(location=2) out vec3 vEye;
layout(location=3) out vec3 vNormal;
layout(location=4) out vec3 vWorldNormal;
void main() {
    vec4 eye=u.modelView*vec4(position,1);
    gl_Position=u.projection*eye;
    gl_Position.xy+=u.alphaJitter.zw*gl_Position.w;
    vUv=uv*u.uvTransform.xy+u.uvTransform.zw;
    vEye=eye.xyz;
    vNormal=mat3(u.normalEye)*normal;
    vWorldNormal=mat3(u.normalWorld)*normal;
    if(u.flags.x>0.5 && u.flags.y<0.5) {
        float d=max(dot(vNormal,normalize(u.light.xyz-eye.xyz*u.light.w)),0);
        vColor=vec4(clamp(vec3(.456)+.8*u.diffuse.rgb*d,0,1),1);
    } else vColor=u.color;
}
