// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <cinttypes>
#include <memory>
#include <vector>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/HW/DVDInterface.h"
#include "Core/HW/Memmap.h"
#include "Core/IPC_HLE/WII_IPC_HLE.h"
#include "Core/IPC_HLE/WII_IPC_HLE_Device_DI.h"
#include "DiscIO/Volume.h"

CWII_IPC_HLE_Device_di::CWII_IPC_HLE_Device_di(u32 _DeviceID, const std::string& _rDeviceName)
    : IWII_IPC_HLE_Device(_DeviceID, _rDeviceName)
{
}

CWII_IPC_HLE_Device_di::~CWII_IPC_HLE_Device_di()
{
}

void CWII_IPC_HLE_Device_di::DoState(PointerWrap& p)
{
  DoStateShared(p);
  p.Do(m_commands_to_execute);
}

IPCCommandResult CWII_IPC_HLE_Device_di::IOCtl(const IOSIOCtlRequest& request)
{
  // DI IOCtls are handled in a special way by Dolphin
  // compared to other WII_IPC_HLE functions.
  // This is a wrapper around DVDInterface's ExecuteCommand,
  // which will execute commands more or less asynchronously.
  // Only one command can be executed at a time, so commands
  // are queued until DVDInterface is ready to handle them.

  bool ready_to_execute = m_commands_to_execute.empty();
  m_commands_to_execute.push_back(request.address);
  if (ready_to_execute)
    StartIOCtl(request);

  // DVDInterface handles the timing and we handle the reply,
  // so WII_IPC_HLE shouldn't handle anything.
  return GetNoReply();
}

void CWII_IPC_HLE_Device_di::StartIOCtl(const IOSIOCtlRequest& request)
{
  const u32 command_0 = Memory::Read_U32(request.buffer_in);
  const u32 command_1 = Memory::Read_U32(request.buffer_in + 4);
  const u32 command_2 = Memory::Read_U32(request.buffer_in + 8);

  // DVDInterface's ExecuteCommand handles most of the work.
  // The IOCtl callback is used to generate a reply afterwards.
  DVDInterface::ExecuteCommand(command_0, command_1, command_2, request.buffer_out,
                               request.buffer_out_size, true);
}

void CWII_IPC_HLE_Device_di::FinishIOCtl(DVDInterface::DIInterruptType interrupt_type)
{
  if (m_commands_to_execute.empty())
  {
    PanicAlert("WII_IPC_HLE_Device_DI: There is no command to execute!");
    return;
  }

  // This command has been executed, so it's removed from the queue
  u32 command_address = m_commands_to_execute.front();
  m_commands_to_execute.pop_front();
  IOSIOCtlRequest request{command_address};

  request.SetReturnValue(interrupt_type);
  WII_IPC_HLE_Interface::EnqueueReply(request);

  // DVDInterface is now ready to execute another command,
  // so we start executing a command from the queue if there is one
  if (!m_commands_to_execute.empty())
  {
    IOSIOCtlRequest next_request{m_commands_to_execute.front()};
    StartIOCtl(next_request);
  }
}

IPCCommandResult CWII_IPC_HLE_Device_di::IOCtlV(const IOSIOCtlVRequest& request)
{
  for (const auto& vector : request.io_vectors)
    Memory::Memset(vector.address, 0, vector.size);
  s32 return_value = IPC_SUCCESS;
  switch (request.request)
  {
  case DVDInterface::DVDLowOpenPartition:
  {
    _dbg_assert_msg_(WII_IPC_DVD, request.in_vectors[1].address == 0,
                     "DVDLowOpenPartition with ticket");
    _dbg_assert_msg_(WII_IPC_DVD, request.in_vectors[2].address == 0,
                     "DVDLowOpenPartition with cert chain");

    u64 const partition_offset = ((u64)Memory::Read_U32(request.in_vectors[0].address + 4) << 2);
    DVDInterface::ChangePartition(partition_offset);

    INFO_LOG(WII_IPC_DVD, "DVDLowOpenPartition: partition_offset 0x%016" PRIx64, partition_offset);

    // Read TMD to the buffer
    std::vector<u8> tmd_buffer = DVDInterface::GetVolume().GetTMD();
    Memory::CopyToEmu(request.io_vectors[0].address, tmd_buffer.data(), tmd_buffer.size());
    WII_IPC_HLE_Interface::ES_DIVerify(tmd_buffer);

    return_value = 1;
    break;
  }
  default:
    request.DumpUnknown(GetDeviceName(), LogTypes::WII_IPC_DVD);
  }
  request.SetReturnValue(return_value);
  return GetDefaultReply();
}
