
#include <common.h>

#include "ge.h"
#include "../../ge_vita_shared.h"

// FIXME: this is cached memory, is that correct?
#define GE_VITA_EDRAM_ADDR    0x0a000000
#define GE_VITA_EDRAM_SIZE    0x200000

#define GE_CMD(x) ((x) >> 24)
#define GE_SIGNAL_TYPE(x) (((x) >> 16) & 0xff)
#define GE_SIGNAL_PARAM(x) ((x) & 0xffff)

int edram_width = 0x400; 

typedef struct {
	SceGeDisplayList *curRunning;
	int isBreak;
	SceGeDisplayList *active_first;
	SceGeDisplayList *active_last;
	SceGeDisplayList *free_first;
	SceGeDisplayList *free_last;
	SceUID drawing_flag;
	SceUID listEvFlagIds[2];
	SceGeStack stack[32];
	int sdkVer;                 // 1060
	int patched;                // 1064
	int syscallId;
	SceGeLazy *lazySyncData;
} SceGeQueue;

// FIXME: can we map these to original SceGe module structures? (save memory)
SceGeContext ge_vita_ctx;
SceGeDisplayList ge_vita_dl[64];
SceGeQueue ge_vita_queue;
int ge_vita_dl_mask;


// original functions to be stored here
int (*sceGeEdramGetAddr_orig)(void);
int (*sceGeEdramGetSize_orgi)(void);
int (*sceGeEdramSetAddrTranslation_orig)(int arg)
int (*sceGeListEnQueue_orig)(void *list, void *stall, int cbid, SceGeListArgs *arg);
int (*sceGeListEnQueueHead_orig)(void *list, void *stall, int cbid, SceGeListArgs *arg);
int (*sceGeListDeQueue_orig)(int dlId);
int (*sceGeListUpdateStallAddr_orig)(int dlId, void *stall);
SceGeListState (*sceGeListSync_orig)(int dlId, int mode);
SceGeListState (*sceGeDrawSync_orig)(int syncType);
int (*sceGeBreak_orig)(u32 resetQueue, void *arg1);
int (*sceGeContinue_orig)(void);
int (*sceGeGetCmd_orig)(u32 cmdOff);
int (*sceGeGetMtx_orig)(int id, int *mtx);
int (*sceGeGetStack_orig)(int stackId, SceGeStack *stack);
int (*sceGeSaveContext_orig)(SceGeContext *ctx);
int (*sceGeRestoreContext_orig)(SceGeContext *ctx);

static int ge_vita_send_request(int cmd, unsigned int arg1, unsigned int arg2) {
	int k1 = pspSdkSetK1(0);

	char buf[sizeof(SceKermitRequest) + 0x40];

	SceKermitRequest *request_aligned = (SceKermitRequest *)ALIGN((u32)buf, 0x40);
	SceKermitRequest *request_uncached = (SceKermitRequest *)((u32)request_aligned | 0x20000000);

	request_aligned->args[0] = arg1;
	request_aligned->args[1] = arg2;

	sceKernelDcacheInvalidateRange(request_aligned, sizeof(SceKermitRequest));

	u64 resp;
	sceKermitSendRequest661(request_uncached, KERMIT_MODE_EXTRA_1, cmd, 2, 0, &resp);

	pspSdkSetK1(k1);
	return resp;
}

int ge_vita_get_int(void)
{
	return ge_vita_send_request(GE_VITA_CMD_REG_GET, GE_VITA_REG_INT);
}

void ge_vita_reset(void)
{
	ge_vita_send_request(GE_VITA_CMD_RESET, 0, 0);
}

void ge_vita_sync(void)
{
	ge_vita_send_request(GE_VITA_CMD_SYNC, 0, 0);
}

void ge_vita_cmd(int cmd)
{
	ge_vita_send_request(GE_VITA_CMD_REG_SET, GE_VITA_REG_CMD, cmd);
}

void ge_vita_reg_set(int reg, unsigned int value)
{
	ge_vita_send_request(GE_VITA_CMD_REG_SET, reg, value);
}

void ge_vita_reg_get(int reg, unsigned int *value)
{
	*value = ge_vita_send_request(GE_VITA_CMD_REG_GET, reg);
}

int ge_vita_stack_save(SceGeStack *s)
{
	ge_vita_send_request(GE_VITA_CMD_STACK_SAVE, s);
}

int ge_vita_stack_restore(SceGeStack *s)
{
	ge_vita_send_request(GE_VITA_CMD_STACK_RESTORE, s);
}

int ge_vita_ctx_save(SceGeContext *ctx)
{
	ge_vita_send_request(GE_VITA_CMD_CTX_SAVE, s);
}

int ge_vita_ctx_restore(SceGeContext *ctx)
{
	ge_vita_send_request(GE_VITA_CMD_CTX_RESTORE, s);
}

void ge_vita_mtx_get(int id, int *mtx)
{
	ge_vita_send_request(GE_VITA_CMD_MTX_GET, id, mtx);
}

int ge_vita_cmd_get(u32 cmd)
{
	ge_vita_send_request(GE_VITA_CMD_CMD_GET, id);
}

int sceGeEdramGetAddr_patched(void)
{
	return GE_VITA_EDRAM_ADDR;
}

int sceGeEdramGetSize_patched(void)
{
	return GE_VITA_EDRAM_SIZE;
}

int sceGeEdramSetAddrTranslation_patched(int arg)
{
	int last = edram_width;
	edram_width = arg;
	return last;
}

static void sceGeQueueInit(void)
{
	int i;

	for (i = 0; i < 4; i++) {
		SceGeDisplayList *dl = &ge_vita_dl[i];
		dl->next = 0;
		dl->prev = 0;

		if (i < 63) {
			dl->next = &ge_vita_dl[i+1];
		}
		if (i > 0) {
			dl->prev = &ge_vita_dl[i-1];
		}
		dl->signal = SCE_GE_DL_SIGNAL_NONE;
		dl->state = SCE_GE_DL_STATE_NONE;
		dl->ctxUpToDate = 0;
	}

	ge_vita_queue.curRunning = NULL;
	ge_vita_queue.free_first = &ge_vita_dl[0];
	ge_vita_queue.free_last = &ge_vita_dl[63];
	ge_vita_queue.active_first = NULL;
	ge_vita_queue.active_last = NULL;
}

static int _sceGeListEnQueue(void *list, void *stall, int cbid, SceGeListArgs *arg, int head)
{
	int old_intr, old_k1, ret;
	SceGeDisplayList *dl;
	
	int ver = sceKernelGetCompiledSdkVersion();
	void *stack = ge_vita_queue.stack;
	SceGeContext *ctx = NULL;
	int numStacks = 32;
	int oldver = 1;

	if (ver > 0x01ffffff) {
		oldver = 0;
		stack = NULL;
		numStacks = 0;
	}

	old_k1 = pspShiftK1();

	// check pointers are ok
	if (!pspK1PtrOk(list) || !pspK1PtrOk(stall) || !pspK1StaBufOk(arg, 16)) {
		pspSetK1(old_k1);
		return 0x80000023;
	}

	// check sanity of arguments
	if (arg != NULL) {
		ctx = arg->ctx;
		if (!pspK1StaBufOk(ctx, sizeof(SceGeContext))) {
			pspSetK1(old_k1);
			return 0x80000023;
		}

		if (arg->size >= 16) {
			numStacks = arg->numStacks;
			if (numStacks >= 256) {
				pspSetK1(old_k1);
				return 0x80000104;
			}
			if (!pspK1DynBufOk(arg->stacks, numStacks * sizeof(SceGeStack))) {
			pspSetK1(old_k1);
			return 0x80000023;
			}
		}
	}

	// check alignment
	if ((((int)list | (int)stall | (int)ctx | (int)stack) & 3) != 0) {
		pspSetK1(old_k1);
		return 0x80000103;
	}

	old_intr = sceKernelCpuSuspendIntr();

	// check list and stack is sane
	for (dl = ge_vita_queue.active_first; dl != NULL; dl = dl->next) {
		if (UCACHED(dl->list ^ list) == NULL) {
			if (oldver) {
				break;
			}
			sceKernelCpuResumeIntr(old_intr);
			pspSetK1(old_k1);
			return 0x80000021;
		}
		if (dl->ctxUpToDate && stack != NULL && dl->stack == stack && !oldVer) {
			sceKernelCpuResumeIntr(old_intr);
			pspSetK1(old_k1);
			return 0x80000021;
		}
	}

	dl = ge_vita_queue.free_first;
	if (dl == NULL) {
		sceKernelCpuResumeIntr(old_intr);
		pspSetK1(old_k1);
		return 0x80000022;
	}

	// was it last free in "free"LL ?
	if (dl->next == NULL) {
		ge_vita_queue.free_last = NULL;
		ge_vita_queue.free_first = NULL;
	} else {
		ge_vita_queue.free_first = ge_vita_queue.free_first->next;
		ge_vita_queue.free_first->prev = NULL;
	}

	dl->prev = NULL;
	dl->next = NULL;

	dl->ctxUpToDate = 0;
	dl->signal = SCE_GE_DL_SIGNAL_NONE;
	dl->cbId = pspMax(cbid, -1);
	dl->numStacks = numStacks;
	dl->stack = stack;
	dl->ctx = ctx;
	dl->flags = 0;
	dl->list = list;
	dl->stall = stall;
	dl->stackOff = 0;

	if (head) {
		// is something in the head?
		if (ge_vita_queue.active_first == NULL) {
			dl->state = SCE_GE_DL_STATE_PAUSED;
			ge_vita_queue.active_last = dl;
			ge_vita_queue.active_first = dl;
		} else {
			// is the head in correct state?
			if (ge_vita_queue.active_first->state != SCE_GE_DL_STATE_PAUSED) {
				sceKernelCpuResumeIntr(old_intr);
				pspSetK1(old_k1);
				return 0x800001FE;
			}

			dl->state = SCE_GE_DL_STATE_PAUSED;
			ge_vita_queue.active_first->state = SCE_GE_DL_STATE_QUEUED;
			ge_vita_queue.active_first->prev = dl;
			dl->next = ge_vita_queue.active_first;
			ge_vita_queue.active_first = dl;
		}
	} else if (ge_vita_queue.active_first == NULL) {
		dl->state = SCE_GE_DL_STATE_RUNNING;
		dl->ctxUpToDate = 1;
		ge_vita_queue.active_first = dl;
		ge_vita_queue.active_last = dl;

		// ge_vita do your work!
		ge_vita_reg_set(GE_VITA_REG_DLIST, dl->list);
		ge_vita_reg_set(GE_VITA_REG_STALLADDR, dl->stall);
		ge_vita_cmd(1);

		ge_vita_queue.curRunning = dl;
		sceKernelClearEventFlag(ge_vita_queue.drawing_flag, 0xFFFFFFFD);

	} else {
		// just put into end of queue
		dl->state = SCE_GE_DL_STATE_QUEUED;
		ge_vita_queue.active_last->next = dl;
		dl->prev = ge_vita_queue.active_last;
		ge_vita_queue.active_last = dl;
	}
	u32 idx = (dl - ge_vita_dl) / sizeof(SceGeDisplayList);
	sceKernelClearEventFlag(ge_vita_queue.list_done_flag[idx / 32], ~(1 << (idx % 32)));

	sceKernelCpuResumeIntr(old_intr);
	pspSetK1(old_k1);

	return ret;
}

int sceGeListEnQueue_patched(void *list, void *stall, int cbid, SceGeListArgs *arg)
{
	return _sceGeListEnQueue(list, stall, cbid, arg, 0);
}
int sceGeListEnQueueHead_patched(void *list, void *stall, int cbid, SceGeListArgs *arg)
{
	return _sceGeListEnQueue(list, stall, cbid, arg, 1);
}

int sceGeListUpdateStallAddr_patched(int dlId, void *stall)
{
	int old_intr, old_k1, ret;
	SceGeDisplayList *dl = (SceGeDisplayList *) dlId ^ ge_vita_dl_mask;

	old_k1 = pspShiftK1();
	if (dl < ge_vita_dl || dl >= &ge_vita_dl[64]) {
		pspSetK1(old_k1);
		return 0x80000100;
	}

	if (!pspK1PtrOk(stall)) {
		pspSetK1(old_k1);
		return 0x80000100;
	}

	dl->stall = stall;
	if (dl->state == SCE_GE_DL_STATE_PAUSED) {
		// tell ge_vita new stall addrress
		ge_vita_reg_set(GE_VITA_REG_STALLADDR, stall);
	}

	sceKernelCpuResumeIntr(old_intr);
	pspSetK1(old_k1);

	return 0;
}

SceGeListState sceGeListSync_patched(int dlId, int mode)
{
	int old_intr, old_k1, ret;
	SceGeDisplayList *dl = (SceGeDisplayList *) dlId ^ ge_vita_dl_mask;

	if (mode < 0 || mode > 2) {
		return 0x80000107;
	}	

	old_k1 = pspShiftK1();
	if (dl < ge_vita_dl || dl >= &ge_vita_dl[64]) {
		pspSetK1(old_k1);
		return 0x80000100;
	}

	if (mode == 0) {
		u32 idx = (dl - ge_vita_dl) / sizeof(SceGeDisplayList);
		ret = sceKernelWaitEventFlag(ge_vita_queue.list_done_flag[idx / 32], 1 << (idx % 32), 0, 0, 0);
	} else if (mode == 1) {
		// check status
		switch (dl->state) {
		case SCE_GE_DL_STATE_QUEUED;
			if (dl->ctxUpToDate) {
				ret = SCE_GE_LIST_PAUSED;
			} else {
				ret = SCE_GE_LIST_QUEUED;
			}
			break;
		case SCE_GE_DL_STATE_RUNNING:
			void *stall;

			ge_vita_reg_get(GE_VITA_REG_STALLADDR, &stall);

			if (dl->stall == stall) {
				ret = SCE_GE_LIST_STALLING;
			} else {
				ret = SCE_GE_LIST_DRAWING;
			}
			break;
		case SCE_GE_DL_STATE_COMPLETED;
			ret = SCE_GE_LIST_COMPLETED;
			break;
		case SCE_GE_DL_STATE_PAUSED;
			ret = SCE_GE_LIST_PAUSED;
			break;
		default:
			ret = 0x80000100;
			break;
		}
	}

	pspSetK1(old_k1);

	return ret;
}

int sceGeListDeQueue_patched(int dlId)
{
	int old_intr, old_k1, ret;
	SceGeDisplayList *dl = (SceGeDisplayList *) dlId ^ ge_vita_dl_mask;

	old_k1 = pspShiftK1();
	if (dl < ge_vita_dl || dl>= &ge_vita_dl[64]) {
		pspSetK1(old_k1);
		return 0x80000100;
	}

	old_intr = sceKernelCpuSuspendIntr();

	// check if dl is queued
	if (dl->state == SCE_GE_DL_STATE_NONE) {
		sceKernelCpuResumeIntr(old_intr);
		pspSetK1(old_k1);
		return 0x80000100;
	}

	if (dl->ctxUpToDate == 1) {
		sceKernelCpuResumeIntr(old_intr);
		pspSetK1(old_k1);
		return 0x80000021;
	}

	// remove from LL
	if (dl->prev != NULL) {
		dl->prev->next = dl->next;
	}
	if (dl->next != NULL) {
		dl->next->prev = dl->prev;
	}

	// check whether DL is first or last in queue
	if (ge_vita_queue.active_first == dl) {
		ge_vita_queue.active_first = dl->next;
	}
	if (ge_vita_queue.active_last == dl) {
		ge_vita_queue.active_last = dl->prev;
	}

	dl->prev = NULL;
	dl->next = NULL;

	// add dl to free LL
	if (ge_vita_queue.free_first == NULL) {
		ge_vita_queue.free_first = dl;
		ge_vita_queue.free_last = dl;
	} else {
		ge_vita_queue.free_last->next = dl;
		dl->prev = ge_vita_queue.free_last;
		ge_vita_queue.free_last = dl;
	}

	dl->state = SCE_GE_DL_STATE_NONE;

	sceKernelCpuResumeIntr(old_intr);
	pspSetK1(old_k1);

	return 0;
}

SceGeListState sceGeDrawSync_patch(int sync_type)
{
	int old_intr, old_k1, ret;

	old_k1 = pspShiftK1();
	if (sync_type < 0 || sync_type > 2) {
		pspSetK1(old_k1);
		return 0x80000107;
	}

	if (sync_type == 0) {
		// wait for end
		ret = sceKernelWaitEventFlag(ge_vita_queue.drawing_flag, 2, 0, 0, 0);
		if (ret >= 0) {
			int i;
			for (i = 0; i < 64; i++) {
				SceGeDisplayList *dl = ge_vita_dl[i];
				if (dl->state == SCE_GE_DL_STATE_COMPLETED) {
					dl->state = SCE_GE_DL_STATE_NONE;
					dl->ctxUpToDate = 0;
				}
			}
			ret = SCE_GE_LIST_COMPLETED;
		}
	} else if (sync_type == 1) {
		// check draw state
		old_intr = sceKernelCpuSuspendIntr();

		SceGeDisplayList *dl = ge_vita_queue.active_first;
		if (dl == NULL) {
			ret = SCE_GE_LIST_COMPLETED;
		} else {
			// TODO: sync dl?
			// FIXME: should this be while instead of if ?
			if (dl->state == SCE_GE_DL_STATE_COMPLETED) {
				dl = dl->next;
			}

			if (dl != NULL) {
				if (dl->stall != /* ge_vita_get_current stall */) {
					ret = SCE_GE_LIST_DRAWING;
				} else {
					ret = SCE_GE_LIST_STALLING;
				}
			} else {
				ret = SCE_GE_LIST_COMPLETED;
			}
		}

		sceKernelCpuResumeIntr(old_intr);
	}

	pspSetK1(old_k1);
	return ret;
}

int sceGeBreak_patch(u32 resetQueue, void *arg1)
{
	int old_intr, old_k1;
	int ret;

	old_k1 = pspShiftK1();
	if (!pspK1StaBufOk(arg1, 16)) {
		pspSetK1(old_k1);
		return 0x80000023;
	}

	old_intr = sceKernelCpuSuspendIntr();
	SceGeDisplayList *dl = g_AwQueue.active_first;
	if (dl == 0) {
		sceKernelCpuResumeIntr(old_intr);
		pspSetK1(old_k1);
		return 0x80000020;
	}

	if (resetQueue) {
		ge_vita_reset();
		sceGeQueueInit();
	} else if (dl->state == SCE_GE_DL_STATE_RUNNING) {
		// stop processing
		ge_vita_cmd(0);
		// wait for sync
		ge_vita_sync();

		if (ge_vita_queue.curRunning != NULL) {
			SceGeStack stack;

			ge_vita_stack_save(&stack);
			ge_vita_reg_get(GE_VITA_REG_STALLADDR, &dl->stall);
			
			// FIXME: all needed ?
			dl->state = s.cmd;
			dl->list = s.pc;
			dl->unk36 = s.offsetAddr;
			dl->unk48 = s.baseAddr;
			//dl->unk28 = s.stack[5];
			//dl->unk32 = s.stack[6];
			//dl->unk40 = s.stack[3];
			//dl->unk44 = s.stack[4];
			
			int op = *((char *)UUNCACHED(dl->list - 1) + 3);
			if (op == SCE_GE_CMD_SIGNAL || op == SCE_GE_CMD_FINISH) {
				dl->list = dl->list - 1;
			} else if (op == SCE_GE_CMD_END) {
				dl->list = dl->list - 2;
			}

			if (dl->signal == SCE_GE_DL_SIGNAL_SYNC) {
				dl->list += 2;
			}
		}

		dl->state = SCE_GE_DL_STATE_PAUSED;
		dl->signal = SCE_GE_DL_SIGNAL_BREAK;

		// TODO: missing part, wtf is g_cmdList ?!
		ge_vita_reg_get(GE_VITA_REG_STALLADDR, 0);
		ge_vita_reg_get(GE_VITA_REG_DLIST, xxx);
		ge_vita_cmd(1);

		ge_vita_queue.isBreak = 1;
		ge_vita_queue.curRunning = NULL;

		ret = (int)dl ^ ge_vita_dl_mask;
	} else if (dl->state == SCE_GE_DL_STATE_PAUSED) {
		ret = 0x80000021;
		if (ge_vita_queue.sdkVer > 0x02000010) {
			if (dl->signal != SCE_GE_DL_SIGNAL_PAUSE) {
				ret = 0x80000020;
			}
		}
	} else if (dl->state == SCE_GE_DL_STATE_QUEUED) {
		dl->state = SCE_GE_DL_STATE_PAUSED;
		ret = (int)dl & ge_vita_dl_mask;
	} else if (ge_vita_queue.sdkVer >= 0x02000000) {
		ret = 0x80000004;
	} else {
		ret = -1;
	}
		
	sceKernelCpuResumeIntr(old_intr);
	pspSetK1(old_k1);
	return ret;

}

int sceGeContinue_patch()
{
	int old_intr, old_k1;
	int ret;

	old_k1 = pspShiftK1();
	old_intr = sceKernelCpuSuspendIntr();
	SceGeDisplayList *dl = g_AwQueue.active_first;

	if (dl == NULL) {
		sceKernelCpuResumeIntr(old_intr);
		pspSetK1(old_k1);
		return 0;
	}

	if (dl->state == SCE_GE_DL_STATE_PAUSED) {
		if (ge_vita_queue.isBreak == 0) {
			if (dl->signal != SCE_GE_DL_SIGNAL_PAUSE) {
				dl->state = SCE_GE_DL_STATE_RUNNING;
				dl->signal = SCE_GE_DL_SIGNAL_NONE;

				
				if (dl->ctx != NULL && dl->ctxUpToDate == 0) {
					sceGeSaveContext_patch(dl->ctx);
				}

				dl->ctxUpToDate = 1;
				ge_vita_reg_set(GE_VITA_REG_STALLADDR, dl->stall);

				// FIXME: are all needed ?
				SceGeStack s;
				s.cmd = dl->flags | 1;
				s.pc = dl->list;
				s.offsetAddr = dl->unk36;
				s.baseAddr = dl->unk48;
				//s.stack[3] = dl->unk40;
				//s.stack[4] = dl->unk44;
				//s.stack[5] = dl->unk28;
				//s.stack[6] = dl->unk32;

				ge_vita_stack_restore(&s);

				ge_vita_queue.curRunning = dl;
				sceKernelClearEventFlag(ge_vita_queue.drawing_flag, 0xFFFFFFFD);
			} else {
				ret = 0x80000021;
			}
		} else {
			dl->state == SCE_GE_DL_STATE_QUEUED;
		}
	} else if (dl->state == SCE_GE_DL_STATE_RUNNING) {
		if (ge_vita_queue.sdkVer >= 0x02000000) {
			ret = 0x80000020;
		} else {
			ret = -1;
		}
	} else {
		if (ge_vita_queue.sdkVer >= 0x02000000) {
			ret = 0x80000004;
		} else {
			ret = -1;
		}
	}

	sceKernelCpuResumeIntr(old_intr);
	pspSetK1(old_k1);
	return 0;
}

int sceGeGetCmd_patch(u32 cmd_idx)
{
	int old_intr, old_k1, cmd;

	if (cmd_idx >= 0xFF) {
		return 0x80000102;
	}

	old_k1 = pspShiftK1();
	old_intr = sceKernelCpuSuspendIntr();

	cmd = ge_vita_cmd_get(cmd_idx);

	sceKernelCpuResumeIntr(old_intr);
	pspSetK1(old_k1);

	return cmd;
}

int sceGeGetMtx_patch(int id, int *mtx)
{
	int old_intr, old_k1;
	int i;
	int *src;
	u32 cnt;

	if (id < 0 || id >= 12) {
		return 0x80000102;
	}

	old_k1 = pspShiftK1();

	if (!pspK1PtrOk(mtx)) {
		pspSetK1(old_k1);
		return 0x80000023;
	}

	old_intr = sceKernelCpuSuspendIntr();

	ge_vita_mtx_get(id, mtx);

	sceKernelCpuResumeIntr(old_intr);
	pspSetK1(old_k1);

	return 0;
}

int sceGeGetStack_patch(int stackId, SceGeStack *stack)
{
	SceGeDisplayList *dl = ge_vita_queue.active_first;
	if (dl == NULL) {
		return 0;
	}

	if (stackId < 0) {
		return dl->stackOff;
	}

	old_k1 = pspShiftK1();
	old_intr = sceKernelCpuSuspendIntr();

	if (stackId >= dl->stackOff) {
		sceKernelCpuResumeIntr(old_intr);
		pspSetK1(old_k1);
		return 0x80000023;
	}

	if (!pspK1PtrOk(stack)) {
		pspSetK1(old_k1);
		sceKernelCpuResumeIntr(old_intr);
	}

	if (stack != NULL) {
		SceGeStack *s = &dl->stack[stackId];
		stack->stack[0] = s->stack[0];
		stack->stack[1] = s->stack[1];
		stack->stack[2] = s->stack[2];
		stack->stack[3] = s->stack[3];
		stack->stack[4] = s->stack[4];
		stack->stack[5] = s->stack[5];
		stack->stack[6] = s->stack[6];
		stack->stack[7] = s->stack[7];
	}

	sceKernelCpuResumeIntr(old_intr);
	pspSetK1(old_k1);

	return dl->stackOff;
}

int sceGeSaveContext_patch(SceGeContext *ctx)
{
	int old_intr, old_k1;

	// check buffer aligned
	if (((int)ctx & 3) != 0) {
		return 0x80000103;
	}

	old_k1 = pspShiftK1();
	if (!pspK1StaBufOk(ctx, sizeof(SceGeContext))) {
		pspSetK1(old_k1);
		return 0x80000023;
	}

	old_intr = sceKernelCpuSuspendIntr();

	// check ge_vita busy
	int val;
	ge_vita_reg_get(GE_VITA_REG_CMD, &val);

	if (val & 1) {
		sceKernelCpuResumeIntr(old_intr);
		pspSetK1(old_k1);
		return -1;
	}
	
	ge_vita_ctx_save(ctx);

	sceKernelCpuResumeIntr(old_intr);
	pspSetK1(old_k1);

	return 0;
}

int sceGeRestoreContext_patch(SceGeContext *ctx)
{
	int old_intr, old_k1;

	// check buffer aligned
	if (((int)ctx & 3) != 0)
		return 0x80000103;

	int old_k1 = pspShiftK1();
	if (!pspK1StaBufOk(ctx, sizeof(SceGeContext))) {
		pspSetK1(old_k1);
		return 0x80000023;
	}

	old_intr = sceKernelCpuSuspendIntr();

	// check ge_vita busy
	int val;
	ge_vita_reg_get(GE_VITA_REG_CMD, &val);

	if (val & 1) {
		sceKernelCpuResumeIntr(old_intr);
		pspSetK1(old_k1);
		return -1;
	}

	ge_vita_ctx_restore(ctx);

	sceKernelCpuResumeIntr(old_intr);
	pspSetK1(old_k1);

	return 0;
}

static void ge_vita_list_interrupt(void)
{
	SceGeDisplayList *dl = ge_vita_queue.active_first;
	if (dl == NULL) {
		return;
	}

	u32 *cmdList;
	u32 *lastCmdPtr1;
	u32 *lastCmdPtr2;
	u32 lastCmd1;
	u32 lastCmd2;

	ge_vita_reg_get(GE_VITA_REG_DLIST, &cmdList);

	lastCmdPtr1 = cmdList - 2;
	lastCmdPtr2 = cmdList - 1;
	lastCmd1 = *lastCmdPtr1;
	lastCmd2 = *lastCmdPtr2;

	if (GE_CMD(lastCmd1) != SCE_GE_CMD_SIGNAL || GE_CMD(lastCmd2) != SCE_GE_CMD_END) {
		// bad signal sequence
		return;
	}

	switch (GE_SIGNAL_TYPE(lastCmd1)) {
	case GE_SIGNAL_HANDLER_SUSPEND:
		if (ge_vita_queue.sdkVer <= 0x02000010) {
			dl->state = SCE_GE_DL_STATE_PAUSED;
			cmdList = NULL;
		}
		if (dl->cbId >= 0) {
			sceKernelCallSubIntrHandler(25, dl->cbId * 2, lastCmd1 & 0xffff, (int)cmdList);
			pspSync();
		}
		if (ge_vita_queue.sdkver <= 0x02000010) {
			dl->state = lastCmd2 & 0xff;
		}
		break;
	case GE_SIGNAL_HANDLER_CONTINUE:
		ge_vita_cmd(1);
		if (dl->cbId >= 0) {
			if (ge_vita_queue.sdkver <= 0x02000010) {
				cmdList = NULL;
			}
			sceKernelCallSubIntrHandler(25, dl->cbId * 2, lastCmd1 & 0xffff, (int)cmdList);
		}
		break;
	case GE_SIGNAL_HANDLER_PAUSE:
		dl->state = SCE_GE_DL_STATE_PAUSED;
		dl->signal = lastCmd2 & 0xff;
		dl->unk54 = lastCmd1;
		break;
	case GE_SIGNAL_SYNC:
		dl->signal = SCE_GE_DL_SIGNAL_SYNC;
		ge_vita_cmd(1);
		break;
	case GE_SIGNAL_CALL:
	case GE_SIGNAL_RCALL:
	case GE_SIGNAL_OCALL:
		{
			if (dl->stackOff >= dl->numStacks) {
				// bad thing
				break;
			}

			SceGeStack *s = &dl->stack[dl->stackOff];
			dl->stackOff ++;

			ge_vita_stack_save(s);


			int cmdOff = (GE_SIGNAL_PARAM(lastCmd1) << 16) | (lastCmd2 & 0xffff);
			u32 *newCmdList;
			if (GE_SIGNAL_TYPE(lastCmd1) == GE_SIGNAL_CALL) {
				newCmdList = (u32 *)cmdOff;
			} else if (GE_SIGNAL_TYPE(lastCmd1) == GE_SIGNAL_RCALL) {
				newCmdList = &cmdList[ cmdOff / 4 - 2];
			} else if (GE_SIGNAL_TYPE(lastCmd1) == GE_SIGNAL_OCALL) {
				newCmdList = s->offsetAddr + cmdOff;
			}

			if (newCmdList & 3) {
				// new list unaligned!
				break;
			}

			ge_vita_reg_set(GE_VITA_REG_DLIST, newCmdList);
			ge_vita_cmd(1);
		}
	case GE_SIGNAL_JUMP:
	case GE_SIGNAL_RJUMP:
	case GE_SIGNAL_OJUMP:
		{
			u32 offsetAddr;
			ge_vita_reg_get(GE_VITA_REG_OFFSETADDR, &offsetAddr);

			int cmdOff = (GE_SIGNAL_PARAM(lastCmd1) << 16) | (lastCmd2 & 0xffff);
			u32 *newCmdList;
			if (GE_SIGNAL_TYPE(lastCmd1) == GE_SIGNAL_JUMP) {
				newCmdList = (u32 *)cmdOff;
			} else if (GE_SIGNAL_TYPE(lastCmd1) == GE_SIGNAL_RJUMP) {
				newCmdList = &cmdList[ cmdOff / 4 - 2];
			} else if (GE_SIGNAL_TYPE(lastCmd1) == GE_SIGNAL_OJUMP) {
				newCmdList = offsetAddr + cmdOff;
			}

			if (newCmdList & 3) {
				// new list unaligned!
				break;
			}

			ge_vita_reg_set(GE_VITA_REG_DLIST, newCmdList);
			ge_vita_cmd(1);
		}
		break;
	case GE_SIGNAL_RET:
		{
			if (dl->stackOff == 0) {
				// cannot return
				break;
			}

			SceGeStack *s = &dl->stack[dl->stackOff];
			dl->stackOff--;

			// restore state;
			ge_vita_stack_restore(s);
			ge_vita_cmd(1);
		}
		break;
// breakpoint stuff... not implemented
	case GE_SIGNAL_RTBP0: case GE_SIGNAL_RTBP1: case GE_SIGNAL_RTBP2: case GE_SIGNAL_RTBP3: case GE_SIGNAL_RTBP4: case GE_SIGNAL_RTBP5: case GE_SIGNAL_RTBP6: case GE_SIGNAL_RTBP7:
	case GE_SIGNAL_OTBP0: case GE_SIGNAL_OTBP1: case GE_SIGNAL_OTBP2: case GE_SIGNAL_OTBP3: case GE_SIGNAL_OTBP4: case GE_SIGNAL_OTBP5: case GE_SIGNAL_OTBP6: case GE_SIGNAL_OTBP7:
	case GE_SIGNAL_RCBP:
	case GE_SIGNAL_OCBP:
	case GE_SIGNAL_BREAK1:
	case GE_SIGNAL_BREAK2:
	default:
		break;
	}

}

static void ge_vita_finish_interrupt(void)
{
	SceGeDisplayList *dl = ge_vita_queue.curRunning;
	ge_vita_queue.isBreak = 0;
	ge_vita_queue.curRunning = NULL;

	if (dl != NULL) {
		if (dl->signal == SCE_GE_DL_SIGNAL_SYNC) {
			dl->signal = SCE_GE_DL_SIGNAL_NONE;
			ge_vita_cmd(1);
			return;
		} else if (dl->signal == SCE_GE_DL_SIGNAL_PAUSE) {
			ge_vita_stack_save(&stack);
			
			dl->state = s.cmd;
			dl->list = s.pc;
			dl->unk36 = s.offsetAddr;
			dl->unk48 = s.baseAddr;
			//dl->unk28 = s.stack[5];
			//dl->unk32 = s.stack[6];
			//dl->unk40 = s.stack[3];
			//dl->unk44 = s.stack[4];

			if (ge_vita_queue.active_first == dl) {
				dl->signal = SCE_GE_DL_SIGNAL_BREAK;
				if (dl->cbId >= 0) {
					void *list = dl->list;
					if (ge_vita_queue.sdkVer <= 0x02000010) {
						list = 0;
					}
					sceKernelCallSubIntrHandler(25, dl->cbId * 2, dl->unk54, (int)list);
				}
				return;
			}
			dl = NULL;
		}

		if (dl != NULL) {
			if (dl->state == SCE_GE_DL_STATE_RUNNING) {
				int *cmdList;
				ge_vita_reg_get(GE_VITA_REG_DLIST, &cmdList);

				u32 lastCmd1 = *(int *)UUNCACHED(cmdList - 2);
				u32 lastCmd2 = *(int *)UUNCACHED(cmdList - 1);

				if (GE_CMD(lastCmd1) != SCE_GE_CMD_FINISH || GE_CMD(lastCmd2) != SCE_GE_CMD_FINISH) {
					// illegal finish sequence
				}

				dl->state = SCE_GE_DL_STATE_COMPLETED;

				// todo: log

				if (dl->cbId >= 0) {
					if (ge_vita_queue.sdkVer <= 0x02000010) {
						cmdList = 0;
					}
					sceKernelCallSubIntrHandler(25, dl->cbId * 2 + 1, lastCmd1 & 0xffff, (int)cmdList);
				}
				if (dl->ctx != NULL) {
					sceGeRestoreContext_patch(dl->ctx);
				}
				if (dl->prev != NULL) {
					dl->prev->next = dl->next;
				}
				if (dl->next != NULL) {
					dl->next->prev = dl->prev;
				}
				if (ge_vita_queue.active_first == dl) {
					ge_vita_queue.active_first = dl->next;
				}
				if (ge_vita_queue.active_last == dl) {
					ge_vita_queue.active_last = dl->prev;
				}
				if (ge_vita_queue.free_first == NULL) {
					ge_vita_queue.free_first = dl;
					dl->prev = NULL;
				} else {
					ge_vita_queue.free_last->next = dl;
					dl->prev = ge_vita_queue.free_last;
				}

				dl->state = SCE_GE_DL_STATE_COMPLETED;
				dl->next = NULL;
				ge_vita_queue.free_last = dl;
			}
		}
	}

	SceGeDisplayList *dl2 = ge_vita_queue.active_first;
	if (dl2 == NULL) {
		ge_vita_cmd(0);
		sceKernelSetEventFlag(ge_vita_queue.drawing_flag, 2);
	} else {
		if (dl2->signal == SCE_GE_DL_SIGNAL_PAUSE) {
			dl2->state = SCE_GE_DL_STATE_PAUSED;
			dl2->signal = SCE_GE_DL_SIGNAL_BREAK;
			if (dl2->cbId >= 0) {
				void *list = dl2->list;
				if (ge_vita_queue.sdkVer <= 0x02000010) {
					list = NULL;
				}
				sceKernelCallSubIntrHandler(25, dl2->cbId * 2, dl2->unk54, (int)list);
			}
		}
		if (dl2->state != SCE_GE_DL_STATE_PAUSED) {
			int *ctx2 = (int *)dl2->ctx;
			dl2->state = SCE_GE_DL_STATE_RUNNING;
			if (ctx2 != NULL && dl2->ctxUpToDate == 0) {
				if (dl == NULL || dl->ctx == NULL) {
					sceGeSaveContext_patch(dl2->ctx);
				} else {
					int *ctx1 = (int *)dl->ctx;
					int i;
					for (i = 0; i < 128; i++) {
						ctx2[0] = ctx1[0];
						ctx2[1] = ctx1[1];
						ctx2[2] = ctx1[2];
						ctx2[3] = ctx1[3];
						ctx1 += 4;
						ctx2 += 4;
					}
				}
			}

			dl2->ctxUpToDate = 1;
			ge_vita_cmd(0);
			ge_vita_reg_set(GE_VITA_REG_STALLADDR, dl2->stall);

			SceGeStack s;
			s.cmd = dl2->flags | 1;
			s.pc = dl2->list;
			s.offsetAddr = dl2->unk36;
			s.baseAddr = dl2->unk48;

			ge_vita_stack_restore(&s);
			ge_vita_queue.curRunning = dl2;

			pspSync();
			// TODO:log

		}
	}

	if (dl != NULL) {
		u32 idx = (dl - g_displayLists) / sizeof(SceGeDisplayList);
		sceKernelSetEventFlag(ge_vita_queue.list_done_flag[idx / 32], 1 << (idx % 32));
	}
}

void ge_vita_interrupt(void)
{
	int old_intr = sceKernelCpuSuspendIntr();

	int flags = ge_vita_get_int();
	if (flags & 1) {
		ge_vita_list_interrupt();
	} else if (flags & 2) {
		ge_vita_finish_interrupt();
	}

	sceKernelCpuResumeIntr(old_intr);
}

void ge_vita_init(u32 text_addr)
{
	int i;

	HIJACK_FUNCTION(text_addr + , sceGeEdramGetAddr_patched, sceGeEdramGetAddr_orig);
	HIJACK_FUNCTION(text_addr + , sceGeEdramGetSize_patched, sceGeEdramGetSize_orig);
	HIJACK_FUNCTION(text_addr + , sceGeEdramSetAddrTranslation_patched, sceGeEdramSetAddrTranslation_orig);
	HIJACK_FUNCTION(text_addr + , sceGeListEnQueue_patched, sceGeListEnQueue_orig);
	HIJACK_FUNCTION(text_addr + , sceGeListEnQueueHead_patched, sceGeListEnQueueHead_orig);
	HIJACK_FUNCTION(text_addr + , sceGeListDeQueue_patched, sceGeListDeQueue_orig);
	HIJACK_FUNCTION(text_addr + , sceGeListUpdateStallAddr_patched, sceGeListUpdateStallAddr_orig);
	HIJACK_FUNCTION(text_addr + , sceGeListSync_patched, sceGeListSync_orig);
	HIJACK_FUNCTION(text_addr + , sceGeDrawSync_patch, sceGeDrawSync_orig);
	HIJACK_FUNCTION(text_addr + , sceGeBreak_patch, sceGeBreak_orig);
	HIJACK_FUNCTION(text_addr + , sceGeContinue_patch, sceGeContinue_orig);
	HIJACK_FUNCTION(text_addr + , sceGeGetCmd_patch, sceGeGetCmd_orig);
	HIJACK_FUNCTION(text_addr + , sceGeGetMtx_patch, sceGeGetMtx_orig);
	HIJACK_FUNCTION(text_addr + , sceGeGetStack_patch, sceGeGetStack_orig);
	HIJACK_FUNCTION(text_addr + , sceGeSaveContext_patch, sceGeSaveContext_orig);
	HIJACK_FUNCTION(text_addr + , sceGeRestoreContext_patch, sceGeRestoreContext_orig);

	// init magic mask
	ge_vita_dl_mask =
		(HW_GE_CMD(SCE_GE_CMD_VADR) ^ HW_GE_CMD(SCE_GE_CMD_PRIM)
		^ HW_GE_CMD(SCE_GE_CMD_BEZIER) ^ HW_GE_CMD(SCE_GE_CMD_SPLINE)
		^ HW_GE_CMD(SCE_GE_CMD_WORLDD)) | 0x80000000;

	// init SceGeQueue
	sceGeQueueInit();

	ge_vita_queue.drawing_flag = sceKernelCreateEventFlag("SceGeDrawing", 0x201, 2, NULL);
	ge_vita_queue.list_done_flag[0] = sceKernelCreateEventFlag("SceGeListDoneFlag0", 0x201, -1, NULL);
	ge_vita_queue.list_done_flag[1] = sceKernelCreateEventFlag("SceGeListDoneFlag1", 0x201, -1, NULL);

	ge_vita_reset();
}
