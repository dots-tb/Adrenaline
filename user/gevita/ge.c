

int gevita_cmd_thread(SceSize args, void *argp)
{
  while (1) {
    int res = -1;

    // Wait and get kermit request
    SceKermitRequest *request;
    ScePspemuKermitWaitAndGetRequest(KERMIT_MODE_EXTRA_1, &request);


    if (request->cmd == GE_VITA_CMD_RESET) {
			ge_state_reset();
		} else if (request->cmd == GE_VITA_CMD_SYNC) {
			ge_state_sync();
		} else if (request->cmd == GE_VITA_CMD_REG_GET) {
			res = ge_state_reg_get(request->arg[0]);
		} else if (request->cmd == GE_VITA_CMD_REG_SET) {
			res = ge_state_reg_get(request->arg[0], request->arg[1]);
		} else if (request->cmd == GE_VITA_CMD_STACK_SAVE) {
			u32 dst_addr = request->arg[0];
			u32 *dst = (u32 *)ScePspemuConvertAddress(dst_addr, KERMIT_OUTPUT_MODE, 8 * 4);

			if (!ge_state_stack_save(dst)) {
				res = 0;
			}
		} else if (request->cmd == GE_VITA_CMD_STACK_RESTORE) {
			u32 src = request->arg[0];
			u32 *src = (u32 *)ScePspemuConvertAddress(src_addr, KERMIT_INPUT_MODE, 4 * 4);

			if (!ge_state_stack_restore(src)) {
				res = 0;
			}
		} else if (request->cmd == GE_VITA_CMD_CTX_SAVE) {
			u32 dst_addr = request->arg[0];
			u32 *dst = (u32 *)ScePspemuConvertAddress(dst_addr, KERMIT_OUTPUT_MODE, 512 * 4);

			if (!ge_state_ctx_save(dst)) {
				res = 0;
			}
		} else if (request->cmd == GE_VITA_CMD_CTX_RESTORE) {
			u32 src = request->arg[0];
			u32 *src = (u32 *)ScePspemuConvertAddress(src_addr, KERMIT_INPUT_MODE, 512 * 4);

			if (!ge_state_ctx_restore(src)) {
				res = 0;
			}
		} else if (request->cmd == GE_VITA_CMD_MTX_GET) {
			u32 mtx_id = request->arg[0];
			u32 dst_addr = request->arg[1];
			u32 cnt = 0;
			u32 *src = 0;

			switch (mtx_id) {
				case SCE_GE_MTX_BONEA:
				case SCE_GE_MTX_BONEB:
				case SCE_GE_MTX_BONEC:
				case SCE_GE_MTX_BONED:
				case SCE_GE_MTX_BONEE:
				case SCE_GE_MTX_BONEF:
				case SCE_GE_MTX_BONEG:
				case SCE_GE_MTX_BONEH:
					src = ge_state.boneMatrix + mtx_id * 12;
					cnt = 12;
					break;
				case SCE_GE_MTX_TGEN:
					src = ge_state.tgenMatrix;
					cnt = 12;
					break;
				case SCE_GE_MTX_WORLD:
					src = ge_state.worldMatrix;
					cnt = 12;
					break;
				case SCE_GE_MTX_VIEW:
					src = ge_state.viewMatrix;
					cnt = 12;
				case SCE_GE_MTX_PROJ:
					src = ge_state.projMatrix;
					cnt = 16;
					break;
				default:
					break
			}

			if (cnt != 0 && src != 0) {
				u32 *dst = (u32 *)ScePspemuConvertAddress(dst_addr, KERMIT_OUTPUT_MODE, cnt * 4);
				u32 *src = (u32 *)ge_state.mtx[id];

				// convert to 24bit float
				for (int i = 0; i < cnt; i++) {
					dst[i] = src[i] >> 8;
				}

				res = 0;
			}
		} else if (request->cmd == GE_VITA_CMD_MTX_SET) {
      res = -1;
		} else if (request->cmd == GE_VITA_CMD_CMD_GET) {
			int cmd = request->arg[0];
			if (cmd <= 255) {
				res = ge_state.cmdmem[cmd];
			}
		} else if (request->cmd == GE_VITA_CMD_CMD_SET) {
      res = -1;
    }

    ScePspemuKermitSendResponse(KERMIT_MODE_EXTRA_1, request, (uint64_t)res);
  }

  return sceKernelExitDeleteThread(0);
}

int gevita_draw_thread(SceSize args, void *argp)
{
}
