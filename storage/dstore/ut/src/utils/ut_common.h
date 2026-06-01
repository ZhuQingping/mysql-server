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

#ifndef __UT_COMMON_H__
#define __UT_COMMON_H__


/* used to visit private member of a struct in single inheritance case
*/
#define DEF_PRIVATE_ROBBER(struct_name, member_type, member) \
    template<member_type struct_name::* MemPtr> \
    struct __robber4##struct_name##_##member { \
        friend member_type struct_name::* steal_##member##_4_##struct_name() { \
            return MemPtr; \
        } \
    }; \
    template struct __robber4##struct_name##_##member<&struct_name::member>; \
    member_type struct_name::* steal_##member##_4_##struct_name() \


#endif // __UT_COMMON_H__
