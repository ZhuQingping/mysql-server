/*
  Copyright (c) 2026, Huawei and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "memstats.h"

void MallocExtension_GetStats(__attribute__((unused)) char *buffer,
                              __attribute__((unused)) int buffer_length) {
  // do nothing. It's just a placeholder.
  // The real function is defined in libtcmalloc.
}

void __attribute__((weak))
CdeMallocStatsPrint(__attribute__((unused)) void (*write_cb)(void *,
                                                             const char *),
                    __attribute__((unused)) void *cbopaque,
                    __attribute__((unused)) const char *opts) {
  // do nothing. It's just a placeholder.
  // The real function is defined in libjemalloc.
}