// license:BSD-3-Clause
// copyright-holders:Substring
/***************************************************************************

    runtimeopt.h

    Options of an external emulator hosted by the running system, such as
    the options of a libretro core. The driver implements this interface;
    the user interface finds it on the root device, so nothing is added to
    running_machine.

***************************************************************************/
#ifndef MAME_EMU_RUNTIMEOPT_H
#define MAME_EMU_RUNTIMEOPT_H

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>


class runtime_option_provider
{
public:
	struct category
	{
		std::string key;
		std::string label;
	};

	struct option
	{
		std::string key;
		std::string label;
		std::string info;                                          // help text
		std::string category;                                      // key of a category, or empty
		std::vector<std::pair<std::string, std::string> > values;  // value and label
		std::string default_value;
		std::string value;                                         // current value
		bool visible = true;
	};

	virtual ~runtime_option_provider() = default;

	// what the options belong to, e.g. the name and version of the core
	virtual std::string runtime_option_owner() const = 0;

	virtual const std::vector<category> &runtime_option_categories() const = 0;
	virtual const std::vector<option> &runtime_options() const = 0;
	virtual void set_runtime_option(std::string_view key, std::string_view value) = 0;
};

#endif // MAME_EMU_RUNTIMEOPT_H
