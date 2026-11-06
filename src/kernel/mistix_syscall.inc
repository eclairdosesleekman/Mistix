{ Mistix syscall ABI. Phase 8 adds the restricted blockd read-only interface. }

const
  MISTIX_SYSCALL_WRITE = 1;
  MISTIX_SYSCALL_EXIT = 2;
  MISTIX_SYSCALL_GETPID = 3;
  MISTIX_SYSCALL_YIELD = 4;
  MISTIX_SYSCALL_FORK = 5;
  MISTIX_SYSCALL_SPAWN = 6;
  MISTIX_SYSCALL_WAITPID = 7;
  MISTIX_SYSCALL_ENDPOINT_CREATE = 8;
  MISTIX_SYSCALL_IPC_SEND = 9;
  MISTIX_SYSCALL_IPC_RECEIVE = 10;
  MISTIX_SYSCALL_CAP_TRANSFER = 11;
  MISTIX_SYSCALL_CAP_RECEIVE = 12;
  MISTIX_SYSCALL_CAP_CLOSE = 13;
  MISTIX_SYSCALL_KILL = 14;
  MISTIX_SYSCALL_OPEN = 15;
  MISTIX_SYSCALL_READ = 16;
  MISTIX_SYSCALL_CLOSE = 17;
  MISTIX_SYSCALL_DUP2 = 18;
  MISTIX_SYSCALL_PIPE = 19;
  MISTIX_SYSCALL_GETDENTS = 20;
  MISTIX_SYSCALL_CHDIR = 21;
  MISTIX_SYSCALL_GETCWD = 22;
  MISTIX_SYSCALL_SPAWN_PATH = 23;
  MISTIX_SYSCALL_GETCMDLINE = 24;
  MISTIX_SYSCALL_SHM_CREATE = 25;
  MISTIX_SYSCALL_SHM_MAP = 26;
  MISTIX_SYSCALL_SHM_UNMAP = 27;
  MISTIX_SYSCALL_BLOCK_REGISTER = 28;
  MISTIX_SYSCALL_BLOCK_INFO = 29;
  MISTIX_SYSCALL_BLOCK_READ = 30;

  MISTIX_SYSCALL_MAX_WRITE = 4096;
  MISTIX_ERR_BADF = $FFFFFFFFFFFFFFF7;
  MISTIX_ERR_FAULT = $FFFFFFFFFFFFFFF2;
  MISTIX_ERR_INVAL = $FFFFFFFFFFFFFFEA;
  MISTIX_ERR_NOSYS = $FFFFFFFFFFFFFFDA;
  MISTIX_ERR_NOMEM = $FFFFFFFFFFFFFFF4;
  MISTIX_ERR_BUSY = $FFFFFFFFFFFFFFF0;

type
  TMistixBlockUserInfo = packed record
    SectorCount: QWord;
    SectorSize: LongWord;
    Flags: LongWord;
  end;

var
  BlockSyscallScratch: array[0..65535] of Byte;

function SyscallBlockRegisterCurrent: QWord;
begin
  Result := QWord(-1); { -EPERM }
  if CurrentProcessIndex >= PROCESS_MAX_COUNT then Exit;
  if ProcessTable[CurrentProcessIndex].State <> psRunning then Exit;
  { PID 1 is the init authority in this bring-up ABI. Only its direct child
    may register as the block service. Registration is one-shot. }
  if (ProcessTable[CurrentProcessIndex].ParentPID <> 1) or
     (ProcessTable[CurrentProcessIndex].PID = 0) then Exit;
  if (BlockServerPID <> 0) and
     (BlockServerPID <> ProcessTable[CurrentProcessIndex].PID) then Exit;
  BlockServerPID := ProcessTable[CurrentProcessIndex].PID;
  Result := 0;
end;

function SyscallBlockInfo(Process: PKernelProcess; UserInfoAddress: QWord): QWord;
var
  Info: TMistixBlockUserInfo;
begin
  Result := QWord(-1); { -EPERM }
  if (Process = nil) or (Process^.PID = 0) or
     (Process^.PID <> BlockServerPID) then Exit;
  Result := QWord(-19); { -ENODEV }
  if not BlockInitialized or (BlockDevices[0].Active = 0) then Exit;
  Info.SectorCount := BlockDevices[0].SectorCount;
  Info.SectorSize := BLOCK_SECTOR_SIZE;
  Info.Flags := 1; { physical ATA PIO device }
  Result := QWord(-14); { -EFAULT }
  if not AddressSpaceCopyTo(Process^.AddressSpaceRoot, UserInfoAddress,
       @Info, SizeOf(Info)) then Exit;
  Result := 0;
end;

function SyscallBlockRead(Process: PKernelProcess; LBA, SectorCount,
  UserBuffer: QWord): QWord;
var
  ByteCount: QWord;
begin
  Result := QWord(-1); { -EPERM }
  if (Process = nil) or (Process^.PID = 0) or
     (Process^.PID <> BlockServerPID) then Exit;
  Result := QWord(-19); { -ENODEV }
  if not BlockInitialized or (BlockDevices[0].Active = 0) then Exit;
  Result := QWord(-22); { -EINVAL }
  if (SectorCount = 0) or (SectorCount > 128) then Exit;
  if (LBA > BlockDevices[0].SectorCount) or
     (SectorCount > BlockDevices[0].SectorCount - LBA) then
  begin
    Result := QWord(-34); { -ERANGE }
    Exit;
  end;
  ByteCount := SectorCount * BLOCK_SECTOR_SIZE;
  Result := QWord(-14); { -EFAULT }
  if not AddressSpaceValidateUserRange(Process^.AddressSpaceRoot,
       UserBuffer, ByteCount) then Exit;
  Result := QWord(-5); { -EIO }
  if not BlockReadSectors(0, LBA, LongWord(SectorCount),
       @BlockSyscallScratch[0]) then Exit;
  if not AddressSpaceCopyTo(Process^.AddressSpaceRoot, UserBuffer,
       @BlockSyscallScratch[0], ByteCount) then
  begin
    Result := QWord(-14);
    Exit;
  end;
  Result := ByteCount;
end;

procedure SyscallISR; assembler; nostackframe; public name 'mistix_syscall_isr';
asm
  push 0
  push 128
  jmp mistix_isr_common
end;

procedure SyscallInitialize;
begin
  SetIDTGate(128, PtrUInt(@SyscallISR), 0);
  IDT[128].Attributes := $EE; { Present, DPL 3, 64-bit interrupt gate. }
  LoadIDT(@IDTRegister);
  WriteText('[ OK ] DPL-3 int 0x80 syscall gate installed.'#13#10);
end;

function SyscallWriteUserBuffer(Process: PKernelProcess;
  FileDescriptor, BufferAddress, ByteCount: QWord): QWord;
begin
  Result := VFSWriteUser(Process^.Files, Process^.AddressSpaceRoot,
    FileDescriptor, BufferAddress, ByteCount);
end;

function SyscallReadUserBuffer(Process: PKernelProcess;
  FileDescriptor, BufferAddress, ByteCount: QWord): QWord;
begin
  Result := VFSReadUser(Process^.Files, Process^.AddressSpaceRoot,
    FileDescriptor, BufferAddress, ByteCount);
end;

function SyscallDispatchFrame(Frame: PInterruptFrame): QWord;
var
  Process: PKernelProcess;
  ReturnValue: QWord;
  ChildPID: QWord;
  Handle: LongWord;
begin
  Result := PtrUInt(Frame);
  ReturnValue := MISTIX_ERR_NOSYS;
  if (Frame = nil) or (CurrentProcessIndex >= PROCESS_MAX_COUNT) then
  begin
    if Frame <> nil then Frame^.SavedRAX := MISTIX_ERR_FAULT;
    Exit;
  end;
  if (Frame^.CS and 3) <> 3 then
  begin
    Frame^.SavedRAX := MISTIX_ERR_FAULT;
    Exit;
  end;
  Process := @ProcessTable[CurrentProcessIndex];
  if Process^.State <> psRunning then
  begin
    Frame^.SavedRAX := MISTIX_ERR_FAULT;
    Exit;
  end;

  case Frame^.SavedRAX of
    MISTIX_SYSCALL_WRITE:
      ReturnValue := SyscallWriteUserBuffer(Process, Frame^.SavedRDI,
        Frame^.SavedRSI, Frame^.SavedRDX);

    MISTIX_SYSCALL_EXIT:
      begin
        ProcessTerminateCurrent(Frame, Frame^.SavedRDI);
        Result := ProcessScheduleFromFrame(Frame);
        Exit;
      end;

    MISTIX_SYSCALL_GETPID:
      ReturnValue := Process^.PID;

    MISTIX_SYSCALL_YIELD:
      begin
        Frame^.SavedRAX := 0;
        Result := ProcessScheduleFromFrame(Frame);
        Exit;
      end;

    MISTIX_SYSCALL_FORK:
      begin
        { SHM mappings have their own lifetime table and are not yet
          integrated with the COW page-reference tracker. Do not clone them. }
        if P7ShmProcessHasMappings(CurrentProcessIndex) then
          ReturnValue := MISTIX_ERR_BUSY
        else if ProcessForkCurrent(Frame, ChildPID) then
          ReturnValue := ChildPID
        else
          ReturnValue := MISTIX_ERR_NOMEM;
      end;

    MISTIX_SYSCALL_SPAWN:
      begin
        if ProcessSpawnFromUser(Frame^.SavedRDI, Frame^.SavedRSI, ChildPID) then
          ReturnValue := ChildPID
        else
          ReturnValue := MISTIX_ERR_NOMEM;
      end;

    MISTIX_SYSCALL_WAITPID:
      begin
        Result := ProcessWaitCurrent(Frame, Frame^.SavedRDI,
          Frame^.SavedRSI, ReturnValue);
        if Result <> PtrUInt(Frame) then Exit;
      end;

    MISTIX_SYSCALL_ENDPOINT_CREATE:
      ReturnValue := ProcessEndpointCreate(Process^, Handle);

    MISTIX_SYSCALL_IPC_SEND:
      ReturnValue := ProcessIPCSend(Process^, Frame^.SavedRDI,
                                    Frame^.SavedRSI);

    MISTIX_SYSCALL_IPC_RECEIVE:
      ReturnValue := ProcessIPCReceive(Process^, Frame^.SavedRDI,
                                       Frame^.SavedRSI);

    MISTIX_SYSCALL_CAP_TRANSFER:
      begin
        if (Frame^.SavedRSI > High(LongWord)) or
           (Frame^.SavedRDX > High(LongWord)) then
          ReturnValue := MISTIX_ERR_INVAL
        else
          ReturnValue := ProcessCapabilityTransfer(CurrentProcessIndex,
            Frame^.SavedRDI, LongWord(Frame^.SavedRSI), LongWord(Frame^.SavedRDX));
      end;

    MISTIX_SYSCALL_CAP_RECEIVE:
      ReturnValue := ProcessCapabilityReceive(CurrentProcessIndex);

    MISTIX_SYSCALL_CAP_CLOSE:
      begin
        if (Frame^.SavedRDI <= High(LongWord)) and
           (ProcessCapabilityClose(Process^.Capabilities,
             LongWord(Frame^.SavedRDI)) = krSuccess) then
          ReturnValue := 0
        else
          ReturnValue := MISTIX_ERR_BADF;
      end;

    MISTIX_SYSCALL_KILL:
      begin
        ReturnValue := ProcessKill(CurrentProcessIndex,
          Frame^.SavedRDI, Frame^.SavedRSI);
        if (Frame^.SavedRDI = Process^.PID) and (ReturnValue = 0) then
        begin
          Result := ProcessScheduleFromFrame(Frame);
          Exit;
        end;
      end;

    MISTIX_SYSCALL_OPEN:
      ReturnValue := VFSOpenUser(Process^.Files, Process^.AddressSpaceRoot,
        Frame^.SavedRDI, Frame^.SavedRSI);

    MISTIX_SYSCALL_READ:
      ReturnValue := SyscallReadUserBuffer(Process, Frame^.SavedRDI,
        Frame^.SavedRSI, Frame^.SavedRDX);

    MISTIX_SYSCALL_CLOSE:
      ReturnValue := VFSCloseUser(Process^.Files, Frame^.SavedRDI);

    MISTIX_SYSCALL_DUP2:
      ReturnValue := VFSDup2User(Process^.Files, Frame^.SavedRDI,
        Frame^.SavedRSI);

    MISTIX_SYSCALL_PIPE:
      ReturnValue := VFSPipeUser(Process^.Files, Process^.AddressSpaceRoot,
        Frame^.SavedRDI);

    MISTIX_SYSCALL_GETDENTS:
      ReturnValue := VFSGetDentsUser(Process^.Files, Process^.AddressSpaceRoot,
        Frame^.SavedRDI, Frame^.SavedRSI, Frame^.SavedRDX);

    MISTIX_SYSCALL_CHDIR:
      ReturnValue := VFSChdirUser(Process^.Files, Process^.AddressSpaceRoot,
        Frame^.SavedRDI);

    MISTIX_SYSCALL_GETCWD:
      ReturnValue := VFSGetCWDUser(Process^.Files, Process^.AddressSpaceRoot,
        Frame^.SavedRDI, Frame^.SavedRSI);

    MISTIX_SYSCALL_SPAWN_PATH:
      begin
        if ProcessSpawnFromPath(Frame^.SavedRDI, Frame^.SavedRSI,
             Frame^.SavedRDX, Frame^.SavedRCX, ChildPID) then
          ReturnValue := ChildPID
        else
          ReturnValue := MISTIX_ERR_NOENT;
      end;

    MISTIX_SYSCALL_GETCMDLINE:
      ReturnValue := ProcessCopyCommandLine(CurrentProcessIndex,
        Process^.AddressSpaceRoot, Frame^.SavedRDI, Frame^.SavedRSI);

    MISTIX_SYSCALL_SHM_CREATE:
      ReturnValue := P7ShmCreate(CurrentProcessIndex, Frame^.SavedRDI);

    MISTIX_SYSCALL_SHM_MAP:
      ReturnValue := P7ShmMap(CurrentProcessIndex, Frame^.SavedRDI,
        Frame^.SavedRSI, Frame^.SavedRDX <> 0);

    MISTIX_SYSCALL_SHM_UNMAP:
      ReturnValue := P7ShmUnmap(CurrentProcessIndex, Frame^.SavedRDI,
        Frame^.SavedRSI);

    MISTIX_SYSCALL_BLOCK_REGISTER:
      ReturnValue := SyscallBlockRegisterCurrent;

    MISTIX_SYSCALL_BLOCK_INFO:
      ReturnValue := SyscallBlockInfo(Process, Frame^.SavedRDI);

    MISTIX_SYSCALL_BLOCK_READ:
      ReturnValue := SyscallBlockRead(Process, Frame^.SavedRDI,
        Frame^.SavedRSI, Frame^.SavedRDX);
  end;

  Frame^.SavedRAX := ReturnValue;
end;
