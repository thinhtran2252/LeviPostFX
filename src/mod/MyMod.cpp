
#include "mod/MyMod.h"

#include <android/log.h>
#include <dlfcn.h>
#include <filesystem>
#include <system_error>
#include <vector>
#include <cstdint>

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
            static_cast<std::size_t>(length > 1 ? length : 1), '\0');

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

    // Preserve game GL state during initialization.
    GLint oldActiveTexture = GL_TEXTURE0;
    GLint oldTexture0 = 0;
    GLint oldVAO = 0;

    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVAO);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);

    // #version MUST be the first character in each shader string.
    const char* vertexSource = R"GLSL(#version 300 es
precision highp float;

out vec2 vUV;

void main() {
    vec2 p = vec2(
        float((gl_VertexID << 1) & 2),
        float(gl_VertexID & 2)
    );

    // Correct oversized fullscreen triangle.
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

    const char* fragmentSource = R"GLSL(#version 300 es
precision highp float;

uniform sampler2D uImage;
uniform vec2 uSize;

in vec2 vUV;
out vec4 outColor;

void main() {
    vec2 uv = clamp(vUV, vec2(0.0), vec2(1.0));
    vec2 centered = uv - vec2(0.5);

    float radius = length(centered);
    float edge = smoothstep(0.12, 0.68, radius);

    vec2 direction = centered / max(radius, 0.00001);
    vec2 offset = direction * edge * (vec2(3.0) / uSize);

    vec4 original = texture(uImage, uv);

    float redChannel = texture(
        uImage,
        clamp(uv + offset, vec2(0.0), vec2(1.0))
    ).r;

    float blueChannel = texture(
        uImage,
        clamp(uv - offset, vec2(0.0), vec2(1.0))
    ).b;

    outColor = vec4(
        redChannel,
        original.g,
        blueChannel,
        original.a
    );
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
            static_cast<std::size_t>(length > 1 ? length : 1), '\0');

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

    glGenTextures(1, &screenTexture);
    glBindTexture(GL_TEXTURE_2D, screenTexture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenVertexArrays(1, &vertexArray);

    // Restore the game's previous state.
    glBindVertexArray(static_cast<GLuint>(oldVAO));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
    glActiveTexture(static_cast<GLenum>(oldActiveTexture));

    if (!screenTexture || !vertexArray) {
        VHS_LOGE("Failed to create texture or VAO");
        return false;
    }

    VHS_LOGI("Chromatic Aberration shader initialized");
    return true;
}

void renderChromaticAberration(
    EGLDisplay display,
    EGLSurface surface
) {
    if (eglGetCurrentContext() == EGL_NO_CONTEXT)
        return;

    if (eglGetCurrentSurface(EGL_DRAW) != surface)
        return;

    EGLint width = 0;
    EGLint height = 0;

    if (!eglQuerySurface(display, surface, EGL_WIDTH, &width) ||
        !eglQuerySurface(display, surface, EGL_HEIGHT, &height)) {
        return;
    }

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

    // All subsequent rendering uses GPU-side operations.
    // No full-frame glReadPixels or per-frame CPU buffer allocation.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, screenTexture);

    // Allocate/reallocate only when the screen resolution changes.
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

        GLenum allocationError = glGetError();

        if (allocationError != GL_NO_ERROR) {
            static bool loggedAllocationError = false;

            if (!loggedAllocationError) {
                VHS_LOGE(
                    "Texture allocation failed: 0x%x",
                    allocationError
                );
                loggedAllocationError = true;
            }

            goto restore_state;
        }

        textureWidth = width;
        textureHeight = height;
    }

    // Copy the current framebuffer directly into the GPU texture.
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
        GLenum copyError = glGetError();

        if (copyError != GL_NO_ERROR) {
            static bool loggedCopyError = false;

            if (!loggedCopyError) {
                VHS_LOGE(
                    "GPU framebuffer copy failed: 0x%x",
                    copyError
                );
                loggedCopyError = true;
            }

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

    glDrawArrays(GL_TRIANGLES, 0, 3);

    {
        GLenum drawError = glGetError();

        if (drawError != GL_NO_ERROR) {
            static bool loggedDrawError = false;

            if (!loggedDrawError) {
                VHS_LOGE(
                    "Chromatic Aberration draw failed: 0x%x",
                    drawError
                );
                loggedDrawError = true;
            }
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
    renderChromaticAberration(display, surface);

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
