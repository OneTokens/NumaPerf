


/*
 * FreeGuard: A Faster Secure Heap Allocator
 * Copyright (C) 2017 Sam Silvestro, Hongyu Liu, Corey Crosser,
 *                    Zhiqiang Lin, and Tongping Liu
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * @file   real.cpp: provides wrappers to glibc thread & memory functions.
 * @author Tongping Liu <http://www.cs.utsa.edu/~tongpingliu/>
 * @author Sam Silvestro <sam.silvestro@utsa.edu>
 */

#ifndef ACCESSPATERN_REAL_H
#define ACCESSPATERN_REAL_H

#include <dlfcn.h>
#include <stdlib.h>
#include <pthread.h>

#define DEFINE_WRAPPER(name) decltype(::name) * name;
#define INIT_WRAPPER(name, handle) name = (decltype(::name)*)dlsym(handle, #name);

namespace Real {
    DEFINE_WRAPPER(malloc);
    DEFINE_WRAPPER(free);

    void init() {
        INIT_WRAPPER(malloc, RTLD_NEXT);
        INIT_WRAPPER(free, RTLD_NEXT);
    }
}
#endif //ACCESSPATERN_REAL_H
