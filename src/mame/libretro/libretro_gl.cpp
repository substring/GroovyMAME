// license:BSD-3-Clause
// copyright-holders:Substring
/***************************************************************************

    libretro_gl.cpp - OpenGL context for hardware rendered libretro cores

    Linux and other EGL platforms: an EGL context without surface on a
    DRM render node, so it works under X11, Wayland or the KMS console.
    EGL and GL are loaded at run time, nothing is linked.

    The frame is read back in the same frame it's rendered, so the
    readback adds no frame of latency:
      - persistent: GL_ARB_buffer_storage buffer in cached system memory,
        mapped once, the bitmap points directly into it
      - pbo: pixel buffer object mapped for every frame and copied
      - direct: plain glReadPixels, for contexts without buffer objects
    The GPU copy is waited for with glFinish: a fence would allow doing
    something else meanwhile, but there's nothing else to do in the
    frame, and fence waits stall for about 500 ms at times with the r600
    driver (Radeon HD 5750). The time spent is logged with -verbose.

***************************************************************************/

#include "emu.h"
#include "libretro_gl.h"

#include "modules/lib/osdlib.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>


#if !defined(_WIN32) && !defined(__APPLE__)

namespace {

//**************************************************************************
//  EGL AND GL DEFINITIONS
//**************************************************************************

typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLConfig;
typedef void *EGLDeviceEXT;
typedef int32_t EGLint;
typedef unsigned EGLBoolean;
typedef unsigned EGLenum;

constexpr EGLint EGL_NONE                                    = 0x3038;
constexpr EGLint EGL_EXTENSIONS                              = 0x3055;
constexpr EGLint EGL_RENDERABLE_TYPE                         = 0x3040;
constexpr EGLint EGL_OPENGL_BIT                              = 0x0008;
constexpr EGLint EGL_OPENGL_ES2_BIT                          = 0x0004;
constexpr EGLint EGL_OPENGL_ES3_BIT                          = 0x0040;
constexpr EGLenum EGL_OPENGL_API                             = 0x30A2;
constexpr EGLenum EGL_OPENGL_ES_API                          = 0x30A0;
constexpr EGLint EGL_CONTEXT_MAJOR_VERSION                   = 0x3098;
constexpr EGLint EGL_CONTEXT_MINOR_VERSION                   = 0x30FB;
constexpr EGLint EGL_CONTEXT_OPENGL_PROFILE_MASK             = 0x30FD;
constexpr EGLint EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT         = 0x0001;
constexpr EGLint EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT = 0x0002;
constexpr EGLint EGL_CONTEXT_OPENGL_DEBUG                    = 0x31B0;
constexpr EGLenum EGL_PLATFORM_SURFACELESS_MESA              = 0x31DD;
constexpr EGLenum EGL_PLATFORM_DEVICE_EXT                    = 0x313F;
constexpr EGLint EGL_DRM_DEVICE_FILE_EXT                     = 0x3233;
constexpr EGLint EGL_DRM_RENDER_NODE_FILE_EXT                = 0x3377;

typedef unsigned GLenum;
typedef unsigned GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned GLbitfield;
typedef unsigned char GLubyte;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;

constexpr GLenum GL_FRAMEBUFFER                  = 0x8D40;
constexpr GLenum GL_READ_FRAMEBUFFER             = 0x8CA8;
constexpr GLenum GL_DRAW_FRAMEBUFFER             = 0x8CA9;
constexpr GLenum GL_RENDERBUFFER                 = 0x8D41;
constexpr GLenum GL_COLOR_ATTACHMENT0            = 0x8CE0;
constexpr GLenum GL_DEPTH_ATTACHMENT             = 0x8D00;
constexpr GLenum GL_DEPTH_STENCIL_ATTACHMENT     = 0x821A;
constexpr GLenum GL_DEPTH24_STENCIL8             = 0x88F0;
constexpr GLenum GL_DEPTH_COMPONENT24            = 0x81A6;
constexpr GLenum GL_DEPTH_COMPONENT16            = 0x81A5;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE         = 0x8CD5;
constexpr GLenum GL_TEXTURE_2D                   = 0x0DE1;
constexpr GLenum GL_TEXTURE_MIN_FILTER           = 0x2801;
constexpr GLenum GL_TEXTURE_MAG_FILTER           = 0x2800;
constexpr GLint  GL_NEAREST                      = 0x2600;
constexpr GLint  GL_RGBA8                        = 0x8058;
constexpr GLenum GL_RGBA                         = 0x1908;
constexpr GLenum GL_BGRA                         = 0x80E1;
constexpr GLenum GL_UNSIGNED_BYTE                = 0x1401;
constexpr GLenum GL_UNSIGNED_INT_8_8_8_8_REV     = 0x8367;
constexpr GLenum GL_PIXEL_PACK_BUFFER            = 0x88EB;
constexpr GLenum GL_STREAM_READ                  = 0x88E1;
constexpr GLbitfield GL_MAP_READ_BIT             = 0x0001;
constexpr GLbitfield GL_MAP_PERSISTENT_BIT       = 0x0040;
constexpr GLbitfield GL_MAP_COHERENT_BIT         = 0x0080;
constexpr GLbitfield GL_CLIENT_STORAGE_BIT       = 0x0200;
constexpr GLenum GL_PACK_ALIGNMENT               = 0x0D05;
constexpr GLenum GL_PACK_ROW_LENGTH              = 0x0D02;
constexpr GLbitfield GL_COLOR_BUFFER_BIT         = 0x4000;
constexpr GLenum GL_VERSION                      = 0x1F02;
constexpr GLenum GL_RENDERER                     = 0x1F01;
constexpr GLenum GL_EXTENSIONS                   = 0x1F03;
constexpr GLenum GL_NUM_EXTENSIONS               = 0x821D;

} // anonymous namespace


//**************************************************************************
//  IMPLEMENTATION
//**************************************************************************

class libretro_gl::impl
{
public:
	~impl() { destroy(); }

	bool create(const retro_hw_render_callback &callback, unsigned width, unsigned height, const std::string &device, const std::string &readback, std::string &error);
	void destroy();
	bool resize(unsigned width, unsigned height, std::string &error);
	void make_current();
	void release();
	retro_proc_address_t proc_address(const char *symbol) const;
	const bitmap_rgb32 &readback(unsigned width, unsigned height, bool flip);

	GLuint m_fbo = 0;
	std::string m_description;

private:
	enum class method { DIRECT, PBO, PERSISTENT };

	bool load_egl(std::string &error);
	bool open_display(const std::string &device, std::string &error);
	bool create_context(const retro_hw_render_callback &callback, std::string &error);
	bool load_gl(const std::string &readback, std::string &error);
	bool has_gl_extension(const char *name) const;
	bool create_framebuffer(unsigned width, unsigned height, std::string &error);
	void destroy_framebuffer();
	void log_statistics();

	template <typename T> void bind_egl(T &function, const char *name) { function = m_egl_module->bind<T>(name); }
	template <typename T> void bind_gl(T &function, const char *name) { function = reinterpret_cast<T>(proc_address(name)); }

	// EGL
	osd::dynamic_module::ptr m_egl_module;
	void *(*eglGetProcAddress)(const char *) = nullptr;
	EGLDisplay (*eglGetDisplay)(void *) = nullptr;
	EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *) = nullptr;
	EGLBoolean (*eglTerminate)(EGLDisplay) = nullptr;
	const char *(*eglQueryString)(EGLDisplay, EGLint) = nullptr;
	EGLBoolean (*eglBindAPI)(EGLenum) = nullptr;
	EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = nullptr;
	EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = nullptr;
	EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext) = nullptr;
	EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
	EGLContext (*eglGetCurrentContext)() = nullptr;
	EGLDisplay (*eglGetCurrentDisplay)() = nullptr;
	EGLSurface (*eglGetCurrentSurface)(EGLint) = nullptr;
	EGLenum (*eglQueryAPI)() = nullptr;
	EGLDisplay (*eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *) = nullptr;
	EGLBoolean (*eglQueryDevicesEXT)(EGLint, EGLDeviceEXT *, EGLint *) = nullptr;
	const char *(*eglQueryDeviceStringEXT)(EGLDeviceEXT, EGLint) = nullptr;

	// GLX, to hand the thread back to an OpenGL renderer using it
	osd::dynamic_module::ptr m_glx_module;
	void *(*glXGetCurrentContext)() = nullptr;
	void *(*glXGetCurrentDisplay)() = nullptr;
	unsigned long (*glXGetCurrentDrawable)() = nullptr;
	unsigned long (*glXGetCurrentReadDrawable)() = nullptr;
	int (*glXMakeContextCurrent)(void *, unsigned long, unsigned long, void *) = nullptr;

	// GL
	const GLubyte *(*glGetString)(GLenum) = nullptr;
	const GLubyte *(*glGetStringi)(GLenum, GLuint) = nullptr;
	void (*glGetIntegerv)(GLenum, GLint *) = nullptr;
	void (*glGenFramebuffers)(GLsizei, GLuint *) = nullptr;
	void (*glDeleteFramebuffers)(GLsizei, const GLuint *) = nullptr;
	void (*glBindFramebuffer)(GLenum, GLuint) = nullptr;
	void (*glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
	void (*glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;
	GLenum (*glCheckFramebufferStatus)(GLenum) = nullptr;
	void (*glGenRenderbuffers)(GLsizei, GLuint *) = nullptr;
	void (*glDeleteRenderbuffers)(GLsizei, const GLuint *) = nullptr;
	void (*glBindRenderbuffer)(GLenum, GLuint) = nullptr;
	void (*glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
	void (*glGenTextures)(GLsizei, GLuint *) = nullptr;
	void (*glDeleteTextures)(GLsizei, const GLuint *) = nullptr;
	void (*glBindTexture)(GLenum, GLuint) = nullptr;
	void (*glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *) = nullptr;
	void (*glTexParameteri)(GLenum, GLenum, GLint) = nullptr;
	void (*glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) = nullptr;
	void (*glPixelStorei)(GLenum, GLint) = nullptr;
	void (*glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) = nullptr;
	void (*glGenBuffers)(GLsizei, GLuint *) = nullptr;
	void (*glDeleteBuffers)(GLsizei, const GLuint *) = nullptr;
	void (*glBindBuffer)(GLenum, GLuint) = nullptr;
	void (*glBufferData)(GLenum, GLsizeiptr, const void *, GLenum) = nullptr;
	void (*glBufferStorage)(GLenum, GLsizeiptr, const void *, GLbitfield) = nullptr;
	void *(*glMapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield) = nullptr;
	unsigned char (*glUnmapBuffer)(GLenum) = nullptr;
	void (*glFinish)() = nullptr;

	// context
	EGLDisplay m_display = nullptr;
	EGLContext m_context = nullptr;
	bool m_gles = false;
	int m_version = 0;           // major * 10 + minor
	int m_depth = 0;

	// previous context, restored by release()
	int m_current_depth = 0;
	EGLDisplay m_prev_display = nullptr;
	EGLContext m_prev_context = nullptr;
	EGLSurface m_prev_draw = nullptr;
	EGLSurface m_prev_read = nullptr;
	EGLenum m_prev_api = 0;
	void *m_prev_glx_display = nullptr;
	void *m_prev_glx_context = nullptr;
	unsigned long m_prev_glx_draw = 0;
	unsigned long m_prev_glx_read = 0;

	// framebuffer
	GLuint m_color = 0;
	GLuint m_depth_stencil = 0;
	GLuint m_flip_fbo = 0;
	GLuint m_flip_color = 0;
	unsigned m_width = 0;
	unsigned m_height = 0;
	bool m_depth_buffer = false;
	bool m_stencil_buffer = false;

	// readback
	method m_method = method::DIRECT;
	GLuint m_pbo = 0;
	u32 *m_mapped = nullptr;
	std::vector<u32> m_read;     // direct readback
	std::vector<u32> m_pixels;   // bitmap for the copying readbacks
	bitmap_rgb32 m_bitmap;

	// statistics
	unsigned m_frames = 0;
	osd_ticks_t m_wait_total = 0;
	osd_ticks_t m_wait_max = 0;
	osd_ticks_t m_copy_total = 0;
};


//-------------------------------------------------
//  load_egl - EGL is loaded at run time
//-------------------------------------------------

bool libretro_gl::impl::load_egl(std::string &error)
{
	m_egl_module = osd::dynamic_module::open({ "libEGL.so.1", "libEGL.so" });
	bind_egl(eglGetProcAddress, "eglGetProcAddress");
	if (!eglGetProcAddress)
	{
		error = "libEGL not found";
		return false;
	}

	bind_egl(eglGetDisplay, "eglGetDisplay");
	bind_egl(eglInitialize, "eglInitialize");
	bind_egl(eglTerminate, "eglTerminate");
	bind_egl(eglQueryString, "eglQueryString");
	bind_egl(eglBindAPI, "eglBindAPI");
	bind_egl(eglChooseConfig, "eglChooseConfig");
	bind_egl(eglCreateContext, "eglCreateContext");
	bind_egl(eglDestroyContext, "eglDestroyContext");
	bind_egl(eglMakeCurrent, "eglMakeCurrent");
	bind_egl(eglGetCurrentContext, "eglGetCurrentContext");
	bind_egl(eglGetCurrentDisplay, "eglGetCurrentDisplay");
	bind_egl(eglGetCurrentSurface, "eglGetCurrentSurface");
	bind_egl(eglQueryAPI, "eglQueryAPI");
	eglGetPlatformDisplayEXT = reinterpret_cast<decltype(eglGetPlatformDisplayEXT)>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
	eglQueryDevicesEXT = reinterpret_cast<decltype(eglQueryDevicesEXT)>(eglGetProcAddress("eglQueryDevicesEXT"));
	eglQueryDeviceStringEXT = reinterpret_cast<decltype(eglQueryDeviceStringEXT)>(eglGetProcAddress("eglQueryDeviceStringEXT"));

	// GLX is optional, only needed if the MAME renderer uses it
	m_glx_module = osd::dynamic_module::open({ "libGLX.so.0", "libGL.so.1" });
	glXGetCurrentContext = m_glx_module->bind<decltype(glXGetCurrentContext)>("glXGetCurrentContext");
	glXGetCurrentDisplay = m_glx_module->bind<decltype(glXGetCurrentDisplay)>("glXGetCurrentDisplay");
	glXGetCurrentDrawable = m_glx_module->bind<decltype(glXGetCurrentDrawable)>("glXGetCurrentDrawable");
	glXGetCurrentReadDrawable = m_glx_module->bind<decltype(glXGetCurrentReadDrawable)>("glXGetCurrentReadDrawable");
	glXMakeContextCurrent = m_glx_module->bind<decltype(glXMakeContextCurrent)>("glXMakeContextCurrent");

	return eglInitialize && eglCreateContext && eglMakeCurrent;
}


//-------------------------------------------------
//  open_display - a render node, chosen by path,
//  or the default GPU without any window system
//-------------------------------------------------

bool libretro_gl::impl::open_display(const std::string &device, std::string &error)
{
	if (!device.empty())
	{
		if (!eglGetPlatformDisplayEXT || !eglQueryDevicesEXT || !eglQueryDeviceStringEXT)
		{
			error = "EGL device selection is not supported by this driver";
			return false;
		}

		EGLDeviceEXT devices[16];
		EGLint count = 0;
		eglQueryDevicesEXT(std::size(devices), devices, &count);
		for (EGLint i = 0; i < count && !m_display; i++)
		{
			for (EGLint name : { EGL_DRM_RENDER_NODE_FILE_EXT, EGL_DRM_DEVICE_FILE_EXT })
			{
				const char *const file = eglQueryDeviceStringEXT(devices[i], name);
				if (file && device == file)
				{
					m_display = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr);
					break;
				}
			}
		}
		if (!m_display)
		{
			error = util::string_format("no EGL device for %s", device);
			return false;
		}
	}
	else if (eglGetPlatformDisplayEXT)
	{
		m_display = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
	}

	if (!m_display)
		m_display = eglGetDisplay(nullptr);

	EGLint major = 0, minor = 0;
	if (!m_display || !eglInitialize(m_display, &major, &minor))
	{
		error = "can't initialize EGL";
		m_display = nullptr;
		return false;
	}
	return true;
}


//-------------------------------------------------
//  create_context - the version and profile
//  requested by the core
//-------------------------------------------------

bool libretro_gl::impl::create_context(const retro_hw_render_callback &callback, std::string &error)
{
	int major = callback.version_major;
	int minor = callback.version_minor;
	EGLint renderable = EGL_OPENGL_BIT;
	EGLint profile = 0;

	switch (callback.context_type)
	{
	case RETRO_HW_CONTEXT_OPENGL:
		profile = EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT;
		break;
	case RETRO_HW_CONTEXT_OPENGL_CORE:
		profile = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT;
		break;
	case RETRO_HW_CONTEXT_OPENGLES2:
		m_gles = true;
		major = 2;
		minor = 0;
		renderable = EGL_OPENGL_ES2_BIT;
		break;
	case RETRO_HW_CONTEXT_OPENGLES3:
		m_gles = true;
		major = 3;
		minor = 0;
		renderable = EGL_OPENGL_ES3_BIT;
		break;
	case RETRO_HW_CONTEXT_OPENGLES_VERSION:
		m_gles = true;
		renderable = (major >= 3) ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT;
		break;
	default:
		error = "unsupported context type";
		return false;
	}

	if (!eglBindAPI(m_gles ? EGL_OPENGL_ES_API : EGL_OPENGL_API))
	{
		error = m_gles ? "OpenGL ES is not supported" : "OpenGL is not supported";
		return false;
	}

	EGLint const config_attribs[] = { EGL_RENDERABLE_TYPE, renderable, EGL_NONE };
	EGLConfig config = nullptr;
	EGLint configs = 0;
	if (!eglChooseConfig(m_display, config_attribs, &config, 1, &configs) || !configs)
		config = nullptr; // EGL_KHR_no_config_context

	std::vector<EGLint> attribs;
	if (major)
	{
		attribs.insert(attribs.end(), { EGL_CONTEXT_MAJOR_VERSION, major, EGL_CONTEXT_MINOR_VERSION, minor });
	}
	if (profile && (major > 3 || (major == 3 && minor >= 2)))
		attribs.insert(attribs.end(), { EGL_CONTEXT_OPENGL_PROFILE_MASK, profile });
	if (callback.debug_context)
		attribs.insert(attribs.end(), { EGL_CONTEXT_OPENGL_DEBUG, 1 });
	attribs.push_back(EGL_NONE);

	m_context = eglCreateContext(m_display, config, nullptr, attribs.data());
	if (!m_context)
	{
		error = util::string_format("can't create an OpenGL%s %d.%d context", m_gles ? " ES" : "", major, minor);
		return false;
	}
	return true;
}


//-------------------------------------------------
//  load_gl - the functions used by the driver,
//  the core loads its own through proc_address
//-------------------------------------------------

bool libretro_gl::impl::load_gl(const std::string &readback, std::string &error)
{
	bind_gl(glGetString, "glGetString");
	bind_gl(glGetStringi, "glGetStringi");
	bind_gl(glGetIntegerv, "glGetIntegerv");
	bind_gl(glGenFramebuffers, "glGenFramebuffers");
	bind_gl(glDeleteFramebuffers, "glDeleteFramebuffers");
	bind_gl(glBindFramebuffer, "glBindFramebuffer");
	bind_gl(glFramebufferTexture2D, "glFramebufferTexture2D");
	bind_gl(glFramebufferRenderbuffer, "glFramebufferRenderbuffer");
	bind_gl(glCheckFramebufferStatus, "glCheckFramebufferStatus");
	bind_gl(glGenRenderbuffers, "glGenRenderbuffers");
	bind_gl(glDeleteRenderbuffers, "glDeleteRenderbuffers");
	bind_gl(glBindRenderbuffer, "glBindRenderbuffer");
	bind_gl(glRenderbufferStorage, "glRenderbufferStorage");
	bind_gl(glGenTextures, "glGenTextures");
	bind_gl(glDeleteTextures, "glDeleteTextures");
	bind_gl(glBindTexture, "glBindTexture");
	bind_gl(glTexImage2D, "glTexImage2D");
	bind_gl(glTexParameteri, "glTexParameteri");
	bind_gl(glBlitFramebuffer, "glBlitFramebuffer");
	bind_gl(glPixelStorei, "glPixelStorei");
	bind_gl(glReadPixels, "glReadPixels");
	bind_gl(glGenBuffers, "glGenBuffers");
	bind_gl(glDeleteBuffers, "glDeleteBuffers");
	bind_gl(glBindBuffer, "glBindBuffer");
	bind_gl(glBufferData, "glBufferData");
	bind_gl(glBufferStorage, "glBufferStorage");
	bind_gl(glMapBufferRange, "glMapBufferRange");
	bind_gl(glUnmapBuffer, "glUnmapBuffer");
	bind_gl(glFinish, "glFinish");

	if (!glGetString || !glGenFramebuffers || !glBindFramebuffer || !glFramebufferTexture2D || !glCheckFramebufferStatus || !glReadPixels || !glFinish)
	{
		error = "the OpenGL context has no framebuffer objects";
		return false;
	}

	// version, from "X.Y" or "OpenGL ES X.Y"
	const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
	if (version)
	{
		while (*version && !isdigit(u8(*version)))
			version++;
		int major = 0, minor = 0;
		sscanf(version, "%d.%d", &major, &minor);
		m_version = major * 10 + minor;
	}

	// the best readback available, all of them complete in this frame
	bool const buffer_objects = glGenBuffers && glBindBuffer && glMapBufferRange && glUnmapBuffer;
	bool const blit = glBlitFramebuffer != nullptr;
	bool const persistent = buffer_objects && glBufferStorage && blit && !m_gles && (m_version >= 44 || has_gl_extension("GL_ARB_buffer_storage"));
	bool const pbo = buffer_objects && (!m_gles || m_version >= 30);
	if (persistent && (readback == "auto" || readback == "persistent"))
		m_method = method::PERSISTENT;
	else if (pbo && (readback == "auto" || readback == "persistent" || readback == "pbo"))
		m_method = method::PBO;
	else
		m_method = method::DIRECT;

	// a method that isn't available falls back to the next one
	if (readback != "auto" && readback != "persistent" && readback != "pbo" && readback != "direct")
		osd_printf_warning("libretro: unknown readback method %s, using the best available\n", readback);

	static char const *const method_names[] = { "direct glReadPixels", "pixel buffer object", "persistent mapped buffer" };
	const char *const renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
	m_description = util::string_format("%s, OpenGL%s %d.%d, readback: %s",
			renderer ? renderer : "unknown renderer",
			m_gles ? " ES" : "", m_version / 10, m_version % 10,
			method_names[int(m_method)]);
	return true;
}

bool libretro_gl::impl::has_gl_extension(const char *name) const
{
	if (glGetStringi && glGetIntegerv && m_version >= 30)
	{
		GLint count = 0;
		glGetIntegerv(GL_NUM_EXTENSIONS, &count);
		for (GLint i = 0; i < count; i++)
		{
			const char *const extension = reinterpret_cast<const char *>(glGetStringi(GL_EXTENSIONS, i));
			if (extension && !strcmp(extension, name))
				return true;
		}
		return false;
	}

	const char *const extensions = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
	return extensions && strstr(extensions, name);
}


//-------------------------------------------------
//  create_framebuffer - the framebuffer object
//  the core renders to, and the readback buffer
//-------------------------------------------------

bool libretro_gl::impl::create_framebuffer(unsigned width, unsigned height, std::string &error)
{
	m_width = width;
	m_height = height;

	glGenFramebuffers(1, &m_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

	glGenTextures(1, &m_color);
	glBindTexture(GL_TEXTURE_2D, m_color);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_color, 0);

	if (m_depth_buffer)
	{
		glGenRenderbuffers(1, &m_depth_stencil);
		glBindRenderbuffer(GL_RENDERBUFFER, m_depth_stencil);
		if (m_stencil_buffer)
		{
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_depth_stencil);
		}
		else
		{
			glRenderbufferStorage(GL_RENDERBUFFER, m_gles && m_version < 30 ? GL_DEPTH_COMPONENT16 : GL_DEPTH_COMPONENT24, width, height);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depth_stencil);
		}
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
	}

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
	{
		error = "incomplete framebuffer object";
		return false;
	}

	// the flipped copy the persistent readback reads from
	if (m_method == method::PERSISTENT)
	{
		glGenFramebuffers(1, &m_flip_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, m_flip_fbo);
		glGenTextures(1, &m_flip_color);
		glBindTexture(GL_TEXTURE_2D, m_flip_color);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_flip_color, 0);
	}
	glBindTexture(GL_TEXTURE_2D, 0);

	size_t const size = size_t(width) * height * 4;
	if (m_method == method::PERSISTENT)
	{
		// cached system memory: the CPU reads it, the GPU writes it by DMA
		glGenBuffers(1, &m_pbo);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
		glBufferStorage(GL_PIXEL_PACK_BUFFER, size, nullptr, GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_CLIENT_STORAGE_BIT);
		m_mapped = reinterpret_cast<u32 *>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, size, GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT));
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		if (!m_mapped)
		{
			// fall back to a buffer mapped for every frame
			glDeleteBuffers(1, &m_pbo);
			m_pbo = 0;
			m_method = method::PBO;
			m_description += " (persistent mapping failed, using pixel buffer object)";
		}
	}
	if (m_method == method::PBO)
	{
		glGenBuffers(1, &m_pbo);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
		glBufferData(GL_PIXEL_PACK_BUFFER, size, nullptr, GL_STREAM_READ);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	}
	m_pixels.resize(size_t(width) * height);
	if (m_method == method::DIRECT)
		m_read.resize(size_t(width) * height);

	glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
	return true;
}

void libretro_gl::impl::destroy_framebuffer()
{
	if (m_pbo)
	{
		if (m_mapped)
		{
			glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		}
		glDeleteBuffers(1, &m_pbo);
	}
	if (m_depth_stencil)
		glDeleteRenderbuffers(1, &m_depth_stencil);
	if (m_color)
		glDeleteTextures(1, &m_color);
	if (m_flip_color)
		glDeleteTextures(1, &m_flip_color);
	if (m_fbo)
		glDeleteFramebuffers(1, &m_fbo);
	if (m_flip_fbo)
		glDeleteFramebuffers(1, &m_flip_fbo);

	m_pbo = m_depth_stencil = m_color = m_flip_color = m_fbo = m_flip_fbo = 0;
	m_mapped = nullptr;
}


//-------------------------------------------------
//  create / destroy
//-------------------------------------------------

bool libretro_gl::impl::create(const retro_hw_render_callback &callback, unsigned width, unsigned height, const std::string &device, const std::string &readback, std::string &error)
{
	m_depth_buffer = callback.depth;
	m_stencil_buffer = callback.stencil;

	if (!load_egl(error) || !open_display(device, error) || !create_context(callback, error))
		return false;

	make_current();
	bool const ok = load_gl(readback, error) && create_framebuffer(width, height, error);
	release();
	return ok;
}

void libretro_gl::impl::destroy()
{
	if (m_context)
	{
		make_current();
		destroy_framebuffer();
		release();
		eglDestroyContext(m_display, m_context);
		m_context = nullptr;
	}
	if (m_display)
	{
		eglTerminate(m_display);
		m_display = nullptr;
	}
}

bool libretro_gl::impl::resize(unsigned width, unsigned height, std::string &error)
{
	if (width <= m_width && height <= m_height)
		return true;

	// the framebuffer object keeps its name, cores ask for it every frame
	make_current();
	destroy_framebuffer();
	bool const ok = create_framebuffer(std::max(width, m_width), std::max(height, m_height), error);
	release();
	return ok;
}


//-------------------------------------------------
//  make_current / release - a thread has a single
//  current context, whatever the API; the one of
//  the MAME OpenGL renderer is restored afterwards
//-------------------------------------------------

void libretro_gl::impl::make_current()
{
	if (m_current_depth++)
		return;

	m_prev_api = eglQueryAPI ? eglQueryAPI() : 0;
	m_prev_context = eglGetCurrentContext ? eglGetCurrentContext() : nullptr;
	if (m_prev_context)
	{
		m_prev_display = eglGetCurrentDisplay();
		m_prev_draw = eglGetCurrentSurface(0x3059); // EGL_DRAW
		m_prev_read = eglGetCurrentSurface(0x305A); // EGL_READ
	}

	m_prev_glx_context = glXGetCurrentContext ? glXGetCurrentContext() : nullptr;
	if (m_prev_glx_context)
	{
		m_prev_glx_display = glXGetCurrentDisplay();
		m_prev_glx_draw = glXGetCurrentDrawable();
		m_prev_glx_read = glXGetCurrentReadDrawable();
		glXMakeContextCurrent(m_prev_glx_display, 0, 0, nullptr);
	}

	eglBindAPI(m_gles ? EGL_OPENGL_ES_API : EGL_OPENGL_API);
	eglMakeCurrent(m_display, nullptr, nullptr, m_context);
}

void libretro_gl::impl::release()
{
	if (--m_current_depth)
		return;

	eglMakeCurrent(m_display, nullptr, nullptr, nullptr);

	if (m_prev_context)
	{
		if (m_prev_api)
			eglBindAPI(m_prev_api);
		eglMakeCurrent(m_prev_display, m_prev_draw, m_prev_read, m_prev_context);
	}
	if (m_prev_glx_context)
		glXMakeContextCurrent(m_prev_glx_display, m_prev_glx_draw, m_prev_glx_read, m_prev_glx_context);

	m_prev_context = m_prev_glx_context = nullptr;
}

retro_proc_address_t libretro_gl::impl::proc_address(const char *symbol) const
{
	return eglGetProcAddress ? reinterpret_cast<retro_proc_address_t>(eglGetProcAddress(symbol)) : nullptr;
}


//-------------------------------------------------
//  readback - wait for the frame in this frame,
//  so no frame of latency is added
//-------------------------------------------------

const bitmap_rgb32 &libretro_gl::impl::readback(unsigned width, unsigned height, bool flip)
{
	width = std::min(width, m_width);
	height = std::min(height, m_height);

	osd_ticks_t const start = osd_ticks();
	u32 const *pixels = nullptr;
	bool cpu_flip = flip;

	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	if (!m_gles || m_version >= 30)
		glPixelStorei(GL_PACK_ROW_LENGTH, 0);

	GLenum const format = m_gles ? GL_RGBA : GL_BGRA;
	GLenum const type = m_gles ? GL_UNSIGNED_BYTE : GL_UNSIGNED_INT_8_8_8_8_REV;

	if (m_method == method::DIRECT)
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
		glReadPixels(0, 0, width, height, format, type, m_read.data());
		pixels = m_read.data();
	}
	else
	{
		// the bitmap points into the buffer: flip on the GPU
		GLuint source = m_fbo;
		if (m_method == method::PERSISTENT && flip)
		{
			glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_flip_fbo);
			glBlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			source = m_flip_fbo;
			cpu_flip = false;
		}

		glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
		glReadPixels(0, 0, width, height, format, type, nullptr);

		glFinish();

		if (m_method == method::PERSISTENT)
			pixels = m_mapped;
		else
			pixels = reinterpret_cast<const u32 *>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, size_t(width) * height * 4, GL_MAP_READ_BIT));
	}
	osd_ticks_t const waited = osd_ticks();

	if (m_method == method::PERSISTENT)
	{
		// no copy at all
		m_bitmap.wrap(m_mapped, width, height, width);
	}
	else if (pixels)
	{
		// copy, flip and swap the components for OpenGL ES
		for (unsigned y = 0; y < height; y++)
		{
			u32 const *const src = pixels + size_t(cpu_flip ? (height - 1 - y) : y) * width;
			u32 *const dst = &m_pixels[size_t(y) * width];
			if (!m_gles)
				std::memcpy(dst, src, width * 4);
			else
			{
				for (unsigned x = 0; x < width; x++)
				{
					u8 const *const c = reinterpret_cast<u8 const *>(&src[x]);
					dst[x] = rgb_t(c[0], c[1], c[2]);
				}
			}
		}
		m_bitmap.wrap(m_pixels.data(), width, height, width);
	}

	if (m_method == method::PBO)
		glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	if (m_method != method::DIRECT)
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

	osd_ticks_t const end = osd_ticks();
	m_wait_total += waited - start;
	m_wait_max = std::max(m_wait_max, waited - start);
	m_copy_total += end - waited;
	if (++m_frames == 600)
		log_statistics();

	return m_bitmap;
}

void libretro_gl::impl::log_statistics()
{
	double const ms = 1000.0 / double(osd_ticks_per_second());
	osd_printf_verbose("libretro: GL readback %ux%u over %u frames: wait avg %.3f ms max %.3f ms, copy avg %.3f ms\n",
			m_bitmap.width(), m_bitmap.height(), m_frames,
			m_wait_total * ms / m_frames, m_wait_max * ms, m_copy_total * ms / m_frames);
	m_frames = 0;
	m_wait_total = m_wait_max = m_copy_total = 0;
}


//**************************************************************************
//  PUBLIC INTERFACE
//**************************************************************************

bool libretro_gl::supports(retro_hw_context_type type)
{
	switch (type)
	{
	case RETRO_HW_CONTEXT_OPENGL:
	case RETRO_HW_CONTEXT_OPENGL_CORE:
	case RETRO_HW_CONTEXT_OPENGLES2:
	case RETRO_HW_CONTEXT_OPENGLES3:
	case RETRO_HW_CONTEXT_OPENGLES_VERSION:
		return true;
	default:
		return false;
	}
}

bool libretro_gl::create(const retro_hw_render_callback &callback, unsigned width, unsigned height, const std::string &device, const std::string &readback)
{
	m_impl = std::make_unique<impl>();
	if (m_impl->create(callback, width, height, device, readback, m_error))
		return true;
	m_impl.reset();
	return false;
}

void libretro_gl::make_current() { m_impl->make_current(); }
void libretro_gl::release() { m_impl->release(); }
uintptr_t libretro_gl::framebuffer() const { return m_impl ? m_impl->m_fbo : 0; }
retro_proc_address_t libretro_gl::proc_address(const char *symbol) const { return m_impl ? m_impl->proc_address(symbol) : nullptr; }
const char *libretro_gl::description() const { return m_impl ? m_impl->m_description.c_str() : ""; }
const bitmap_rgb32 &libretro_gl::readback(unsigned width, unsigned height, bool flip) { return m_impl->readback(width, height, flip); }
bool libretro_gl::resize(unsigned width, unsigned height) { return m_impl && m_impl->resize(width, height, m_error); }

#else // no EGL: Windows and macOS are not supported yet

class libretro_gl::impl { };

bool libretro_gl::supports(retro_hw_context_type type) { return false; }
bool libretro_gl::create(const retro_hw_render_callback &callback, unsigned width, unsigned height, const std::string &device, const std::string &readback) { m_error = "not supported on this platform yet"; return false; }
void libretro_gl::make_current() { }
void libretro_gl::release() { }
uintptr_t libretro_gl::framebuffer() const { return 0; }
retro_proc_address_t libretro_gl::proc_address(const char *symbol) const { return nullptr; }
const char *libretro_gl::description() const { return ""; }
const bitmap_rgb32 &libretro_gl::readback(unsigned width, unsigned height, bool flip) { static bitmap_rgb32 empty; return empty; }
bool libretro_gl::resize(unsigned width, unsigned height) { return false; }

#endif

libretro_gl::libretro_gl() { }
libretro_gl::~libretro_gl() { }
void libretro_gl::destroy() { m_impl.reset(); }
