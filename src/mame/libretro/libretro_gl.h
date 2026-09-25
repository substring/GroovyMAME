// license:BSD-3-Clause
// copyright-holders:Substring
/***************************************************************************

    libretro_gl.h - OpenGL context for hardware rendered libretro cores

    The context belongs to the libretro driver, not to the MAME renderer:
    the core renders into a framebuffer object that is read back into a
    bitmap, so hardware rendered cores work with every video backend,
    including software ones (kmsraw) and remote ones (MiSTer).

***************************************************************************/
#ifndef MAME_LIBRETRO_LIBRETRO_GL_H
#define MAME_LIBRETRO_LIBRETRO_GL_H

#pragma once

#include "libretro/libretro.h"

#include <memory>
#include <string>


class libretro_gl
{
public:
	libretro_gl();
	~libretro_gl();

	// whether a context of this type can be created on this platform
	static bool supports(retro_hw_context_type type);

	// create the context and a framebuffer of the given size; device is
	// a DRM render node (/dev/dri/renderD128), empty for the default GPU;
	// readback is auto, persistent, pbo or direct
	bool create(const retro_hw_render_callback &callback, unsigned width, unsigned height, const std::string &device, const std::string &readback);
	void destroy();
	const std::string &error() const { return m_error; }
	const char *description() const;

	// grow the framebuffer for a larger maximum geometry
	bool resize(unsigned width, unsigned height);

	// make the context current around calls to the core, restoring the
	// one of the MAME renderer afterwards; calls can be nested
	void make_current();
	void release();

	uintptr_t framebuffer() const;
	retro_proc_address_t proc_address(const char *symbol) const;

	// read back the frame the core just rendered, flipped to top-left
	// origin when needed; the bitmap stays valid until the next call
	const bitmap_rgb32 &readback(unsigned width, unsigned height, bool flip);

private:
	class impl;
	std::unique_ptr<impl> m_impl;
	std::string m_error;
};


// makes the context current for the lifetime of the object
class libretro_gl_scope
{
public:
	libretro_gl_scope(libretro_gl *gl) : m_gl(gl) { if (m_gl) m_gl->make_current(); }
	~libretro_gl_scope() { if (m_gl) m_gl->release(); }

private:
	libretro_gl *const m_gl;
};

#endif // MAME_LIBRETRO_LIBRETRO_GL_H
