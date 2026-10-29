
#include "mod/MyMod.h"

#include <dlfcn.h>
#include <filesystem>
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

EGLBoolean redScreenSwap(EGLDisplay display, EGLSurface surface) {
    // Experimental test: paint the default framebuffer red.
    GLint oldFramebuffer = 0;
    GLint oldScissor[4] = {};
    GLboolean oldColorMask[4] = {};
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFramebuffer);
    glGetIntegerv(GL_SCISSOR_BOX, oldScissor);
    glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindFramebuffer(GL_FRAMEBUFFER,
                      static_cast<GLuint>(oldFramebuffer));
    glColorMask(oldColorMask[0], oldColorMask[1],
                oldColorMask[2], oldColorMask[3]);
    glScissor(oldScissor[0], oldScissor[1],
              oldScissor[2], oldScissor[3]);

    if (scissorEnabled)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);

    return originalSwap
        ? originalSwap(display, surface)
        : EGL_FALSE;
}

void removeRenderHook() {
    if (hookInstalled && swapTarget) {
        pl::memory::unhook(
            swapTarget,
            reinterpret_cast<void*>(&redScreenSwap));
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
    self.getLogger().debug("Loading...");

    std::error_code ec;
    std::filesystem::create_directories(self.getDataDir(), ec);
    if (ec) {
        self.getLogger().error(
            "Failed to create data directory {}: {}",
            self.getDataDir().string(), ec.message());
        return false;
    }

    std::filesystem::create_directories(self.getConfigDir(), ec);
    if (ec) {
        self.getLogger().error(
            "Failed to create config directory {}: {}",
            self.getConfigDir().string(), ec.message());
        return false;
    }

    mConfigFile.emplace();
    if (!mConfigFile->load()) {
        self.getLogger().warn("Failed to load typed config");
        return false;
    }

    mConfig = mConfigFile->value();
    self.getLogger().info(
        "Loaded {} from {}", self.getName(),
        self.getModDir().string());

    return true;
}

bool ClangeMeMod::enable() {
    auto& self = getSelf();
    self.getLogger().debug("Enabling...");

    if (!mConfig.enabled) {
        self.getLogger().info(
            "clange_me is disabled by config");
        return true;
    }

    eglLibrary = dlopen("libEGL.so", RTLD_NOW);
    if (!eglLibrary) {
        self.getLogger().error(
            "PostFX test: cannot open libEGL.so");
        return false;
    }

    swapTarget = dlsym(eglLibrary, "eglSwapBuffers");
    if (!swapTarget) {
        self.getLogger().error(
            "PostFX test: eglSwapBuffers not found");
        removeRenderHook();
        return false;
    }

    void* original = nullptr;
    const int result = pl::memory::hook(
        swapTarget,
        reinterpret_cast<void*>(&redScreenSwap),
        &original);

    if (result != 0 || !original) {
        self.getLogger().error(
            "PostFX test: hook failed, code={}", result);
        removeRenderHook();
        return false;
    }

    originalSwap = reinterpret_cast<SwapFn>(original);
    hookInstalled = true;

    self.getLogger().warn(
        "PostFX experimental EGL hook installed");

    return true;
}

bool ClangeMeMod::disable() {
    getSelf().getLogger().debug("Disabling...");
    removeRenderHook();
    return true;
}

bool ClangeMeMod::unload() {
    getSelf().getLogger().debug("Unloading...");
    removeRenderHook();
    mConfigFile.reset();
    return true;
}

} // namespace clange_me
