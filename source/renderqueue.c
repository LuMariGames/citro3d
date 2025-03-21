#include "internal.h"
#include <c3d/base.h>
#include <c3d/renderqueue.h>
#include <stdlib.h>

static C3D_RenderTarget *firstTarget, *lastTarget;
static C3D_RenderTarget *linkedTarget[3];

static TickCounter gpuTime, cpuTime;

static bool inFrame, inSafeTransfer, measureGpuTime;
static bool needSwapTop, needSwapBot, isTopStereo;
static float framerate = 60.0f;
static float framerateCounter[2] = { 60.0f, 60.0f };
static uint_fast8_t frameCounter[2];
static void (* frameEndCb)(void*);
static void* frameEndCbData;

static void C3Di_RenderTargetDestroy(C3D_RenderTarget* target);

static bool framerateLimit(int id)
{
	framerateCounter[id] -= framerate;
	if (framerateCounter[id] <= 0.0f)
	{
		framerateCounter[id] += 60.0f;
		return true;
	}
	return false;
}

static void onVBlank0(C3D_UNUSED void* unused)
{
	if (framerateLimit(0))
		frameCounter[0]++;
}

static void onVBlank1(C3D_UNUSED void* unused)
{
	if (framerateLimit(1))
		frameCounter[1]++;
}

static void onQueueFinish(gxCmdQueue_s* queue)
{
	if (measureGpuTime)
	{
		osTickCounterUpdate(&gpuTime);
		measureGpuTime = false;
	}
	if (inSafeTransfer)
	{
		inSafeTransfer = false;
		if (inFrame)
		{
			gxCmdQueueStop(queue);
			gxCmdQueueClear(queue);
		}
	}
	else
	{
		if (needSwapTop)
		{
			gfxScreenSwapBuffers(GFX_TOP, isTopStereo);
			needSwapTop = false;
		}
		if (needSwapBot)
		{
			gfxScreenSwapBuffers(GFX_BOTTOM, false);
			needSwapBot = false;
		}
	}
}

void C3D_FrameSync(void)
{
	uint_fast8_t cur[2];
	uint_fast8_t start[2] = { frameCounter[0], frameCounter[1] };
	do
	{
		gspWaitForAnyEvent();
		cur[0] = frameCounter[0];
		cur[1] = frameCounter[1];
	} while (cur[0]==start[0] || cur[1]==start[1]);
}

u32 C3D_FrameCounter(int id)
{
	return frameCounter[id];
}

static bool C3Di_WaitAndClearQueue(s64 timeout)
{
	gxCmdQueue_s* queue = &C3Di_GetContext()->gxQueue;
	if (!gxCmdQueueWait(queue, timeout))
		return false;
	gxCmdQueueStop(queue);
	gxCmdQueueClear(queue);
	return true;
}

void C3Di_RenderQueueEnableVBlank(void)
{
	gspSetEventCallback(GSPGPU_EVENT_VBlank0, onVBlank0, NULL, false);
	gspSetEventCallback(GSPGPU_EVENT_VBlank1, onVBlank1, NULL, false);
}

void C3Di_RenderQueueDisableVBlank(void)
{
	gspSetEventCallback(GSPGPU_EVENT_VBlank0, NULL, NULL, false);
	gspSetEventCallback(GSPGPU_EVENT_VBlank1, NULL, NULL, false);
}

void C3Di_RenderQueueInit(void)
{
	C3D_Context* ctx = C3Di_GetContext();

	C3Di_RenderQueueEnableVBlank();

	GX_BindQueue(&ctx->gxQueue);
	gxCmdQueueSetCallback(&ctx->gxQueue, onQueueFinish, NULL);
	gxCmdQueueRun(&ctx->gxQueue);
}

void C3Di_RenderQueueExit(void)
{
	int i;
	C3D_RenderTarget *a, *next;

	C3Di_WaitAndClearQueue(-1);
	gxCmdQueueSetCallback(&C3Di_GetContext()->gxQueue, NULL, NULL);
	GX_BindQueue(NULL);

	C3Di_RenderQueueDisableVBlank();

	for (i = 0; i < 3; ++i)
		linkedTarget[i] = NULL;

	for (a = firstTarget; a; a = next)
	{
		next = a->next;
		C3Di_RenderTargetDestroy(a);
	}
}

void C3Di_RenderQueueWaitDone(void)
{
	C3Di_WaitAndClearQueue(-1);
}

float C3D_FrameRate(float fps)
{
	float old = framerate;
	if (fps > 0.0f && fps <= 60.0f)
	{
		framerate = fps;
		framerateCounter[0] = fps;
		framerateCounter[1] = fps;
	}
	return old;
}

bool C3D_FrameBegin(u8 flags)
{
	C3D_Context* ctx = C3Di_GetContext();
	if (inFrame) return false;

	if (!C3Di_WaitAndClearQueue((flags & C3D_FRAME_NONBLOCK) ? 0 : -1))
		return false;

	inFrame = true;
	osTickCounterStart(&cpuTime);
	GPUCMD_SetBuffer(ctx->cmdBuf, ctx->cmdBufSize, 0);
	return true;
}

bool C3D_FrameDrawOn(C3D_RenderTarget* target)
{
	if (!inFrame) return false;

	target->used = true;
	C3D_SetFrameBuf(&target->frameBuf);
	C3D_SetViewport(0, 0, target->frameBuf.width, target->frameBuf.height);
	return true;
}

void C3D_FrameSplit(u8 flags)
{
	u32 *cmdBuf, cmdBufSize;
	if (!inFrame) return;
	if (C3Di_SplitFrame(&cmdBuf, &cmdBufSize))
		GX_ProcessCommandList(cmdBuf, cmdBufSize*4, flags);
}

void C3D_FrameEnd(u8 flags)
{
	C3D_Context* ctx = C3Di_GetContext();
	if (!inFrame) return;

	if (frameEndCb)
		frameEndCb(frameEndCbData);

	C3D_FrameSplit(flags);
	GPUCMD_SetBuffer(NULL, 0, 0);
	osTickCounterUpdate(&cpuTime);
	inFrame = false;

	// Flush the entire linear memory if the user did not explicitly mandate to flush the command list
	if (!(flags & GX_CMDLIST_FLUSH))
	{
		extern u32 __ctru_linear_heap;
		extern u32 __ctru_linear_heap_size;
		GSPGPU_FlushDataCache((void*)__ctru_linear_heap, __ctru_linear_heap_size);
	}

	int i;
	C3D_RenderTarget* target;
	isTopStereo = false;
	for (i = 2; i >= 0; --i)
	{
		target = linkedTarget[i];
		if (!target || !target->used)
			continue;
		target->used = false;
		C3D_FrameBufTransfer(&target->frameBuf, target->screen, target->side, target->transferFlags);
		if (target->screen == GFX_TOP)
		{
			needSwapTop = true;
			if (target->side == GFX_RIGHT)
				isTopStereo = true;
		}
		else if (target->screen == GFX_BOTTOM)
			needSwapBot = true;
	}

	measureGpuTime = true;
	osTickCounterStart(&gpuTime);
	gxCmdQueueRun(&ctx->gxQueue);
}

void C3D_FrameEndHook(void (* hook)(void*), void* param)
{
	frameEndCb = hook;
	frameEndCbData = param;
}

float C3D_GetDrawingTime(void)
{
	return osTickCounterRead(&gpuTime);
}

float C3D_GetProcessingTime(void)
{
	return osTickCounterRead(&cpuTime);
}

static C3D_RenderTarget* C3Di_RenderTargetNew(void)
{
	C3D_RenderTarget* target = (C3D_RenderTarget*)malloc(sizeof(C3D_RenderTarget));
	if (!target) return NULL;
	memset(target, 0, sizeof(C3D_RenderTarget));
	return target;
}

static void C3Di_RenderTargetFinishInit(C3D_RenderTarget* target)
{
	target->prev = lastTarget;
	target->next = NULL;
	if (lastTarget)
		lastTarget->next = target;
	if (!firstTarget)
		firstTarget = target;
	lastTarget = target;
}

C3D_RenderTarget* C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt, C3D_DEPTHTYPE depthFmt)
{
	GPU_DEPTHBUF depthFmtReal = GPU_RB_DEPTH16;
	void* depthBuf = NULL;
	void* colorBuf = vramAlloc(C3D_CalcColorBufSize(width,height,colorFmt));
	if (!colorBuf) goto _fail0;
	if (C3D_DEPTHTYPE_OK(depthFmt))
	{
		depthFmtReal = C3D_DEPTHTYPE_VAL(depthFmt);
		size_t depthSize = C3D_CalcDepthBufSize(width,height,depthFmtReal);
		vramAllocPos vramBank = addrGetVRAMBank(colorBuf);
		depthBuf = vramAllocAt(depthSize, vramBank ^ VRAM_ALLOC_ANY); // Attempt opposite bank first...
		if (!depthBuf) depthBuf = vramAllocAt(depthSize, vramBank); // ... if that fails, attempt same bank
		if (!depthBuf) goto _fail1;
	}

	C3D_RenderTarget* target = C3Di_RenderTargetNew();
	if (!target) goto _fail2;

	C3D_FrameBuf* fb = &target->frameBuf;
	C3D_FrameBufAttrib(fb, width, height, false);
	C3D_FrameBufColor(fb, colorBuf, colorFmt);
	target->ownsColor = true;
	if (depthBuf)
	{
		C3D_FrameBufDepth(fb, depthBuf, depthFmtReal);
		target->ownsDepth = true;
	}
	C3Di_RenderTargetFinishInit(target);
	return target;

_fail2:
	if (depthBuf) vramFree(depthBuf);
_fail1:
	vramFree(colorBuf);
_fail0:
	return NULL;
}

C3D_RenderTarget* C3D_RenderTargetCreateFromTex(C3D_Tex* tex, GPU_TEXFACE face, int level, C3D_DEPTHTYPE depthFmt)
{
	if (!addrIsVRAM(tex->data)) return NULL; // Render targets must be in VRAM
	C3D_RenderTarget* target = C3Di_RenderTargetNew();
	if (!target) return NULL;

	C3D_FrameBuf* fb = &target->frameBuf;
	C3D_FrameBufTex(fb, tex, face, level);

	if (C3D_DEPTHTYPE_OK(depthFmt))
	{
		GPU_DEPTHBUF depthFmtReal = C3D_DEPTHTYPE_VAL(depthFmt);
		size_t depthSize = C3D_CalcDepthBufSize(fb->width,fb->height,depthFmtReal);
		vramAllocPos vramBank = addrGetVRAMBank(tex->data);
		void* depthBuf = vramAllocAt(depthSize, vramBank ^ VRAM_ALLOC_ANY); // Attempt opposite bank first...
		if (!depthBuf) depthBuf = vramAllocAt(depthSize, vramBank); // ... if that fails, attempt same bank
		if (!depthBuf)
		{
			free(target);
			return NULL;
		}

		C3D_FrameBufDepth(fb, depthBuf, depthFmtReal);
		target->ownsDepth = true;
	}

	C3Di_RenderTargetFinishInit(target);
	return target;
}

void C3Di_RenderTargetDestroy(C3D_RenderTarget* target)
{
	if (target->ownsColor)
		vramFree(target->frameBuf.colorBuf);
	if (target->ownsDepth)
		vramFree(target->frameBuf.depthBuf);

	C3D_RenderTarget** prevNext = target->prev ? &target->prev->next : &firstTarget;
	C3D_RenderTarget** nextPrev = target->next ? &target->next->prev : &lastTarget;
	*prevNext = target->next;
	*nextPrev = target->prev;
	free(target);
}

void C3D_RenderTargetDelete(C3D_RenderTarget* target)
{
	if (inFrame)
		svcBreak(USERBREAK_PANIC); // Shouldn't happen.
	if (target->linked)
		C3D_RenderTargetDetachOutput(target);
	else
		C3Di_WaitAndClearQueue(-1);
	C3Di_RenderTargetDestroy(target);
}

void C3D_RenderTargetSetOutput(C3D_RenderTarget* target, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags)
{
	int id = 0;
	if (screen==GFX_BOTTOM) id = 2;
	else if (side==GFX_RIGHT) id = 1;
	if (linkedTarget[id])
	{
		linkedTarget[id]->linked = false;
		if (!inFrame)
			C3Di_WaitAndClearQueue(-1);
	}
	linkedTarget[id] = target;
	if (target)
	{
		target->linked = true;
		target->transferFlags = transferFlags;
		target->screen = screen;
		target->side = side;
	}
}

static void C3Di_SafeDisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 flags)
{
	C3Di_WaitAndClearQueue(-1);
	inSafeTransfer = true;
	GX_DisplayTransfer(inadr, indim, outadr, outdim, flags);
	gxCmdQueueRun(&C3Di_GetContext()->gxQueue);
}

static void C3Di_SafeTextureCopy(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 size, u32 flags)
{
	C3Di_WaitAndClearQueue(-1);
	inSafeTransfer = true;
	GX_TextureCopy(inadr, indim, outadr, outdim, size, flags);
	gxCmdQueueRun(&C3Di_GetContext()->gxQueue);
}

static void C3Di_SafeMemoryFill(u32* buf0a, u32 buf0v, u32* buf0e, u16 control0, u32* buf1a, u32 buf1v, u32* buf1e, u16 control1)
{
	C3Di_WaitAndClearQueue(-1);
	inSafeTransfer = true;
	GX_MemoryFill(buf0a, buf0v, buf0e, control0, buf1a, buf1v, buf1e, control1);
	gxCmdQueueRun(&C3Di_GetContext()->gxQueue);
}

void C3D_SyncDisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 flags)
{
	if (inFrame)
	{
		C3D_FrameSplit(0);
		GX_DisplayTransfer(inadr, indim, outadr, outdim, flags);
	} else
	{
		C3Di_SafeDisplayTransfer(inadr, indim, outadr, outdim, flags);
		gspWaitForPPF();
	}
}

void C3D_SyncTextureCopy(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 size, u32 flags)
{
	if (inFrame)
	{
		C3D_FrameSplit(0);
		GX_TextureCopy(inadr, indim, outadr, outdim, size, flags);
	} else
	{
		C3Di_SafeTextureCopy(inadr, indim, outadr, outdim, size, flags);
		gspWaitForPPF();
	}
}

void C3D_SyncMemoryFill(u32* buf0a, u32 buf0v, u32* buf0e, u16 control0, u32* buf1a, u32 buf1v, u32* buf1e, u16 control1)
{
	if (inFrame)
	{
		C3D_FrameSplit(0);
		GX_MemoryFill(buf0a, buf0v, buf0e, control0, buf1a, buf1v, buf1e, control1);
	} else
	{
		C3Di_SafeMemoryFill(buf0a, buf0v, buf0e, control0, buf1a, buf1v, buf1e, control1);
		gspWaitForPSC0();
	}
}
