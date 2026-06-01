#ifndef SQL_VFD_LRU_H
#define SQL_VFD_LRU_H

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

#include <cstddef>
#include <list>
#include <unordered_map>
#include <utility>

/// A generic LRU where there is at most one value per key.
template <class KEY, class VALUE>
class LRU {
 public:
  using list_type = std::list<std::pair<KEY, VALUE>>;

  /// Add the given key/value pair to the front of the LRU.
  void Add(const KEY &key, VALUE &value);

  /// Delete the given key from the LRU.
  bool Delete(const KEY &key) noexcept;

  /// Bump the object with the given key to the front of the LRU.
  void Get(const KEY &key);

  /// @returns true if the given key exists in the LRU.
  bool Contains(const KEY &key) const;

  /// Returns the least recently used key/value pair.
  std::pair<KEY, VALUE> LeastRecentlyUsed() { return m_items.back(); }

  size_t size() const { return m_items.size(); }

  typename list_type::iterator begin() { return m_items.begin(); }
  typename list_type::iterator end() { return m_items.end(); }

 private:
  list_type m_items;
  std::unordered_map<KEY, typename list_type::iterator> m_item_map;
};

#endif
