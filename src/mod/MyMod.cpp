#include "mod/MyMod.h"

#include <dlfcn.h>
#include <filesystem>
#include <vector>
#include <cstdint>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>

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

GLint positionLocation = -1;
GLint uvLocation = -1;
GLint imageLocation = -1;
GLint sizeLocation = -1;

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

    if (!success) {
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

bool initializeEffect() {
    if (shaderProgram)
        return true;

    const char* vertexSource = R"(
        #version 300 es
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
    )";

    const char* fragmentSource = R"(
        #version 300 es
        precision highp float;

        uniform sampler2D uImage;
        uniform vec2 uSize;

        in vec2 vUV;
        out vec4 outColor;

        void main() {
            vec2 uv = clamp(vUV, vec2(0.0), vec2(1.0));

            vec2 centered = uv - vec2(0.5);
            float radius = length(centered);

            // Effect gradually increases toward screen edges.
            float edge = smoothstep(0.12, 0.68, radius);

            // Subtle RGB separation, measured in screen pixels.
            vec2 direction = normalize(centered + vec2(0.00001));
            vec2 offset = direction * edge * (1.35 / uSize);

            vec4 original = texture(uImage, uv);

            float redChannel =
                texture(uImage, clamp(uv + offset, 0.0, 1.0)).r;

            float blueChannel =
                texture(uImage, clamp(uv - offset, 0.0, 1.0)).b;

            outColor = vec4(
                redChannel,
                original.g,
                blueChannel,
                original.a
            );
        }
    )";

    GLuint vs = compileShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSource);

    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);

    if (!linked) {
        glDeleteProgram(program);
        return false;
    }

    shaderProgram = program;

    imageLocation = glGetUniformLocation(program, "uImage");
    sizeLocation = glGetUniformLocation(program, "uSize");

    glGenTextures(1, &screenTexture);
    glBindTexture(GL_TEXTURE_2D, screenTexture);

    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenVertexArrays(1, &vertexArray);

    return true;
}

void destroyEffect() {
    if (vertexArray) {
        glDeleteVertexArrays(1, &vertexArray);
        vertexArray = 0;
    }

    if (screenTexture) {
        glDeleteTextures(1, &screenTexture);
        screenTexture = 0;
    }

    if (shaderProgram) {
        glDeleteProgram(shaderProgram);
        shaderProgram = 0;
    }
}

void renderChromaticAberration(
    EGLDisplay display,
    EGLSurface surface
) {
    // Config disabled: leave the original image untouched.
    if (!ClangeMeMod::instance().getSelf().isEnabled())
        return;

    if (eglGetCurrentContext() == EGL_NO_CONTEXT)
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
    GLint oldPackAlignment = 4;

    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVAO);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFramebuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    glGetIntegerv(GL_PACK_ALIGNMENT, &oldPackAlignment);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);

    const GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    const GLboolean depthEnabled = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean stencilEnabled = glIsEnabled(GL_STENCIL_TEST);
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean cullEnabled = glIsEnabled(GL_CULL_FACE);

    GLint oldReadBuffer = GL_BACK;
    glGetIntegerv(GL_READ_BUFFER, &oldReadBuffer);

    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height) * 4);

    // Capture the current screen before drawing the effect.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    glReadPixels(
        0, 0, width, height,
        GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    glBindTexture(GL_TEXTURE_2D, screenTexture);
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA8,
        width, height, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    glViewport(0, 0, width, height);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(shaderProgram);
    glBindVertexArray(vertexArray);

    glUniform1i(imageLocation, 0);
    glUniform2f(
        sizeLocation,
        static_cast<float>(width),
        static_cast<float>(height));

    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Restore state used by Minecraft.
    glBindVertexArray(static_cast<GLuint>(oldVAO));
    glUseProgram(static_cast<GLuint>(oldProgram));
    glBindFramebuffer(
        GL_FRAMEBUFFER, static_cast<GLuint>(oldFramebuffer));
    glViewport(
        oldViewport[0], oldViewport[1],
        oldViewport[2], oldViewport[3]);

    if (blendEnabled) glEnable(GL_BLEND);
    else glDisable(GL_BLEND);

    if (depthEnabled) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);

    if (stencilEnabled) glEnable(GL_STENCIL_TEST);
    else glDisable(GL_STENCIL_TEST);

    if (scissorEnabled) glEnable(GL_SCISSOR_TEST);
    else glDisable(GL_SCISSOR_TEST);

    if (cullEnabled) glEnable(GL_CULL_FACE);
    else glDisable(GL_CULL_FACE);

    glPixelStorei(GL_PACK_ALIGNMENT, oldPackAlignment);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
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
            reinterpret_cast<void*>(&vhsSwap));
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
    if (ec)
        return false;

    std::filesystem::create_directories(self.getConfigDir(), ec);
    if (ec)
        return false;

    mConfigFile.emplace();

    if (!mConfigFile->load())
        return false;

    mConfig = mConfigFile->value();

    self.getLogger().info("VHS Shader loaded");
    return true;
}

bool ClangeMeMod::enable() {
    auto& self = getSelf();

    eglLibrary = dlopen("libEGL.so", RTLD_NOW);
    if (!eglLibrary) {
        self.getLogger().error("VHS Shader: cannot open libEGL.so");
        return false;
    }

    swapTarget = dlsym(eglLibrary, "eglSwapBuffers");
    if (!swapTarget) {
        self.getLogger().error("VHS Shader: eglSwapBuffers not found");
        removeRenderHook();
        return false;
    }

    void* original = nullptr;

    const int result = pl::memory::hook(
        swapTarget,
        reinterpret_cast<void*>(&vhsSwap),
        &original);

    if (result != 0 || !original) {
        self.getLogger().error(
            "VHS Shader: hook failed, code={}", result);
        removeRenderHook();
        return false;
    }

    originalSwap = reinterpret_cast<SwapFn>(original);
    hookInstalled = true;

    self.getLogger().info("VHS Shader render hook installed");
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