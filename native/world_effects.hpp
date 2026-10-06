#pragma once

namespace yami {
// Perspective depth, not artist-authored material data, drives this world-only pass.
inline constexpr char world_effect_fragment_source[] = R"GLSL(
#ifdef GL_ES
precision highp float;
precision highp int;
precision highp sampler2D;
#endif
in vec2 vUv;
out vec4 outColor;
uniform sampler2D uFrame, uDepth;
uniform vec4 uProjectionInfo;
uniform vec2 uTexelSize;
uniform vec3 uWorldUp;
uniform vec4 uEffects;

bool insideFrame(vec2 uv) {
    return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
}
float viewDepth(float depth) {
    // The perspective denominator is negative, including at the far plane.
    return uProjectionInfo.w / min(depth * 2.0 - 1.0 + uProjectionInfo.z, -0.000001);
}
vec3 viewPosition(vec2 uv, float depth) {
    float z = viewDepth(depth);
    return vec3((uv * 2.0 - 1.0) * uProjectionInfo.xy * z, -z);
}
vec3 safeNormal(vec3 value) {
    return value * inversesqrt(max(dot(value, value), 0.00000001));
}
bool projectPosition(vec3 position, out vec2 uv) {
    float nearPlane = uProjectionInfo.w / min(uProjectionInfo.z - 1.0, -0.000001);
    if (position.z >= -nearPlane) return false;
    uv = 0.5 + 0.5 * position.xy / (-position.z * uProjectionInfo.xy);
    return insideFrame(uv);
}
vec3 surfaceNormal(vec2 uv, vec3 position, out float confidence) {
    vec2 leftUv = clamp(uv - vec2(uTexelSize.x, 0.0), 0.0, 1.0);
    vec2 rightUv = clamp(uv + vec2(uTexelSize.x, 0.0), 0.0, 1.0);
    vec2 downUv = clamp(uv - vec2(0.0, uTexelSize.y), 0.0, 1.0);
    vec2 upUv = clamp(uv + vec2(0.0, uTexelSize.y), 0.0, 1.0);
    float leftDepth = texture(uDepth, leftUv).r;
    float rightDepth = texture(uDepth, rightUv).r;
    float downDepth = texture(uDepth, downUv).r;
    float upDepth = texture(uDepth, upUv).r;
    float left2 = texture(uDepth, clamp(uv - vec2(2.0*uTexelSize.x, 0.0), 0.0, 1.0)).r;
    float right2 = texture(uDepth, clamp(uv + vec2(2.0*uTexelSize.x, 0.0), 0.0, 1.0)).r;
    float down2 = texture(uDepth, clamp(uv - vec2(0.0, 2.0*uTexelSize.y), 0.0, 1.0)).r;
    float up2 = texture(uDepth, clamp(uv + vec2(0.0, 2.0*uTexelSize.y), 0.0, 1.0)).r;
    float centerDepth = texture(uDepth, uv).r;
    vec3 left = viewPosition(leftUv, leftDepth);
    vec3 right = viewPosition(rightUv, rightDepth);
    vec3 down = viewPosition(downUv, downDepth);
    vec3 up = viewPosition(upUv, upDepth);
    // Projected depth is affine on a plane. Second-neighbor extrapolation
    // selects the same surface at a floor/wall corner, unlike closest eye Z.
    float leftError = leftDepth < 0.999999 && left2 < 0.999999
        ? abs(2.0*leftDepth-left2-centerDepth) : 1.0;
    float rightError = rightDepth < 0.999999 && right2 < 0.999999
        ? abs(2.0*rightDepth-right2-centerDepth) : 1.0;
    float downError = downDepth < 0.999999 && down2 < 0.999999
        ? abs(2.0*downDepth-down2-centerDepth) : 1.0;
    float upError = upDepth < 0.999999 && up2 < 0.999999
        ? abs(2.0*upDepth-up2-centerDepth) : 1.0;
    bool useLeft = leftError < rightError;
    bool useDown = downError < upError;
    vec3 dx = useLeft ? position - left : right - position;
    vec3 dy = useDown ? position - down : up - position;
    vec3 normal = cross(dx, dy);
    float area = dot(normal, normal);
    confidence = (useLeft ? leftDepth : rightDepth) < 0.999999 &&
                 (useDown ? downDepth : upDepth) < 0.999999 && area > 0.00000001 ? 1.0 : 0.0;
    float jump = max(abs(dx.z), abs(dy.z)) / max(-position.z, 0.0001);
    confidence *= 1.0 - smoothstep(0.04, 0.12, jump);
    normal = safeNormal(normal);
    return dot(normal, -position) < 0.0 ? -normal : normal;
}

const vec2 directions[8] = vec2[8](
    vec2(1.0, 0.0), vec2(0.70710678, 0.70710678),
    vec2(0.0, 1.0), vec2(-0.70710678, 0.70710678),
    vec2(-1.0, 0.0), vec2(-0.70710678, -0.70710678),
    vec2(0.0, -1.0), vec2(0.70710678, -0.70710678));

float ambientOcclusion(vec3 position, vec3 normal) {
    float radius = clamp(-position.z * 0.045, 0.15, 45.0);
    float bias = max(radius * 0.025, 0.002);
    vec2 screenRadius = radius / (-position.z * uProjectionInfo.xy) * 0.5;
    float occlusion = 0.0;
    for (int ring = 0; ring < 2; ++ring) {
        float scale = ring == 0 ? 0.35 : 1.0;
        for (int direction = 0; direction < 8; ++direction) {
            vec2 uv = vUv + directions[direction] * screenRadius * scale;
            if (!insideFrame(uv)) continue;
            float depth = texture(uDepth, uv).r;
            if (depth >= 0.999999) continue;
            vec3 delta = viewPosition(uv, depth) - position;
            float distance = length(delta);
            float angle = max((dot(normal, delta) - bias) / max(distance, 0.0001) - 0.06, 0.0);
            float attenuation = 1.0 - smoothstep(radius * 0.25, radius, distance);
            occlusion += angle * attenuation;
        }
    }
    return clamp(occlusion * (2.4 / 16.0), 0.0, 0.65);
}

vec4 groundReflection(vec3 position, vec3 normal) {
    // ponytail: upward-facing geometry substitutes for roughness/material masks;
    // use material masks if the original assets gain PBR metadata.
    float ground = smoothstep(0.7, 0.95, dot(normal, safeNormal(uWorldUp)));
    if (ground <= 0.0) return vec4(0.0);
    vec3 incident = safeNormal(position);
    vec3 ray = reflect(incident, normal);
    float maxDistance = clamp(-position.z * 3.0, 32.0, 1600.0);
    float bias = max(-position.z * 0.0015, 0.02);
    vec3 origin = position + normal * bias;
    float previousDistance = 0.0;
    float previousGap = 0.0;
    bool previousValid = false;
    for (int stepIndex = 1; stepIndex <= 32; ++stepIndex) {
        float fraction = float(stepIndex) / 32.0;
        float distance = bias + maxDistance * fraction * fraction;
        vec3 samplePosition = origin + ray * distance;
        vec2 uv;
        if (!projectPosition(samplePosition, uv)) break;
        float depth = texture(uDepth, uv).r;
        if (depth >= 0.999999) {
            previousValid = false;
            continue;
        }
        float gap = -samplePosition.z - viewDepth(depth);
        if (previousValid && previousGap < 0.0 && gap >= 0.0) {
            float low = previousDistance;
            float high = distance;
            // Refine the front-to-back crossing, not just a nearby depth value.
            for (int refinement = 0; refinement < 5; ++refinement) {
                float middle = (low + high) * 0.5;
                vec3 middlePosition = origin + ray * middle;
                vec2 middleUv;
                if (!projectPosition(middlePosition, middleUv)) return vec4(0.0);
                float middleDepth = texture(uDepth, middleUv).r;
                if (middleDepth >= 0.999999) return vec4(0.0);
                if (-middlePosition.z - viewDepth(middleDepth) >= 0.0) high = middle;
                else low = middle;
            }
            vec3 hitPosition = origin + ray * high;
            vec2 hitUv;
            if (!projectPosition(hitPosition, hitUv)) return vec4(0.0);
            float hitDepth = texture(uDepth, hitUv).r;
            if (hitDepth >= 0.999999) return vec4(0.0);
            float hitZ = viewDepth(hitDepth);
            float thickness = max(hitZ * 0.003, 0.02) + (distance - previousDistance) * 0.06;
            float hitGap = abs(-hitPosition.z - hitZ);
            float confidence = 1.0 - smoothstep(thickness * 0.15, thickness, hitGap);
            float pixelTravel = length((hitUv - vUv) / uTexelSize);
            confidence *= smoothstep(2.0, 6.0, pixelTravel);
            float edge = min(min(hitUv.x, 1.0 - hitUv.x), min(hitUv.y, 1.0 - hitUv.y));
            confidence *= smoothstep(0.0, 0.08, edge);
            confidence *= 1.0 - smoothstep(maxDistance * 0.65, maxDistance, high);
            float fresnel = 0.06 + 0.94 * pow(1.0 - clamp(dot(normal, -incident), 0.0, 1.0), 5.0);
            return vec4(texture(uFrame, hitUv).rgb, confidence * fresnel * ground);
        }
        previousDistance = distance;
        previousGap = gap;
        previousValid = true;
    }
    return vec4(0.0); // A miss never invents reflected geometry or environment color.
}

vec3 highlights(vec3 color) {
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return color * smoothstep(0.72, 0.95, luminance);
}
vec3 highlightBloom() {
    vec3 sum = vec3(0.0);
    for (int tap = 0; tap < 8; ++tap) {
        float radius = tap % 2 == 0 ? 3.0 : 5.0;
        vec2 uv = clamp(vUv + directions[tap] * uTexelSize * radius, 0.0, 1.0);
        sum += highlights(texture(uFrame, uv).rgb);
    }
    return sum * (0.16 / 8.0);
}
vec3 localContrast(vec3 color, float centerZ) {
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (int tap = 0; tap < 4; ++tap) {
        vec2 uv = clamp(vUv + directions[tap * 2] * uTexelSize, 0.0, 1.0);
        float depth = texture(uDepth, uv).r;
        float connected = (depth < 0.999999 ? 1.0 : 0.0) *
            (1.0 - smoothstep(0.01, 0.04, abs(viewDepth(depth) - centerZ) / max(centerZ, 0.0001)));
        sum += texture(uFrame, uv).rgb * connected;
        weight += connected;
    }
    if (weight <= 0.0) return vec3(0.0);
    return clamp(color - sum / weight, vec3(-0.08), vec3(0.08)) * 0.6;
}
void main() {
    vec4 original = texture(uFrame, clamp(vUv, 0.0, 1.0));
    vec4 effects = clamp(uEffects, 0.0, 1.0);
    if (dot(effects, vec4(1.0)) == 0.0) {
        outColor = original;
        return;
    }
    float depth = texture(uDepth, clamp(vUv, 0.0, 1.0)).r;
    if (depth >= 0.999999) {
        outColor = original;
        return;
    }
    vec3 position = viewPosition(vUv, depth);
    vec3 color = original.rgb;
    if (effects.x > 0.0 || effects.y > 0.0) {
        float confidence;
        vec3 normal = surfaceNormal(vUv, position, confidence);
        if (confidence > 0.0) {
            if (effects.x > 0.0) color *= 1.0 - effects.x * confidence * ambientOcclusion(position, normal);
            if (effects.y > 0.0) {
                vec4 reflection = groundReflection(position, normal);
                color = mix(color, reflection.rgb, effects.y * confidence * reflection.a);
            }
        }
    }
    if (effects.z > 0.0) color += effects.z * highlightBloom();
    if (effects.w > 0.0) color += effects.w * localContrast(original.rgb, -position.z);
    outColor = vec4(clamp(color, 0.0, 1.0), original.a);
}
)GLSL";
} // namespace yami
