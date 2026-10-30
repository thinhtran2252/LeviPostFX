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

std::atomic_bool effectEnabled{true};
std::atomic_bool distortionEnabled{true};
std::atomic<float> distortionStrength{0.035f};
std::atomic<float> distortionZoom{1.025f};
std::atomic_bool chromaticEnabled{true};
std::atomic<float> chromaticIntensity{1.0f};
std::atomic_bool scanlinesEnabled{true};
std::atomic<float> scanlineIntensity{0.035f};
std::atomic_bool aspectRatio43Enabled{false};

float effectTime = 0.0f;

bool parseFloat(std::string_view text, float& output) {
    std::string value(text);
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(value.c_str(), &end);

    if (errno == ERANGE || end == value.c_str() || !end || *end != '\0' ||
        !std::isfinite(parsed)) {
        return false;
    }

    output = parsed;
    return true;
}

void applyRuntimeConfig(const ModConfig& config) {
    effectEnabled.store(config.enabled, std::memory_order_release);
    distortionEnabled.store(config.distortion_enabled, std::memory_order_release);
    distortionStrength.store(
        std::clamp(config.distortion_strength, 0.0f, 0.08f),
        std::memory_order_release
    );
    distortionZoom.store(
        std::clamp(config.distortion_zoom, 1.0f, 1.10f),
        std::memory_order_release
    );
    chromaticEnabled.store(config.chromatic_enabled, std::memory_order_release);
    chromaticIntensity.store(
        std::clamp(config.chromatic_intensity, 0.0f, 3.0f),
        std::memory_order_release
    );
    scanlinesEnabled.store(config.scanlines_enabled, std::memory_order_release);
    scanlineIntensity.store(
        std::clamp(config.scanline_intensity, 0.0f, 0.08f),
        std::memory_order_release
    );
    aspectRatio43Enabled.store(config.aspect_ratio_4_3, std::memory_order_release);
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

        std::vector<char> log(
            static_cast<std::size_t>(length > 1 ? length : 1), '\0'
        );

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
    vec2 p = vec2(
        float((gl_VertexID << 1) & 2),
        float(gl_VertexID & 2)
    );

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

in vec2 vUV;
out vec4 outColor;

const float PI = 3.14159265359;
const float TARGET_ASPECT = 1.33333333333;

void main() {
    vec2 outputUV = clamp(vUV, vec2(0.0), vec2(1.0));

    // Build a centred 4:3 content rectangle. The source is cropped to the
    // same rectangle and displayed without stretching. Areas outside it are
    // deliberately black (pillarbox or letterbox, depending on display shape).
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
        ? TARGET_ASPECT
        : (uSize.x / max(uSize.y, 1.0));

    vec2 aspectCentered = centered;
    aspectCentered.x *= effectAspect;
    float radius = length(aspectCentered);

    // 1) Apply lens/screen distortion.
    float distortion = (uDistortionEnabled > 0.5)
        ? max(uDistortionStrength, 0.0)
        : 0.0;
    vec2 distortedFrameUV = 0.5 + centered * (1.0 + radius * radius * distortion);

    // 2) Apply the small zoom AFTER distortion. This crops the warped outer
    // border instead of scaling the viewport or changing the distortion itself.
    float zoom = (uDistortionEnabled > 0.5)
        ? max(uDistortionZoom, 1.0)
        : 1.0;
    vec2 sampleFrameUV = 0.5 + (distortedFrameUV - 0.5) * zoom;
    sampleFrameUV = clamp(sampleFrameUV, 0.0, 1.0);

    // Map the 0..1 content coordinates back into the cropped source rectangle.
    vec2 sampleUV = frameOrigin + sampleFrameUV * frameSize;
    vec2 sourceMin = frameOrigin;
    vec2 sourceMax = frameOrigin + frameSize;

    // Chromatic aberration uses a multi-pixel baseline so RGB separation is
    // visible across the full image. A radial component increases separation
    // toward the edges. Direction is measured in screen pixels to avoid skew.
    float edge = smoothstep(0.08, 0.72, radius);
    vec2 pixelDirectionVector = centered * (uSize * frameSize);
    float directionLength = length(pixelDirectionVector);
    vec2 pixelDirection = (directionLength > 0.0001)
        ? (pixelDirectionVector / directionLength)
        : vec2(1.0, 0.0);
    // A stronger constant offset keeps RGB separation visible across the
    // whole image; the radial term still adds extra lens-like separation
    // toward the corners. The old 1 px centre offset was too subtle at phone
    // resolutions and looked like edge-only CA.
    float caPixels = (2.5 + edge * 3.5) * max(uChromaticIntensity, 0.0);
    vec2 caOffset = pixelDirection * caPixels / max(uSize, vec2(1.0));
    caOffset *= (uChromaticEnabled > 0.5) ? 1.0 : 0.0;

    float red = texture(
        uImage,
        clamp(sampleUV + caOffset, sourceMin, sourceMax)
    ).r;

    float green = texture(uImage, sampleUV).g;

    float blue = texture(
        uImage,
        clamp(sampleUV - caOffset, sourceMin, sourceMax)
    ).b;

    vec3 color = vec3(red, green, blue);

    // Horizontal color bleeding (kept from the existing shader).
    vec2 bleedOffset = vec2(2.0 / max(uSize.x, 1.0), 0.0);

    vec3 bleedLeft = texture(
        uImage,
        clamp(sampleUV - bleedOffset, sourceMin, sourceMax)
    ).rgb;

    vec3 bleedRight = texture(
        uImage,
        clamp(sampleUV + bleedOffset, sourceMin, sourceMax)
    ).rgb;

    color = mix(
        color,
        (color * 0.72 + bleedLeft * 0.14 + bleedRight * 0.14),
        0.35
    );

    // Scanlines retain the old default appearance at intensity 0.035.
    if (uScanlinesEnabled > 0.5 && uScanlineIntensity > 0.0) {
        float scanline = sin(frameUV.y * uSize.y * frameSize.y * PI);
        float amount = clamp(uScanlineIntensity, 0.0, 0.08);
        float baseline = 1.0 - amount * (0.06 / 0.035);
        color *= baseline + scanline * amount;
    }

    // Subtle moving noise (kept from the existing shader).
    vec2 pixelUV = frameUV * (uSize * frameSize);
    float noise = fract(
        sin(dot(
            pixelUV + vec2(uTime * 17.0, uTime * 31.0),
            vec2(12.9898, 78.233)
        )) * 43758.5453
    );

    color += (noise - 0.5) * 0.018;

    // Vignette.
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

        std::vector<char> log(
            static_cast<std::size_t>(length > 1 ? length : 1), '\0'
        );

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

    EGLint width = 0;
    EGLint height = 0;

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

    GLint oldProgram = 0;
    GLint oldVAO = 0;
    GLint oldFramebuffer = 0;
    GLint oldActiveTexture = 0;
    GLint oldTexture0 = 0;
    GLint oldViewport[4] = {};

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

    GLboolean oldColorMask[4] = {
        GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE
    };

    glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, screenTexture);

    if (textureWidth != width || textureHeight != height) {
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            width,
            height,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            nullptr
        );

        {
            const GLenum error = glGetError();
            if (error != GL_NO_ERROR) {
                VHS_LOGE("Screen texture allocation failed: GL error 0x%04X", error);
                goto restore_state;
            }
        }

        textureWidth = width;
        textureHeight = height;
    }

    glCopyTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        0,
        0,
        width,
        height
    );

    {
        const GLenum error = glGetError();
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
    glUniform2f(
        sizeLocation,
        static_cast<float>(width),
        static_cast<float>(height)
    );

    effectTime += 0.016f;
    glUniform1f(timeLocation, effectTime);

    glUniform1f(
        distortionEnabledLocation,
        distortionEnabled.load(std::memory_order_acquire) ? 1.0f : 0.0f
    );
    glUniform1f(
        distortionStrengthLocation,
        distortionStrength.load(std::memory_order_acquire)
    );
    glUniform1f(
        distortionZoomLocation,
        distortionZoom.load(std::memory_order_acquire)
    );
    glUniform1f(
        chromaticEnabledLocation,
        chromaticEnabled.load(std::memory_order_acquire) ? 1.0f : 0.0f
    );
    glUniform1f(
        chromaticIntensityLocation,
        chromaticIntensity.load(std::memory_order_acquire)
    );
    glUniform1f(
        scanlinesEnabledLocation,
        scanlinesEnabled.load(std::memory_order_acquire) ? 1.0f : 0.0f
    );
    glUniform1f(
        scanlineIntensityLocation,
        scanlineIntensity.load(std::memory_order_acquire)
    );
    glUniform1f(
        aspectRatio43Location,
        aspectRatio43Enabled.load(std::memory_order_acquire) ? 1.0f : 0.0f
    );

    glDrawArrays(GL_TRIANGLES, 0, 3);

    {
        const GLenum error = glGetError();
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

    glBindFramebuffer(
        GL_FRAMEBUFFER,
        static_cast<GLuint>(oldFramebuffer)
    );

    glViewport(
        oldViewport[0],
        oldViewport[1],
        oldViewport[2],
        oldViewport[3]
    );

    glColorMask(
        oldColorMask[0],
        oldColorMask[1],
        oldColorMask[2],
        oldColorMask[3]
    );

    if (oldBlend) glEnable(GL_BLEND);
    else glDisable(GL_BLEND);

    if (oldDepth) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);

    if (oldStencil) glEnable(GL_STENCIL_TEST);
    else glDisable(GL_STENCIL_TEST);

    if (oldScissor) glEnable(GL_SCISSOR_TEST);
    else glDisable(GL_SCISSOR_TEST);

    if (oldCull) glEnable(GL_CULL_FACE);
    else glDisable(GL_CULL_FACE);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(
        GL_TEXTURE_2D,
        static_cast<GLuint>(oldTexture0)
    );

    glActiveTexture(static_cast<GLenum>(oldActiveTexture));
}

EGLBoolean vhsSwap(EGLDisplay display, EGLSurface surface) {
    renderVHS(display, surface);

    return originalSwap
        ? originalSwap(display, surface)
        : EGL_FALSE;
}

void removeRenderHook() {
    if (hookInstalled && swapTarget) {
        pl::memory::unhook(
            swapTarget,
            reinterpret_cast<void*>(&vhsSwap)
        );
    }

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
            else mConfig.distortion_strength = std::clamp(parsed, 0.0f, 0.08f);
        } else if (key == "distortion_zoom") {
            float parsed = 1.0f;
            if (!parseFloat(value, parsed)) recognized = false;
            else mConfig.distortion_zoom = std::clamp(parsed, 1.0f, 1.10f);
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
        } else if (key == "aspect_ratio_4_3") {
            mConfig.aspect_ratio_4_3 = devtools::parseBool(value);
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

    // Levi's documented pattern is to register runtime Mod Menu UI during
    // enable(), using the loader-provided owner ID. This also keeps the
    // registration inside the current mod lifecycle/ownership scope.
    const bool menuRegistered = devtools::registerMenu(
        mConfig,
        self.getId(),
        [this](bool enabled) {
            setMenuEnabled(enabled);
        },
        [this](std::string_view key, std::string_view value) {
            setMenuConfig(key, value);
        }
    );

    if (!menuRegistered) {
        VHS_LOGE(
            "VHS Mod Menu registration failed; see Preloader logs for the rejected module reason"
        );
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
        swapTarget,
        reinterpret_cast<void*>(&vhsSwap),
        &original
    );

    if (result != 0 || !original) {
        self.getLogger().error(
            "VHS Shader: hook failed, code={}", result
        );

        VHS_LOGE("Hook failed: %d", result);
        removeRenderHook();
        devtools::unregisterMenu();
        return false;
    }

    originalSwap = reinterpret_cast<SwapFn>(original);
    hookInstalled = true;

    self.getLogger().info("VHS Shader render hook installed");
    VHS_LOGI("eglSwapBuffers hook installed successfully");
    VHS_LOGW("UI exclusion is unavailable at this hook: the framebuffer already contains the final world + UI image");

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
