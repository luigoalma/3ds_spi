#include <3ds/ipc.h>
#include <3ds/result.h>
#include <3ds/types.h>
#include <3ds/svc.h>
#include <3ds/srv.h>
#include <3ds/synchronization.h>
#include <spi.h>
#include <err.h>
#include <memset.h>

/*
Generally gathered history of spi module
  v0:
   - Released with all known commands.
  v0 -> v1025:
   - Changed handling thread creation
    - v0 only lets create 5 threads during its life time, even if a session ended and thread stopped
    - v1025 adds tracking of thread stops on a global thread counter instead
  v1025 -> v2049:
   - Start getting buffer sizes for IPC cmds 6 and 7 from the buffer descriptor
    - v1025 would get size copy from cmdbuf[4] previously
  v2049 -> v3072:
   - No significant service related changes were found, just environment. Rebuild.
  v3072 (O3DS) <-> v4096 (N3DS):
   - in N3DS, SPI::CD2 thread starts in Core3 with priority 15.
*/

#define OS_REMOTE_SESSION_CLOSED MAKERESULT(RL_STATUS,    RS_CANCELED,   RM_OS, 26)
#define OS_INVALID_HEADER        MAKERESULT(RL_PERMANENT, RS_WRONGARG,   RM_OS, 47)
#define OS_INVALID_IPC_PARAMATER MAKERESULT(RL_PERMANENT, RS_WRONGARG,   RM_OS, 48)
#define OS_MISALIGNED_ADDRESS    MAKERESULT(RL_USAGE,     RS_INVALIDARG, RM_OS, RD_MISALIGNED_ADDRESS)

#define CFG11_SPI_CNT            (*(vu16*)0x1EC401C0)
// since we got CFG11, SOCINFO to get if we got a core3, for n3ds specifically
#define CFG11_SOCINFO            (*(vu16*)0x1EC40FFC)
#define CFG11_SOCINFO_LGR2       BIT(2)
#define IS_SOCINFO_LGR2_SET      ((CFG11_SOCINFO & CFG11_SOCINFO_LGR2) != 0)

static __attribute__((section(".data.TerminationFlag"))) bool TerminationFlag = false;

extern uptr _thread_stack_sp_top_offset;

void _thread_start(void*);

static u8 _div3_u8(u8 x) {
	return (u8)(((u16)x * 0xABu) >> 9);
}

static u8 _mod3_u8(u8 x) { // I don't want __aeabi_uidivmod put into the binary just for % 3 ops
	return x - (_div3_u8(x) * 3u);
}

Result StartThread(Handle* threadhandle, ThreadFunc function, void* arg, uptr stack_top, s32 priority, s32 processor_id) {
	if(stack_top & 0x7) return OS_MISALIGNED_ADDRESS;
	//_thread_start will pop these out
	((u32*)stack_top)[-1] = (u32)function;
	((u32*)stack_top)[-2] = (u32)arg;
	return svcCreateThread(threadhandle, _thread_start, (u32)NULL, (u32*)stack_top, priority, processor_id);
}

inline static void HandleSRVNotification() {
	u32 id;
	Err_FailedThrow(srvReceiveNotification(&id));
	if (id == 0x100)
		TerminationFlag = true;
}

typedef struct {
	vu16 CNT;
	vu8 DATA;
} SPI_Bus_Regs;

typedef struct {
	vu32 CNT;
	vu32 DONE;
	vu32 BLKLEN;
	vu32 FIFO;
	vu32 STATUS;
	vu32 AUTOPOLL; // not used
	vu32 INT_MASK; // not used
	vu32 INT_STAT; // not used
} NSPI_Bus_Regs;

typedef struct {
	SPI_Bus_Regs* const spi_bus;
	NSPI_Bus_Regs* const nspi_bus;
	LightLock lock;
	bool is_nspi_mode;
} SPI_Bus;

typedef struct {
	bool init;
	u8 rate; // I'd imagine
} SPI_DeviceBaudrate;

// For consistency, I shall refer to as BUSes by the indexes of the list below
// So refer to this list when if you see BUS0, BUS1 and BUS2 being referenced

static SPI_Bus SPI_Bus_list[3] = {
	{ /* for device ids 0, 1, 2 */
		(SPI_Bus_Regs*)0x1EC60000,
		(NSPI_Bus_Regs*)0x1EC60800,
		LIGHTLOCK_STATICINIT,
		false
	},
	{ /* for device ids 3, 4, 5 */
		(SPI_Bus_Regs*)0x1EC42000,
		(NSPI_Bus_Regs*)0x1EC42800,
		LIGHTLOCK_STATICINIT,
		false
	},
	{ /* for device id 6 */
		(SPI_Bus_Regs*)0x1EC43000, // does it use this address? it appears whenever dev 6 was used in old SPI mode, wrong bus was used instead
		//(SPI_Bus_Regs*)0x1EC42000, // the wrong bus originally used
		(NSPI_Bus_Regs*)0x1EC43800,
		LIGHTLOCK_STATICINIT,
		false
	}
};

// adding extra slot for dev 6, whatever that is
static SPI_DeviceBaudrate SPI_DeviceRates[7] = {0};

static SPI_Bus* GetBusFromDeviceId(u8 deviceid) {
	if (deviceid <= 2)
		return &SPI_Bus_list[0];
	if (deviceid >= 3 && deviceid <= 5)
		return &SPI_Bus_list[1];
	if (deviceid == 6)
		return &SPI_Bus_list[2];
	return NULL;
}

static int GetBusIndexFromDeviceId(u8 deviceid) {
	if (deviceid <= 2)
		return 0;
	if (deviceid >= 3 && deviceid <= 5)
		return 1;
	if (deviceid == 6)
		return 2;
	return -1;
}

#define SPI_BUS_ENABLE_BIT          BIT(15)
#define SPI_BUS_SELECTHOLD_BIT      BIT(11)
#define SPI_BUS_BUSY_BIT            BIT(7)

#define NSPI_BUS_ENABLE_BIT         BIT(15)
#define NSPI_BUS_TRANSFER_READ_BIT  (0)
#define NSPI_BUS_TRANSFER_WRITE_BIT BIT(13)
#define NSPI_BUS_BUSY_BIT           BIT(15)
#define NSPI_FIFO_WIDTH             (32)
#define NSPI_STATUS_FIFO_FULL_BIT   BIT(0)

// silences any alignment warnings
#define SILENT_PTR_CAST(type, ptr, i)   ((type*)(void*)(((u8*)ptr) + (i)))

static void __SPIWriteLoop(SPI_Bus_Regs* bus, const void* data, u32 length) {
	for (u32 i = 0; i < length; ++i) {
		bus->DATA = *SILENT_PTR_CAST(const u8, data, i);
		while (bus->CNT & SPI_BUS_BUSY_BIT);
	}
}

static void __SPIReadLoop(SPI_Bus_Regs* bus, void* data, u32 length) {
	for (u32 i = 0; i < length; ++i) {
		bus->DATA = 0; // full duplex go brrrr
		while (bus->CNT & SPI_BUS_BUSY_BIT);
		*SILENT_PTR_CAST(u8, data, i) = bus->DATA;
	}
}

// Old SPI register mode would use device 6 with BUS1 device select 3 in the spi binary
// When it should initially be using BUS2 device select 0
// I suspect it was miscoded, but should be fixed here
// device 6 should not be a thing that happens in normal retail environment situations however
// but still like to see things clear, also documentation reasons

static void _SPISendCmdOnly(SPI_Bus_Regs* bus, u8 deviceid, u8 rate, const void* cmd, u32 length) {
	deviceid = _mod3_u8(deviceid);

	bus->CNT = SPI_BUS_ENABLE_BIT | SPI_BUS_SELECTHOLD_BIT | (deviceid << 8) | rate;

	__SPIWriteLoop(bus, cmd, length - 1);

	bus->CNT = SPI_BUS_ENABLE_BIT | (deviceid << 8) | rate;

	bus->DATA = *SILENT_PTR_CAST(const u8, cmd, length - 1);
	while (bus->CNT & SPI_BUS_BUSY_BIT);
}

static void _SPICmdAndReadBuf(SPI_Bus_Regs* bus, u8 deviceid, u8 rate, const void* cmd, u32 cmd_length, void* data, u32 data_length) {
	deviceid = _mod3_u8(deviceid);

	bus->CNT = SPI_BUS_ENABLE_BIT | SPI_BUS_SELECTHOLD_BIT | (deviceid << 8) | rate;

	__SPIWriteLoop(bus, cmd, cmd_length);

	__SPIReadLoop(bus, data, data_length - 1);

	bus->CNT = SPI_BUS_ENABLE_BIT | (deviceid << 8) | rate;

	bus->DATA = 0;
	while (bus->CNT & SPI_BUS_BUSY_BIT);
	*SILENT_PTR_CAST(u8, data, data_length - 1) = bus->DATA;
}

static void _SPICmdAndWriteBuf(SPI_Bus_Regs* bus, u8 deviceid, u8 rate, const void* cmd, u32 cmd_length, const void* data, u32 data_length) {
	deviceid = _mod3_u8(deviceid);

	bus->CNT = SPI_BUS_ENABLE_BIT | SPI_BUS_SELECTHOLD_BIT | (deviceid << 8) | rate;

	__SPIWriteLoop(bus, cmd, cmd_length);

	__SPIWriteLoop(bus, data, data_length - 1);

	bus->CNT = SPI_BUS_ENABLE_BIT | (deviceid << 8) | rate;

	bus->DATA = *SILENT_PTR_CAST(const u8, data, data_length - 1);
	while (bus->CNT & SPI_BUS_BUSY_BIT);
}

static u64 __NSPIGetRateReadSleepTime(u8 rate) {
	if (rate == 1)
		return 268800LLU;
	if (rate == 2)
		return 134400LLU;
	if (rate == 3)
		return 67200LLU;
	if (rate == 4)
		return 33600LLU;
	if (rate == 5)
		return 16800LLU;
	return 537600LLU; // rate == 0 || rate >= 6
}

static void __NSPIWriteLoop(NSPI_Bus_Regs* bus, const void* data, u32 length) {
	for (u32 i = 0; i < length; i += 4) {
		if ((i & (NSPI_FIFO_WIDTH - 1)) == 0) {
			while (bus->STATUS & NSPI_STATUS_FIFO_FULL_BIT);
		}
		bus->FIFO = *SILENT_PTR_CAST(const u32, data, i);
	}

	while (bus->CNT & NSPI_BUS_BUSY_BIT);
}

static void __NSPIReadLoop(NSPI_Bus_Regs* bus, void* data, u32 length, u64 sleep_wait) {
	for (u32 i = 0; i < length; i += 4) {
		if ((i & (NSPI_FIFO_WIDTH - 1)) == 0) {
			while (bus->STATUS & NSPI_STATUS_FIFO_FULL_BIT);
			if (length >= NSPI_FIFO_WIDTH * 2)
				svcSleepThread(sleep_wait);
		}
		*SILENT_PTR_CAST(u32, data, i) = bus->FIFO;
	}

	while (bus->CNT & NSPI_BUS_BUSY_BIT);
}

static void _NSPISendCmdOnly(NSPI_Bus_Regs* bus, u8 deviceid, u8 rate, const void* cmd, u32 length) {
	deviceid = _mod3_u8(deviceid);

	while (bus->CNT & NSPI_BUS_BUSY_BIT);

	bus->BLKLEN = length;
	bus->CNT = NSPI_BUS_ENABLE_BIT | NSPI_BUS_TRANSFER_WRITE_BIT | (deviceid << 6) | rate;

	__NSPIWriteLoop(bus, cmd, length);

	bus->DONE = 0;
}

static void _NSPICmdAndReadBuf(NSPI_Bus_Regs* bus, u8 deviceid, u8 rate, const void* cmd, u32 cmd_length, void* data, u32 data_length) {
	u64 sleep_wait = __NSPIGetRateReadSleepTime(rate);

	deviceid = _mod3_u8(deviceid);

	while (bus->CNT & NSPI_BUS_BUSY_BIT);

	bus->BLKLEN = cmd_length;
	bus->CNT = NSPI_BUS_ENABLE_BIT | NSPI_BUS_TRANSFER_WRITE_BIT | (deviceid << 6) | rate;

	__NSPIWriteLoop(bus, cmd, cmd_length);

	bus->BLKLEN = data_length;
	bus->CNT = NSPI_BUS_ENABLE_BIT | NSPI_BUS_TRANSFER_READ_BIT | (deviceid << 6) | rate;

	__NSPIReadLoop(bus, data, data_length, sleep_wait);

	bus->DONE = 0;
}

static void _NSPICmdAndWriteBuf(NSPI_Bus_Regs* bus, u8 deviceid, u8 rate, const void* cmd, u32 cmd_length, const void* data, u32 data_length) {
	deviceid = _mod3_u8(deviceid);

	while (bus->CNT & NSPI_BUS_BUSY_BIT);

	bus->BLKLEN = cmd_length;
	bus->CNT = NSPI_BUS_ENABLE_BIT | NSPI_BUS_TRANSFER_WRITE_BIT | (deviceid << 6) | rate;

	__NSPIWriteLoop(bus, cmd, cmd_length);

	bus->BLKLEN = data_length;
	bus->CNT = NSPI_BUS_ENABLE_BIT | NSPI_BUS_TRANSFER_WRITE_BIT | (deviceid << 6) | rate;

	__NSPIWriteLoop(bus, data, data_length);

	bus->DONE = 0;
}

static void SPIIPC_InitDeviceRate(u8 deviceid, u8 rate) {
	// original SPI does not prevent a buffer overrun, also did not have a slot for dev 6 despite having supposed support for it
	if (deviceid > 6)
		Err_Panic(SPI_INVALID_SELECTION);

	SPI_DeviceRates[deviceid].init = true;
	SPI_DeviceRates[deviceid].rate = rate;
}

static Result SPIIPC_SendCmdAndRead(u8 deviceid, const void* cmd, u32 cmd_length, void* data, u32 data_length) {
	if (cmd_length > 4)
		return SPI_OUT_OF_RANGE;

	SPI_Bus* bus = GetBusFromDeviceId(deviceid);

	if (!bus) // extra checks not part of original spi binary, my way to check if cant do this device
		Err_Panic(SPI_INVALID_SELECTION);

	if (!SPI_DeviceRates[deviceid].init)
		return SPI_NOT_INITIALIZED;

	LightLock_Lock(&bus->lock);

	if (bus->is_nspi_mode)
		_NSPICmdAndReadBuf(bus->nspi_bus, deviceid, SPI_DeviceRates[deviceid].rate, cmd, cmd_length, data, data_length);
	else
		_SPICmdAndReadBuf(bus->spi_bus, deviceid, SPI_DeviceRates[deviceid].rate, cmd, cmd_length, data, data_length);

	LightLock_Unlock(&bus->lock);

	return 0;
}

static Result SPIIPC_SendCmdAndWrite(u8 deviceid, const void* cmd, u32 cmd_length, const void* data, u32 data_length) {
	if (cmd_length > 4)
		return SPI_OUT_OF_RANGE;

	SPI_Bus* bus = GetBusFromDeviceId(deviceid);

	if (!bus) // extra checks not part of original spi binary, my way to check if cant do this device
		Err_Panic(SPI_INVALID_SELECTION);

	if (!SPI_DeviceRates[deviceid].init)
		return SPI_NOT_INITIALIZED;

	LightLock_Lock(&bus->lock);

	if (bus->is_nspi_mode)
		_NSPICmdAndWriteBuf(bus->nspi_bus, deviceid, SPI_DeviceRates[deviceid].rate, cmd, cmd_length, data, data_length);
	else
		_SPICmdAndWriteBuf(bus->spi_bus, deviceid, SPI_DeviceRates[deviceid].rate, cmd, cmd_length, data, data_length);

	LightLock_Unlock(&bus->lock);

	return 0;
}

static Result SPIIPC_SendCmdOnly(u8 deviceid, const void* cmd, u32 cmd_length) {
	if (cmd_length > 4)
		return SPI_OUT_OF_RANGE;

	SPI_Bus* bus = GetBusFromDeviceId(deviceid);

	if (!bus) // extra checks not part of original spi binary, my way to check if cant do this device
		Err_Panic(SPI_INVALID_SELECTION);

	if (!SPI_DeviceRates[deviceid].init)
		return SPI_NOT_INITIALIZED;

	LightLock_Lock(&bus->lock);

	if (bus->is_nspi_mode)
		_NSPISendCmdOnly(bus->nspi_bus, deviceid, SPI_DeviceRates[deviceid].rate, cmd, cmd_length);
	else
		_SPISendCmdOnly(bus->spi_bus, deviceid, SPI_DeviceRates[deviceid].rate, cmd, cmd_length);

	LightLock_Unlock(&bus->lock);

	return 0;
}

static void SPIIPC_SetDeviceNSPIModeAndRate(u8 deviceid, u8 enable_nspi, u8 rate) {
	int index = GetBusIndexFromDeviceId(deviceid);
	if (index < 0)
		Err_Panic(SPI_INVALID_SELECTION);

	SPI_Bus* bus = &SPI_Bus_list[index];

	// original spi binary did not have anything preventing mode switch while another thread *could've* been working on the bus
	LightLock_Lock(&bus->lock);

	bus->is_nspi_mode = enable_nspi ? true : false;

	if (enable_nspi)
		CFG11_SPI_CNT |= BIT(index);
	else
		CFG11_SPI_CNT &= ~BIT(index);

	SPI_DeviceRates[deviceid].rate = rate;
	// should I also flag init?

	LightLock_Unlock(&bus->lock);
}

static void SPIIPC_SetBUS2NSPIMode(u8 enable_nspi) {
	SPI_Bus* bus = &SPI_Bus_list[2];

	// original spi binary did not have anything preventing mode switch while another thread *could've* been working on the bus
	LightLock_Lock(&bus->lock);

	// also originally nothing informing internally that this has suffered a mode switch for this ipc alone
	bus->is_nspi_mode = enable_nspi ? true : false;

	if (enable_nspi)
		CFG11_SPI_CNT |= BIT(2);
	else
		CFG11_SPI_CNT &= ~BIT(2);

	LightLock_Unlock(&bus->lock);
}

static void SPI_IPCSession() {
	u32* cmdbuf = getThreadCommandBuffer();

	switch (cmdbuf[0] >> 16) {
	case 0x1:
		SPIIPC_InitDeviceRate(cmdbuf[1], cmdbuf[2]);
		cmdbuf[0] = IPC_MakeHeader(0x1, 1, 0);
		cmdbuf[1] = 0;
		break;
	case 0x2: // stub, always 0
		cmdbuf[0] = IPC_MakeHeader(0x2, 1, 0);
		cmdbuf[1] = 0;
		break;
	case 0x3: {
			u8 deviceid = cmdbuf[1];
			u32 cmd = cmdbuf[2];
			u32 cmd_length = cmdbuf[3];

			void* data_out = (void*)&cmdbuf[2];
			u32 data_length = cmdbuf[4];

			if (data_length > 64)
				cmdbuf[1] = SPI_OUT_OF_RANGE;
			else
				cmdbuf[1] = SPIIPC_SendCmdAndRead(deviceid, &cmd, cmd_length, data_out, data_length);
		}
		cmdbuf[0] = IPC_MakeHeader(0x3, 17, 0);
		break;
	case 0x4: {
			u8 deviceid = cmdbuf[1];
			u32 cmd = cmdbuf[2];
			u32 cmd_length = cmdbuf[3];

			const void* data_in = (void*)&cmdbuf[4];
			u32 data_length = cmdbuf[20];

			if (data_length > 64)
				cmdbuf[1] = SPI_OUT_OF_RANGE;
			else
				cmdbuf[1] = SPIIPC_SendCmdAndWrite(deviceid, &cmd, cmd_length, data_in, data_length);
		}
		cmdbuf[0] = IPC_MakeHeader(0x4, 1, 0);
		break;
	case 0x5: {
			u8 deviceid = cmdbuf[1];
			u32 cmd = cmdbuf[2];
			u32 cmd_length = cmdbuf[3];

			cmdbuf[1] = SPIIPC_SendCmdOnly(deviceid, &cmd, cmd_length);
		}
		cmdbuf[0] = IPC_MakeHeader(0x5, 1, 0);
		break;
	case 0x6:
		if (!IPC_CompareHeader(cmdbuf[0], 0x6, 4, 2) || !IPC_Is_Desc_Buffer(cmdbuf[5], IPC_BUFFER_W)) {
			cmdbuf[0] = IPC_MakeHeader(0x0, 1, 0);
			cmdbuf[1] = OS_INVALID_IPC_PARAMATER;
		} else {
			u8 deviceid = cmdbuf[1];
			u32 cmd = cmdbuf[2];
			u32 cmd_length = cmdbuf[3];

			// v1025 -> v2049, start getting buffer length from desc buffer instead of length copy in cmdbuf[4]
			// cmdbuf[4] - buffer length also

			u32 data_length = IPC_Get_Desc_Buffer_Size(cmdbuf[5]);
			void *data_out = (void*)cmdbuf[6];

			cmdbuf[1] = SPIIPC_SendCmdAndRead(deviceid, &cmd, cmd_length, data_out, data_length);
			cmdbuf[0] = IPC_MakeHeader(0x6, 1, 2);
			cmdbuf[2] = IPC_Desc_Buffer(data_length, IPC_BUFFER_W);
			cmdbuf[3] = (u32)data_out;
		}
		break;
	case 0x7:
		if (!IPC_CompareHeader(cmdbuf[0], 0x7, 4, 2) || !IPC_Is_Desc_Buffer(cmdbuf[5], IPC_BUFFER_R)) {
			cmdbuf[0] = IPC_MakeHeader(0x0, 1, 0);
			cmdbuf[1] = OS_INVALID_IPC_PARAMATER;
		} else {
			u8 deviceid = cmdbuf[1];
			u32 cmd = cmdbuf[2];
			u32 cmd_length = cmdbuf[3];

			// v1025 -> v2049, start getting buffer length from desc buffer instead of length copy in cmdbuf[4]
			// cmdbuf[4] - buffer length also

			u32 data_length = IPC_Get_Desc_Buffer_Size(cmdbuf[5]);
			const void *data_in = (void*)cmdbuf[6];

			cmdbuf[1] = SPIIPC_SendCmdAndWrite(deviceid, &cmd, cmd_length, data_in, data_length);
			cmdbuf[0] = IPC_MakeHeader(0x7, 1, 2);
			cmdbuf[2] = IPC_Desc_Buffer(data_length, IPC_BUFFER_R);
			cmdbuf[3] = (u32)data_in;
		}
		break;
	case 0x8:
		SPIIPC_SetDeviceNSPIModeAndRate(cmdbuf[1], cmdbuf[2], cmdbuf[3]);
		cmdbuf[0] = IPC_MakeHeader(0x8, 1, 0);
		cmdbuf[1] = 0;
		break;
	case 0x9: // specifically set BUS 2 nspi on/off, for some reason
		SPIIPC_SetBUS2NSPIMode(cmdbuf[1]);
		cmdbuf[0] = IPC_MakeHeader(0x9, 1, 0);
		cmdbuf[1] = 0;
		break;
	default:
		cmdbuf[0] = IPC_MakeHeader(0x0, 1, 0);
		cmdbuf[1] = OS_INVALID_HEADER;
	}
}

static void SPIThread(void* _service_name) {
	const char* service_name = (const char*)_service_name;

	const s32 SERVICE_COUNT = 1;
	const s32 INDEX_MAX = 2;
	const s32 REMOTE_SESSION_INDEX = SERVICE_COUNT;

	s32 handle_count = 1;

	Handle session_handles[2];

	Err_FailedThrow(srvRegisterService(&session_handles[0], service_name, 1));

	Handle target = 0;
	s32 target_index = -1;
	for (;;) {
		s32 index;

		if (!target) {
			if (TerminationFlag && handle_count == REMOTE_SESSION_INDEX)
				break;
			else
				*getThreadCommandBuffer() = 0xFFFF0000;
		}

		Result res = svcReplyAndReceive(&index, session_handles, handle_count, target);
		s32 last_target_index = target_index;
		target = 0;
		target_index = -1;

		if (R_FAILED(res)) {

			if (res != OS_REMOTE_SESSION_CLOSED)
				Err_Throw(res);

			else if (index == -1) {
				if (last_target_index == -1)
					Err_Throw(SPI_CANCELED_RANGE);
				else
					index = last_target_index;
			}

			else if (index >= handle_count)
				Err_Throw(SPI_CANCELED_RANGE);

			svcCloseHandle(session_handles[index]);

			handle_count--;

			continue;
		}

		if (index == 0) {
			Handle newsession = 0;
			Err_FailedThrow(svcAcceptSession(&newsession, session_handles[index]));

			if (handle_count >= INDEX_MAX) {
				svcCloseHandle(newsession);
				continue;
			}

			session_handles[handle_count] = newsession;
			handle_count++;

		} else if (index >= REMOTE_SESSION_INDEX && index < INDEX_MAX) {
			SPI_IPCSession();
			target = session_handles[index];
			target_index = index;
		} else {
			Err_Throw(SPI_INTERNAL_RANGE);
		}
	}

	Err_FailedThrow(srvUnregisterService(service_name));
	svcCloseHandle(session_handles[0]);
}

static inline void initBSS() {
	extern void* __bss_start__;
	extern void* __bss_end__;
	_memset32_aligned(__bss_start__, 0, (size_t)__bss_end__ - (size_t)__bss_start__);
}

Result __sync_init(void);
void __sync_fini(void);

static void LoadSPICFGStatus() {
	u16 spi_cnt = CFG11_SPI_CNT;
	SPI_Bus_list[0].is_nspi_mode = (spi_cnt & BIT(0)) ? true : false;
	SPI_Bus_list[1].is_nspi_mode = (spi_cnt & BIT(1)) ? true : false;
	SPI_Bus_list[2].is_nspi_mode = (spi_cnt & BIT(2)) ? true : false;
}

void SPIMain() {
	initBSS();

	Err_Panic(__sync_init());

	LoadSPICFGStatus();

	Handle thread_handles[5];
	Handle notification_handle;

	static const char* const service_names[] = {"SPI::NOR", "SPI::CD2", "SPI::CS2", "SPI::CS3", "SPI::DEF"};

	Err_FailedThrow(srvInit());

	Err_FailedThrow(srvEnableNotification(&notification_handle));

	// we will make all the threads now and have them control the service name handle
	for (int i = 0; i < 5; ++i) {
		s32 priority = 20;
		s32 processor_id = -2;

		if (i == 1 && IS_SOCINFO_LGR2_SET) { // n3ds specific, for SPI::CD2 only
			priority = 15;
			processor_id = 3;
		}

		Err_FailedThrow(StartThread(&thread_handles[i], SPIThread, (void*)service_names[i], _thread_stack_sp_top_offset - i * 0x280, priority, processor_id));
	}

	while (!TerminationFlag) {
		svcWaitSynchronization(notification_handle, U64_MAX);
		HandleSRVNotification();
	}

	for (int i = 0; i < 5; ++i) {
		svcWaitSynchronization(thread_handles[i], U64_MAX);
	}

	svcCloseHandle(notification_handle);

	srvExit();
	__sync_fini();
}
