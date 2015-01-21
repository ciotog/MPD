/*
 * Copyright (C) 2003-2015 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPD_CONFIG_BLOCK_HXX
#define MPD_CONFIG_BLOCK_HXX

#include "check.h"
#include "Compiler.h"

#include <string>

struct block_param {
	std::string name;
	std::string value;
	int line;

	/**
	 * This flag is false when nobody has queried the value of
	 * this option yet.
	 */
	mutable bool used;

	gcc_nonnull_all
	block_param(const char *_name, const char *_value, int _line=-1)
		:name(_name), value(_value), line(_line), used(false) {}

	gcc_pure
	int GetIntValue() const;

	gcc_pure
	unsigned GetUnsignedValue() const;

	gcc_pure
	bool GetBoolValue() const;
};

#endif