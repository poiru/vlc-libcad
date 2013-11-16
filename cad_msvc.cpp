/*****************************************************************************
 * Copyright (C) 2013 Birunthan Mohanathas
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

// Hacks to compile the plugin out-of-tree.
#define __PLUGIN__
#define LIBVLC_USE_PTHREAD_CANCEL
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;

// The VLC header files do not work with the outdated MS Visual C compiler so we need to compile
// cad as C++.
#include "cad.c"
