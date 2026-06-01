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

#include "securec.h"

#include "sql/dd/impl/types/table_impl.h"
#include "ut_mysql_mock.h"

// todo: 暂时写死，后续支持自定义类型
ut_mysql_table::ut_mysql_table(int id, const char *name) {
  assert(strlen(name) * 4 < 255);
  int col_num = 2;
  constexpr int raw_data_len = 1024;  // 预估的数据长度
  int ret = 0;

  s = new TABLE_SHARE;
  s->fields = col_num;
  field = new Field *[col_num];

  unsigned char *mysql_ptr = new unsigned char[raw_data_len];
  memset_s(mysql_ptr, raw_data_len, 0, raw_data_len);

  // 设置null比特位
  mysql_ptr[0] = (unsigned char)0b11111001;

  field[0] = new Field_long(nullptr, 10, nullptr, 0, 0, "id", false, true);
  field[1] = new Field_varstring(
      nullptr, 20, 1, nullptr, 0, 0, "name", nullptr,
      &my_charset_utf8mb4_bin);  // 字符串长度20，存放字符串长度的整型占用1个字节

  uint32_t offset = 1;

  *(reinterpret_cast<int *>(&mysql_ptr[offset])) = id;
  field[0]->set_field_ptr(&mysql_ptr[offset]);
  field[0]->set_null_ptr(&mysql_ptr[0], 1 << 1);

  offset += field[0]->pack_length();

  field[1]->set_field_ptr(&mysql_ptr[offset]);
  field[1]->set_null_ptr(&mysql_ptr[0], 1 << 2);

  *(reinterpret_cast<char *>(&mysql_ptr[offset])) =
      strlen(name);  // 一字节存放字符串长度

  offset += 1;
  ret = memcpy_s(&mysql_ptr[offset], strlen(name), name, strlen(name));
  assert(ret == 0);
  (void)ret;
  record[0] = mysql_ptr;
  record[1] = nullptr;
}

ut_mysql_table::ut_mysql_table(int id_old, const char *name_old, int id_new,
                               const char *name_new)
    : ut_mysql_table(id_old, name_old) {
  int ret = 0;
  assert(strlen(name_new) * 4 < 255);
  constexpr int raw_data_len = 1024;  // 预估的数据长度
  unsigned char *mysql_ptr = new unsigned char[raw_data_len];
  memset_s(mysql_ptr, raw_data_len, 0, raw_data_len);

  // 设置null比特位
  mysql_ptr[0] = (unsigned char)0b11111001;
  uint32_t offset = 1;
  *(reinterpret_cast<int *>(&mysql_ptr[offset])) = id_new;
  offset += field[0]->pack_length();
  *(reinterpret_cast<char *>(&mysql_ptr[offset])) =
      strlen(name_new);  // 一字节存放字符串长度
  offset += 1;
  ret = memcpy_s(&mysql_ptr[offset], strlen(name_new), name_new,
                 strlen(name_new));
  assert(ret == 0);
  (void)ret;
  record[1] = mysql_ptr;
}

ut_mysql_table::~ut_mysql_table() {
  delete s;
  delete field[0];
  delete field[1];
  delete[] field;
  delete[] record[0];
  if (record[1] != nullptr) delete[] record[1];
}

dd::Table *ut_mysql_dd_init() {
  dd::Table *table_def = new dd::Table_impl();
  return table_def;
}

void ut_mysql_dd_release(dd::Table *table_def) { delete table_def; }
