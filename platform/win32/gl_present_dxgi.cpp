// See gl_context.h (cane::gl::dxgi). The games' frames reach the screen through a DXGI flip-model swap chain
// instead of the OpenGL driver's own present.
//
// A borderless window that covers its monitor is presented by the OpenGL driver like exclusive fullscreen.
// Another window on top (a focus change, Alt+Tab) forces it back to the desktop compositor, and that switch
// blanks the screen. A flip-model swap chain moves between direct scan-out and composition without blanking
// (Windows' fullscreen optimizations), as D3D games' borderless windows do.
//
// OpenGL draws where the game binds framebuffer 0: an offscreen framebuffer. At the swap it is copied, flipped
// to D3D's top-down rows, into a D3D11 texture shared through WGL_NV_DX_interop2, and that texture into the swap
// chain's back buffer. glad loads glBindFramebuffer / glDrawBuffer / glReadBuffer / glGetIntegerv through
// ProcAddress, so framebuffer 0 means the offscreen one everywhere, the GL state mirror (Sly1-Release glstate.h)
// included. Without the extension, or with CANE_GL_PRESENT=native, the games present through OpenGL as before.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// Sly's forced Windows compatibility header undefines "interface" to avoid
// game-identifier collisions; DXGI's COM declarations still require it.
#ifndef interface
#define interface struct
#endif
#include <d3d11.h>
#include <dxgi1_5.h>

#include <glad/glad.h>   // (before GLFW, which would include <GL/gl.h> otherwise)
#include "gl_context.h"
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cane::gl::dxgi {
namespace {

// WGL_NV_DX_interop / WGL_NV_DX_interop2
#define CANE_WGL_ACCESS_WRITE_DISCARD_NV 0x0002
using PFNOPENDEVICE = HANDLE(WINAPI*)(void* dxDevice);
using PFNCLOSEDEVICE = BOOL(WINAPI*)(HANDLE device);
using PFNREGISTEROBJECT = HANDLE(WINAPI*)(HANDLE device, void* dxObject, GLuint name, GLenum type, GLenum access);
using PFNUNREGISTEROBJECT = BOOL(WINAPI*)(HANDLE device, HANDLE object);
using PFNLOCKOBJECTS = BOOL(WINAPI*)(HANDLE device, GLint count, HANDLE* objects);

using PFNCREATEDEVICE = decltype(&D3D11CreateDevice);
using PFNCREATEFACTORY = HRESULT(WINAPI*)(REFIID, void**);

template <typename T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Presenter {
    bool active = false;
    GLFWwindow* window = nullptr;
    HWND hwnd = nullptr;
    int width = 0, height = 0;   // the offscreen framebuffer's and the swap chain's size
    int interval = 1;
    bool tearing = false;        // DXGI_FEATURE_PRESENT_ALLOW_TEARING: an interval of 0 without VSync

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain1* swapChain = nullptr;
    ID3D11Texture2D* shared = nullptr;   // GL draws into it (interopColor), D3D copies it to the back buffer
    HANDLE interopDevice = nullptr, interopObject = nullptr;

    GLuint sceneFbo = 0, sceneColor = 0;          // what the game draws as framebuffer 0
    GLuint interopFbo = 0, interopColor = 0;
    GLuint drawFbo = 0, readFbo = 0;              // the real bindings

    PFNGLBINDFRAMEBUFFERPROC bindFramebuffer = nullptr;
    PFNGLDRAWBUFFERPROC drawBuffer = nullptr;
    PFNGLREADBUFFERPROC readBuffer = nullptr;
    PFNGLGETINTEGERVPROC getIntegerv = nullptr;
    PFNGLGENFRAMEBUFFERSPROC genFramebuffers = nullptr;
    PFNGLDELETEFRAMEBUFFERSPROC deleteFramebuffers = nullptr;
    PFNGLGENRENDERBUFFERSPROC genRenderbuffers = nullptr;
    PFNGLDELETERENDERBUFFERSPROC deleteRenderbuffers = nullptr;
    PFNGLBINDRENDERBUFFERPROC bindRenderbuffer = nullptr;
    PFNGLRENDERBUFFERSTORAGEPROC renderbufferStorage = nullptr;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC framebufferRenderbuffer = nullptr;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC checkFramebufferStatus = nullptr;
    PFNGLBLITFRAMEBUFFERPROC blitFramebuffer = nullptr;
    PFNGLISENABLEDPROC isEnabled = nullptr;
    PFNGLENABLEPROC enable = nullptr;
    PFNGLDISABLEPROC disable = nullptr;
    PFNGLGETSTRINGPROC getString = nullptr;

    PFNOPENDEVICE openDevice = nullptr;
    PFNCLOSEDEVICE closeDevice = nullptr;
    PFNREGISTEROBJECT registerObject = nullptr;
    PFNUNREGISTEROBJECT unregisterObject = nullptr;
    PFNLOCKOBJECTS lockObjects = nullptr;
    PFNLOCKOBJECTS unlockObjects = nullptr;
};
Presenter s;

template <typename T> bool Load(T& pfn, const char* name) {
    pfn = reinterpret_cast<T>(glfwGetProcAddress(name));
    return pfn != nullptr;
}

bool LoadFunctions() {
    return Load(s.bindFramebuffer, "glBindFramebuffer") && Load(s.drawBuffer, "glDrawBuffer") &&
           Load(s.readBuffer, "glReadBuffer") && Load(s.getIntegerv, "glGetIntegerv") &&
           Load(s.genFramebuffers, "glGenFramebuffers") && Load(s.deleteFramebuffers, "glDeleteFramebuffers") &&
           Load(s.genRenderbuffers, "glGenRenderbuffers") && Load(s.deleteRenderbuffers, "glDeleteRenderbuffers") &&
           Load(s.bindRenderbuffer, "glBindRenderbuffer") && Load(s.renderbufferStorage, "glRenderbufferStorage") &&
           Load(s.framebufferRenderbuffer, "glFramebufferRenderbuffer") &&
           Load(s.checkFramebufferStatus, "glCheckFramebufferStatus") && Load(s.blitFramebuffer, "glBlitFramebuffer") &&
           Load(s.isEnabled, "glIsEnabled") && Load(s.enable, "glEnable") && Load(s.disable, "glDisable") &&
           Load(s.getString, "glGetString") && Load(s.openDevice, "wglDXOpenDeviceNV") &&
           Load(s.closeDevice, "wglDXCloseDeviceNV") && Load(s.registerObject, "wglDXRegisterObjectNV") &&
           Load(s.unregisterObject, "wglDXUnregisterObjectNV") && Load(s.lockObjects, "wglDXLockObjectsNV") &&
           Load(s.unlockObjects, "wglDXUnlockObjectsNV");
}

// The D3D device must be on the GPU that runs the OpenGL context (hybrid laptops have two): the adapter of the
// GL vendor, else the default one.
IDXGIAdapter1* AdapterForGl(IDXGIFactory1* factory) {
    const char* vendor = reinterpret_cast<const char*>(s.getString(GL_VENDOR));
    UINT id = 0;
    if (vendor && std::strstr(vendor, "NVIDIA")) id = 0x10DE;
    else if (vendor && (std::strstr(vendor, "AMD") || std::strstr(vendor, "ATI"))) id = 0x1002;
    else if (vendor && std::strstr(vendor, "Intel")) id = 0x8086;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; id && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == id && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            return adapter;
        Release(adapter);
    }
    return nullptr;
}

void ReleaseTargets() {
    if (s.interopObject) { s.unregisterObject(s.interopDevice, s.interopObject); s.interopObject = nullptr; }
    if (s.interopColor) { s.deleteRenderbuffers(1, &s.interopColor); s.interopColor = 0; }
    Release(s.shared);
}

// The shared texture as interopFbo's colour, with interopFbo bound for drawing. Only a locked interop object has
// storage, and NVIDIA's driver leaves the framebuffer incomplete after the next unlock: attached after every lock.
void AttachShared() {
    s.framebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s.interopColor);
}

// The shared texture at the current size, registered with OpenGL (as a renderbuffer) and checked as interopFbo's
// colour.
bool CreateTargets() {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = UINT(s.width);
    desc.Height = UINT(s.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(s.device->CreateTexture2D(&desc, nullptr, &s.shared))) return false;
    s.genRenderbuffers(1, &s.interopColor);
    s.interopObject = s.registerObject(s.interopDevice, s.shared, s.interopColor, GL_RENDERBUFFER,
                                       CANE_WGL_ACCESS_WRITE_DISCARD_NV);
    if (!s.interopObject || !s.lockObjects(s.interopDevice, 1, &s.interopObject)) return false;
    s.bindFramebuffer(GL_DRAW_FRAMEBUFFER, s.interopFbo);
    AttachShared();
    const GLenum status = s.checkFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    s.bindFramebuffer(GL_DRAW_FRAMEBUFFER, s.drawFbo);
    s.unlockObjects(s.interopDevice, 1, &s.interopObject);
    return status == GL_FRAMEBUFFER_COMPLETE;
}

void SizeScene() {
    s.bindRenderbuffer(GL_RENDERBUFFER, s.sceneColor);
    s.renderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, s.width, s.height);
    s.bindRenderbuffer(GL_RENDERBUFFER, 0);
}

void Shutdown() {
    const bool wasActive = s.active;
    s.active = false;
    if (s.interopDevice) {
        ReleaseTargets();
        s.closeDevice(s.interopDevice);
        s.interopDevice = nullptr;
    }
    Release(s.swapChain);
    Release(s.context);
    Release(s.device);
    if (s.bindFramebuffer) {
        // framebuffer 0 is the window's own again
        if (s.drawFbo == s.sceneFbo) s.bindFramebuffer(GL_DRAW_FRAMEBUFFER, s.drawFbo = 0);
        if (s.readFbo == s.sceneFbo) s.bindFramebuffer(GL_READ_FRAMEBUFFER, s.readFbo = 0);
    }
    if (s.sceneFbo) { s.deleteFramebuffers(1, &s.sceneFbo); s.sceneFbo = 0; }
    if (s.interopFbo) { s.deleteFramebuffers(1, &s.interopFbo); s.interopFbo = 0; }
    if (s.sceneColor) { s.deleteRenderbuffers(1, &s.sceneColor); s.sceneColor = 0; }
    if (wasActive) std::fprintf(stderr, "Presenting through OpenGL\n");
}

bool Fail(const char* what, HRESULT hr = S_OK) {
    std::fprintf(stderr, "DXGI presentation unavailable (%s, 0x%08lx)\n", what, static_cast<unsigned long>(hr));
    Shutdown();
    return false;
}

// framebuffer 0 follows the window's size (glfwGetFramebufferSize, as the games read it); a minimized window
// (0 x 0) keeps the last one.
void FollowWindowSize(GLFWwindow* window) {
    int width = 0, height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    if (width <= 0 || height <= 0 || (width == s.width && height == s.height)) return;
    s.width = width;
    s.height = height;
    SizeScene();
    ReleaseTargets();
    const HRESULT hr = s.swapChain->ResizeBuffers(0, UINT(width), UINT(height), DXGI_FORMAT_UNKNOWN,
                                                  s.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr)) { Fail("ResizeBuffers", hr); return; }
    if (!CreateTargets()) Fail("the shared texture");
}

void APIENTRY BindFramebuffer(GLenum target, GLuint framebuffer) {
    if (framebuffer == 0 && s.active) {
        FollowWindowSize(s.window);
        if (s.active) framebuffer = s.sceneFbo;
    }
    s.bindFramebuffer(target, framebuffer);
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) s.drawFbo = framebuffer;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) s.readFbo = framebuffer;
}

// The window's buffers (GL_BACK, GL_FRONT, ...) are the offscreen framebuffer's colour attachment.
GLenum WindowBuffer(GLenum buffer) {
    switch (buffer) {
    case GL_BACK: case GL_BACK_LEFT: case GL_FRONT: case GL_FRONT_LEFT: case GL_LEFT: case GL_FRONT_AND_BACK:
        return GL_COLOR_ATTACHMENT0;
    default:
        return buffer;
    }
}

void APIENTRY DrawBuffer(GLenum buffer) {
    s.drawBuffer(s.active && s.drawFbo == s.sceneFbo ? WindowBuffer(buffer) : buffer);
}

void APIENTRY ReadBuffer(GLenum buffer) {
    s.readBuffer(s.active && s.readFbo == s.sceneFbo ? WindowBuffer(buffer) : buffer);
}

void APIENTRY GetIntegerv(GLenum name, GLint* data) {
    s.getIntegerv(name, data);
    if (s.active && (name == GL_DRAW_FRAMEBUFFER_BINDING || name == GL_READ_FRAMEBUFFER_BINDING) &&
        GLuint(*data) == s.sceneFbo)
        *data = 0;
}

}  // namespace

bool Start(GLFWwindow* window) {
    if (const char* mode = std::getenv("CANE_GL_PRESENT"); mode && !std::strcmp(mode, "native")) return false;
    if (!glfwExtensionSupported("WGL_NV_DX_interop2")) {
        std::fprintf(stderr, "DXGI presentation unavailable (no WGL_NV_DX_interop2)\n");
        return false;
    }
    if (!LoadFunctions()) return Fail("the interop functions");
    s.window = window;
    s.hwnd = glfwGetWin32Window(window);
    glfwGetFramebufferSize(window, &s.width, &s.height);
    if (s.width <= 0) s.width = 1;
    if (s.height <= 0) s.height = 1;

    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto createDevice = d3d11 ? reinterpret_cast<PFNCREATEDEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice")) : nullptr;
    auto createFactory = dxgi ? reinterpret_cast<PFNCREATEFACTORY>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (!createDevice || !createFactory) return Fail("d3d11.dll / dxgi.dll");

    IDXGIFactory2* factory = nullptr;
    HRESULT hr = createFactory(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) return Fail("CreateDXGIFactory1", hr);
    IDXGIAdapter1* adapter = AdapterForGl(factory);
    hr = createDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                      D3D11_SDK_VERSION, &s.device, nullptr, &s.context);
    Release(adapter);
    if (FAILED(hr)) { Release(factory); return Fail("D3D11CreateDevice", hr); }

    IDXGIFactory5* factory5 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&factory5)))) {
        BOOL allow = FALSE;
        s.tearing = SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)) &&
                    allow;
        Release(factory5);
    }
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = UINT(s.width);
    desc.Height = UINT(s.height);
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = s.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    hr = factory->CreateSwapChainForHwnd(s.device, s.hwnd, &desc, nullptr, nullptr, &s.swapChain);
    if (FAILED(hr)) {
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;   // (before Windows 10)
        hr = factory->CreateSwapChainForHwnd(s.device, s.hwnd, &desc, nullptr, nullptr, &s.swapChain);
    }
    if (SUCCEEDED(hr)) factory->MakeWindowAssociation(s.hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    Release(factory);
    if (FAILED(hr)) return Fail("CreateSwapChainForHwnd", hr);

    s.interopDevice = s.openDevice(s.device);
    if (!s.interopDevice) return Fail("wglDXOpenDeviceNV", HRESULT(GetLastError()));
    s.genFramebuffers(1, &s.sceneFbo);
    s.genFramebuffers(1, &s.interopFbo);
    s.genRenderbuffers(1, &s.sceneColor);
    SizeScene();
    s.bindFramebuffer(GL_FRAMEBUFFER, s.sceneFbo);
    s.framebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s.sceneColor);
    s.drawFbo = s.readFbo = s.sceneFbo;
    if (s.checkFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return Fail("the offscreen framebuffer");
    if (!CreateTargets()) return Fail("the shared texture");
    s.active = true;
    std::fprintf(stderr, "Presenting through a DXGI flip-model swap chain\n");
    return true;
}

void Stop() { Shutdown(); }

GLFWglproc ProcAddress(const char* name) {
    if (!s.active) return nullptr;
    if (!std::strcmp(name, "glBindFramebuffer")) return reinterpret_cast<GLFWglproc>(BindFramebuffer);
    if (!std::strcmp(name, "glDrawBuffer")) return reinterpret_cast<GLFWglproc>(DrawBuffer);
    if (!std::strcmp(name, "glReadBuffer")) return reinterpret_cast<GLFWglproc>(ReadBuffer);
    if (!std::strcmp(name, "glGetIntegerv")) return reinterpret_cast<GLFWglproc>(GetIntegerv);
    return nullptr;
}

bool SetSwapInterval(int interval) {
    if (!s.active) return false;
    s.interval = interval;
    return true;
}

bool SwapBuffers(GLFWwindow* window) {
    if (!s.active) return false;
    // The frame, bottom-up rows flipped to D3D's top-down, into the shared texture (the scissor test clips blits).
    const GLboolean scissor = s.isEnabled(GL_SCISSOR_TEST);
    if (scissor) s.disable(GL_SCISSOR_TEST);
    s.bindFramebuffer(GL_READ_FRAMEBUFFER, s.sceneFbo);
    s.bindFramebuffer(GL_DRAW_FRAMEBUFFER, s.interopFbo);
    const bool locked = s.lockObjects(s.interopDevice, 1, &s.interopObject);
    if (locked) {
        AttachShared();
        s.blitFramebuffer(0, 0, s.width, s.height, 0, s.height, s.width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        s.unlockObjects(s.interopDevice, 1, &s.interopObject);
    }
    s.bindFramebuffer(GL_READ_FRAMEBUFFER, s.readFbo);
    s.bindFramebuffer(GL_DRAW_FRAMEBUFFER, s.drawFbo);
    if (scissor) s.enable(GL_SCISSOR_TEST);
    if (!locked) return Fail("wglDXLockObjectsNV", HRESULT(GetLastError()));

    ID3D11Texture2D* back = nullptr;
    HRESULT hr = s.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    if (FAILED(hr)) return Fail("GetBuffer", hr);
    s.context->CopyResource(back, s.shared);
    Release(back);
    const UINT interval = UINT(s.interval < 0 ? 0 : s.interval > 4 ? 4 : s.interval);
    hr = s.swapChain->Present(interval, interval == 0 && s.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(hr)) {
        Fail("Present", hr);
        glfwSwapBuffers(window);
        return true;
    }
    FollowWindowSize(window);
    return true;
}

}  // namespace cane::gl::dxgi
