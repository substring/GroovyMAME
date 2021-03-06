// license:BSD-3-Clause
// copyright-holders:Krzysztof Strzecha
/*****************************************************************************
 *
 * includes/pmd85.h
 *
 ****************************************************************************/
#ifndef MAME_INCLUDES_PMD85_H
#define MAME_INCLUDES_PMD85_H

#pragma once

#include "machine/i8251.h"
#include "machine/pit8253.h"
#include "machine/i8255.h"
#include "imagedev/cassette.h"
#include "machine/ram.h"
#include "emupal.h"


class pmd85_state : public driver_device
{
public:
	pmd85_state(const machine_config &mconfig, device_type type, const char *tag) :
		driver_device(mconfig, type, tag),
		m_maincpu(*this, "maincpu"),
		m_ram(*this, RAM_TAG),
		m_cassette(*this, "cassette"),
		m_pit8253(*this, "pit8253"),
		m_uart(*this, "uart"),
		m_ppi8255_0(*this, "ppi8255_0"),
		m_ppi8255_1(*this, "ppi8255_1"),
		m_ppi8255_2(*this, "ppi8255_2"),
		m_ppi8255_3(*this, "ppi8255_3"),
		m_region_maincpu(*this, "maincpu"),
		m_bank1(*this, "bank1"),
		m_bank2(*this, "bank2"),
		m_bank3(*this, "bank3"),
		m_bank4(*this, "bank4"),
		m_bank5(*this, "bank5"),
		m_bank6(*this, "bank6"),
		m_bank7(*this, "bank7"),
		m_bank8(*this, "bank8"),
		m_bank9(*this, "bank9"),
		m_bank10(*this, "bank10"),
		m_bank11(*this, "bank11"),
		m_bank12(*this, "bank12"),
		m_bank13(*this, "bank13"),
		m_bank14(*this, "bank14"),
		m_bank15(*this, "bank15"),
		m_bank16(*this, "bank16"),
		m_io_dsw0(*this, "DSW0"),
		m_palette(*this, "palette"),
		m_leds(*this, "led%u", 0U)
	{ }

	void pmd85(machine_config &config, bool with_uart = true);
	void pmd851(machine_config &config);
	void pmd853(machine_config &config);
	void pmd852a(machine_config &config);
	void alfa(machine_config &config);
	void c2717(machine_config &config);
	void mato(machine_config &config);

	void init_mato();
	void init_pmd852a();
	void init_pmd851();
	void init_pmd852();
	void init_pmd853();
	void init_alfa();
	void init_c2717();

	DECLARE_INPUT_CHANGED_MEMBER(pmd85_reset);

private:
	enum
	{
		TIMER_CASSETTE
	};

	uint8_t m_rom_module_present;
	uint8_t m_ppi_port_outputs[4][3];
	uint8_t m_startup_mem_map;
	uint8_t m_pmd853_memory_mapping;
	bool m_previous_level;
	bool m_clk_level;
	bool m_clk_level_tape;
	uint8_t m_model;
	emu_timer * m_cassette_timer;
	void (pmd85_state::*update_memory)();
	uint8_t pmd85_io_r(offs_t offset);
	void pmd85_io_w(offs_t offset, uint8_t data);
	uint8_t mato_io_r(offs_t offset);
	void mato_io_w(offs_t offset, uint8_t data);

	virtual void machine_reset() override;
	uint32_t screen_update_pmd85(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect);
	TIMER_CALLBACK_MEMBER(pmd85_cassette_timer_callback);
	uint8_t pmd85_ppi_0_porta_r();
	uint8_t pmd85_ppi_0_portb_r();
	uint8_t pmd85_ppi_0_portc_r();
	void pmd85_ppi_0_porta_w(uint8_t data);
	void pmd85_ppi_0_portb_w(uint8_t data);
	void pmd85_ppi_0_portc_w(uint8_t data);
	uint8_t mato_ppi_0_portb_r();
	uint8_t mato_ppi_0_portc_r();
	void mato_ppi_0_portc_w(uint8_t data);
	uint8_t pmd85_ppi_1_porta_r();
	uint8_t pmd85_ppi_1_portb_r();
	uint8_t pmd85_ppi_1_portc_r();
	void pmd85_ppi_1_porta_w(uint8_t data);
	void pmd85_ppi_1_portb_w(uint8_t data);
	void pmd85_ppi_1_portc_w(uint8_t data);
	uint8_t pmd85_ppi_2_porta_r();
	uint8_t pmd85_ppi_2_portb_r();
	uint8_t pmd85_ppi_2_portc_r();
	void pmd85_ppi_2_porta_w(uint8_t data);
	void pmd85_ppi_2_portb_w(uint8_t data);
	void pmd85_ppi_2_portc_w(uint8_t data);
	uint8_t pmd85_ppi_3_porta_r();
	uint8_t pmd85_ppi_3_portb_r();
	uint8_t pmd85_ppi_3_portc_r();
	void pmd85_ppi_3_porta_w(uint8_t data);
	void pmd85_ppi_3_portb_w(uint8_t data);
	void pmd85_ppi_3_portc_w(uint8_t data);

	void alfa_mem(address_map &map);
	void c2717_mem(address_map &map);
	void mato_io_map(address_map &map);
	void mato_mem(address_map &map);
	void pmd852a_mem(address_map &map);
	void pmd853_mem(address_map &map);
	void pmd85_io_map(address_map &map);
	void pmd85_mem(address_map &map);

	virtual void machine_start() override { m_leds.resolve(); }

	required_device<cpu_device> m_maincpu;
	required_device<ram_device> m_ram;
	required_device<cassette_image_device> m_cassette;
	required_device<pit8253_device> m_pit8253;
	optional_device<i8251_device> m_uart;
	optional_device<i8255_device> m_ppi8255_0;
	optional_device<i8255_device> m_ppi8255_1;
	optional_device<i8255_device> m_ppi8255_2;
	optional_device<i8255_device> m_ppi8255_3;
	required_memory_region m_region_maincpu;
	required_memory_bank m_bank1;
	required_memory_bank m_bank2;
	required_memory_bank m_bank3;
	required_memory_bank m_bank4;
	optional_memory_bank m_bank5;
	optional_memory_bank m_bank6;
	optional_memory_bank m_bank7;
	optional_memory_bank m_bank8;
	optional_memory_bank m_bank9;
	optional_memory_bank m_bank10;
	optional_memory_bank m_bank11;
	optional_memory_bank m_bank12;
	optional_memory_bank m_bank13;
	optional_memory_bank m_bank14;
	optional_memory_bank m_bank15;
	optional_memory_bank m_bank16;
	optional_ioport m_io_dsw0;
	ioport_port *m_io_port[16];
	required_device<palette_device> m_palette;
	output_finder<3> m_leds;

	void pmd851_update_memory();
	void pmd852a_update_memory();
	void pmd853_update_memory();
	void alfa_update_memory();
	void mato_update_memory();
	void c2717_update_memory();
	void pmd85_common_driver_init();
	virtual void device_timer(emu_timer &timer, device_timer_id id, int param, void *ptr) override;

	bool m_txd, m_rts;
};


#endif // MAME_INCLUDES_PMD85_H
