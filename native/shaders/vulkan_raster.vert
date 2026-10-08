#version 460
layout(location=0) in vec3 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec3 normal;
layout(location=3) in vec3 previousPosition;
layout(location=4) in vec3 previousNormal;
layout(set=0,binding=1,std140) uniform Draw {
    mat4 modelView, projection, normalEye, normalWorld;
    vec4 light, diffuse, color, uvTransform, worldUp, flags, alphaJitter;
    mat4 previousMVP, previousNormalWorld;
    ivec4 temporal;
} u;
layout(location=0) out vec2 vUv;
layout(location=1) out vec4 vColor;
layout(location=2) out vec3 vEye;
layout(location=3) out vec3 vNormal;
layout(location=4) out vec3 vWorldNormal;
layout(location=5) out vec4 vPreviousClip;
layout(location=6) out vec3 vPreviousNormal;
layout(location=7) out vec2 vCurrentDepth;
void main() {
    vec4 eye=u.modelView*vec4(position,1);
    gl_Position=u.projection*eye;
    gl_Position.xy+=u.alphaJitter.zw*gl_Position.w;
    vCurrentDepth=gl_Position.zw;
    vUv=uv*u.uvTransform.xy+u.uvTransform.zw;
    vEye=eye.xyz;
    vNormal=mat3(u.normalEye)*normal;
    vWorldNormal=mat3(u.normalWorld)*normal;
    vPreviousClip=u.previousMVP*vec4(previousPosition,1);
    vPreviousNormal=mat3(u.previousNormalWorld)*previousNormal;
    if(u.flags.x>0.5 && u.flags.y<0.5) {
        float d=max(dot(vNormal,normalize(u.light.xyz-eye.xyz*u.light.w)),0);
        vColor=vec4(clamp(vec3(.456)+.8*u.diffuse.rgb*d,0,1),1);
    } else vColor=u.color;
}
