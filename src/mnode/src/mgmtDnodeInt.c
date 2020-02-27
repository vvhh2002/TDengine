/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "os.h"
#include "taoserror.h"
#include "tsched.h"
#include "tstatus.h"
#include "tsystem.h"
#include "tutil.h"
#include "dnode.h"
#include "mnode.h"
#include "mgmtBalance.h"
#include "mgmtDb.h"
#include "mgmtDnode.h"
#include "mgmtDnodeInt.h"
#include "mgmtTable.h"
#include "mgmtVgroup.h"

void (*mgmtSendMsgToDnodeFp)(int8_t msgType, void *pCont, int32_t contLen, void *ahandle) = NULL;
void (*mgmtSendRspToDnodeFp)(void *handle, int32_t code, void *pCont, int32_t contLen) = NULL;
void *mgmtStatusTimer = NULL;

static void mgmtSendMsgToDnodeQueueFp(SSchedMsg *sched) {
  int32_t contLen  = *(int32_t *) (sched->msg - 4);
  int32_t code     = *(int32_t *) (sched->msg - 8);
  int8_t  msgType  = *(int8_t *) (sched->msg - 9);
  void    *ahandle = sched->ahandle;
  int8_t  *pCont   = sched->msg;

  dnodeProcessMsgFromMgmt(msgType, pCont, contLen, ahandle, code);
  rpcFreeCont(sched->msg);
}

void mgmtSendMsgToDnode(int8_t msgType, void *pCont, int32_t contLen, void *ahandle) {
  mTrace("msg:%s is sent to dnode", taosMsg[msgType]);
  if (mgmtSendMsgToDnodeFp) {
    mgmtSendMsgToDnodeFp(msgType, pCont, contLen, ahandle);
  } else {
    SSchedMsg schedMsg = {0};
    schedMsg.fp      = mgmtSendMsgToDnodeQueueFp;
    schedMsg.msg     = pCont;
    schedMsg.ahandle = ahandle;
    *(int32_t *) (pCont - 4) = contLen;
    *(int32_t *) (pCont - 8) = TSDB_CODE_SUCCESS;
    *(int8_t *)  (pCont - 9) = msgType;
    taosScheduleTask(tsDnodeMgmtQhandle, &schedMsg);
  }
}

void mgmtSendRspToDnode(void *pConn, int8_t msgType, int32_t code, void *pCont, int32_t contLen) {
  mTrace("rsp:%s is sent to dnode", taosMsg[msgType]);
  if (mgmtSendRspToDnodeFp) {
    mgmtSendRspToDnodeFp(pConn, code, pCont, contLen);
  } else {
    SSchedMsg schedMsg = {0};
    schedMsg.fp  = mgmtSendMsgToDnodeQueueFp;
    schedMsg.msg = pCont;
    *(int32_t *) (pCont - 4) = contLen;
    *(int32_t *) (pCont - 8) = code;
    *(int8_t *)  (pCont - 9) = msgType;
    taosScheduleTask(tsDnodeMgmtQhandle, &schedMsg);
  }
}

static void mgmtProcessTableCfgMsg(int8_t msgType, int8_t *pCont, int32_t contLen, void *pConn) {
  STableCfgMsg *pCfg = (STableCfgMsg *) pCont;
  pCfg->dnode = htonl(pCfg->dnode);
  pCfg->vnode = htonl(pCfg->vnode);
  pCfg->sid   = htonl(pCfg->sid);
  mTrace("dnode:%s, vnode:%d, sid:%d, receive table config msg", taosIpStr(pCfg->dnode), pCfg->vnode, pCfg->sid);

  if (!sdbMaster) {
    mError("dnode:%s, vnode:%d, sid:%d, not master, redirect it", taosIpStr(pCfg->dnode), pCfg->vnode, pCfg->sid);
    mgmtSendRspToDnode(pConn, msgType + 1, TSDB_CODE_REDIRECT, NULL, 0);
    return;
  }

  STableInfo *pTable = mgmtGetTableByPos(pCfg->dnode, pCfg->vnode, pCfg->sid);
  if (pTable == NULL) {
    mError("dnode:%s, vnode:%d, sid:%d, table not found", taosIpStr(pCfg->dnode), pCfg->vnode, pCfg->sid);
    mgmtSendRspToDnode(pConn, msgType + 1, TSDB_CODE_INVALID_TABLE, NULL, 0);
    return;
  }

  mgmtSendRspToDnode(pConn, msgType + 1, TSDB_CODE_SUCCESS, NULL, 0);
  mgmtSendCreateTableMsg(pTable, NULL);
}

static void mgmtProcessVnodeCfgMsg(int8_t msgType, int8_t *pCont, int32_t contLen, void *pConn) {
  if (!sdbMaster) {
    mgmtSendRspToDnode(pConn, msgType + 1, TSDB_CODE_REDIRECT, NULL, 0);
    return;
  }

  SVpeerCfgMsg *pCfg = (SVpeerCfgMsg *) pCont;
  pCfg->dnode = htonl(pCfg->dnode);
  pCfg->vnode = htonl(pCfg->vnode);

  SVgObj *pVgroup = mgmtGetVgroupByVnode(pCfg->dnode, pCfg->vnode);
  if (pVgroup == NULL) {
    mTrace("dnode:%s, vnode:%d, no vgroup info", taosIpStr(pCfg->dnode), pCfg->vnode);
    mgmtSendRspToDnode(pConn, msgType + 1, TSDB_CODE_NOT_ACTIVE_VNODE, NULL, 0);
    return;
  }

  mgmtSendRspToDnode(pConn, msgType + 1, TSDB_CODE_SUCCESS, NULL, 0);
  mgmtSendVPeersMsg(pVgroup, pCfg->vnode, NULL);
}

static void mgmtProcessCreateTableRsp(int8_t msgType, int8_t *pCont, int32_t contLen, void *thandle, int32_t code) {
  mTrace("create table rsp received, handle:%p code:%d", thandle, code);
}

static void mgmtProcessRemoveTableRsp(int8_t msgType, int8_t *pCont, int32_t contLen, void *thandle, int32_t code) {
  mTrace("remove table rsp received, handle:%p code:%d", thandle, code);
}

static void mgmtProcessFreeVnodeRsp(int8_t msgType, int8_t *pCont, int32_t contLen, void *thandle, int32_t code) {
  mTrace("free vnode rsp received, handle:%p code:%d", thandle, code);
}

static void mgmtProcessVPeersRsp(int8_t msgType, int8_t *pCont, int32_t contLen, void *thandle, int32_t code) {
  mTrace("vpeers rsp received, handle:%p code:%d", thandle, code);
}

void mgmtProcessMsgFromDnode(char msgType, void *pCont, int32_t contLen, void *handle, int32_t code) {
  if (msgType == TSDB_MSG_TYPE_TABLE_CFG) {
    mgmtProcessTableCfgMsg(msgType, pCont, contLen, handle);
  } else if (msgType == TSDB_MSG_TYPE_VNODE_CFG) {
    mgmtProcessVnodeCfgMsg(msgType, pCont, contLen, handle);
  } else if (msgType == TSDB_MSG_TYPE_DNODE_CREATE_TABLE_RSP) {
    mgmtProcessCreateTableRsp(msgType, pCont, contLen, handle, code);
  } else if (msgType == TSDB_MSG_TYPE_DNODE_REMOVE_TABLE_RSP) {
    mgmtProcessRemoveTableRsp(msgType, pCont, contLen, handle, code);
  } else if (msgType == TSDB_MSG_TYPE_DNODE_VPEERS_RSP) {
    mgmtProcessVPeersRsp(msgType, pCont, contLen, handle, code);
  } else if (msgType == TSDB_MSG_TYPE_DNODE_FREE_VNODE_RSP) {
    mgmtProcessFreeVnodeRsp(msgType, pCont, contLen, handle, code);
  } else if (msgType == TSDB_MSG_TYPE_DNODE_CFG_RSP) {
  } else if (msgType == TSDB_MSG_TYPE_ALTER_STREAM_RSP) {
  } else {
    mError("%s from dnode is not processed", taosMsg[msgType]);
  }
}

void mgmtSendCreateTableMsg(STableInfo *pTable, SRpcIpSet *ipSet, void *handle) {
  mTrace("table:%s, sid:%d send create table msg, handle:%p", pTable->tableId, pTable->sid);

  SDCreateTableMsg *pCreate = mgmtBuildCreateTableMsg(pTable);
  if (pCreate != NULL) {
    mgmtSendMsgToDnode(TSDB_MSG_TYPE_DNODE_CREATE_TABLE, pCreate, htonl(pCreate->contLen), handle);
  }
}

void mgmtSendRemoveTableMsg(STableInfo *pTable, SRpcIpSet *ipSet, void *handle) {
  mTrace("table:%s, sid:%d send remove table msg, handle:%p", pTable->tableId, pTable->sid);

  SDRemoveTableMsg *pRemove = mgmtBuildRemoveTableMsg(pTable);
  if (pRemove != NULL) {
    mgmtSendMsgToDnode(TSDB_MSG_TYPE_DNODE_REMOVE_TABLE, pRemove, sizeof(SDRemoveTableMsg), handle);
  }
}

void mgmtSendAlterStreamMsg(STableInfo *pTable, SRpcIpSet *ipSet, void *handle) {
  mTrace("table:%s, sid:%d send alter stream msg, handle:%p", pTable->tableId, pTable->sid);
}

void mgmtSendVPeersMsg(SVgObj *pVgroup, int32_t vnode, SRpcIpSet *ipSet, void *handle) {
  mTrace("vgroup:%d, vnode:%d send vpeer msg, handle:%p", pVgroup->vgId, vnode, handle);

  SVPeersMsg *pVpeer = mgmtBuildVpeersMsg(pVgroup, vnode);
  if (pVpeer != NULL) {
    mgmtSendMsgToDnode(TSDB_MSG_TYPE_DNODE_VPEERS, pVpeer, sizeof(SVPeersMsg), handle);
  }
}

void mgmtSendOneFreeVnodeMsg(int32_t vnode, SRpcIpSet *ipSet, void *handle) {
  mTrace("vnode:%d send free vnode msg, handle:%p", vnode, handle);

  SFreeVnodeMsg *pFreeVnode = rpcMallocCont(sizeof(SFreeVnodeMsg));
  if (pFreeVnode != NULL) {
    pFreeVnode->vnode = htonl(vnode);
    mgmtSendMsgToDnode(TSDB_MSG_TYPE_DNODE_FREE_VNODE, pFreeVnode, sizeof(SFreeVnodeMsg), handle);
  }
}

void mgmtSendFreeVnodesMsg(SVgObj *pVgroup, SRpcIpSet *ipSet, void *handle) {
  for (int32_t i = 0; i < pVgroup->numOfVnodes; ++i) {
    SRpcIpSet ipSet = mgmtGetIpSetFromIp(pVgroup->vnodeGid[i].ip);
    mgmtSendOneFreeVnodeMsg(pVgroup->vnodeGid + i, &ipSet, handle);
  }
}

int32_t mgmtCfgDynamicOptions(SDnodeObj *pDnode, char *msg) {
  char *option, *value;
  int32_t   olen, valen;

  paGetToken(msg, &option, &olen);
  if (strncasecmp(option, "unremove", 8) == 0) {
    mgmtSetDnodeUnRemove(pDnode);
    return TSDB_CODE_SUCCESS;
  } else if (strncasecmp(option, "score", 5) == 0) {
    paGetToken(option + olen + 1, &value, &valen);
    if (valen > 0) {
      int32_t score = atoi(value);
      mTrace("dnode:%s, custom score set from:%d to:%d", taosIpStr(pDnode->privateIp), pDnode->customScore, score);
      pDnode->customScore = score;
      mgmtUpdateDnode(pDnode);
      mgmtStartBalanceTimer(15);
    }
    return TSDB_CODE_INVALID_SQL;
  } else if (strncasecmp(option, "bandwidth", 9) == 0) {
    paGetToken(msg, &value, &valen);
    if (valen > 0) {
      int32_t bandwidthMb = atoi(value);
      if (bandwidthMb >= 0 && bandwidthMb < 10000000) {
        mTrace("dnode:%s, bandwidth(Mb) set from:%d to:%d", taosIpStr(pDnode->privateIp), pDnode->bandwidthMb, bandwidthMb);
        pDnode->bandwidthMb = bandwidthMb;
        mgmtUpdateDnode(pDnode);
        return TSDB_CODE_SUCCESS;
      }
    }
    return TSDB_CODE_INVALID_SQL;
  }

  return -1;
}

int32_t mgmtSendCfgDnodeMsg(char *cont) {
#ifdef CLUSTER
  char *     pMsg, *pStart;
  int32_t        msgLen = 0;
#endif

  SDnodeObj *pDnode;
  SCfgDnodeMsg *  pCfg = (SCfgDnodeMsg *)cont;
  uint32_t   ip;

  ip = inet_addr(pCfg->ip);
  pDnode = mgmtGetDnode(ip);
  if (pDnode == NULL) {
    mError("dnode ip:%s not configured", pCfg->ip);
    return TSDB_CODE_NOT_CONFIGURED;
  }

  mTrace("dnode:%s, dynamic option received, content:%s", taosIpStr(pDnode->privateIp), pCfg->config);
  int32_t code = mgmtCfgDynamicOptions(pDnode, pCfg->config);
  if (code != -1) {
    return code;
  }

#ifdef CLUSTER
  pStart = taosBuildReqMsg(pDnode->thandle, TSDB_MSG_TYPE_DNODE_CFG);
  if (pStart == NULL) return TSDB_CODE_NODE_OFFLINE;
  pMsg = pStart;

  memcpy(pMsg, cont, sizeof(SCfgDnodeMsg));
  pMsg += sizeof(SCfgDnodeMsg);

  msgLen = pMsg - pStart;
  mgmtSendMsgToDnode(pDnode, pStart, msgLen);
#else
  (void)tsCfgDynamicOptions(pCfg->config);
#endif
  return 0;
}

int32_t mgmtInitDnodeIntImp() { return 0; }
int32_t (*mgmtInitDnodeInt)() = mgmtInitDnodeIntImp;

void mgmtCleanUpDnodeIntImp() {}
void (*mgmtCleanUpDnodeInt)() = mgmtCleanUpDnodeIntImp;

void mgmtProcessDnodeStatusImp(void *handle, void *tmrId) {
/*
  SDnodeObj *pObj = &tsDnodeObj;
  pObj->openVnodes = tsOpenVnodes;
  pObj->status = TSDB_DN_STATUS_READY;

  float memoryUsedMB = 0;
  taosGetSysMemory(&memoryUsedMB);
  pObj->diskAvailable = tsAvailDataDirGB;

  for (int32_t vnode = 0; vnode < pObj->numOfVnodes; ++vnode) {
    SVnodeLoad *pVload = &(pObj->vload[vnode]);
    SVnodeObj * pVnode = vnodeList + vnode;

    // wait vnode dropped
    if (pVload->dropStatus == TSDB_VN_DROP_STATUS_DROPPING) {
      if (vnodeList[vnode].cfg.maxSessions <= 0) {
        pVload->dropStatus = TSDB_VN_DROP_STATUS_READY;
        pVload->status = TSDB_VN_STATUS_OFFLINE;
        mPrint("dnode:%s, vid:%d, drop finished", taosIpStr(pObj->privateIp), vnode);
        taosTmrStart(mgmtMonitorDbDrop, 10000, NULL, tsMgmtTmr);
      }
    }

    if (vnodeList[vnode].cfg.maxSessions <= 0) {
      continue;
    }

    pVload->vnode = vnode;
    pVload->status = TSDB_VN_STATUS_MASTER;
    pVload->totalStorage = pVnode->vnodeStatistic.totalStorage;
    pVload->compStorage = pVnode->vnodeStatistic.compStorage;
    pVload->pointsWritten = pVnode->vnodeStatistic.pointsWritten;
    uint32_t vgId = pVnode->cfg.vgId;

    SVgObj *pVgroup = mgmtGetVgroup(vgId);
    if (pVgroup == NULL) {
      mError("vgroup:%d is not there, but associated with vnode %d", vgId, vnode);
      pVload->dropStatus = TSDB_VN_DROP_STATUS_DROPPING;
      continue;
    }

    SDbObj *pDb = mgmtGetDb(pVgroup->dbName);
    if (pDb == NULL) {
      mError("vgroup:%d not belongs to any database, vnode:%d", vgId, vnode);
      continue;
    }

    if (pVload->vgId == 0 || pVload->dropStatus == TSDB_VN_DROP_STATUS_DROPPING) {
      mError("vid:%d, mgmt not exist, drop it", vnode);
      pVload->dropStatus = TSDB_VN_DROP_STATUS_DROPPING;
    }
  }

  taosTmrReset(mgmtProcessDnodeStatus, tsStatusInterval * 1000, NULL, tsMgmtTmr, &mgmtStatusTimer);
  if (mgmtStatusTimer == NULL) {
    mError("Failed to start status timer");
  }
*/
}
void (*mgmtProcessDnodeStatus)(void *handle, void *tmrId) = mgmtProcessDnodeStatusImp;