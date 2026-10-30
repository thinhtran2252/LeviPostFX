
#include "mod/MyMod.h"

#include <android/log.h>
#include <dlfcn.h>
#include <filesystem>
#include <system_error>
#include <vector>
#include <cstdint>
#include <algorithm>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>

#define VHS_LOG_TAG "VHSShader"
#define VHS_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, VHS_LOG_TAG, __VA_ARGS__)
#define VHS_LOGI(...) __android_log_print(ANDROID_LOG_INFO, VHS_LOG_TAG, __VA_ARGS__)

namespace clange_me {
namespace {

using SwapFn = EGLBoolean (*)(EGLDisplay, EGLSurface);

SwapFn originalSwap = nullptr;
void* swapTarget = nullptr;
void* eglLibrary = nullptr;
bool hookInstalled = false;

GLuint shaderProgram = 0;
GLuint screenTexture = 0;
GLuint vertexArray = 0;

GLint textureWidth = 0;
GLint textureHeight = 0;

GLint imageLocation = -1;
GLint sizeLocation = -1;
GLint timeLocation = -1;

bool effectEnabled = true;
float effectTime = 0.0f;

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

in vec2 vUV;
out vec4 outColor;

const float PI = 3.14159265359;

void main() {
    vec2 uv = clamp(vUV, vec2(0.0), vec2(1.0));
    vec2 centered = uv - 0.5;

    float aspect = uSize.x / max(uSize.y, 1.0);
    vec2 aspectCentered = centered;
    aspectCentered.x *= aspect;

    float radius = length(aspectCentered);

    // Lens distortion
    float distortion = 0.035;
    vec2 distorted = centered * (1.0 + radius * radius * distortion);
    vec2 sampleUV = clamp(0.5 + distorted, 0.0, 1.0);

    // Chromatic aberration
    float edge = smoothstep(0.08, 0.72, radius);
    vec2 direction = normalize(centered + vec2(0.00001));
    vec2 caOffset = direction * edge * (5.0 / uSize);

    float red = texture(
        uImage,
        clamp(sampleUV + caOffset, 0.0, 1.0)
    ).r;

    float green = texture(uImage, sampleUV).g;

    float blue = texture(
        uImage,
        clamp(sampleUV - caOffset, 0.0, 1.0)
    ).b;

    vec3 color = vec3(red, green, blue);

    // Horizontal color bleeding
    vec2 bleedOffset = vec2(2.0 / uSize.x, 0.0);

    vec3 bleedLeft = texture(
        uImage,
        clamp(sampleUV - bleedOffset, 0.0, 1.0)
    ).rgb;

    vec3 bleedRight = texture(
        uImage,
        clamp(sampleUV + bleedOffset, 0.0, 1.0)
    ).rgb;

    color = mix(
        color,
        (color * 0.72 + bleedLeft * 0.14 + bleedRight * 0.14),
        0.35
    );

    // Scanlines
    float scanline = sin(uv.y * uSize.y * PI);
    color *= 0.94 + scanline * 0.035;

    // Subtle moving noise
    float noise = fract(
        sin(dot(
            uv * uSize + vec2(uTime * 17.0, uTime * 31.0),
            vec2(12.9898, 78.233)
        )) * 43758.5453
    );

    color += (noise - 0.5) * 0.018;

    // Vignette
    float vignette = smoothstep(0.95, 0.25, radius);
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
    if (!effectEnabled)
        return;

    if (eglGetCurrentContext() == EGL_NO_CONTEXT)
        return;

    if (eglGetCurrentSurface(EGL_DRAW) != surface)
        return;

    EGLint width = 0;
    EGLint height = 0;

    if (!eglQuerySurface(display, surface, EGL_WIDTH, &width) ||
        !eglQuerySurface(display, surface, EGL_HEIGHT, &height))
        return;

    if (width <= 0 || height <= 0)
        return;

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

        if (glGetError() != GL_NO_ERROR)
            goto restore_state;

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

    if (glGetError() != GL_NO_ERROR)
        goto restore_state;

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

    glDrawArrays(GL_TRIANGLES, 0, 3);

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
    effectEnabled = mConfig.enabled;

    self.getLogger().info("VHS Shader loaded");
    VHS_LOGI("Mod loaded");

    return true;
}

bool ClangeMeMod::enable() {
    auto& self = getSelf();

    if (hookInstalled)
        return true;

    eglLibrary = dlopen("libEGL.so", RTLD_NOW);

    if (!eglLibrary) {
        self.getLogger().error("VHS Shader: cannot open libEGL.so");
        VHS_LOGE("dlopen libEGL.so failed: %s", dlerror());
        return false;
    }

    swapTarget = dlsym(eglLibrary, "eglSwapBuffers");

    if (!swapTarget) {
        self.getLogger().error("VHS Shader: eglSwapBuffers not found");
        VHS_LOGE("eglSwapBuffers symbol not found");
        removeRenderHook();
        return false;
    }

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
        return false;
    }

    originalSwap = reinterpret_cast<SwapFn>(original);
    hookInstalled = true;

    self.getLogger().info("VHS Shader render hook installed");
    VHS_LOGI("eglSwapBuffers hook installed");

    return true;
}

bool ClangeMeMod::disable() {
    removeRenderHook();
    return true;
}

bool ClangeMeMod::unload() {
    removeRenderHook();
    mConfigFile.reset();
    return true;
}

} // namespace clange_me
