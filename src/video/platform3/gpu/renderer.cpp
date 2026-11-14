#include "clickhle/video/classic_gpu.h"
#include <unordered_map>
#include <vector>

#if CLICKHLE_HAVE_CLASSIC_GPU
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#endif

namespace clickhle::classic {

struct GpuRenderer::Impl {
#if CLICKHLE_HAVE_CLASSIC_GPU
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    std::unordered_map<u32, GLuint> textures;
    GLuint program = 0;
    GLuint vertex_buffer = 0;
    GLuint vertex_array = 0;
#endif
};

GpuRenderer::GpuRenderer() : impl_(std::make_unique<Impl>()) {}
GpuRenderer::~GpuRenderer() {
#if CLICKHLE_HAVE_CLASSIC_GPU
    if (impl_->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(impl_->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (impl_->context != EGL_NO_CONTEXT) eglDestroyContext(impl_->display, impl_->context);
        if (impl_->surface != EGL_NO_SURFACE) eglDestroySurface(impl_->display, impl_->surface);
        eglTerminate(impl_->display);
    }
#endif
}

bool GpuRenderer::initialise(std::string& error) {
#if CLICKHLE_HAVE_CLASSIC_GPU
    auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (platform) impl_->display = platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (impl_->display == EGL_NO_DISPLAY) impl_->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (impl_->display == EGL_NO_DISPLAY || !eglInitialize(impl_->display, nullptr, nullptr) ||
        !eglBindAPI(EGL_OPENGL_API)) {
        error = "Could not initialize an EGL desktop OpenGL display";
        return false;
    }
    constexpr EGLint config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
        EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig config{};
    EGLint count = 0;
    if (!eglChooseConfig(impl_->display, config_attrs, &config, 1, &count) || count == 0) {
        error = "No EGL OpenGL pbuffer configuration is available"; return false;
    }
    constexpr EGLint surface_attrs[] = {EGL_WIDTH, 320, EGL_HEIGHT, 240, EGL_NONE};
    impl_->surface = eglCreatePbufferSurface(impl_->display, config, surface_attrs);
    constexpr EGLint context_attrs[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
        EGL_CONTEXT_MINOR_VERSION_KHR, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE};
    impl_->context = eglCreateContext(impl_->display, config, EGL_NO_CONTEXT, context_attrs);
    if (impl_->surface == EGL_NO_SURFACE || impl_->context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(impl_->display, impl_->surface, impl_->surface, impl_->context)) {
        error = "Could not create an EGL OpenGL context"; return false;
    }
    glViewport(0, 0, 320, 240);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    return true;
#else
    error = "Desktop OpenGL backend was not built (EGL and OpenGL development libraries required)";
    return false;
#endif
}

bool GpuRenderer::bind_texture(u32 target, u32 name) {
#if CLICKHLE_HAVE_CLASSIC_GPU
    if (eglGetCurrentContext() != impl_->context &&
        !eglMakeCurrent(impl_->display, impl_->surface, impl_->surface, impl_->context)) return false;
    auto [it, inserted] = impl_->textures.try_emplace(name, 0);
    if (inserted) glGenTextures(1, &it->second);
    const GLuint host_name = it->second;
    glBindTexture(target, host_name); return glGetError() == GL_NO_ERROR;
#else
    (void)target; (void)name; return false;
#endif
}
bool GpuRenderer::texture_image(u32 target, i32 level, i32 internal_format, i32 width,
                                 i32 height, i32 border, u32 format, u32 type, const void* pixels) {
#if CLICKHLE_HAVE_CLASSIC_GPU
    if (eglGetCurrentContext() != impl_->context &&
        !eglMakeCurrent(impl_->display, impl_->surface, impl_->surface, impl_->context)) return false;
    // Core OpenGL removed ES 1.x luminance and alpha formats. Store them in
    // red/green textures and reproduce their sampling with texture swizzles.
    constexpr u32 red = 0x1903, rg = 0x8227, red8 = 0x8229, rg8 = 0x822b;
    constexpr u32 swizzle_r = 0x8e42, swizzle_g = 0x8e43;
    constexpr u32 swizzle_b = 0x8e44, swizzle_a = 0x8e45;
    if (format == 0x1906 || format == 0x1909 || format == 0x190a) {
        if (type != 0x1401) return false;
        const bool alpha = format == 0x1906;
        const bool luminance_alpha = format == 0x190a;
        internal_format = static_cast<i32>(luminance_alpha ? rg8 : red8);
        format = luminance_alpha ? rg : red;
        glTexParameteri(target, swizzle_r, alpha ? GL_ONE : GL_RED);
        glTexParameteri(target, swizzle_g, alpha ? GL_ONE : GL_RED);
        glTexParameteri(target, swizzle_b, alpha ? GL_ONE : GL_RED);
        glTexParameteri(target, swizzle_a, alpha ? GL_RED : luminance_alpha ? GL_GREEN : GL_ONE);
    } else {
        glTexParameteri(target, swizzle_r, GL_RED);
        glTexParameteri(target, swizzle_g, GL_GREEN);
        glTexParameteri(target, swizzle_b, GL_BLUE);
        glTexParameteri(target, swizzle_a, GL_ALPHA);
    }
    glTexImage2D(target, level, internal_format, width, height, border, format, type, pixels);
    return glGetError() == GL_NO_ERROR;
#else
    (void)target; (void)level; (void)internal_format; (void)width; (void)height;
    (void)border; (void)format; (void)type; (void)pixels; return false;
#endif
}
bool GpuRenderer::texture_parameter(u32 target, u32 parameter, i32 value) {
#if CLICKHLE_HAVE_CLASSIC_GPU
    if (eglGetCurrentContext() != impl_->context &&
        !eglMakeCurrent(impl_->display, impl_->surface, impl_->surface, impl_->context)) return false;
    glTexParameteri(target, parameter, value); return glGetError() == GL_NO_ERROR;
#else
    (void)target; (void)parameter; (void)value; return false;
#endif
}

bool GpuRenderer::texture_sub_image(u32 target, i32 level, i32 x, i32 y, i32 width,
                                    i32 height, u32 format, u32 type, const void* pixels) {
#if CLICKHLE_HAVE_CLASSIC_GPU
    if (eglGetCurrentContext() != impl_->context &&
        !eglMakeCurrent(impl_->display, impl_->surface, impl_->surface, impl_->context)) return false;
    // Match the storage swizzles selected by texture_image() for GLES 1.x
    // alpha, luminance, and luminance-alpha images.
    constexpr u32 red = 0x1903, rg = 0x8227;
    if (type == 0x1401) {
        if (format == 0x1906 || format == 0x1909) format = red;
        else if (format == 0x190a) format = rg;
    }
    glTexSubImage2D(target, level, x, y, width, height, format, type, pixels);
    return glGetError() == GL_NO_ERROR;
#else
    (void)target; (void)level; (void)x; (void)y; (void)width; (void)height;
    (void)format; (void)type; (void)pixels; return false;
#endif
}

bool GpuRenderer::draw_arrays(u32 mode, i32 first, i32 count, std::span<const float> xy,
                              std::span<const float, 16> matrix, std::span<const float, 4> color,
                              bool cull_face_enabled,
                              std::span<u8> rgb565, u32 stride_pixels, u32 width, u32 height) {
#if CLICKHLE_HAVE_CLASSIC_GPU
    if (eglGetCurrentContext() != impl_->context &&
        !eglMakeCurrent(impl_->display, impl_->surface, impl_->surface, impl_->context)) return false;
    if (count < 0 || first < 0 || xy.size() < (static_cast<std::size_t>(first) +
        static_cast<std::size_t>(count)) * 2 || rgb565.size() <
        static_cast<std::size_t>(stride_pixels) * height * 2) return false;
    if (!impl_->program) {
        constexpr char vertex_source[] = "#version 330 core\nlayout(location=0) in vec2 p; uniform mat4 transform; void main(){gl_Position=transform*vec4(p,0.0,1.0);}";
        constexpr char fragment_source[] = "#version 330 core\nuniform vec4 current_color; out vec4 color; void main(){color=current_color;}";
        const auto compile = [](GLenum type, const char* source) {
            const GLuint shader = glCreateShader(type); glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader); GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (!ok) { glDeleteShader(shader); return GLuint{0}; } return shader;
        };
        const GLuint vs = compile(GL_VERTEX_SHADER, vertex_source), fs = compile(GL_FRAGMENT_SHADER, fragment_source);
        if (!vs || !fs) return false;
        impl_->program = glCreateProgram(); glAttachShader(impl_->program, vs); glAttachShader(impl_->program, fs);
        glLinkProgram(impl_->program); glDeleteShader(vs); glDeleteShader(fs);
        GLint linked = 0; glGetProgramiv(impl_->program, GL_LINK_STATUS, &linked);
        if (!linked) { glDeleteProgram(impl_->program); impl_->program = 0; return false; }
        glGenBuffers(1, &impl_->vertex_buffer);
        glGenVertexArrays(1, &impl_->vertex_array);
    }
    glBindVertexArray(impl_->vertex_array);
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, 320, 240);
    if (cull_face_enabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glUseProgram(impl_->program); glBindBuffer(GL_ARRAY_BUFFER, impl_->vertex_buffer);
    const GLint transform = glGetUniformLocation(impl_->program, "transform");
    glUniformMatrix4fv(transform, 1, GL_FALSE, matrix.data());
    const GLint current_color = glGetUniformLocation(impl_->program, "current_color");
    glUniform4fv(current_color, 1, color.data());
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(xy.size_bytes()), xy.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    GLenum primitive = mode == 0x0004 ? GL_TRIANGLES : mode == 0x0005 ? GL_TRIANGLE_STRIP :
                       mode == 0x0006 ? GL_TRIANGLE_FAN : mode == 0x0001 ? GL_LINES : 0;
    if (!primitive) return false;
    glDrawArrays(primitive, first, count);
    std::vector<u8> rgba(static_cast<std::size_t>(width) * height * 4);
    glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    if (glGetError() != GL_NO_ERROR) return false;
    for (u32 y = 0; y < height; ++y) for (u32 x = 0; x < width; ++x) {
        const std::size_t src = (static_cast<std::size_t>(height - 1 - y) * width + x) * 4;
        const u16 pixel = static_cast<u16>(((rgba[src] >> 3) << 11) | ((rgba[src + 1] >> 2) << 5) | (rgba[src + 2] >> 3));
        const std::size_t dst = (static_cast<std::size_t>(y) * stride_pixels + x) * 2;
        rgb565[dst] = static_cast<u8>(pixel); rgb565[dst + 1] = static_cast<u8>(pixel >> 8);
    }
    return true;
#else
    (void)mode; (void)first; (void)count; (void)xy; (void)matrix; (void)color; (void)cull_face_enabled; (void)rgb565; (void)stride_pixels; (void)width; (void)height; return false;
#endif
}

} // namespace clickhle::classic
