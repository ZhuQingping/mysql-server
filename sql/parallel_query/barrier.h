#ifndef SQL_BARRIER_H
#define SQL_BARRIER_H

/* Copyright (c) 2021, Huawei and/or its affiliates. All rights reserved.

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

#include <cassert>
#include <condition_variable>
#include <functional>
#include <mutex>

/// From Wikipedia[1]: "In parallel computing, a barrier is a type of
/// synchronization method. A barrier for a group of threads or processes in the
/// source code means any thread/process must stop at this point and cannot
/// proceed until all other threads/processes reach this barrier."
///
/// This class provides a simple barrier implementation. When C++20 becomes
/// available, this class should probably be replaced with std::barrier.
///
/// [1] https://en.wikipedia.org/wiki/Barrier_(computer_science)
class Barrier {
 public:
  explicit Barrier(int num_participants)
      : m_num_workers(num_participants),
        m_remaining_workers(num_participants) {}

  /// Reset the barrier back to its initial state, and set the number of
  /// expected threads to "num_participants". Note that if "this" is in use
  /// (that is, there are active threads that are currently waiting, or is about
  /// to wait on "m_cond"), we will end up in unpredictable behavior if this
  /// method is being called. So use this only when there are no active threads
  /// using it!
  void Reset(int num_participants) {
    // Assert that we are not resetting a Barrier in use.
    assert(m_remaining_workers == m_num_workers);
    m_num_workers = num_participants;
    m_remaining_workers = num_participants;
    m_generation++;
  }

  /// Decrement the number of waiting/remaining workers by one. When the count
  /// becomes zero, all waiting threads are woken up. An optional completion
  /// function can be provided, which is run when the count becomes zero.
  void ArriveAndWait(const std::function<void()> &completion_function) {
    std::unique_lock<std::mutex> lock(m_mutex);

    assert(m_remaining_workers > 0);
    assert(m_remaining_workers <= m_num_workers);
    int gen = m_generation;
    --m_remaining_workers;
    if (m_remaining_workers == 0) {
      // Reset the barrier for a next round. Change the generation's number,
      // so that any other waiting thread notices that it's a new one and thus
      // does not wait on it.
      ++m_generation;
      m_remaining_workers = m_num_workers;

      if (completion_function != nullptr) {
        completion_function();
      }

      // Notify the workers waiting on m_cond that the completion function has
      // been executed.
      m_execute_completion_function = false;

      m_cond.notify_all();
      return;
    }

    // The while-loop is here to guard against spurious wakeups.
    while (gen == m_generation) {
      m_cond.wait(lock);
    }

    if (m_execute_completion_function && completion_function != nullptr) {
      completion_function();

      // Notify the other workers waiting on m_cond that the completion function
      // has been executed.
      m_execute_completion_function = false;
    }
  }

  /// Decrement the number of waiting/remaining workers for both this phase and
  /// any subsequent phase by one. When the count becomes zero, all waiting
  /// threads are woken up. In contrast to ArriveAndWait(), calling this
  /// function will not make the thread wait for other threads to arrive at this
  /// point.
  void ArriveAndDrop() {
    std::unique_lock<std::mutex> lock(m_mutex);

    assert(m_num_workers > 0);
    assert(m_remaining_workers > 0);
    assert(m_remaining_workers <= m_num_workers);
    --m_remaining_workers;
    --m_num_workers;
    if (m_remaining_workers == 0) {
      // Reset the barrier for a next round. Change the generation's number,
      // so that any other waiting thread notices that it's a new one and thus
      // does not wait on it.
      ++m_generation;
      m_remaining_workers = m_num_workers;

      // Before waking up any waiting worker, set a flag saying that the first
      // worker to wake up must execute the completion function. Without this,
      // no one will execute the completion function, because all the workers
      // (except this one) are waiting on m_cond signal, which is in a different
      // branch than the execution of the completion function.
      m_execute_completion_function = true;
      m_cond.notify_all();
    }
  }

 private:
  int m_num_workers;
  int m_remaining_workers;
  std::mutex m_mutex;
  std::condition_variable m_cond;
  int m_generation{0};
  bool m_execute_completion_function{false};
};

#endif
