// license:LGPL-2.1+
// copyright-holders:Angelo Salese, R. Belmont, Juergen Buchmueller
/***************************************************************************

	Acorn VIDC10 (VIDeo Controller) device chip

***************************************************************************/

#ifndef MAME_MACHINE_ACORN_VIDC_H
#define MAME_MACHINE_ACORN_VIDC_H

#pragma once

#include "speaker.h"
#include "sound/dac.h"
#include "sound/volt_reg.h"

//**************************************************************************
//  INTERFACE CONFIGURATION MACROS
//**************************************************************************



//**************************************************************************
//  TYPE DEFINITIONS
//**************************************************************************

// ======================> acorn_vidc10_device

class acorn_vidc10_device : public device_t,
							public device_memory_interface,
							public device_palette_interface,
							public device_video_interface
{
public:
	// construction/destruction
	acorn_vidc10_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);

	// I/O operations
	DECLARE_WRITE32_MEMBER( write );
	DECLARE_READ_LINE_MEMBER( flyback_r );
	auto vblank() { return m_vblank_cb.bind(); }
	auto sound_drq() { return m_sound_drq_cb.bind(); }
	// MEMC comms
	void write_vram(uint32_t offset, uint8_t data) { m_data_vram[offset & (m_data_vram_mask)] = data; }
	void write_cram(uint32_t offset, uint8_t data) { m_cursor_vram[offset & (m_cursor_vram_mask)] = data; }
	void write_dac(uint8_t channel, uint8_t data);
	void clear_dac(uint8_t channel) { m_dac[channel & 7]->write(0); }
	void update_sound_mode(bool state) { m_sound_mode = state; refresh_sound_frequency(); }
	void set_cursor_enable(bool state) { m_cursor_enable = state; }
	uint32_t get_cursor_size() { return (m_crtc_regs[CRTC_VCER] - m_crtc_regs[CRTC_VCSR]) * (32/4); }
	
protected:
	acorn_vidc10_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, u32 clock);

	// device-level overrides
	//virtual void device_validity_check(validity_checker &valid) const override;
	virtual void device_add_mconfig(machine_config &config) override;
	virtual uint32_t palette_entries() const override;
	virtual void device_config_complete() override;
	virtual void device_start() override;
	virtual void device_reset() override;
	virtual void device_timer(emu_timer &timer, device_timer_id id, int param, void *ptr) override;
	virtual space_config_vector memory_space_config() const override;
	u32 screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);
	virtual u32 get_pixel_clock();

	address_space_config  m_space_config;
	void regs_map(address_map &map);

	enum {
		CRTC_HCR = 0, CRTC_HSWR, CRTC_HBSR, CRTC_HDSR, CRTC_HDER, CRTC_HBER, CRTC_HCSR, CRTC_HIR,
		CRTC_VCR,     CRTC_VSWR, CRTC_VBSR, CRTC_VDSR, CRTC_VDER, CRTC_VBER, CRTC_VCSR, CRTC_VCER
	};
	uint32_t m_crtc_regs[16];
	enum {
		TIMER_VIDEO = 1,
		TIMER_SOUND = 2
	};

	inline void screen_vblank_line_update();
	inline void screen_dynamic_res_change();
	void refresh_sound_frequency();

	const uint16_t m_pal_4bpp_base = 0x100;
	const uint16_t m_pal_cursor_base = 0x10;
	const uint16_t m_pal_border_base = 0x110;
	const double m_sound_internal_divider = 8.0;

	uint8_t m_bpp_mode, m_crtc_interlace;
	u8       m_sound_frequency_latch;
	bool     m_sound_mode;

	required_device_array<dac_16bit_r2r_twos_complement_device, 8> m_dac;

private:
	required_device<speaker_device> m_lspeaker;
	required_device<speaker_device> m_rspeaker;
	devcb_write_line m_vblank_cb;
	devcb_write_line m_sound_drq_cb;

	DECLARE_WRITE32_MEMBER( pal_data_display_w );
	DECLARE_WRITE32_MEMBER( pal_data_cursor_w );
	DECLARE_WRITE32_MEMBER( stereo_image_w );
	DECLARE_WRITE32_MEMBER( crtc_w );
	DECLARE_WRITE32_MEMBER( sound_frequency_w );
	DECLARE_WRITE32_MEMBER( control_w );
	
	uint8_t m_pixel_clock;
	//bool m_flyback;
	emu_timer *m_video_timer;
	emu_timer *m_sound_timer;
		
	u8 	     *m_data_vram;
	u8       *m_cursor_vram;
	// TODO: correct data vram size
	const u32 m_data_vram_mask = 0x1fffff; 
	const u32 m_cursor_vram_mask = 0x7fff;
	const u32 m_data_vram_size = m_data_vram_mask+1;
	const u32 m_cursor_vram_size = m_cursor_vram_mask+1;
	bool     m_cursor_enable;
	void draw(bitmap_rgb32 &bitmap, const rectangle &cliprect, u8 *vram, uint8_t bpp, int xstart, int ystart, int xsize, int ysize, bool is_cursor);
	inline void update_4bpp_palette(uint16_t index, uint32_t paldata);
	uint32_t m_crtc_raw_horz[2];
	inline uint32_t convert_crtc_hdisplay(uint8_t index);

	bool     m_sound_frequency_test_bit;
	u8       m_stereo_image[8];
	const float m_sound_input_gain = 0.05;
	const int m_sound_max_channels = 8;
	int16_t  m_ulaw_lookup[256];
	inline void refresh_stereo_image(uint8_t channel);
};

class acorn_vidc10_lcd_device : public acorn_vidc10_device
{
public:
	// construction/destruction
	acorn_vidc10_lcd_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);
protected:
	virtual void device_add_mconfig(machine_config &config) override;
};

// device type definition
DECLARE_DEVICE_TYPE(ACORN_VIDC10, acorn_vidc10_device)
DECLARE_DEVICE_TYPE(ACORN_VIDC10_LCD, acorn_vidc10_lcd_device)

class arm_vidc20_device : public acorn_vidc10_device
{
public:
	// construction/destruction
	arm_vidc20_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);

	void write_dac32(uint8_t channel, uint16_t data);
	bool get_dac_mode();

protected:
	virtual void device_add_mconfig(machine_config &config) override;
	void regs_map(address_map &map);
	virtual uint32_t palette_entries() const override;
	virtual void device_start() override;
	virtual void device_config_complete() override;
	virtual void device_timer(emu_timer &timer, device_timer_id id, int param, void *ptr) override;
	u32 screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);
	virtual u32 get_pixel_clock() override;

	const uint16_t m_pal_4bpp_base = 0x000;
	const uint16_t m_pal_cursor_base = 0x100;
	const uint16_t m_pal_border_base = 0x100;
	const double m_sound_internal_divider = 1.0;

private:
	DECLARE_WRITE32_MEMBER( vidc20_pal_data_display_w );
	DECLARE_WRITE32_MEMBER( vidc20_pal_data_index_w );
	DECLARE_WRITE32_MEMBER( vidc20_crtc_w );
	DECLARE_WRITE32_MEMBER( vidc20_control_w );
	DECLARE_WRITE32_MEMBER( vidc20_sound_frequency_w );
	DECLARE_WRITE32_MEMBER( vidc20_sound_control_w );
	
	uint8_t m_pal_data_index;
	inline void update_8bpp_palette(uint16_t index, uint32_t paldata);
	bool m_dac_serial_mode;
	u8 m_pixel_source;
	u8 m_pixel_rate;
};

DECLARE_DEVICE_TYPE(ARM_VIDC20, arm_vidc20_device)


//**************************************************************************
//  GLOBAL VARIABLES
//**************************************************************************


#endif // MAME_MACHINE_ACORN_VIDC10_H
