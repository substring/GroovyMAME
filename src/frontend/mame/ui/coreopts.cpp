// license:BSD-3-Clause
// copyright-holders:Substring
/***************************************************************************

    ui/coreopts.cpp

    Options of an external emulator hosted by the running system, such
    as a libretro core. The top level shows the categories and the
    options without a category, each category has its own submenu.

***************************************************************************/

#include "emu.h"
#include "ui/coreopts.h"

#include "ui/ui.h"

#include <algorithm>


namespace ui {

namespace {

constexpr uintptr_t ITEM_RESET          = 0x00000001;
constexpr uintptr_t ITEM_OPTION_FIRST   = 0x00001000;
constexpr uintptr_t ITEM_CATEGORY_FIRST = 0x00100000;

using option = runtime_option_provider::option;

const option *find_option(runtime_option_provider const &provider, std::string_view key)
{
	auto const &options = provider.runtime_options();
	auto const found = std::find_if(options.begin(), options.end(), [key] (const option &opt) { return opt.key == key; });
	return (found != options.end()) ? &*found : nullptr;
}

} // anonymous namespace


//-------------------------------------------------
//  menu_core_options - constructor
//-------------------------------------------------

menu_core_options::menu_core_options(mame_ui_manager &mui, render_target &target, std::string &&category)
	: menu(mui, target)
	, m_category(std::move(category))
{
	std::string heading = _("Core Options");
	if (runtime_option_provider const *const provider = machine().runtime_options())
	{
		for (const auto &cat : provider->runtime_option_categories())
			if (cat.key == m_category)
				heading = cat.label;
	}
	set_heading(std::move(heading));
}

menu_core_options::~menu_core_options()
{
}


//-------------------------------------------------
//  menu_activated - options may have been
//  shown or hidden by a submenu
//-------------------------------------------------

void menu_core_options::menu_activated()
{
	reset(reset_options::REMEMBER_REF);
}


//-------------------------------------------------
//  populate - build the menu
//-------------------------------------------------

void menu_core_options::populate()
{
	m_categories.clear();
	m_keys.clear();

	runtime_option_provider const *const provider = machine().runtime_options();
	if (!provider)
	{
		item_append(_("No options"), FLAG_DISABLE, nullptr);
		return;
	}

	auto const &categories = provider->runtime_option_categories();
	auto const &options = provider->runtime_options();
	auto const is_category = [&categories] (std::string const &key)
	{
		return std::any_of(categories.begin(), categories.end(), [&key] (auto const &cat) { return cat.key == key; });
	};

	// categories that have something to show
	if (m_category.empty())
	{
		for (const auto &cat : categories)
		{
			if (std::any_of(options.begin(), options.end(), [&cat] (const option &opt) { return opt.visible && (opt.category == cat.key); }))
			{
				m_categories.emplace_back(cat.key);
				item_append(cat.label, 0, reinterpret_cast<void *>(ITEM_CATEGORY_FIRST + m_categories.size() - 1));
			}
		}
		if (!m_categories.empty())
			item_append(menu_item_type::SEPARATOR);
	}

	// options of this level, left and right cycle through the values
	for (const option &opt : options)
	{
		bool const here = m_category.empty() ? !is_category(opt.category) : (opt.category == m_category);
		if (!here || !opt.visible)
			continue;

		auto const value = std::find_if(opt.values.begin(), opt.values.end(), [&opt] (const auto &v) { return v.first == opt.value; });
		std::string label = (value != opt.values.end()) ? value->second : opt.value;
		uint32_t const flags = (opt.values.size() > 1) ? (FLAG_LEFT_ARROW | FLAG_RIGHT_ARROW) : FLAG_DISABLE;

		m_keys.emplace_back(opt.key);
		item_append(opt.label, std::move(label), flags, reinterpret_cast<void *>(ITEM_OPTION_FIRST + m_keys.size() - 1));
	}

	item_append(menu_item_type::SEPARATOR);

	// some options only apply when the core starts
	if (m_category.empty())
		item_append(_("Reset System"), 0, reinterpret_cast<void *>(ITEM_RESET));
}


//-------------------------------------------------
//  recompute_metrics - leave space for the help
//  text of the selected option
//-------------------------------------------------

void menu_core_options::recompute_metrics(uint32_t width, uint32_t height, float aspect)
{
	menu::recompute_metrics(width, height, aspect);

	set_custom_space(0.0F, (3.0F * line_height()) + (3.0F * tb_border()));
}


//-------------------------------------------------
//  custom_render - draw the help text of the
//  selected option
//-------------------------------------------------

void menu_core_options::custom_render(uint32_t flags, void *selectedref, float top, float bottom, float origx1, float origy1, float origx2, float origy2)
{
	uintptr_t const ref = reinterpret_cast<uintptr_t>(selectedref);
	if (ref < ITEM_OPTION_FIRST || ref >= ITEM_CATEGORY_FIRST || (ref - ITEM_OPTION_FIRST) >= m_keys.size())
		return;

	runtime_option_provider const *const provider = machine().runtime_options();
	option const *const opt = provider ? find_option(*provider, m_keys[ref - ITEM_OPTION_FIRST]) : nullptr;
	if (!opt || opt->info.empty())
		return;

	char const *const text[] = { opt->info.c_str() };
	draw_text_box(
			std::begin(text), std::end(text),
			origx1, origx2, origy2 + tb_border(), origy2 + bottom,
			text_layout::text_justify::CENTER, text_layout::word_wrapping::WORD, false,
			ui().colors().text_color(), ui().colors().background_color());
}


//-------------------------------------------------
//  handle - process an input event
//-------------------------------------------------

bool menu_core_options::handle(event const *ev)
{
	if (!ev || !ev->itemref)
		return false;

	runtime_option_provider *const provider = machine().runtime_options();
	if (!provider)
		return false;

	uintptr_t const ref = reinterpret_cast<uintptr_t>(ev->itemref);
	if (ref == ITEM_RESET)
	{
		if (ev->iptkey == IPT_UI_SELECT)
			machine().schedule_hard_reset();
	}
	else if (ref >= ITEM_CATEGORY_FIRST)
	{
		if (ev->iptkey == IPT_UI_SELECT && (ref - ITEM_CATEGORY_FIRST) < m_categories.size())
			menu::stack_push<menu_core_options>(ui(), target(), std::string(m_categories[ref - ITEM_CATEGORY_FIRST]));
	}
	else if (ref >= ITEM_OPTION_FIRST && (ref - ITEM_OPTION_FIRST) < m_keys.size())
	{
		option const *const opt = find_option(*provider, m_keys[ref - ITEM_OPTION_FIRST]);
		if (!opt || opt->values.empty())
			return false;

		auto const current = std::find_if(opt->values.begin(), opt->values.end(), [opt] (const auto &v) { return v.first == opt->value; });
		size_t const index = (current != opt->values.end()) ? (current - opt->values.begin()) : 0;
		size_t const count = opt->values.size();
		std::string value;
		switch (ev->iptkey)
		{
		case IPT_UI_LEFT:
			value = opt->values[(index + count - 1) % count].first;
			break;
		case IPT_UI_RIGHT:
		case IPT_UI_SELECT:
			value = opt->values[(index + 1) % count].first;
			break;
		case IPT_UI_CLEAR:
			value = opt->default_value;
			break;
		default:
			return false;
		}

		// changing an option may show or hide others
		provider->set_runtime_option(opt->key, value);
		reset(reset_options::REMEMBER_REF);
	}

	return false;
}

} // namespace ui
