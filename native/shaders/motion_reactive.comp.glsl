#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depthTexture;
layout(set = 0, binding = 1) uniform sampler2D colorTexture;
layout(set = 0, binding = 2, rg16f) uniform writeonly image2D motionVectors;
layout(set = 0, binding = 3, r8) uniform writeonly image2D reactiveMask;

layout(push_constant) uniform FrameParameters {
    mat4 currentToPreviousClip;
    vec2 inverseRenderSize;
    float reactiveScale;
    uint resetHistory;
} frame;

float luminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 dimensions = imageSize(motionVectors);
    if (any(greaterThanEqual(pixel, dimensions))) {
        return;
    }

    vec2 uv = (vec2(pixel) + vec2(0.5)) * frame.inverseRenderSize;
    float depth = texelFetch(depthTexture, pixel, 0).r;
    vec2 currentNdc = uv * 2.0 - 1.0;
    vec2 motion = vec2(0.0);
    if (frame.resetHistory == 0u && depth > 0.0) {
        vec4 currentClip = vec4(currentNdc, depth, 1.0);
        vec4 previousClip = frame.currentToPreviousClip * currentClip;
        if (abs(previousClip.w) > 1.0e-6) {
            motion = previousClip.xy / previousClip.w - currentNdc;
        }
    }
    imageStore(motionVectors, pixel, vec4(motion, 0.0, 0.0));

    ivec2 rightPixel = ivec2(min(pixel.x + 1, dimensions.x - 1), pixel.y);
    ivec2 upperPixel = ivec2(pixel.x, min(pixel.y + 1, dimensions.y - 1));
    vec4 center = texelFetch(colorTexture, pixel, 0);
    float centerLuma = luminance(center.rgb);
    float edge = max(
        abs(centerLuma - luminance(texelFetch(colorTexture, rightPixel, 0).rgb)),
        abs(centerLuma - luminance(texelFetch(colorTexture, upperPixel, 0).rgb))
    );
    float alphaReactivity = clamp(1.0 - center.a, 0.0, 1.0);
    float reactive = clamp(max(edge * 4.0, alphaReactivity) * frame.reactiveScale, 0.0, 0.9);
    imageStore(reactiveMask, pixel, vec4(reactive, 0.0, 0.0, 0.0));
}
