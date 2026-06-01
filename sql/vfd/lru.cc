/* Copyright (c) 2023, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/vfd/lru.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "sql/vfd/vfd_file.h"

template <class KEY, class VALUE>
void LRU<KEY, VALUE>::Add(const KEY &key, VALUE &value) {
  auto inserted = m_items.emplace(m_items.begin(), key, value);
  m_item_map.emplace(key, inserted);
}

template <class KEY, class VALUE>
bool LRU<KEY, VALUE>::Delete(const KEY &key) noexcept {
  try {
    auto iterator = m_item_map.find(key);
    m_items.erase(iterator->second);
    m_item_map.erase(iterator);
    return false;
  } catch (...) {
    // Maybe not the best error message, but this should really not happen in a
    // normal environment.
    my_error(ER_TEMP_FILE_WRITE_FAILURE, MYF(0));
    return true;
  }
}

/// Bump the file with the given key to the front of the LRU.
template <class KEY, class VALUE>
void LRU<KEY, VALUE>::Get(const KEY &key) {
  if (m_items.front().first == key) {
    // Fast-path: Do nothing if the file is already in front.
    return;
  }

  auto iterator = m_item_map[key];
  m_items.splice(m_items.begin(), m_items, iterator);
}

template <class KEY, class VALUE>
bool LRU<KEY, VALUE>::Contains(const KEY &key) const {
  return m_item_map.find(key) != m_item_map.end();
}

// Explicit template instantiations.
template class LRU<int, VfdFile *>;
