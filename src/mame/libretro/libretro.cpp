// license:BSD-3-Clause
// copyright-holders:Substring
/***************************************************************************

    libretro.cpp - runs a libretro core as an emulated system

    The core runs one retro_run per emulated frame, so GroovyMAME's
    switchres, frame delay and vsync handling apply to it like to any
    other system.

    Software rendered cores hand a CPU framebuffer to retro_video_refresh.
    Hardware rendered OpenGL cores get an OpenGL context of their own
    (libretro_gl.cpp) and their frame is read back in the same frame, so
    both kinds work with every video backend, including kmsraw and
    MiSTer, and no context is shared with the MAME renderer. Vulkan and
    Direct3D cores aren't supported yet, nor hardware rendering on
    Windows and macOS.

    Usage:
      groovymame libretro -L genesis_plus_gx -cart sonic.md

    -L (-libretro_core) accepts a full path, a file name or a core name,
    the last two being searched in libretropath. A core name gets the
    platform suffix appended (genesis_plus_gx -> genesis_plus_gx_libretro.so).
    The content is loaded from the cartridge slot (-cart), like for other
    consoles, so it can also be picked from the file manager.

    Directories:
      -libretropath           search path for the cores
      -libretro_system_directory
                              system directory given to the core (BIOS)
      -libretro_gl_device     GPU rendering hardware rendered cores, as a
                              DRM render node (/dev/dri/renderD128)
      -libretro_gl_readback   auto, persistent, pbo or direct, to compare
                              the readback methods (timings with -verbose)
      -cfg_directory          core options, in libretro/<core name>.opt
                              (RetroArch format: key = "value"), written
                              when they are changed in the Core Options
                              menu; <system directory>/<core name>.opt is
                              also read, before it
      -nvram_directory        save directory given to the core, and
                              battery save RAM (libretro/<content>.srm)

    The screen is reconfigured to the geometry and refresh rate reported
    by the core. Inputs are 4 RetroPads with a left analog stick.

***************************************************************************/

#include "emu.h"

#include "emuopts.h"
#include "fileio.h"
#include "screen.h"
#include "speaker.h"

#include "imagedev/cartrom.h"

#include "corestr.h"
#include "path.h"
#include "unzip.h"

#include "modules/lib/osdlib.h"

#include "libretro/libretro.h"
#include "libretro_gl.h"

#include <algorithm>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>


//**************************************************************************
//  SOUND DEVICE
//**************************************************************************

// queues the samples produced by the core for one frame and feeds them
// to the MAME mixer at the core sample rate
class libretro_sound_device : public device_t, public device_sound_interface
{
public:
	libretro_sound_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock);

	void set_rate(u32 rate);
	void push(const int16_t *data, size_t frames);

protected:
	virtual void device_start() override ATTR_COLD;
	virtual void sound_stream_update(sound_stream &stream) override;

private:
	sound_stream        *m_stream;
	std::vector<int16_t> m_fifo;        // interleaved stereo
	size_t               m_read;
	int16_t              m_last[2];
	u32                  m_rate;
};

DECLARE_DEVICE_TYPE(LIBRETRO_SOUND, libretro_sound_device)
DECLARE_DEVICE_TYPE(LIBRETRO_CART, libretro_cart_device)

libretro_sound_device::libretro_sound_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
	: device_t(mconfig, LIBRETRO_SOUND, tag, owner, clock)
	, device_sound_interface(mconfig, *this)
	, m_stream(nullptr)
	, m_read(0)
	, m_last{ 0, 0 }
	, m_rate(48000)
{
}

void libretro_sound_device::device_start()
{
	m_stream = stream_alloc(0, 2, m_rate);
}

void libretro_sound_device::set_rate(u32 rate)
{
	if (!rate || rate == m_rate)
		return;

	m_rate = rate;
	if (m_stream)
		m_stream->set_sample_rate(rate);
}

void libretro_sound_device::push(const int16_t *data, size_t frames)
{
	m_stream->update();
	m_fifo.insert(m_fifo.end(), data, data + frames * 2);

	// keep at most a fifth of a second queued, dropping the oldest samples
	size_t const limit = size_t(m_rate / 5) * 2;
	if (m_fifo.size() - m_read > limit)
		m_read = m_fifo.size() - limit;
}

void libretro_sound_device::sound_stream_update(sound_stream &stream)
{
	for (int i = 0; i < stream.samples(); i++)
	{
		// hold the last sample on underrun to avoid clicks
		if (m_read + 2 <= m_fifo.size())
		{
			m_last[0] = m_fifo[m_read++];
			m_last[1] = m_fifo[m_read++];
		}
		stream.put_int(0, i, m_last[0], 32768);
		stream.put_int(1, i, m_last[1], 32768);
	}

	if (m_read >= m_fifo.size() / 2)
	{
		m_fifo.erase(m_fifo.begin(), m_fifo.begin() + m_read);
		m_read = 0;
	}
}

DEFINE_DEVICE_TYPE(LIBRETRO_SOUND, libretro_sound_device, "libretro_sound", "libretro core audio")


//**************************************************************************
//  CARTRIDGE SLOT
//**************************************************************************

// holds the content for the core: the file stays open and is handed to
// the core when it starts, as a path or in memory depending on the core
class libretro_cart_device : public device_t, public device_cartrom_image_interface
{
public:
	libretro_cart_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
		: device_t(mconfig, LIBRETRO_CART, tag, owner, clock)
		, device_cartrom_image_interface(mconfig, *this)
		, m_extensions("bin")
	{
	}

	// the extensions supported by the core, as reported by it ("md|bin")
	void set_extensions(const char *extensions)
	{
		if (extensions && *extensions)
		{
			m_extensions = extensions;
			std::replace(m_extensions.begin(), m_extensions.end(), '|', ',');
		}
	}

	virtual bool is_reset_on_load() const noexcept override { return true; }
	virtual const char *file_extensions() const noexcept override { return m_extensions.c_str(); }

protected:
	virtual void device_start() override ATTR_COLD { }

private:
	std::string m_extensions;
};

DEFINE_DEVICE_TYPE(LIBRETRO_CART, libretro_cart_device, "libretro_cart", "libretro content")


namespace {

//**************************************************************************
//  CONSTANTS
//**************************************************************************

constexpr unsigned MAX_PADS = 4;


//**************************************************************************
//  HELPERS
//**************************************************************************

// extensions reported by cores are like "md|bin|zip"
bool accepts_extension(const char *extensions, const std::string &extension)
{
	return extensions && !extension.empty() && (std::string("|") + strmakelower(extensions) + "|").find("|" + extension + "|") != std::string::npos;
}


//**************************************************************************
//  DRIVER STATE
//**************************************************************************

class libretro_state : public driver_device, public runtime_option_provider
{
public:
	libretro_state(const machine_config &mconfig, device_type type, const char *tag)
		: driver_device(mconfig, type, tag)
		, m_screen(*this, "screen")
		, m_sound(*this, "retro_audio")
		, m_cart(*this, "cartslot")
		, m_pads(*this, "PAD%u", 1U)
		, m_stick_x(*this, "STICKX%u", 1U)
		, m_stick_y(*this, "STICKY%u", 1U)
	{
	}

	void libretro(machine_config &config) ATTR_COLD;

protected:
	virtual void machine_start() override ATTR_COLD;
	virtual void machine_reset() override ATTR_COLD;

private:
	// entry points of the core
	struct core_api
	{
		void     (*init)();
		void     (*deinit)();
		unsigned (*api_version)();
		void     (*get_system_info)(retro_system_info *);
		void     (*get_system_av_info)(retro_system_av_info *);
		void     (*set_environment)(retro_environment_t);
		void     (*set_video_refresh)(retro_video_refresh_t);
		void     (*set_audio_sample)(retro_audio_sample_t);
		void     (*set_audio_sample_batch)(retro_audio_sample_batch_t);
		void     (*set_input_poll)(retro_input_poll_t);
		void     (*set_input_state)(retro_input_state_t);
		void     (*set_controller_port_device)(unsigned, unsigned);
		void     (*reset)();
		void     (*run)();
		size_t   (*serialize_size)();
		bool     (*serialize)(void *, size_t);
		bool     (*unserialize)(const void *, size_t);
		bool     (*load_game)(const retro_game_info *);
		void     (*unload_game)();
		void *   (*get_memory_data)(unsigned);
		size_t   (*get_memory_size)(unsigned);
	};

	// libretro callbacks carry no context
	static libretro_state *s_instance;

	static bool env_callback(unsigned cmd, void *data) { return s_instance->environment(cmd, data); }
	static void video_callback(const void *data, unsigned width, unsigned height, size_t pitch) { s_instance->video_refresh(data, width, height, pitch); }
	static uintptr_t hw_framebuffer_callback() { return s_instance->m_gl ? s_instance->m_gl->framebuffer() : 0; }
	static retro_proc_address_t hw_proc_address_callback(const char *symbol) { return s_instance->m_gl ? s_instance->m_gl->proc_address(symbol) : nullptr; }
	static void audio_callback(int16_t left, int16_t right) { int16_t const frame[2] = { left, right }; s_instance->m_sound->push(frame, 1); }
	static size_t audio_batch_callback(const int16_t *data, size_t frames) { s_instance->m_sound->push(data, frames); return frames; }
	static void input_poll_callback() { }
	static int16_t input_state_callback(unsigned port, unsigned device, unsigned index, unsigned id) { return s_instance->input_state(port, device, index, id); }
	static void log_callback(retro_log_level level, const char *fmt, ...) ATTR_PRINTF(2, 3);

	bool environment(unsigned cmd, void *data);
	void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch);
	int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id);

	std::string find_core(const std::string &name) const;
	void load_core();
	void load_content();
	void extract_content(const std::string &archive, const char *extensions, bool to_file);
	void apply_geometry(const retro_game_geometry &geometry);
	void apply_av_info(const retro_system_av_info &info);
	void configure_screen(int width, int height);
	// runtime_option_provider implementation
	virtual const std::vector<category> &runtime_option_categories() const override { return m_option_categories; }
	virtual const std::vector<option> &runtime_options() const override { return m_options; }
	virtual void set_runtime_option(std::string_view key, std::string_view value) override;

	option *find_option(std::string_view key);
	void define_option(const char *key, const char *label, const char *info, const char *category, const retro_core_option_value *values, const char *default_value);
	void define_variables(const retro_variable *variables);
	void apply_option_value(option &opt);
	void parse_options(std::istream &stream);
	void load_options();
	void save_options();
	void load_save_ram();
	void save_save_ram();
	void machine_exit();
	void state_presave();
	void state_postload();

	void vblank(int state);
	u32 screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);

	required_device<screen_device> m_screen;
	required_device<libretro_sound_device> m_sound;
	required_device<libretro_cart_device> m_cart;
	required_ioport_array<MAX_PADS> m_pads;
	required_ioport_array<MAX_PADS> m_stick_x;
	required_ioport_array<MAX_PADS> m_stick_y;

	osd::dynamic_module::ptr m_module;
	core_api                 m_api;
	bool                     m_loaded = false;
	bool                     m_started = false;

	std::string              m_core_path;
	std::string              m_content_name;
	std::string              m_system_dir;
	std::string              m_save_dir;
	std::string              m_library_name;
	std::vector<u8>          m_content;
	std::string              m_temp_content;           // file extracted from an archive, deleted on exit
	bool                     m_support_no_game = false;

	retro_pixel_format       m_pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
	bitmap_rgb32             m_frame;
	const bitmap_rgb32      *m_display = nullptr;      // frame to show, software or read back

	// hardware rendering
	bool                     m_hw_requested = false;
	retro_hw_render_callback m_hw = { };
	std::unique_ptr<libretro_gl> m_gl;
	int                      m_width = 0;
	int                      m_height = 0;
	double                   m_fps = 60.0;

	std::vector<category>    m_option_categories;
	std::vector<option>      m_options;
	std::map<std::string, std::string> m_option_values;  // chosen by the user, from files or the menu
	bool                     m_options_updated = false;
	retro_core_options_update_display_callback_t m_update_display = nullptr;

	std::vector<u8>          m_state;
};

libretro_state *libretro_state::s_instance = nullptr;


//**************************************************************************
//  CORE LOADING
//**************************************************************************

// -L accepts a path, a file name or a core name searched in libretropath
std::string libretro_state::find_core(const std::string &name) const
{
	if (name.find_first_of("/\\") != std::string::npos)
		return name;

#if defined(_WIN32)
	char const *const suffix = "_libretro.dll";
#elif defined(__APPLE__)
	char const *const suffix = "_libretro.dylib";
#else
	char const *const suffix = "_libretro.so";
#endif

	std::vector<std::string> candidates{ name };
	if (!name.ends_with(suffix))
		candidates.emplace_back(name + suffix);

	path_iterator path(machine().options().libretro_path());
	std::string dir;
	while (path.next(dir))
	{
		for (const std::string &candidate : candidates)
		{
			std::string const full = dir.empty() ? candidate : dir + PATH_SEPARATOR + candidate;
			if (osd_stat(full))
				return full;
		}
	}

	throw emu_fatalerror("libretro: core %s not found in libretropath (%s)\n", name, machine().options().libretro_path());
}

void libretro_state::load_core()
{
	m_core_path = find_core(m_core_path);
	osd_printf_verbose("libretro: loading core %s\n", m_core_path);
	m_module = osd::dynamic_module::open({ m_core_path });

#define BIND(member, symbol) \
	m_api.member = m_module->bind<decltype(m_api.member)>(symbol); \
	if (!m_api.member) \
		throw emu_fatalerror("libretro: %s is not a libretro core (missing %s)\n", m_core_path, symbol);

	BIND(init,                       "retro_init")
	BIND(deinit,                     "retro_deinit")
	BIND(api_version,                "retro_api_version")
	BIND(get_system_info,            "retro_get_system_info")
	BIND(get_system_av_info,         "retro_get_system_av_info")
	BIND(set_environment,            "retro_set_environment")
	BIND(set_video_refresh,          "retro_set_video_refresh")
	BIND(set_audio_sample,           "retro_set_audio_sample")
	BIND(set_audio_sample_batch,     "retro_set_audio_sample_batch")
	BIND(set_input_poll,             "retro_set_input_poll")
	BIND(set_input_state,            "retro_set_input_state")
	BIND(set_controller_port_device, "retro_set_controller_port_device")
	BIND(reset,                      "retro_reset")
	BIND(run,                        "retro_run")
	BIND(serialize_size,             "retro_serialize_size")
	BIND(serialize,                  "retro_serialize")
	BIND(unserialize,                "retro_unserialize")
	BIND(load_game,                  "retro_load_game")
	BIND(unload_game,                "retro_unload_game")
	BIND(get_memory_data,            "retro_get_memory_data")
	BIND(get_memory_size,            "retro_get_memory_size")
#undef BIND

	if (m_api.api_version() != RETRO_API_VERSION)
		throw emu_fatalerror("libretro: %s uses API version %u, %u expected\n", m_core_path, m_api.api_version(), RETRO_API_VERSION);
}


//**************************************************************************
//  CONTENT
//**************************************************************************

void libretro_state::load_content()
{
	retro_system_info info = { };
	m_api.get_system_info(&info);
	m_library_name = info.library_name ? info.library_name : "libretro";
	osd_printf_info("libretro: %s %s\n", m_library_name, info.library_version ? info.library_version : "");

	// show the core instead of this driver in the system information
	std::string description = m_library_name;
	if (info.library_version && *info.library_version)
		description.append(" ").append(info.library_version);
	machine().set_system_description(std::move(description), "libretro core", "");

	// core options are known by now, apply the user settings
	load_options();
	m_cart->set_extensions(info.valid_extensions);

	if (!m_cart->exists())
	{
		if (!m_support_no_game)
			throw emu_fatalerror("libretro: %s needs content, use -cart\n", m_library_name);

		m_content_name = m_library_name;
		if (!m_api.load_game(nullptr))
			throw emu_fatalerror("libretro: %s failed to start without content\n", m_library_name);
		return;
	}

	// the file given with -cart may be an archive: cores that don't open
	// archives themselves get the first file of it they accept, like RetroArch
	// does, in memory or extracted to a temporary file if they need a path
	std::string const source = machine().options().image_option(m_cart->instance_name()).value();
	std::string const extension = strmakelower(core_filename_extract_extension(source, true));
	bool const archive = (extension == "zip") || (extension == "7z");
	bool const core_opens_archive = info.block_extract || accepts_extension(info.valid_extensions, extension);

	std::string path;
	if (archive && !core_opens_archive)
	{
		extract_content(source, info.valid_extensions, info.need_fullpath);
		path = info.need_fullpath ? m_temp_content : source + "#" + m_content_name;
		size_t const dot = m_content_name.find_last_of('.');
		if (dot != std::string::npos)
			m_content_name.erase(dot);
	}
	else if (archive)
	{
		path = source;
		m_content_name = std::string(core_filename_extract_base(source, true));
		if (!info.need_fullpath)
		{
			std::ifstream file(source, std::ios::binary);
			m_content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		}
	}
	else
	{
		path = m_cart->filename();
		m_content_name = m_cart->basename_noext();
		if (!info.need_fullpath)
		{
			m_content.resize(m_cart->length());
			m_cart->fseek(0, SEEK_SET);
			if (m_cart->fread(m_content.data(), m_content.size()) != m_content.size())
				throw emu_fatalerror("libretro: can't read %s\n", path);
		}
	}

	retro_game_info game = { };
	game.path = path.c_str();
	if (!info.need_fullpath)
	{
		game.data = m_content.data();
		game.size = m_content.size();
	}

	if (!m_api.load_game(&game))
		throw emu_fatalerror("libretro: %s failed to load %s\n", m_library_name, path);
}

// m_content_name gets the name of the extracted file
void libretro_state::extract_content(const std::string &archive, const char *extensions, bool to_file)
{
	util::archive_file::ptr file;
	std::error_condition const err = (strmakelower(core_filename_extract_extension(archive, true)) == "7z") ? util::archive_file::open_7z(archive, file) : util::archive_file::open_zip(archive, file);
	if (err || !file)
		throw emu_fatalerror("libretro: can't open %s\n", archive);

	int found = -1;
	for (int i = file->first_file(); i >= 0 && found < 0; i = file->next_file())
	{
		if (!file->current_is_directory() && (!extensions || !*extensions || accepts_extension(extensions, strmakelower(core_filename_extract_extension(file->current_name(), true)))))
			found = i;
	}
	if (found < 0)
		throw emu_fatalerror("libretro: no file %s accepts in %s\n", m_library_name, archive);

	m_content_name = std::string(core_filename_extract_base(file->current_name()));
	m_content.resize(file->current_uncompressed_length());
	if (file->decompress(m_content.data(), m_content.size()))
		throw emu_fatalerror("libretro: can't extract %s from %s\n", m_content_name, archive);

	if (to_file)
	{
		std::error_code ec;
		std::filesystem::path const dir = std::filesystem::temp_directory_path(ec) / "groovymame-libretro";
		std::filesystem::create_directories(dir, ec);
		m_temp_content = (dir / m_content_name).string();
		std::ofstream out(m_temp_content, std::ios::binary);
		out.write(reinterpret_cast<const char *>(m_content.data()), m_content.size());
		if (!out)
			throw emu_fatalerror("libretro: can't write %s\n", m_temp_content);
		m_content.clear();
	}
}


//**************************************************************************
//  CORE OPTIONS
//**************************************************************************

runtime_option_provider::option *libretro_state::find_option(std::string_view key)
{
	auto const found = std::find_if(m_options.begin(), m_options.end(), [key] (const option &opt) { return opt.key == key; });
	return (found != m_options.end()) ? &*found : nullptr;
}

// a user value is kept only if the core accepts it
void libretro_state::apply_option_value(option &opt)
{
	auto const found = m_option_values.find(opt.key);
	bool const valid = (found != m_option_values.end()) && (opt.values.empty() || std::any_of(
			opt.values.begin(), opt.values.end(),
			[&found] (const std::pair<std::string, std::string> &value) { return value.first == found->second; }));
	opt.value = valid ? found->second : opt.default_value;
}

void libretro_state::define_option(const char *key, const char *label, const char *info, const char *category, const retro_core_option_value *values, const char *default_value)
{
	if (!key)
		return;

	option opt;
	opt.key = key;
	opt.label = (label && *label) ? label : key;
	opt.info = info ? info : "";
	opt.category = category ? category : "";
	for (int i = 0; values && (i < RETRO_NUM_CORE_OPTION_VALUES_MAX) && values[i].value; i++)
		opt.values.emplace_back(values[i].value, values[i].label ? values[i].label : values[i].value);
	opt.default_value = default_value ? default_value : opt.values.empty() ? "" : opt.values.front().first;
	apply_option_value(opt);

	if (option *const existing = find_option(opt.key))
		*existing = std::move(opt);
	else
		m_options.emplace_back(std::move(opt));
}

// legacy definitions: "Description; default|other|..."
void libretro_state::define_variables(const retro_variable *variables)
{
	for (auto *var = variables; var && var->key; var++)
	{
		std::string const desc = var->value ? var->value : "";
		size_t const split = desc.find("; ");
		if (split == std::string::npos)
			continue;

		std::vector<std::string> names;
		for (size_t start = split + 2; start <= desc.size(); )
		{
			size_t const end = std::min(desc.find('|', start), desc.size());
			names.emplace_back(desc.substr(start, end - start));
			start = end + 1;
		}

		std::vector<retro_core_option_value> values;
		for (const std::string &name : names)
			values.push_back({ name.c_str(), nullptr });
		values.push_back({ nullptr, nullptr });
		define_option(var->key, desc.substr(0, split).c_str(), nullptr, nullptr, values.data(), nullptr);
	}
}

void libretro_state::set_runtime_option(std::string_view key, std::string_view value)
{
	option *const opt = find_option(key);
	if (!opt)
		return;

	m_option_values[opt->key] = value;
	apply_option_value(*opt);
	m_options_updated = true;
	save_options();

	// let the core show or hide the options depending on this one
	if (m_update_display)
		m_update_display();
}

// RetroArch format: key = "value"
void libretro_state::parse_options(std::istream &stream)
{
	std::string line;
	while (std::getline(stream, line))
	{
		size_t const equal = line.find('=');
		if (equal == std::string::npos)
			continue;
		size_t const open = line.find('"', equal);
		size_t const close = line.rfind('"');
		if (open == std::string::npos || close <= open)
			continue;

		std::string key = line.substr(0, equal);
		key.erase(key.find_last_not_of(" \t") + 1);
		key.erase(0, key.find_first_not_of(" \t"));
		m_option_values[key] = line.substr(open + 1, close - open - 1);
	}
}

// <system>/<core name>.opt, then <cfg>/libretro/<core name>.opt
void libretro_state::load_options()
{
	std::ifstream legacy(m_system_dir + PATH_SEPARATOR + m_library_name + ".opt");
	if (legacy)
		parse_options(legacy);

	emu_file file(machine().options().cfg_directory(), OPEN_FLAG_READ);
	if (!file.open(std::string("libretro" PATH_SEPARATOR) + m_library_name + ".opt"))
	{
		std::string contents(file.size(), '\0');
		file.read(contents.data(), contents.size());
		std::istringstream stream(contents);
		parse_options(stream);
		osd_printf_verbose("libretro: core options read from %s\n", file.fullpath());
	}

	for (option &opt : m_options)
		apply_option_value(opt);
	m_options_updated = true;
}

void libretro_state::save_options()
{
	emu_file file(machine().options().cfg_directory(), OPEN_FLAG_WRITE | OPEN_FLAG_CREATE | OPEN_FLAG_CREATE_PATHS);
	if (file.open(std::string("libretro" PATH_SEPARATOR) + m_library_name + ".opt"))
	{
		osd_printf_error("libretro: can't save the core options in %s\n", machine().options().cfg_directory());
		return;
	}

	// options of the core, then values of options it doesn't declare (yet)
	for (const option &opt : m_options)
		file.puts(util::string_format("%s = \"%s\"\n", opt.key, opt.value));
	for (const auto &[key, value] : m_option_values)
		if (!find_option(key))
			file.puts(util::string_format("%s = \"%s\"\n", key, value));
}


//**************************************************************************
//  ENVIRONMENT
//**************************************************************************

void libretro_state::log_callback(retro_log_level level, const char *fmt, ...)
{
	char buffer[2048];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	switch (level)
	{
	case RETRO_LOG_ERROR: osd_printf_error("libretro: %s", buffer); break;
	case RETRO_LOG_WARN:  osd_printf_warning("libretro: %s", buffer); break;
	case RETRO_LOG_INFO:  osd_printf_verbose("libretro: %s", buffer); break;
	default:              s_instance->logerror("%s", buffer); break;
	}
}

bool libretro_state::environment(unsigned cmd, void *data)
{
	switch (cmd)
	{
	// video
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
	{
		retro_pixel_format const format = *reinterpret_cast<const retro_pixel_format *>(data);
		if (format != RETRO_PIXEL_FORMAT_0RGB1555 && format != RETRO_PIXEL_FORMAT_XRGB8888 && format != RETRO_PIXEL_FORMAT_RGB565)
			return false;
		m_pixel_format = format;
		return true;
	}

	case RETRO_ENVIRONMENT_SET_HW_RENDER:
	{
		auto *hw = reinterpret_cast<retro_hw_render_callback *>(data);
		if (!libretro_gl::supports(hw->context_type))
		{
			osd_printf_error("libretro: the core asks for a hardware rendering context that isn't supported (type %u)\n", unsigned(hw->context_type));
			return false;
		}
		hw->get_current_framebuffer = hw_framebuffer_callback;
		hw->get_proc_address = hw_proc_address_callback;
		m_hw = *hw;
		m_hw_requested = true;
		return true;
	}

	case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
		if (!libretro_gl::supports(RETRO_HW_CONTEXT_OPENGL))
			return false;
		*reinterpret_cast<unsigned *>(data) = RETRO_HW_CONTEXT_OPENGL;
		return true;

	case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
		// the context is never shared with anything else anyway
		return true;

	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*reinterpret_cast<bool *>(data) = true;
		return true;

	case RETRO_ENVIRONMENT_GET_OVERSCAN:
		// a CRT shows the overscan area
		*reinterpret_cast<bool *>(data) = true;
		return true;

	case RETRO_ENVIRONMENT_SET_GEOMETRY:
		apply_geometry(*reinterpret_cast<const retro_game_geometry *>(data));
		return true;

	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		apply_av_info(*reinterpret_cast<const retro_system_av_info *>(data));
		return true;

	case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE:
		*reinterpret_cast<float *>(data) = float(m_fps);
		return true;

	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		*reinterpret_cast<int *>(data) = 3;
		return true;

	case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
		*reinterpret_cast<bool *>(data) = false;
		return true;

	// directories
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
		*reinterpret_cast<const char **>(data) = m_system_dir.c_str();
		return true;

	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*reinterpret_cast<const char **>(data) = m_save_dir.c_str();
		return true;

	case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
		*reinterpret_cast<const char **>(data) = m_core_path.c_str();
		return true;

	// input
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;

	// core options
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*reinterpret_cast<unsigned *>(data) = 2;
		return true;

	case RETRO_ENVIRONMENT_SET_VARIABLES:
		m_options.clear();
		define_variables(reinterpret_cast<const retro_variable *>(data));
		return true;

	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
	{
		auto *def = (cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS)
				? reinterpret_cast<const retro_core_option_definition *>(data)
				: reinterpret_cast<const retro_core_options_intl *>(data)->us;
		m_options.clear();
		m_option_categories.clear();
		for ( ; def && def->key; def++)
			define_option(def->key, def->desc, def->info, nullptr, def->values, def->default_value);
		return true;
	}

	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
	{
		auto *options = (cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2)
				? reinterpret_cast<const retro_core_options_v2 *>(data)
				: reinterpret_cast<const retro_core_options_v2_intl *>(data)->us;
		m_options.clear();
		m_option_categories.clear();
		for (auto *cat = options ? options->categories : nullptr; cat && cat->key; cat++)
			m_option_categories.push_back({ cat->key, cat->desc ? cat->desc : cat->key });
		for (auto *def = options ? options->definitions : nullptr; def && def->key; def++)
		{
			// options shown in a category have shorter descriptions
			bool const categorized = def->category_key && *def->category_key;
			define_option(
					def->key,
					(categorized && def->desc_categorized) ? def->desc_categorized : def->desc,
					(categorized && def->info_categorized) ? def->info_categorized : def->info,
					def->category_key,
					def->values,
					def->default_value);
		}
		return true;
	}

	case RETRO_ENVIRONMENT_GET_VARIABLE:
	{
		auto *var = reinterpret_cast<retro_variable *>(data);
		if (option *const opt = find_option(var->key))
		{
			var->value = opt->value.c_str();
			return true;
		}
		auto const found = m_option_values.find(var->key);
		if (found == m_option_values.end())
			return false;
		var->value = found->second.c_str();
		return true;
	}

	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*reinterpret_cast<bool *>(data) = std::exchange(m_options_updated, false);
		return true;

	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
	{
		auto *display = reinterpret_cast<const retro_core_option_display *>(data);
		if (option *const opt = display ? find_option(display->key) : nullptr)
			opt->visible = display->visible;
		return true;
	}

	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
	{
		auto *callback = reinterpret_cast<const retro_core_options_update_display_callback *>(data);
		m_update_display = callback ? callback->callback : nullptr;
		return true;
	}

	// miscellaneous
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		reinterpret_cast<retro_log_callback *>(data)->log = log_callback;
		return true;

	case RETRO_ENVIRONMENT_GET_LANGUAGE:
		*reinterpret_cast<unsigned *>(data) = RETRO_LANGUAGE_ENGLISH;
		return true;

	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		m_support_no_game = *reinterpret_cast<const bool *>(data);
		return true;

	case RETRO_ENVIRONMENT_SET_MESSAGE:
		machine().popmessage("%s", reinterpret_cast<const retro_message *>(data)->msg);
		return true;

	case RETRO_ENVIRONMENT_SHUTDOWN:
		machine().schedule_exit();
		return true;

	// informative, nothing to do
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
	case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
	case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
	case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
		return true;

	default:
		logerror("unsupported environment command %u\n", cmd);
		return false;
	}
}


//**************************************************************************
//  VIDEO
//**************************************************************************

void libretro_state::configure_screen(int width, int height)
{
	m_width = width;
	m_height = height;

	// no vertical blanking: the frame produced by retro_run in the VBLANK
	// callback is displayed by the frame update that immediately follows
	m_screen->configure(width, height, rectangle(0, width - 1, 0, height - 1), HZ_TO_ATTOSECONDS(m_fps));
}

void libretro_state::apply_geometry(const retro_game_geometry &geometry)
{
	int const max_width = std::max(geometry.max_width, geometry.base_width);
	int const max_height = std::max(geometry.max_height, geometry.base_height);
	if (max_width > m_frame.width() || max_height > m_frame.height())
		m_frame.resize(std::max(max_width, m_frame.width()), std::max(max_height, m_frame.height()));

	if (m_gl && !m_gl->resize(max_width, max_height))
		osd_printf_error("libretro: can't resize the OpenGL framebuffer: %s\n", m_gl->error());

	if (geometry.base_width && geometry.base_height)
		configure_screen(geometry.base_width, geometry.base_height);
}

void libretro_state::apply_av_info(const retro_system_av_info &info)
{
	if (info.timing.fps > 0.0)
		m_fps = info.timing.fps;
	m_sound->set_rate(u32(info.timing.sample_rate + 0.5));
	apply_geometry(info.geometry);
	osd_printf_verbose("libretro: %dx%d %.6f Hz, audio %.1f Hz\n", m_width, m_height, m_fps, info.timing.sample_rate);
}

void libretro_state::video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
	// duplicated frame: keep the previous one
	if (!data || !width || !height)
		return;

	// hardware rendered frame, read back in this frame
	if (data == RETRO_HW_FRAME_BUFFER_VALID)
	{
		if (!m_gl)
			return;
		if (int(width) != m_width || int(height) != m_height)
			configure_screen(width, height);
		m_display = &m_gl->readback(width, height, m_hw.bottom_left_origin);
		return;
	}
	m_display = &m_frame;

	if (int(width) > m_frame.width() || int(height) > m_frame.height())
		m_frame.resize(std::max<int>(width, m_frame.width()), std::max<int>(height, m_frame.height()));

	// follow resolution changes, switchres picks a new mode for them
	if (int(width) != m_width || int(height) != m_height)
		configure_screen(width, height);

	for (unsigned y = 0; y < height; y++)
	{
		u8 const *const src = reinterpret_cast<const u8 *>(data) + y * pitch;
		u32 *const dst = &m_frame.pix(y);
		switch (m_pixel_format)
		{
		case RETRO_PIXEL_FORMAT_XRGB8888:
			for (unsigned x = 0; x < width; x++)
				dst[x] = reinterpret_cast<const u32 *>(src)[x] & 0x00ffffff;
			break;

		case RETRO_PIXEL_FORMAT_RGB565:
			for (unsigned x = 0; x < width; x++)
			{
				u16 const p = reinterpret_cast<const u16 *>(src)[x];
				dst[x] = rgb_t(pal5bit(p >> 11), pal6bit(p >> 5), pal5bit(p));
			}
			break;

		default:
			for (unsigned x = 0; x < width; x++)
			{
				u16 const p = reinterpret_cast<const u16 *>(src)[x];
				dst[x] = rgb_t(pal5bit(p >> 10), pal5bit(p >> 5), pal5bit(p));
			}
			break;
		}
	}
}

void libretro_state::vblank(int state)
{
	if (state && m_loaded)
	{
		libretro_gl_scope scope(m_gl.get());
		m_api.run();
	}
}

u32 libretro_state::screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	const bitmap_rgb32 &frame = m_display ? *m_display : m_frame;
	rectangle clip = cliprect;
	clip &= rectangle(0, frame.width() - 1, 0, frame.height() - 1);
	copybitmap(bitmap, frame, 0, 0, 0, 0, clip);
	return 0;
}


//**************************************************************************
//  INPUT
//**************************************************************************

int16_t libretro_state::input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
	if (port >= MAX_PADS)
		return 0;

	switch (device & RETRO_DEVICE_MASK)
	{
	case RETRO_DEVICE_JOYPAD:
	{
		u32 const pad = m_pads[port]->read();
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
			return int16_t(pad);
		return (id < 16) ? BIT(pad, id) : 0;
	}

	case RETRO_DEVICE_ANALOG:
		if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
		{
			ioport_port &axis = (id == RETRO_DEVICE_ID_ANALOG_X) ? *m_stick_x[port] : *m_stick_y[port];
			return int16_t(std::clamp<int>(int(axis.read()) - 0x8000, -0x7fff, 0x7fff));
		}
		if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON)
			return (id < 16 && BIT(m_pads[port]->read(), id)) ? 0x7fff : 0;
		return 0;

	default:
		return 0;
	}
}


//**************************************************************************
//  BATTERY SAVE RAM
//**************************************************************************

void libretro_state::load_save_ram()
{
	void *const data = m_api.get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t const size = m_api.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!data || !size)
		return;

	emu_file file(machine().options().nvram_directory(), OPEN_FLAG_READ);
	if (!file.open(std::string("libretro" PATH_SEPARATOR) + m_content_name + ".srm"))
		file.read(data, size);
}

void libretro_state::save_save_ram()
{
	void *const data = m_api.get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t const size = m_api.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!data || !size)
		return;

	emu_file file(machine().options().nvram_directory(), OPEN_FLAG_WRITE | OPEN_FLAG_CREATE | OPEN_FLAG_CREATE_PATHS);
	if (!file.open(std::string("libretro" PATH_SEPARATOR) + m_content_name + ".srm"))
		file.write(data, size);
}


//**************************************************************************
//  MACHINE
//**************************************************************************

void libretro_state::machine_start()
{
	emu_options const &options = machine().options();
	m_core_path = options.libretro_core();
	m_system_dir = options.libretro_system_directory();
	m_save_dir = std::string(options.nvram_directory()) + PATH_SEPARATOR "libretro";

	if (m_core_path.empty())
		throw emu_fatalerror("libretro: no core specified, use -L\n");

	m_frame.allocate(640, 480);

	load_core();

	s_instance = this;
	m_api.set_environment(env_callback);
	m_api.init();
	m_api.set_video_refresh(video_callback);
	m_api.set_audio_sample(audio_callback);
	m_api.set_audio_sample_batch(audio_batch_callback);
	m_api.set_input_poll(input_poll_callback);
	m_api.set_input_state(input_state_callback);

	load_content();
	m_loaded = true;
	machine().set_runtime_option_provider(this);

	for (unsigned port = 0; port < MAX_PADS; port++)
		m_api.set_controller_port_device(port, RETRO_DEVICE_JOYPAD);

	retro_system_av_info av = { };
	m_api.get_system_av_info(&av);
	apply_av_info(av);

	// hardware rendered core: its context, then let it create its resources
	if (m_hw_requested)
	{
		m_gl = std::make_unique<libretro_gl>();
		if (!m_gl->create(m_hw, std::max(av.geometry.max_width, av.geometry.base_width), std::max(av.geometry.max_height, av.geometry.base_height), options.libretro_gl_device(), options.libretro_gl_readback()))
			throw emu_fatalerror("libretro: can't create the OpenGL context for %s: %s\n", m_library_name, m_gl->error());
		osd_printf_info("libretro: hardware rendering on %s\n", m_gl->description());

		libretro_gl_scope scope(m_gl.get());
		if (m_hw.context_reset)
			m_hw.context_reset();
	}

	load_save_ram();
	machine().add_notifier(MACHINE_NOTIFY_EXIT, machine_notify_delegate(&libretro_state::machine_exit, this));

	// MAME save states wrap the core's own serialization
	libretro_gl_scope scope(m_gl.get());
	size_t const state_size = m_api.serialize_size();
	if (state_size)
	{
		m_state.resize(state_size);
		save_item(NAME(m_state));
		machine().save().register_presave(save_prepost_delegate(FUNC(libretro_state::state_presave), this));
		machine().save().register_postload(save_prepost_delegate(FUNC(libretro_state::state_postload), this));
	}
}

void libretro_state::state_presave()
{
	libretro_gl_scope scope(m_gl.get());
	m_api.serialize(m_state.data(), m_state.size());
}

void libretro_state::state_postload()
{
	libretro_gl_scope scope(m_gl.get());
	m_api.unserialize(m_state.data(), m_state.size());
}

void libretro_state::machine_reset()
{
	// the core starts in its reset state, only forward later resets
	if (std::exchange(m_started, true))
	{
		libretro_gl_scope scope(m_gl.get());
		m_api.reset();
	}
}

void libretro_state::machine_exit()
{
	save_save_ram();

	m_loaded = false;
	machine().set_runtime_option_provider(nullptr);
	{
		libretro_gl_scope scope(m_gl.get());
		m_api.unload_game();
		if (m_gl && m_hw.context_destroy)
			m_hw.context_destroy();
		m_api.deinit();
	}
	m_gl.reset();
	s_instance = nullptr;

	if (!m_temp_content.empty())
	{
		std::error_code ec;
		std::filesystem::remove(m_temp_content, ec);
	}
}


//**************************************************************************
//  INPUT PORTS
//**************************************************************************

#define RETROPAD(n) \
	PORT_START("PAD" #n) \
	PORT_BIT(1U << RETRO_DEVICE_ID_JOYPAD_B,      IP_ACTIVE_HIGH, IPT_BUTTON1)        PORT_NAME("%p B")  PORT_PLAYER(n) \
	PORT_BIT(1U << RETRO_DEVICE_ID_JOYPAD_Y,      IP_ACTIVE_HIGH, IPT_BUTTON3)        PORT_NAME("%p Y")  PORT_PLAYER(n) \
	PORT_BIT(1U << RETRO_DEVICE_ID_JOYPAD_SELECT, IP_ACTIVE_HIGH, IPT_SELECT)                            PORT_PLAYER(n) \
	PORT_BIT(1U << RETRO_DEVICE_ID_JOYPAD_START,  IP_ACTIVE_HIGH, IPT_START)                             PORT_PLAYER(n) \
	PORT_BIT(1U << RETRO_DEVICE_ID_JOYPAD_UP,     IP_ACTIVE_HIGH, IPT_JOYSTICK_UP)    PORT_8WAY          PORT_PLAYER(n) \
	PORT_BIT(1U << RETRO_DEVICE_ID_JOYPAD_DOWN,   IP_ACTIVE_HIGH, IPT_JOYSTICK_DOWN)  PORT_8WAY          PORT_PLAYER(n) \
	PORT_BIT(1U << RETRO_DEVICE_ID_JOYPAD_LEFT,   IP_ACTIVE_HIGH, IPT_JOYSTICK_LEFT)  PORT_8WAY          PORT_PLAYER(n) \
	PORT_BIT(1U << RETRO_DEVICE_ID_JOYPAD_RIGHT,  IP_ACTIVE_HIGH, IPT_JOYSTICK_RIGHT) PORT_8WAY          PORT_PLAYER(n) \
	PORT_BIT(1U << RETRO_DEVICE_ID_JOYPAD_A,      IP_ACTIVE_HIGH, IPT_BUTTON2)        PORT_NAME("%p A")  PORT_PLAYER(n) \
	PORT_BIT(1U << RETRO_DEVICE_ID_JOYPAD_X,      IP_ACTIVE_HIGH, IPT_BUTTON4)        PORT_NAME("%p X")  PORT_PLAYER(n) \
	PORT_BIT(1U << RETRO_DEVICE_ID_JOYPAD_L,      IP_ACTIVE_HIGH, IPT_BUTTON5)        PORT_NAME("%p L")  PORT_PLAYER(n) \
	PORT_BIT(1U << RETRO_DEVICE_ID_JOYPAD_R,      IP_ACTIVE_HIGH, IPT_BUTTON6)        PORT_NAME("%p R")  PORT_PLAYER(n) \
	PORT_BIT(1U << RETRO_DEVICE_ID_JOYPAD_L2,     IP_ACTIVE_HIGH, IPT_BUTTON7)        PORT_NAME("%p L2") PORT_PLAYER(n) \
	PORT_BIT(1U << RETRO_DEVICE_ID_JOYPAD_R2,     IP_ACTIVE_HIGH, IPT_BUTTON8)        PORT_NAME("%p R2") PORT_PLAYER(n) \
	PORT_BIT(1U << RETRO_DEVICE_ID_JOYPAD_L3,     IP_ACTIVE_HIGH, IPT_BUTTON9)        PORT_NAME("%p L3") PORT_PLAYER(n) \
	PORT_BIT(1U << RETRO_DEVICE_ID_JOYPAD_R3,     IP_ACTIVE_HIGH, IPT_BUTTON10)       PORT_NAME("%p R3") PORT_PLAYER(n) \
	PORT_START("STICKX" #n) \
	PORT_BIT(0xffff, 0x8000, IPT_AD_STICK_X) PORT_MINMAX(0, 0xffff) PORT_SENSITIVITY(100) PORT_KEYDELTA(0x800) PORT_PLAYER(n) \
	PORT_START("STICKY" #n) \
	PORT_BIT(0xffff, 0x8000, IPT_AD_STICK_Y) PORT_MINMAX(0, 0xffff) PORT_SENSITIVITY(100) PORT_KEYDELTA(0x800) PORT_PLAYER(n)

INPUT_PORTS_START( libretro )
	RETROPAD(1)
	RETROPAD(2)
	RETROPAD(3)
	RETROPAD(4)
INPUT_PORTS_END


//**************************************************************************
//  MACHINE CONFIGURATION
//**************************************************************************

void libretro_state::libretro(machine_config &config)
{
	// reconfigured at start with the geometry and timing of the core
	SCREEN(config, m_screen, SCREEN_TYPE_RASTER);
	m_screen->set_refresh_hz(60);
	m_screen->set_size(320, 240);
	m_screen->set_visarea(0, 319, 0, 239);
	m_screen->set_video_attributes(VIDEO_UPDATE_AFTER_VBLANK);
	m_screen->set_screen_update(FUNC(libretro_state::screen_update));
	m_screen->screen_vblank().set(FUNC(libretro_state::vblank));

	SPEAKER(config, "speaker", 2).front();
	LIBRETRO_CART(config, m_cart, 0);

	LIBRETRO_SOUND(config, m_sound, 0);
	m_sound->add_route(0, "speaker", 1.0, 0);
	m_sound->add_route(1, "speaker", 1.0, 1);
}


ROM_START( libretro )
ROM_END

} // anonymous namespace


//    YEAR  NAME      PARENT  COMPAT  MACHINE   INPUT     CLASS           INIT        COMPANY     FULLNAME            FLAGS
CONS( 2024, libretro, 0,      0,      libretro, libretro, libretro_state, empty_init, "libretro", "libretro core",    MACHINE_SUPPORTS_SAVE )
