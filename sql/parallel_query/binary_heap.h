/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in license.xml
   elsewhere in this distribution.  You may use this software under
   the terms of the GNU General Public License, version 2.0,
   or the terms of any other license that is available in this distribution.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef SQL_PARALLEL_QUERY_BINARY_HEAP_INCLUDED
#define SQL_PARALLEL_QUERY_BINARY_HEAP_INCLUDED

#include <cassert>
#include <vector>

typedef bool (*binaryheap_comparator)(int a, int b, void *arg);

class binary_heap {
 public:
  binary_heap(int capacity, void *arg, binaryheap_comparator cmp)
      : m_capacity(capacity), m_compare(cmp), m_arg(arg) {}

  bool init_binary_heap() {
    if (m_capacity <= 0 || m_compare == nullptr) return true;
    m_queue.reserve(static_cast<size_t>(m_capacity));
    return false;
  }

  void reset() { m_queue.clear(); }
  unsigned int size() const { return static_cast<unsigned int>(m_queue.size()); }
  bool empty() const { return m_queue.empty(); }

  int left_child(int i) const {
    const int idx = left(i);
    return idx < static_cast<int>(m_queue.size()) ? m_queue[idx] : -1;
  }

  int right_child(int i) const {
    const int idx = right(i);
    return idx < static_cast<int>(m_queue.size()) ? m_queue[idx] : -1;
  }

  bool add_unordered(int element) {
    if (static_cast<int>(m_queue.size()) >= m_capacity) return true;
    m_queue.push_back(element);
    return false;
  }

  bool add(int element) {
    if (add_unordered(element)) return true;
    sift_up(static_cast<int>(m_queue.size()) - 1);
    return false;
  }

  void build() {
    if (m_queue.size() < 2) return;
    for (int i = parent(static_cast<int>(m_queue.size()) - 1); i >= 0; --i) {
      sift_down(i);
    }
  }

  int first() const {
    assert(!empty());
    return m_queue[0];
  }

  int remove_first() {
    assert(!empty());
    if (m_queue.size() == 1) {
      const int result = m_queue[0];
      m_queue.pop_back();
      return result;
    }

    swap_node(0, static_cast<int>(m_queue.size()) - 1);
    const int result = m_queue.back();
    m_queue.pop_back();
    sift_down(0);
    return result;
  }

  void replace_first(int element, bool direct_read = false) {
    assert(!empty());
    m_queue[0] = element;
    if (m_queue.size() > 1 && !direct_read) sift_down(0);
  }

 private:
  static int parent(int i) {
    assert(i > 0);
    return (i - 1) >> 1;
  }

  static int left(int i) { return (i << 1) + 1; }
  static int right(int i) { return (i << 1) + 2; }

  void swap_node(int a, int b) {
    const int tmp = m_queue[a];
    m_queue[a] = m_queue[b];
    m_queue[b] = tmp;
  }

  void sift_down(int node_off) {
    for (;;) {
      const int left_off = left(node_off);
      const int right_off = right(node_off);
      int swap_off = node_off;

      if (left_off < static_cast<int>(m_queue.size()) &&
          m_compare(m_queue[left_off], m_queue[swap_off], m_arg)) {
        swap_off = left_off;
      }
      if (right_off < static_cast<int>(m_queue.size()) &&
          m_compare(m_queue[right_off], m_queue[swap_off], m_arg)) {
        swap_off = right_off;
      }
      if (swap_off == node_off) break;

      swap_node(swap_off, node_off);
      node_off = swap_off;
    }
  }

  void sift_up(int node_off) {
    while (node_off != 0) {
      const int parent_off = parent(node_off);
      if (!m_compare(m_queue[node_off], m_queue[parent_off], m_arg)) break;
      swap_node(node_off, parent_off);
      node_off = parent_off;
    }
  }

  std::vector<int> m_queue;
  int m_capacity{0};
  binaryheap_comparator m_compare{nullptr};
  void *m_arg{nullptr};
};

#endif  // SQL_PARALLEL_QUERY_BINARY_HEAP_INCLUDED
