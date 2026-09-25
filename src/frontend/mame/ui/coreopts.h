// license:BSD-3-Clause
// copyright-holders:Substring
/***************************************************************************

    ui/coreopts.h

    Options of an external emulator hosted by the running system, such
    as a libretro core

***************************************************************************/
#ifndef MAME_FRONTEND_UI_COREOPTS_H
#define MAME_FRONTEND_UI_COREOPTS_H

#pragma once

#include "ui/menu.h"

#include <string>
#include <vector>


namespace ui {

class menu_core_options : public menu
{
public:
	menu_core_options(mame_ui_manager &mui, render_target &target, std::string &&category = std::string());
	virtual ~menu_core_options() override;

protected:
	virtual void recompute_metrics(uint32_t width, uint32_t height, float aspect) override;
	virtual void custom_render(uint32_t flags, void *selectedref, float top, float bottom, float origx1, float origy1, float origx2, float origy2) override;
	virtual void menu_activated() override;

private:
	virtual void populate() override;
	virtual bool handle(event const *ev) override;

	std::string const m_category;           // empty for the top level
	std::vector<std::string> m_categories;  // categories shown, in item order
	std::vector<std::string> m_keys;        // options shown, in item order
};

} // namespace ui

#endif // MAME_FRONTEND_UI_COREOPTS_H
