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

#include <condition_variable>
#include <mutex>
#include <thread>

#include "cde_dict.h"
#include "dict/cde_dict_bg_evict.h"
#include "sql/sql_class.h"
#include "sql/sql_thd_internal_api.h"

namespace CDE {

class BgEvictService {
 public:
  BgEvictService() {}
  /** Start eviction thread. */
  virtual void Start();
  /** Stop eviction thread. */
  virtual void Stop();
  virtual ~BgEvictService() {}

 private:
  /** Runs background evicition thread*/
  void BgProcess();

  std::condition_variable m_dictBgEvictCV;
  std::mutex m_mtx;

  std::thread m_worker;
  bool m_stopFlag{false};

  const std::chrono::seconds EVICT_DICT_LRU_INTERVAL{47};
};

void BgEvictService::Start() {
  CDE_LOG_INFO_NOT_SECURITY_FILTER("bg evict service start");
  m_stopFlag = false;
  m_worker = std::thread(std::bind(&BgEvictService::BgProcess, this));
}

void BgEvictService::Stop() {
  CDE_LOG_INFO("bg evict service stop -- begin");
  m_stopFlag = true;
  m_dictBgEvictCV.notify_one();

  if (m_worker.joinable()) m_worker.join();
  CDE_LOG_INFO("bg evict service stop -- end");
}

void BgEvictService::BgProcess() {
  CDE_LOG_INFO("begin bg evict service");

  my_thread_init();
  THD *bgThd = create_internal_thd();
  bgThd->set_new_thread_id();
  // set sql_mode to 0 in system background update histogram
  bgThd->variables.sql_mode = 0;

  // dstore will set thread name, here we reset
  pthread_setname_np(pthread_self(), "cde_bg_evict");

  std::unique_lock<std::mutex> lck(m_mtx);

  do {
    DBUG_EXECUTE_IF("run_eviction", DictSysFreeSpaceInCache(););
    m_dictBgEvictCV.wait_for(lck, EVICT_DICT_LRU_INTERVAL);
    DictSysFreeSpaceInCache();
  } while (!m_stopFlag);

  destroy_internal_thd(bgThd);
  my_thread_end();
  CDE_LOG_INFO("quit bg evict service");
}

static BgEvictService bgEvictSrv;

void CdeBgEvictSrvInit() { bgEvictSrv.Start(); }

void CdeBgEvictSrvDeinit() { bgEvictSrv.Stop(); }

} /* namespace CDE */