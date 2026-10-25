#version 460
layout(location=0) in vec3 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec3 normal;
layout(location=3) in vec3 previousPosition;
layout(location=4) in vec3 previousNormal;
layout(set=0,binding=1,std140) uniform Draw {
    mat4 modelView, projection;
    mat3 normalEye;
    vec4 rasterGrid;
    mat4 normalWorld;
    vec4 light, diffuse, color, uvTransform, worldUp, flags, alphaJitter;
    mat4 previousMVP, previousNormalWorld;
    ivec4 temporal;
    vec4 environment,fog;
} u;
layout(location=0) out vec2 vUv;
layout(location=1) out vec4 vColor;
layout(location=2) out vec3 vEye;
layout(location=3) out vec3 vNormal;
layout(location=4) out vec3 vWorldNormal;
layout(location=5) out vec4 vPreviousClip;
layout(location=6) out vec3 vPreviousNormal;
layout(location=7) out vec4 vCurrentClip;
layout(location=8) out vec3 vPosition;
layout(location=9) out vec3 vPreviousPosition;
void main() {
    vec4 eye=u.modelView*vec4(position,1);
    gl_Position=u.projection*eye;
    gl_Position.xy+=u.alphaJitter.zw*gl_Position.w;
    vCurrentClip=gl_Position;
    vUv=uv*u.uvTransform.xy+u.uvTransform.zw;
    vEye=eye.xyz;
    vNormal=u.normalEye*normal;
    vWorldNormal=mat3(u.normalWorld)*normal;
    vPreviousClip=u.temporal.w!=0?u.previousMVP*vec4(previousPosition,1):vec4(0);
    vPreviousNormal=u.temporal.w!=0?mat3(u.previousNormalWorld)*previousNormal:vec3(0);
    vPosition=position;
    vPreviousPosition=previousPosition;
    if(u.flags.x>0.5 && u.flags.y<0.5) {
        float d=max(dot(vNormal,normalize(u.light.xyz-eye.xyz*u.light.w)),0);
        vColor=vec4(clamp(vec3(.456)*u.environment.x+.8*u.diffuse.rgb*d*u.environment.y,0,1),1);
    } else vColor=u.color;
}
