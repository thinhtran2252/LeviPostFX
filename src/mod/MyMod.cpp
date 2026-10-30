#include "mod/MyMod.h"
#include "mod/DevTools.h"

#include <android/log.h>
#include <dlfcn.h>
#include <filesystem>
#include <system_error>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>
#include <chrono>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>

#define VHS_LOGE(...) clange_me::devtools::logf(ANDROID_LOG_ERROR, __VA_ARGS__)
#define VHS_LOGW(...) clange_me::devtools::logf(ANDROID_LOG_WARN, __VA_ARGS__)
#define VHS_LOGI(...) clange_me::devtools::logf(ANDROID_LOG_INFO, __VA_ARGS__)

namespace clange_me {
namespace {

using SwapFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
SwapFn originalSwap = nullptr;
void* swapTarget = nullptr;
void* eglLibrary = nullptr;
bool hookInstalled = false;

std::atomic_bool loggedSwapReached{false};
std::atomic_bool loggedNoEglContext{false};
std::atomic_bool loggedSurfaceMismatch{false};
std::atomic_bool loggedSurfaceQueryFailure{false};
std::atomic_bool loggedInvalidSurfaceSize{false};
std::atomic_bool loggedRenderPassSubmitted{false};
std::atomic_bool loggedCopyFailure{false};
std::atomic_bool loggedDrawFailure{false};

GLuint shaderProgram = 0;
GLuint screenTexture = 0;
GLuint vertexArray = 0;
GLint textureWidth = 0;
GLint textureHeight = 0;

GLint imageLocation = -1;
GLint sizeLocation = -1;
GLint timeLocation = -1;
GLint distortionEnabledLocation = -1;
GLint distortionStrengthLocation = -1;
GLint distortionZoomLocation = -1;
GLint chromaticEnabledLocation = -1;
GLint chromaticIntensityLocation = -1;
GLint scanlinesEnabledLocation = -1;
GLint scanlineIntensityLocation = -1;
GLint aspectRatio43Location = -1;
GLint desaturationEnabledLocation = -1;
GLint desaturationIntensityLocation = -1;
GLint shadowDarknessLocation = -1;
GLint colorBleedingEnabledLocation = -1;
GLint colorBleedingIntensityLocation = -1;
GLint lumaNoiseEnabledLocation = -1;
GLint lumaNoiseIntensityLocation = -1;
GLint chromaNoiseEnabledLocation = -1;
GLint chromaNoiseIntensityLocation = -1;
GLint flashlightEnabledLocation = -1;
GLint flashlightIntensityLocation = -1;
GLint flashlightRangeLocation = -1;
GLint flashlightDonutSizeLocation = -1;
GLint bloomEnabledLocation = -1;
GLint bloomIntensityLocation = -1;
GLint bloomThresholdLocation = -1;

std::atomic_bool effectEnabled{true};
std::atomic_bool distortionEnabled{true};
std::atomic<float> distortionStrength{0.4f};
std::atomic<float> distortionZoom{0.0f};
std::atomic_bool chromaticEnabled{true};
std::atomic<float> chromaticIntensity{1.0f};
std::atomic_bool scanlinesEnabled{true};
std::atomic<float> scanlineIntensity{0.035f};
std::atomic_bool aspectRatio43Enabled{false};
std::atomic_bool desaturationEnabled{false};
std::atomic<float> desaturationIntensity{0.15f};
std::atomic<float> shadowDarkness{0.85f};
std::atomic_bool colorBleedingEnabled{true};
std::atomic<float> colorBleedingIntensity{0.35f};
std::atomic_bool lumaNoiseEnabled{true};
std::atomic<float> lumaNoiseIntensity{0.05f};
std::atomic_bool chromaNoiseEnabled{true};
std::atomic<float> chromaNoiseIntensity{0.20f};
std::atomic_bool flashlightEnabled{true};
std::atomic<float> flashlightIntensity{5.0f};
std::atomic<float> flashlightRange{1.0f};
std::atomic<float> flashlightDonutSize{0.22f};
std::atomic_bool bloomEnabled{true};
std::atomic<float> bloomIntensity{0.5f};
std::atomic<float> bloomThreshold{0.15f};

float effectTime = 0.0f;

bool parseFloat(std::string_view text, float& output) {
    std::string value(text);
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(value.c_str(), &end);
    if (errno == ERANGE || end == value.c_str() || !end ||
        *end != '\0' || !std::isfinite(parsed))
        return false;
    output = parsed;
    return true;
}

void applyRuntimeConfig(const ModConfig& config) {
    effectEnabled.store(config.enabled, std::memory_order_release);
    distortionEnabled.store(config.distortion_enabled, std::memory_order_release);
    distortionStrength.store(std::clamp(config.distortion_strength, 0.0f, 1.0f),
        std::memory_order_release);
    distortionZoom.store(std::clamp(config.distortion_zoom, -1.0f, 1.0f),
        std::memory_order_release);
    chromaticEnabled.store(config.chromatic_enabled, std::memory_order_release);
    chromaticIntensity.store(std::clamp(config.chromatic_intensity, 0.0f, 3.0f),
        std::memory_order_release);
    scanlinesEnabled.store(config.scanlines_enabled, std::memory_order_release);
    scanlineIntensity.store(std::clamp(config.scanline_intensity, 0.0f, 0.08f),
        std::memory_order_release);
    aspectRatio43Enabled.store(config.aspect_ratio_4_3, std::memory_order_release);
    desaturationEnabled.store(config.desaturation_enabled, std::memory_order_release);
    desaturationIntensity.store(std::clamp(config.desaturation_intensity, 0.0f, 1.0f),
        std::memory_order_release);
    shadowDarkness.store(std::clamp(config.shadow_darkness, 0.0f, 1.0f),
        std::memory_order_release);
    colorBleedingEnabled.store(config.color_bleeding_enabled, std::memory_order_release);
    colorBleedingIntensity.store(std::clamp(config.color_bleeding_intensity, 0.0f, 1.0f),
        std::memory_order_release);
    lumaNoiseEnabled.store(config.luma_noise_enabled, std::memory_order_release);
    lumaNoiseIntensity.store(std::clamp(config.luma_noise_intensity, 0.0f, 1.0f),
        std::memory_order_release);
    chromaNoiseEnabled.store(config.chroma_noise_enabled, std::memory_order_release);
    chromaNoiseIntensity.store(std::clamp(config.chroma_noise_intensity, 0.0f, 1.0f),
        std::memory_order_release);
    flashlightEnabled.store(config.flashlight_enabled, std::memory_order_release);
    flashlightIntensity.store(std::clamp(config.flashlight_intensity, 3.0f, 10.0f),
        std::memory_order_release);
    flashlightRange.store(std::clamp(config.flashlight_range, 0.5f, 2.0f),
        std::memory_order_release);
    flashlightDonutSize.store(std::clamp(config.flashlight_donut_size, 0.0f, 1.0f),
        std::memory_order_release);
    bloomEnabled.store(config.bloom_enabled, std::memory_order_release);
    bloomIntensity.store(std::clamp(config.bloom_intensity, 0.0f, 1.0f),
        std::memory_order_release);
    bloomThreshold.store(std::clamp(config.bloom_threshold, 0.0f, 1.0f),
        std::memory_order_release);
}

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    if (!shader) {
        VHS_LOGE("glCreateShader failed: 0x%x", glGetError());
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success != GL_TRUE) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<std::size_t>(length > 1 ? length : 1), '\0');
        glGetShaderInfoLog(shader, length, nullptr, log.data());
        VHS_LOGE("Shader compile failed: %s", log.data());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool initializeEffect() {
    if (shaderProgram && screenTexture && vertexArray)
        return true;

    GLint oldActiveTexture = GL_TEXTURE0;
    GLint oldTexture0 = 0;
    GLint oldVAO = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVAO);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);

    const char* vertexSource = R"GLSL(#version 300 es
precision highp float;
out vec2 vUV;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

    const char* fragmentSource = R"GLSL(#version 300 es
precision highp float;

uniform sampler2D uImage;
uniform vec2 uSize;
uniform float uTime;
uniform float uDistortionEnabled;
uniform float uDistortionStrength;
uniform float uDistortionZoom;
uniform float uChromaticEnabled;
uniform float uChromaticIntensity;
uniform float uScanlinesEnabled;
uniform float uScanlineIntensity;
uniform float uAspectRatio43;
uniform float uDesaturationEnabled;
uniform float uDesaturationIntensity;
uniform float uShadowDarkness;
uniform float uColorBleedingEnabled;
uniform float uColorBleedingIntensity;
uniform float uLumaNoiseEnabled;
uniform float uLumaNoiseIntensity;
uniform float uChromaNoiseEnabled;
uniform float uChromaNoiseIntensity;
uniform float uFlashlightEnabled;
uniform float uFlashlightIntensity;
uniform float uFlashlightRange;
uniform float uFlashlightDonutSize;
uniform float uBloomEnabled;
uniform float uBloomIntensity;
uniform float uBloomThreshold;

in vec2 vUV;
out vec4 outColor;

const float PI = 3.14159265359;
const float TARGET_ASPECT = 1.33333333333;

float hashNoise(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

vec3 rgbToYuv(vec3 rgb) {
    float y = 0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b;
    return vec3(y, 0.493 * (rgb.b - y), 0.877 * (rgb.r - y));
}

vec3 yuvToRgb(vec3 yuv) {
    float y = yuv.x;
    float u = yuv.y;
    float v = yuv.z;
    return vec3(
        y + (1.0 / 0.877) * v,
        y - 0.39393 * u - 0.58081 * v,
        y + (1.0 / 0.493) * u
    );
}

vec3 sampleBloom(vec2 uv, float threshold) {
    vec2 texel = 1.0 / max(uSize, vec2(1.0));
    vec3 glow = vec3(0.0);
    for (int i = 0; i < 8; ++i) {
        float angle = float(i) * (6.28318530718 / 8.0);
        vec2 offset = vec2(cos(angle), sin(angle)) * 4.0 * texel;
        vec3 sampleColor = texture(uImage, clamp(uv + offset, 0.0, 1.0)).rgb;
        float brightness = max(sampleColor.r, max(sampleColor.g, sampleColor.b));
        float highlight = smoothstep(threshold, 1.0, brightness);
        glow += sampleColor * highlight;
    }
    return glow * 0.125;
}

void main() {
    vec2 outputUV = clamp(vUV, vec2(0.0), vec2(1.0));
    vec2 frameOrigin = vec2(0.0);
    vec2 frameSize = vec2(1.0);

    if (uAspectRatio43 > 0.5) {
        float screenAspect = uSize.x / max(uSize.y, 1.0);
        if (screenAspect > TARGET_ASPECT) {
            frameSize.x = TARGET_ASPECT / screenAspect;
            frameOrigin.x = (1.0 - frameSize.x) * 0.5;
        } else {
            frameSize.y = screenAspect / TARGET_ASPECT;
            frameOrigin.y = (1.0 - frameSize.y) * 0.5;
        }
        vec2 frameEnd = frameOrigin + frameSize;
        if (outputUV.x < frameOrigin.x || outputUV.x > frameEnd.x ||
            outputUV.y < frameOrigin.y || outputUV.y > frameEnd.y) {
            outColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
    }

    vec2 frameUV = clamp((outputUV - frameOrigin) / frameSize, 0.0, 1.0);
    vec2 centered = frameUV - 0.5;
    float effectAspect = (uAspectRatio43 > 0.5)
        ? TARGET_ASPECT : (uSize.x / max(uSize.y, 1.0));
    vec2 aspectCentered = centered;
    aspectCentered.x *= effectAspect;
    float radius = length(aspectCentered);

    float distortion = (uDistortionEnabled > 0.5)
        ? clamp(uDistortionStrength, 0.0, 1.0) * 0.08 : 0.0;
    vec2 distortedFrameUV = 0.5 + centered *
        (1.0 + radius * radius * distortion);

    // Signed coefficient: -1 => 2x sample span, 0 => neutral, +1 => 0.5x.
    float zoomCoefficient = (uDistortionEnabled > 0.5)
        ? clamp(uDistortionZoom, -1.0, 1.0) : 0.0;
    float zoomScale = exp2(-zoomCoefficient);
    vec2 sampleFrameUV = 0.5 + (distortedFrameUV - 0.5) * zoomScale;
    bool sampleOutsideFrame =
        any(lessThan(sampleFrameUV, vec2(0.0))) ||
        any(greaterThan(sampleFrameUV, vec2(1.0)));
    sampleFrameUV = clamp(sampleFrameUV, 0.0, 1.0);

    vec2 sampleUV = frameOrigin + sampleFrameUV * frameSize;
    vec2 sourceMin = frameOrigin;
    vec2 sourceMax = frameOrigin + frameSize;

    float edge = smoothstep(0.08, 0.72, radius);
    vec2 pixelDirectionVector = centered * (uSize * frameSize);
    float directionLength = length(pixelDirectionVector);
    vec2 pixelDirection = directionLength > 0.0001
        ? pixelDirectionVector / directionLength : vec2(1.0, 0.0);
    float caPixels = (2.5 + edge * 3.5) * max(uChromaticIntensity, 0.0);
    vec2 caOffset = pixelDirection * caPixels / max(uSize, vec2(1.0));
    caOffset *= (uChromaticEnabled > 0.5) ? 1.0 : 0.0;

    float red = texture(uImage, clamp(sampleUV + caOffset, sourceMin, sourceMax)).r;
    float green = texture(uImage, sampleUV).g;
    float blue = texture(uImage, clamp(sampleUV - caOffset, sourceMin, sourceMax)).b;
    vec3 color = vec3(red, green, blue);
    if (sampleOutsideFrame)
        color = vec3(0.0);

    if (uColorBleedingEnabled > 0.5 && uColorBleedingIntensity > 0.0) {
        vec2 bleedOffset = vec2(2.0 / max(uSize.x, 1.0), 0.0);
        vec3 bleedLeft = texture(uImage,
            clamp(sampleUV - bleedOffset, sourceMin, sourceMax)).rgb;
        vec3 bleedRight = texture(uImage,
            clamp(sampleUV + bleedOffset, sourceMin, sourceMax)).rgb;
        vec3 bledColor = color * 0.72 + bleedLeft * 0.14 + bleedRight * 0.14;
        color = mix(color, bledColor, clamp(uColorBleedingIntensity, 0.0, 1.0));
    }

    if (uScanlinesEnabled > 0.5 && uScanlineIntensity > 0.0) {
        float scanline = sin(frameUV.y * uSize.y * frameSize.y * PI);
        float amount = clamp(uScanlineIntensity, 0.0, 0.08);
        float baseline = 1.0 - amount * (0.06 / 0.035);
        color *= baseline + scanline * amount;
    }

    // Existing fine moving VHS grain.
    vec2 pixelUV = frameUV * (uSize * frameSize);
    float baseNoise = hashNoise(pixelUV + vec2(uTime * 17.0, uTime * 31.0));
    color += (baseNoise - 0.5) * 0.018;

    // Port of the example's Luma/Chroma noise stage. The source shader
    // samples a noise texture; this port uses independent procedural
    // channels because the EGL post-process pass has no source noise texture.
    if ((uLumaNoiseEnabled > 0.5 && uLumaNoiseIntensity > 0.0) ||
        (uChromaNoiseEnabled > 0.5 && uChromaNoiseIntensity > 0.0)) {
        vec2 noiseCoord = pixelUV / 256.0;
        float timePhase = fract(uTime * 5.0);
        vec3 noise = vec3(
            hashNoise(noiseCoord * 256.0 + vec2(0.0, 13.0)),
            hashNoise(noiseCoord * 256.0 + vec2(37.0, 71.0)),
            hashNoise(noiseCoord * 256.0 + vec2(91.0, 29.0))
        );
        noise = fract(noise + timePhase) * 2.0 - 1.0;
        vec3 yuv = rgbToYuv(color);
        if (uLumaNoiseEnabled > 0.5)
            yuv.x += noise.x * clamp(uLumaNoiseIntensity, 0.0, 1.0) * 0.5;
        if (uChromaNoiseEnabled > 0.5)
            yuv.yz += noise.yz * clamp(uChromaNoiseIntensity, 0.0, 1.0) * 0.5;
        color = yuvToRgb(yuv);
    }

    // Deep shadow response: low-luminance pixels are pushed much closer to
    // black, while midtones and highlights remain largely unchanged. Apply
    // this before the flashlight so its beam can still lift the centre view.
    float shadowLuminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float shadowMask = 1.0 - smoothstep(0.035, 0.42, shadowLuminance);
    float shadowMultiplier = 1.0 - clamp(uShadowDarkness, 0.0, 1.0) * shadowMask;
    color *= shadowMultiplier;

    // Screen-space flashlight approximation. It follows the centre of the
    // camera view, but has no world depth, surface normals, or occlusion.
    if (uFlashlightEnabled > 0.5 && uFlashlightIntensity > 0.0) {
        vec2 beamPosition = centered;
        beamPosition.x *= effectAspect;
        float beamAngle = length(beamPosition) * (45.0 / max(uFlashlightRange, 0.5));
        // A smooth centre-weighted beam avoids the dense yellow ring/hotspot
        // produced by the previous overlapping donut and spill masks.
        float beamCore = 1.0 - smoothstep(0.0, 34.0, beamAngle);
        float beamFalloff = 1.0 - smoothstep(0.0, 62.0, beamAngle);
        float beamMask = beamCore * 0.72 + beamFalloff * 0.28;
        // A donut-size value of zero keeps a solid beam. Higher values carve
        // a dark, unlit centre while leaving the outer beam radius unchanged.
        float holeRadius = clamp(uFlashlightDonutSize, 0.0, 1.0) * 13.0;
        float centreCutout = (holeRadius > 0.001)
            ? smoothstep(holeRadius, holeRadius + 1.5, beamAngle) : 1.0;
        beamMask *= centreCutout;
        float flicker = 0.985 + 0.015 * sin(uTime * 17.0) * sin(uTime * 5.3);
        float intensity01 = (clamp(uFlashlightIntensity, 3.0, 10.0) - 3.0) / 7.0;
        float lightAmount = beamMask * mix(0.10, 0.36, intensity01) * flicker;
        // Neutral white with only a tiny warm bias, not the old strong amber tint.
        vec3 flashlightTint = vec3(1.0, 0.985, 0.96);
        color += color * flashlightTint * lightAmount;
        color += vec3(0.012, 0.012, 0.011) * beamMask * lightAmount;
    }

    if (uDesaturationEnabled > 0.5 && uDesaturationIntensity > 0.0) {
        float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
        color = mix(color, vec3(luminance),
            clamp(uDesaturationIntensity, 0.0, 1.0));
    }

    if (uBloomEnabled > 0.5 && uBloomIntensity > 0.0) {
        // Bloom is composited in output-screen space after lens distortion.
        // Sampling sampleUV here would warp/stretch the glow with the lens.
        vec3 glow = sampleBloom(outputUV, clamp(uBloomThreshold, 0.0, 0.99));
        color += glow * clamp(uBloomIntensity, 0.0, 1.0);
    }

    float vignette = 1.0 - smoothstep(0.25, 0.95, radius);
    color *= mix(0.78, 1.0, vignette);
    outColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
)GLSL";

    GLuint vs = compileShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        glBindVertexArray(static_cast<GLuint>(oldVAO));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
        glActiveTexture(static_cast<GLenum>(oldActiveTexture));
        return false;
    }

    GLuint program = glCreateProgram();
    if (!program) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        glBindVertexArray(static_cast<GLuint>(oldVAO));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
        glActiveTexture(static_cast<GLenum>(oldActiveTexture));
        return false;
    }
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<std::size_t>(length > 1 ? length : 1), '\0');
        glGetProgramInfoLog(program, length, nullptr, log.data());
        VHS_LOGE("Program link failed: %s", log.data());
        glDeleteProgram(program);
        glBindVertexArray(static_cast<GLuint>(oldVAO));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
        glActiveTexture(static_cast<GLenum>(oldActiveTexture));
        return false;
    }

    shaderProgram = program;
    imageLocation = glGetUniformLocation(shaderProgram, "uImage");
    sizeLocation = glGetUniformLocation(shaderProgram, "uSize");
    timeLocation = glGetUniformLocation(shaderProgram, "uTime");
    distortionEnabledLocation = glGetUniformLocation(shaderProgram, "uDistortionEnabled");
    distortionStrengthLocation = glGetUniformLocation(shaderProgram, "uDistortionStrength");
    distortionZoomLocation = glGetUniformLocation(shaderProgram, "uDistortionZoom");
    chromaticEnabledLocation = glGetUniformLocation(shaderProgram, "uChromaticEnabled");
    chromaticIntensityLocation = glGetUniformLocation(shaderProgram, "uChromaticIntensity");
    scanlinesEnabledLocation = glGetUniformLocation(shaderProgram, "uScanlinesEnabled");
    scanlineIntensityLocation = glGetUniformLocation(shaderProgram, "uScanlineIntensity");
    aspectRatio43Location = glGetUniformLocation(shaderProgram, "uAspectRatio43");
    desaturationEnabledLocation = glGetUniformLocation(shaderProgram, "uDesaturationEnabled");
    desaturationIntensityLocation = glGetUniformLocation(shaderProgram, "uDesaturationIntensity");
    shadowDarknessLocation = glGetUniformLocation(shaderProgram, "uShadowDarkness");
    colorBleedingEnabledLocation = glGetUniformLocation(shaderProgram, "uColorBleedingEnabled");
    colorBleedingIntensityLocation = glGetUniformLocation(shaderProgram, "uColorBleedingIntensity");
    lumaNoiseEnabledLocation = glGetUniformLocation(shaderProgram, "uLumaNoiseEnabled");
    lumaNoiseIntensityLocation = glGetUniformLocation(shaderProgram, "uLumaNoiseIntensity");
    chromaNoiseEnabledLocation = glGetUniformLocation(shaderProgram, "uChromaNoiseEnabled");
    chromaNoiseIntensityLocation = glGetUniformLocation(shaderProgram, "uChromaNoiseIntensity");
    flashlightEnabledLocation = glGetUniformLocation(shaderProgram, "uFlashlightEnabled");
    flashlightIntensityLocation = glGetUniformLocation(shaderProgram, "uFlashlightIntensity");
    flashlightRangeLocation = glGetUniformLocation(shaderProgram, "uFlashlightRange");
    flashlightDonutSizeLocation = glGetUniformLocation(shaderProgram, "uFlashlightDonutSize");
    bloomEnabledLocation = glGetUniformLocation(shaderProgram, "uBloomEnabled");
    bloomIntensityLocation = glGetUniformLocation(shaderProgram, "uBloomIntensity");
    bloomThresholdLocation = glGetUniformLocation(shaderProgram, "uBloomThreshold");

    glGenTextures(1, &screenTexture);
    glBindTexture(GL_TEXTURE_2D, screenTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenVertexArrays(1, &vertexArray);

    glBindVertexArray(static_cast<GLuint>(oldVAO));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
    glActiveTexture(static_cast<GLenum>(oldActiveTexture));
    if (!screenTexture || !vertexArray) {
        VHS_LOGE("Failed to create texture or VAO");
        return false;
    }
    VHS_LOGI("VHS post-processing initialized");
    return true;
}

void renderVHS(EGLDisplay display, EGLSurface surface) {
    if (!loggedSwapReached.exchange(true, std::memory_order_acq_rel))
        VHS_LOGI("eglSwapBuffers hook reached");
    if (!effectEnabled.load(std::memory_order_acquire))
        return;
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        if (!loggedNoEglContext.exchange(true, std::memory_order_acq_rel))
            VHS_LOGW("Hook reached, but no current EGL context is available");
        return;
    }
    if (eglGetCurrentSurface(EGL_DRAW) != surface) {
        if (!loggedSurfaceMismatch.exchange(true, std::memory_order_acq_rel))
            VHS_LOGW("EGL draw surface does not match the hooked surface");
        return;
    }

    EGLint width = 0, height = 0;
    if (!eglQuerySurface(display, surface, EGL_WIDTH, &width) ||
        !eglQuerySurface(display, surface, EGL_HEIGHT, &height)) {
        if (!loggedSurfaceQueryFailure.exchange(true, std::memory_order_acq_rel))
            VHS_LOGE("eglQuerySurface failed: 0x%04X", eglGetError());
        return;
    }
    if (width <= 0 || height <= 0) {
        if (!loggedInvalidSurfaceSize.exchange(true, std::memory_order_acq_rel))
            VHS_LOGW("Invalid EGL surface size: %d x %d", width, height);
        return;
    }
    if (!initializeEffect())
        return;

    GLint oldProgram = 0, oldVAO = 0, oldFramebuffer = 0;
    GLint oldActiveTexture = 0, oldTexture0 = 0, oldViewport[4] = {};
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVAO);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFramebuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
    glGetIntegerv(GL_VIEWPORT, oldViewport);

    GLboolean oldBlend = glIsEnabled(GL_BLEND);
    GLboolean oldDepth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean oldStencil = glIsEnabled(GL_STENCIL_TEST);
    GLboolean oldScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean oldCull = glIsEnabled(GL_CULL_FACE);
    GLboolean oldColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, screenTexture);

    if (textureWidth != width || textureHeight != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            VHS_LOGE("Screen texture allocation failed: GL error 0x%04X", error);
            goto restore_state;
        }
        textureWidth = width;
        textureHeight = height;
    }

    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
    {
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            if (!loggedCopyFailure.exchange(true, std::memory_order_acq_rel))
                VHS_LOGE("Framebuffer copy failed: GL error 0x%04X", error);
            goto restore_state;
        }
    }

    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(shaderProgram);
    glBindVertexArray(vertexArray);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, screenTexture);

    glUniform1i(imageLocation, 0);
    glUniform2f(sizeLocation, static_cast<float>(width), static_cast<float>(height));
    effectTime += 0.016f;
    glUniform1f(timeLocation, effectTime);
    glUniform1f(distortionEnabledLocation,
        distortionEnabled.load(std::memory_order_acquire) ? 1.0f : 0.0f);
    glUniform1f(distortionStrengthLocation,
        distortionStrength.load(std::memory_order_acquire));
    glUniform1f(distortionZoomLocation,
        distortionZoom.load(std::memory_order_acquire));
    glUniform1f(chromaticEnabledLocation,
        chromaticEnabled.load(std::memory_order_acquire) ? 1.0f : 0.0f);
    glUniform1f(chromaticIntensityLocation,
        chromaticIntensity.load(std::memory_order_acquire));
    glUniform1f(scanlinesEnabledLocation,
        scanlinesEnabled.load(std::memory_order_acquire) ? 1.0f : 0.0f);
    glUniform1f(scanlineIntensityLocation,
        scanlineIntensity.load(std::memory_order_acquire));
    glUniform1f(aspectRatio43Location,
        aspectRatio43Enabled.load(std::memory_order_acquire) ? 1.0f : 0.0f);
    glUniform1f(desaturationEnabledLocation,
        desaturationEnabled.load(std::memory_order_acquire) ? 1.0f : 0.0f);
    glUniform1f(desaturationIntensityLocation,
        desaturationIntensity.load(std::memory_order_acquire));
    glUniform1f(shadowDarknessLocation,
        shadowDarkness.load(std::memory_order_acquire));
    glUniform1f(colorBleedingEnabledLocation,
        colorBleedingEnabled.load(std::memory_order_acquire) ? 1.0f : 0.0f);
    glUniform1f(colorBleedingIntensityLocation,
        colorBleedingIntensity.load(std::memory_order_acquire));
    glUniform1f(lumaNoiseEnabledLocation,
        lumaNoiseEnabled.load(std::memory_order_acquire) ? 1.0f : 0.0f);
    glUniform1f(lumaNoiseIntensityLocation,
        lumaNoiseIntensity.load(std::memory_order_acquire));
    glUniform1f(chromaNoiseEnabledLocation,
        chromaNoiseEnabled.load(std::memory_order_acquire) ? 1.0f : 0.0f);
    glUniform1f(chromaNoiseIntensityLocation,
        chromaNoiseIntensity.load(std::memory_order_acquire));
    glUniform1f(flashlightEnabledLocation,
        flashlightEnabled.load(std::memory_order_acquire) ? 1.0f : 0.0f);
    glUniform1f(flashlightIntensityLocation,
        flashlightIntensity.load(std::memory_order_acquire));
    glUniform1f(flashlightRangeLocation,
        flashlightRange.load(std::memory_order_acquire));
    glUniform1f(flashlightDonutSizeLocation,
        flashlightDonutSize.load(std::memory_order_acquire));
    glUniform1f(bloomEnabledLocation,
        bloomEnabled.load(std::memory_order_acquire) ? 1.0f : 0.0f);
    glUniform1f(bloomIntensityLocation,
        bloomIntensity.load(std::memory_order_acquire));
    glUniform1f(bloomThresholdLocation,
        bloomThreshold.load(std::memory_order_acquire));

    glDrawArrays(GL_TRIANGLES, 0, 3);
    {
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            if (!loggedDrawFailure.exchange(true, std::memory_order_acq_rel))
                VHS_LOGE("Fullscreen draw failed: GL error 0x%04X", error);
        } else if (!loggedRenderPassSubmitted.exchange(true, std::memory_order_acq_rel)) {
            VHS_LOGI("Fullscreen post-processing pass submitted");
        }
    }

restore_state:
    glBindVertexArray(static_cast<GLuint>(oldVAO));
    glUseProgram(static_cast<GLuint>(oldProgram));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(oldFramebuffer));
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
    glColorMask(oldColorMask[0], oldColorMask[1], oldColorMask[2], oldColorMask[3]);
    if (oldBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (oldDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (oldStencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (oldScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (oldCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
    glActiveTexture(static_cast<GLenum>(oldActiveTexture));
}

EGLBoolean vhsSwap(EGLDisplay display, EGLSurface surface) {
    devtools::refreshOverlayIfNeeded();
    renderVHS(display, surface);
    return originalSwap ? originalSwap(display, surface) : EGL_FALSE;
}

void removeRenderHook() {
    if (hookInstalled && swapTarget)
        pl::memory::unhook(swapTarget, reinterpret_cast<void*>(&vhsSwap));
    hookInstalled = false;
    originalSwap = nullptr;
    swapTarget = nullptr;
    if (eglLibrary) {
        dlclose(eglLibrary);
        eglLibrary = nullptr;
    }
}

} // namespace

ClangeMeMod& ClangeMeMod::instance() {
    static ClangeMeMod instance;
    return instance;
}

ClangeMeMod::ClangeMeMod()
    : mSelf(*ll::mod::NativeMod::current()) {}

bool ClangeMeMod::load() {
    auto& self = getSelf();
    VHS_LOGI("Loading VHS Shader mod");
    devtools::startInfoClock();
    std::error_code ec;
    std::filesystem::create_directories(self.getDataDir(), ec);
    if (ec) {
        VHS_LOGE("Failed to create mod data directory");
        return false;
    }
    std::filesystem::create_directories(self.getConfigDir(), ec);
    if (ec) {
        VHS_LOGE("Failed to create mod config directory");
        return false;
    }
    mConfigFile.emplace();
    if (!mConfigFile->load()) {
        VHS_LOGE("Failed to load configuration");
        return false;
    }
    mConfig = mConfigFile->value();

    // Version 1 stored zoom as an absolute scale (1.000..1.100).
    // Version 2+ stores a signed coefficient (-1.000..+1.000).
    if (mConfig.version < 2) {
        if (mConfig.distortion_zoom >= 1.0f &&
            mConfig.distortion_zoom <= 1.10f) {
            mConfig.distortion_zoom -= 1.0f;
        }
    }
    if (mConfig.version < 7) {
        // Earlier releases stored lens distortion in the old 0..0.08 range.
        if (mConfig.version < 7 && mConfig.distortion_strength <= 0.0801f) {
            if (std::fabs(mConfig.distortion_strength - 0.035f) < 0.0001f)
                mConfig.distortion_strength = 0.4f;
            else
                mConfig.distortion_strength = std::clamp(mConfig.distortion_strength / 0.08f,
                    0.0f, 1.0f);
        }
        // Adopt the new defaults only when the saved value still equals the
        // previous release's default. User-customized values are preserved.
        if (std::fabs(mConfig.shadow_darkness - 0.85f) < 0.0001f)
            mConfig.shadow_darkness = 0.5f;
        if (std::fabs(mConfig.bloom_threshold - 0.35f) < 0.0001f)
            mConfig.bloom_threshold = 0.15f;
        if (std::fabs(mConfig.flashlight_intensity - 3.0f) < 0.0001f)
            mConfig.flashlight_intensity = 5.0f;
        mConfig.version = 7;
        mConfigFile->value() = mConfig;
        if (!mConfigFile->save())
            VHS_LOGW("Could not save migrated version 7 configuration");
    }

    applyRuntimeConfig(mConfig);
    self.getLogger().info("VHS Shader loaded");
    VHS_LOGI("Mod loaded");
    return true;
}

void ClangeMeMod::setMenuEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    mConfig.enabled = enabled;
    applyRuntimeConfig(mConfig);
    if (mConfigFile) {
        mConfigFile->value() = mConfig;
        if (!mConfigFile->save())
            VHS_LOGE("Failed to save module enabled state");
    }
}

void ClangeMeMod::setMenuConfig(std::string_view key, std::string_view value) {
    bool recognized = true;
    bool saved = true;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        if (key == "distortion_enabled") {
            mConfig.distortion_enabled = devtools::parseBool(value);
        } else if (key == "distortion_strength") {
            float parsed = 0.0f;
            if (!parseFloat(value, parsed)) recognized = false;
            else mConfig.distortion_strength = std::clamp(parsed, 0.0f, 1.0f);
        } else if (key == "distortion_zoom") {
            float parsed = 0.0f;
            if (!parseFloat(value, parsed)) recognized = false;
            else mConfig.distortion_zoom = std::clamp(parsed, -1.0f, 1.0f);
        } else if (key == "chromatic_enabled") {
            mConfig.chromatic_enabled = devtools::parseBool(value);
        } else if (key == "chromatic_intensity") {
            float parsed = 0.0f;
            if (!parseFloat(value, parsed)) recognized = false;
            else mConfig.chromatic_intensity = std::clamp(parsed, 0.0f, 3.0f);
        } else if (key == "scanlines_enabled") {
            mConfig.scanlines_enabled = devtools::parseBool(value);
        } else if (key == "scanline_intensity") {
            float parsed = 0.0f;
            if (!parseFloat(value, parsed)) recognized = false;
            else mConfig.scanline_intensity = std::clamp(parsed, 0.0f, 0.08f);
        } else if (key == "desaturation_enabled") {
            mConfig.desaturation_enabled = devtools::parseBool(value);
        } else if (key == "desaturation_intensity") {
            float parsed = 0.0f;
            if (!parseFloat(value, parsed)) recognized = false;
            else mConfig.desaturation_intensity = std::clamp(parsed, 0.0f, 1.0f);
        } else if (key == "shadow_darkness") {
            float parsed = 0.0f;
            if (!parseFloat(value, parsed)) recognized = false;
            else mConfig.shadow_darkness = std::clamp(parsed, 0.0f, 1.0f);
        } else if (key == "color_bleeding_enabled") {
            mConfig.color_bleeding_enabled = devtools::parseBool(value);
        } else if (key == "color_bleeding_intensity") {
            float parsed = 0.0f;
            if (!parseFloat(value, parsed)) recognized = false;
            else mConfig.color_bleeding_intensity = std::clamp(parsed, 0.0f, 1.0f);
        } else if (key == "luma_noise_enabled") {
            mConfig.luma_noise_enabled = devtools::parseBool(value);
        } else if (key == "luma_noise_intensity") {
            float parsed = 0.0f;
            if (!parseFloat(value, parsed)) recognized = false;
            else mConfig.luma_noise_intensity = std::clamp(parsed, 0.0f, 1.0f);
        } else if (key == "chroma_noise_enabled") {
            mConfig.chroma_noise_enabled = devtools::parseBool(value);
        } else if (key == "chroma_noise_intensity") {
            float parsed = 0.0f;
            if (!parseFloat(value, parsed)) recognized = false;
            else mConfig.chroma_noise_intensity = std::clamp(parsed, 0.0f, 1.0f);
        } else if (key == "flashlight_enabled") {
            mConfig.flashlight_enabled = devtools::parseBool(value);
        } else if (key == "flashlight_intensity") {
            float parsed = 0.0f;
            if (!parseFloat(value, parsed)) recognized = false;
            else mConfig.flashlight_intensity = std::clamp(parsed, 3.0f, 10.0f);
        } else if (key == "flashlight_range") {
            float parsed = 0.0f;
            if (!parseFloat(value, parsed)) recognized = false;
            else mConfig.flashlight_range = std::clamp(parsed, 0.5f, 2.0f);
        } else if (key == "flashlight_donut_size") {
            float parsed = 0.0f;
            if (!parseFloat(value, parsed)) recognized = false;
            else mConfig.flashlight_donut_size = std::clamp(parsed, 0.0f, 1.0f);
        } else if (key == "bloom_enabled") {
            mConfig.bloom_enabled = devtools::parseBool(value);
        } else if (key == "bloom_intensity") {
            float parsed = 0.0f;
            if (!parseFloat(value, parsed)) recognized = false;
            else mConfig.bloom_intensity = std::clamp(parsed, 0.0f, 1.0f);
        } else if (key == "bloom_threshold") {
            float parsed = 0.0f;
            if (!parseFloat(value, parsed)) recognized = false;
            else mConfig.bloom_threshold = std::clamp(parsed, 0.0f, 1.0f);
        } else if (key == "aspect_ratio_4_3") {
            mConfig.aspect_ratio_4_3 = devtools::parseBool(value);
        } else if (key == "vhs_info_enabled") {
            mConfig.vhs_info_enabled = devtools::parseBool(value);
        } else if (key == "reset_all_settings") {
            mConfig = ModConfig{};
            devtools::gEnabled.store(false, std::memory_order_release);
            devtools::gVhsInfoEnabled.store(mConfig.vhs_info_enabled,
                std::memory_order_release);
            devtools::gLastOverlaySecond.store(-1, std::memory_order_release);
        } else {
            recognized = false;
        }
        if (recognized) {
            applyRuntimeConfig(mConfig);
            if (mConfigFile) {
                mConfigFile->value() = mConfig;
                saved = mConfigFile->save();
            }
        }
    }
    if (!recognized) {
        VHS_LOGW("Ignored invalid or unknown Mod Menu setting: %.*s",
            static_cast<int>(key.size()), key.data());
        return;
    }
    if (!saved)
        VHS_LOGE("Failed to save Mod Menu configuration");
}

bool ClangeMeMod::enable() {
    auto& self = getSelf();
    const bool menuRegistered = devtools::registerMenu(
        mConfig, self.getId(),
        [this](bool enabled) { setMenuEnabled(enabled); },
        [this](std::string_view key, std::string_view value) {
            setMenuConfig(key, value);
        }
    );
    if (!menuRegistered) {
        VHS_LOGE("VHS Mod Menu registration failed; see Preloader logs for the rejected module reason");
        return false;
    }
    if (hookInstalled)
        return true;

    VHS_LOGI("Opening libEGL.so");
    eglLibrary = dlopen("libEGL.so", RTLD_NOW);
    if (!eglLibrary) {
        self.getLogger().error("VHS Shader: cannot open libEGL.so");
        VHS_LOGE("dlopen libEGL.so failed: %s", dlerror());
        devtools::unregisterMenu();
        return false;
    }
    swapTarget = dlsym(eglLibrary, "eglSwapBuffers");
    if (!swapTarget) {
        self.getLogger().error("VHS Shader: eglSwapBuffers not found");
        VHS_LOGE("eglSwapBuffers symbol not found");
        removeRenderHook();
        devtools::unregisterMenu();
        return false;
    }
    VHS_LOGI("eglSwapBuffers symbol found; installing hook");
    void* original = nullptr;
    const int result = pl::memory::hook(
        swapTarget, reinterpret_cast<void*>(&vhsSwap), &original);
    if (result != 0 || !original) {
        self.getLogger().error("VHS Shader: hook failed, code={}", result);
        VHS_LOGE("Hook failed: %d", result);
        removeRenderHook();
        devtools::unregisterMenu();
        return false;
    }
    originalSwap = reinterpret_cast<SwapFn>(original);
    hookInstalled = true;
    self.getLogger().info("VHS Shader render hook installed");
    VHS_LOGI("eglSwapBuffers hook installed successfully");
    VHS_LOGW("UI exclusion is unavailable at this hook: framebuffer contains world + UI");
    VHS_LOGW("Flashlight is a screen-space overlay, not world lighting or item detection");
    return true;
}

bool ClangeMeMod::disable() {
    removeRenderHook();
    devtools::unregisterMenu();
    VHS_LOGI("Render hook removed and temporary Mod Menu entry unregistered");
    return true;
}

bool ClangeMeMod::unload() {
    removeRenderHook();
    devtools::unregisterMenu();
    mConfigFile.reset();
    return true;
}

} // namespace clange_me
