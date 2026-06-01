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

#ifndef __CDE_INSTANCE_H
#define __CDE_INSTANCE_H

#include "common/cde_session.h"
#include "framework/dstore_instance_interface.h"
#include "framework/dstore_thread_interface.h"
namespace CDE {
extern unsigned int g_numObjSpaceMgrWorkers;
extern char *g_tenantConfig;
extern unsigned long int g_walLevel;
extern unsigned long int g_logMinMessages;
extern unsigned long int g_foldLevel;
extern unsigned long int g_perfLevel;
extern unsigned long int g_lastAccessMode;
extern unsigned int g_perfCounterInterval;
extern unsigned long int g_walThrottlingMode;
extern unsigned long int g_flushDataMethod;
extern DSTORE::StorageGUC g_guc;
extern char *g_dstoreLogPath;
extern bool g_useDefaultTemplatePDB;
extern bool g_enable_btree_trace;
extern char *g_walDirConfig;

int CdeStartupDstoreInstance(bool optInitialize);
void CdeShutdownDstoreInstance(bool optInitialize);

DSTORE::StorageInstanceInterface *CdeGetDstoreInstance();

void CdeThreadlocalCreateKey();
void CdeThreadlocalDeleteKey();

void CdeConstructSession(cde_session_t *&cdeSessionRef);
int CdeDestorySession(cde_session_t *&cdeSessionRef);
void CdeDestroyDstoreThrd();
void CdeConstructDstoreThrd();
/*
 * @brief used to check whether the thrd is valid
 * @param thrd: the thread context to be checked
 *
 * @return true if thrd is valid, otherwise false
 */
bool CdeCheckThrdValid(DSTORE::ThreadContextInterface *thrd);
int CdeModuleInit();
int CdeModuleExit();

void CdeRegisterWriteExtraCommitWalCallback(
    DSTORE::WriteExtraWalCallback callback);
void CdeRegisterDbugPushdownCallback(DSTORE::DbugPushDownCallback callback);
void CdeRegisterUndoExtraCallback(DSTORE::UndoExtraCallback callback);

void CdeCreateUserPdb();

} /* namespace CDE */
#endif  // __CDE_INSTANCE_H
