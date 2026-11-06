{ Mistix syscall ABI. Phase 9 retains the restricted blockd read-only interface. }

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
  MISTIX_SYSCALL_VFS_REGISTER = 31;

  MISTIX_SYSCALL_MAX_WRITE = 4096;
  MISTIX_ERR_BADF = $FFFFFFFFFFFFFFF7;
  MISTIX_ERR_FAULT = $FFFFFFFFFFFFFFF2;
  MISTIX_ERR_INVAL = $FFFFFFFFFFFFFFEA;
  MISTIX_ERR_NOSYS = $FFFFFFFFFFFFFFDA;
  MISTIX_ERR_NOMEM = $FFFFFFFFFFFFFFF4;
  MISTIX_ERR_BUSY = $FFFFFFFFFFFFFFF0;
  MISTIX_ERR_AGAIN = $FFFFFFFFFFFFFFF5;
  VFS_BROKER_SHARED_BASE = QWord($0000500000000000);
  VFS_BROKER_SHARED_SIZE = 65536;
  VFS_BROKER_OPEN = $4D56534600000011;
  VFS_BROKER_READ = $4D56534600000012;
  VFS_BROKER_WRITE = $4D56534600000013;
  VFS_BROKER_CLOSE = $4D56534600000014;
  VFS_BROKER_OP_OPEN = 1;
  VFS_BROKER_OP_READ = 2;
  VFS_BROKER_OP_WRITE = 3;
  VFS_BROKER_OP_CLOSE = 4;
  VFS_BROKER_CLOSE_QUEUE_MAX = 128;

type
  TMistixBlockUserInfo = packed record
    SectorCount: QWord;
    SectorSize: LongWord;
    Flags: LongWord;
  end;

type
  TVFSBrokerPending = record
    Active: Byte;
    Ready: Byte;
    Operation: Byte;
    OwnerIndex: LongWord;
    OwnerPID: QWord;
    TransactionID: QWord;
    RemoteHandle: QWord;
    UserBuffer: QWord;
    ByteCount: QWord;
    Flags: QWord;
    ResultValue: QWord;
    Payload: QWord;
    RequestPath: array[0..VFS_PATH_MAX] of Char;
  end;

var
  BlockSyscallScratch: array[0..65535] of Byte;
var
  VFSBrokerRegistered: Boolean;
  VFSBrokerServerPID: QWord;
  VFSBrokerServerIndex: LongWord;
  VFSBrokerRequestEndpointIndex: Word;
  VFSBrokerRequestEndpointGeneration: Word;
  VFSBrokerReplyEndpointIndex: Word;
  VFSBrokerReplyEndpointGeneration: Word;
  VFSBrokerNextTransaction: QWord;
  VFSBrokerPending: TVFSBrokerPending;
  VFSBrokerScratch: array[0..VFS_IO_MAX - 1] of Byte;
  VFSBrokerCloseQueue: array[0..VFS_BROKER_CLOSE_QUEUE_MAX - 1] of QWord;
  VFSBrokerCloseCount: LongWord;

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

procedure VFSBrokerInitialize; forward;

procedure SyscallInitialize;
begin
  VFSBrokerInitialize;
  SetIDTGate(128, PtrUInt(@SyscallISR), 0);
  IDT[128].Attributes := $EE; { Present, DPL 3, 64-bit interrupt gate. }
  LoadIDT(@IDTRegister);
  WriteText('[ OK ] DPL-3 int 0x80 syscall gate installed.'#13#10);
end;

procedure VFSBrokerInitialize;
var
  I: LongWord;
begin
  VFSBrokerRegistered := False;
  VFSBrokerServerPID := 0;
  VFSBrokerServerIndex := PROCESS_INDEX_NONE;
  VFSBrokerRequestEndpointIndex := 0;
  VFSBrokerRequestEndpointGeneration := 0;
  VFSBrokerReplyEndpointIndex := 0;
  VFSBrokerReplyEndpointGeneration := 0;
  VFSBrokerNextTransaction := 1;
  VFSBrokerPending.Active := 0;
  VFSBrokerPending.Ready := 0;
  VFSBrokerPending.Operation := 0;
  VFSBrokerPending.OwnerIndex := PROCESS_INDEX_NONE;
  VFSBrokerPending.OwnerPID := 0;
  VFSBrokerPending.TransactionID := 0;
  VFSBrokerPending.RemoteHandle := 0;
  VFSBrokerPending.UserBuffer := 0;
  VFSBrokerPending.ByteCount := 0;
  VFSBrokerPending.Flags := 0;
  VFSBrokerPending.ResultValue := QWord(-11);
  VFSBrokerPending.Payload := 0;
  for I := 0 to VFS_PATH_MAX do VFSBrokerPending.RequestPath[I] := #0;
  VFSBrokerCloseCount := 0;
end;

function SyscallVFSRegister(ServerPID, RequestHandle,
  ReplyHandle: QWord): QWord;
var
  ServerIndex: LongWord;
  RequestObject, RequestGeneration: Word;
  ReplyObject, ReplyGeneration: Word;
  Offset, InitPhysical, ServerPhysical, InitFlags, ServerFlags: QWord;
begin
  Result := QWord(-1); { -EPERM: only init registers the global VFS broker. }
  if CurrentProcessIndex >= PROCESS_MAX_COUNT then Exit;
  if ProcessTable[CurrentProcessIndex].PID <> 1 then Exit;
  Result := QWord(-22);
  if (RequestHandle > High(LongWord)) or (ReplyHandle > High(LongWord)) or
     (RequestHandle = ReplyHandle) then Exit;
  Result := QWord(-3); { -ESRCH }
  if not ProcessFindByPID(ServerPID, ServerIndex) then Exit;
  if (ServerIndex = CurrentProcessIndex) or
     (ProcessTable[ServerIndex].ParentPID <> 1) or
     (ProcessTable[ServerIndex].State = psZombie) or
     (ProcessTable[ServerIndex].State = psFree) then Exit;
  Result := QWord(-9);
  if ProcessCapabilityLookup(ProcessTable[CurrentProcessIndex].Capabilities,
       LongWord(RequestHandle), CAP_RIGHT_SEND, okEndpoint,
       RequestObject, RequestGeneration) <> krSuccess then Exit;
  if ProcessCapabilityLookup(ProcessTable[CurrentProcessIndex].Capabilities,
       LongWord(ReplyHandle), CAP_RIGHT_RECEIVE, okEndpoint,
       ReplyObject, ReplyGeneration) <> krSuccess then Exit;
  Result := QWord(-22);
  if (RequestObject = ReplyObject) or
     (RequestObject >= ENDPOINT_MAX_COUNT) or
     (ReplyObject >= ENDPOINT_MAX_COUNT) then Exit;
  if (EndpointTable[RequestObject].Active = 0) or
     (EndpointTable[RequestObject].Generation <> RequestGeneration) or
     (EndpointTable[ReplyObject].Active = 0) or
     (EndpointTable[ReplyObject].Generation <> ReplyGeneration) then Exit;
  Result := QWord(-14);
  if not AddressSpaceValidateUserRange(ProcessTable[CurrentProcessIndex].AddressSpaceRoot,
       VFS_BROKER_SHARED_BASE, VFS_BROKER_SHARED_SIZE) or
     not AddressSpaceValidateUserRange(ProcessTable[ServerIndex].AddressSpaceRoot,
       VFS_BROKER_SHARED_BASE, VFS_BROKER_SHARED_SIZE) then Exit;
  { Verify that PID 1 and vfsd mapped the *same* shared pages, not merely
    unrelated writable pages at the agreed virtual address. }
  Offset := 0;
  while Offset < VFS_BROKER_SHARED_SIZE do
  begin
    if not AddressSpaceTranslate(ProcessTable[CurrentProcessIndex].AddressSpaceRoot,
         VFS_BROKER_SHARED_BASE + Offset, InitPhysical, InitFlags) or
       not AddressSpaceTranslate(ProcessTable[ServerIndex].AddressSpaceRoot,
         VFS_BROKER_SHARED_BASE + Offset, ServerPhysical, ServerFlags) then Exit;
    if ((InitPhysical and not QWord(PAGE_SIZE - 1)) <>
        (ServerPhysical and not QWord(PAGE_SIZE - 1))) or
       ((InitFlags and (PTE_USER or PTE_WRITABLE)) <> (PTE_USER or PTE_WRITABLE)) or
       ((ServerFlags and (PTE_USER or PTE_WRITABLE)) <> (PTE_USER or PTE_WRITABLE)) then Exit;
    Inc(Offset, PAGE_SIZE);
  end;
  if VFSBrokerRegistered then
  begin
    if (VFSBrokerServerPID = ServerPID) and
       (VFSBrokerServerIndex = ServerIndex) and
       (VFSBrokerRequestEndpointIndex = RequestObject) and
       (VFSBrokerRequestEndpointGeneration = RequestGeneration) and
       (VFSBrokerReplyEndpointIndex = ReplyObject) and
       (VFSBrokerReplyEndpointGeneration = ReplyGeneration) then
      Result := 0
    else
      Result := QWord(-16);
    Exit;
  end;
  if VFSBrokerPending.Active <> 0 then
  begin
    Result := QWord(-16);
    Exit;
  end;
  VFSBrokerServerPID := ServerPID;
  VFSBrokerServerIndex := ServerIndex;
  VFSBrokerRequestEndpointIndex := RequestObject;
  VFSBrokerRequestEndpointGeneration := RequestGeneration;
  VFSBrokerReplyEndpointIndex := ReplyObject;
  VFSBrokerReplyEndpointGeneration := ReplyGeneration;
  VFSBrokerRegistered := True;
  WriteText('[ OK ] Kernel VFS syscall broker registered with userland vfsd.'#13#10);
  VFSBrokerFlushRemoteCloses;
  Result := 0;
end;

function VFSBrokerServerAlive: Boolean;
begin
  Result := False;
  if not VFSBrokerRegistered then Exit;
  if VFSBrokerServerIndex >= PROCESS_MAX_COUNT then Exit;
  if ProcessTable[VFSBrokerServerIndex].PID <> VFSBrokerServerPID then Exit;
  if ProcessTable[VFSBrokerServerIndex].State = psFree then Exit;
  if ProcessTable[VFSBrokerServerIndex].State = psZombie then Exit;
  Result := True;
end;

function VFSBrokerQueueRequest(const Message: TIPCMessage): Boolean;
var
  Endpoint: ^TEndpoint;
  ObjectIndex: Word;
  ObjectGeneration: Word;
begin
  Result := False;
  ObjectIndex := VFSBrokerRequestEndpointIndex;
  ObjectGeneration := VFSBrokerRequestEndpointGeneration;
  if ObjectIndex >= ENDPOINT_MAX_COUNT then Exit;
  Endpoint := @EndpointTable[ObjectIndex];
  if (Endpoint^.Active = 0) or
     (Endpoint^.Generation <> ObjectGeneration) then Exit;
  if Endpoint^.Count >= IPC_QUEUE_DEPTH then Exit;
  Endpoint^.Queue[Endpoint^.Tail] := Message;
  Endpoint^.Tail := (Endpoint^.Tail + 1) mod IPC_QUEUE_DEPTH;
  Inc(Endpoint^.Count);
  ThreadWakeWaiters(EndpointWaitKey(ObjectIndex, ObjectGeneration,
    WAIT_KEY_ENDPOINT_RECEIVE), True);
  Result := True;
end;

procedure VFSQueueRemoteClose(RemoteHandle: QWord);
begin
  if RemoteHandle = 0 then Exit;
  { Handles from an old/dead server must never be replayed to a replacement. }
  if not VFSBrokerRegistered then Exit;
  if VFSBrokerCloseCount >= VFS_BROKER_CLOSE_QUEUE_MAX then Exit;
  VFSBrokerCloseQueue[VFSBrokerCloseCount] := RemoteHandle;
  Inc(VFSBrokerCloseCount);
end;

procedure VFSBrokerFlushRemoteCloses;
var
  Message: TIPCMessage;
  I: LongWord;
begin
  if not VFSBrokerRegistered or not VFSBrokerServerAlive then Exit;
  while VFSBrokerCloseCount <> 0 do
  begin
    Message.MessageId := VFS_BROKER_CLOSE;
    Message.WordCount := 4;
    for I := 0 to 6 do Message.Reserved[I] := 0;
    for I := 0 to IPC_MAX_WORDS - 1 do Message.Words[I] := 0;
    { Transaction zero identifies an asynchronous last-reference close. }
    Message.Words[2] := VFSBrokerCloseQueue[0];
    if not VFSBrokerQueueRequest(Message) then Exit;
    for I := 1 to VFSBrokerCloseCount - 1 do
      VFSBrokerCloseQueue[I - 1] := VFSBrokerCloseQueue[I];
    Dec(VFSBrokerCloseCount);
  end;
end;

procedure VFSBrokerPumpReplies;
var
  Endpoint: ^TEndpoint;
  Message: TIPCMessage;
  ObjectIndex: Word;
  ObjectGeneration: Word;
  OwnerPID: QWord;
  I: LongWord;
begin
  if not VFSBrokerRegistered then
  begin
    if VFSBrokerPending.Active <> 0 then
      if VFSBrokerPending.OwnerIndex < PROCESS_MAX_COUNT then
        if (ProcessTable[VFSBrokerPending.OwnerIndex].PID <> VFSBrokerPending.OwnerPID) or
           (ProcessTable[VFSBrokerPending.OwnerIndex].State = psZombie) or
           (ProcessTable[VFSBrokerPending.OwnerIndex].State = psFree) then
        begin
          VFSBrokerPending.Active := 0;
          VFSBrokerPending.Ready := 0;
        end;
    Exit;
  end;
  if not VFSBrokerServerAlive then
  begin
    if VFSBrokerPending.Active <> 0 then
    begin
      VFSBrokerPending.Ready := 1;
      VFSBrokerPending.ResultValue := QWord(-5); { -EIO }
      VFSBrokerPending.Payload := 0;
    end;
    VFSBrokerRegistered := False;
    VFSBrokerCloseCount := 0;
    Exit;
  end;
  ObjectIndex := VFSBrokerReplyEndpointIndex;
  ObjectGeneration := VFSBrokerReplyEndpointGeneration;
  if ObjectIndex >= ENDPOINT_MAX_COUNT then
  begin
    if VFSBrokerPending.Active <> 0 then
    begin
      VFSBrokerPending.Ready := 1;
      VFSBrokerPending.ResultValue := QWord(-5);
      VFSBrokerPending.Payload := 0;
    end;
    VFSBrokerRegistered := False;
    VFSBrokerCloseCount := 0;
    Exit;
  end;
  Endpoint := @EndpointTable[ObjectIndex];
  if (Endpoint^.Active = 0) or
     (Endpoint^.Generation <> ObjectGeneration) then
  begin
    if VFSBrokerPending.Active <> 0 then
    begin
      VFSBrokerPending.Ready := 1;
      VFSBrokerPending.ResultValue := QWord(-5);
      VFSBrokerPending.Payload := 0;
    end;
    VFSBrokerRegistered := False;
    VFSBrokerCloseCount := 0;
    Exit;
  end;
  while Endpoint^.Count <> 0 do
  begin
    Message := Endpoint^.Queue[Endpoint^.Head];
    Endpoint^.Head := (Endpoint^.Head + 1) mod IPC_QUEUE_DEPTH;
    Dec(Endpoint^.Count);
    if (VFSBrokerPending.Active = 0) or (VFSBrokerPending.Ready <> 0) then
      Continue;
    if (Message.Words[0] <> VFSBrokerPending.TransactionID) then Continue;
    OwnerPID := VFSBrokerPending.OwnerPID;
    if (Message.WordCount < 3) or (Message.Words[1] <> OwnerPID) then
    begin
      VFSBrokerPending.ResultValue := QWord(-71); { -EPROTO }
      VFSBrokerPending.Payload := 0;
      VFSBrokerPending.Ready := 1;
      Continue;
    end;
    VFSBrokerPending.ResultValue := Message.MessageId;
    VFSBrokerPending.Payload := Message.Words[2];
    VFSBrokerPending.Ready := 1;
  end;
  { Replies can free queue slots for any sender waiting on this endpoint. }
  ThreadWakeWaiters(EndpointWaitKey(ObjectIndex, ObjectGeneration,
    WAIT_KEY_ENDPOINT_SEND), False);
  for I := 0 to PROCESS_MAX_COUNT - 1 do
    if (VFSBrokerPending.Active <> 0) and
       (VFSBrokerPending.OwnerIndex = I) and
       ((ProcessTable[I].PID <> VFSBrokerPending.OwnerPID) or
        (ProcessTable[I].State = psZombie) or
        (ProcessTable[I].State = psFree)) and
       (VFSBrokerPending.Ready <> 0) then
    begin
      if (VFSBrokerPending.Operation = VFS_BROKER_OP_OPEN) and
         (VFSBrokerPending.ResultValue = 0) and
         (VFSBrokerPending.Payload <> 0) then
        VFSQueueRemoteClose(VFSBrokerPending.Payload);
      VFSBrokerPending.Active := 0;
      VFSBrokerPending.Ready := 0;
    end;
  VFSBrokerFlushRemoteCloses;
end;

function VFSBrokerCall(ProcessIndex, Operation: LongWord;
  RemoteHandle, UserBuffer, ByteCount, Flags: QWord;
  const Path: array of Char): QWord;
var
  Message: TIPCMessage;
  I, PathLength: LongWord;
  ServerRoot: QWord;
  Payload: QWord;
  Status: QWord;
  FD: LongInt;
  LocalPath: array[0..VFS_PATH_MAX] of Byte;
begin
  Result := QWord(-19); { -ENODEV }
  if ((Operation = VFS_BROKER_OP_READ) or
      (Operation = VFS_BROKER_OP_WRITE)) and (ByteCount > VFS_IO_MAX) then
    ByteCount := VFS_IO_MAX;
  VFSBrokerPumpReplies;
  Result := QWord(-11); { -EAGAIN: a different process owns the serial RPC slot. }
  if VFSBrokerPending.Active <> 0 then
  begin
    if VFSBrokerPending.OwnerIndex <> ProcessIndex then Exit;
    if ProcessTable[ProcessIndex].PID <> VFSBrokerPending.OwnerPID then Exit;
    if (VFSBrokerPending.Operation <> Operation) or
       (VFSBrokerPending.RemoteHandle <> RemoteHandle) or
       (VFSBrokerPending.UserBuffer <> UserBuffer) or
       (VFSBrokerPending.ByteCount <> ByteCount) or
       (VFSBrokerPending.Flags <> Flags) then
    begin
      Result := QWord(-16); { -EBUSY: caller changed request while pending. }
      Exit;
    end;
    if Operation = VFS_BROKER_OP_OPEN then
      for I := 0 to VFS_PATH_MAX do
        if VFSBrokerPending.RequestPath[I] <> Path[I] then
        begin
          Result := QWord(-16);
          Exit;
        end;
    if VFSBrokerPending.Ready = 0 then Exit;
    Status := VFSBrokerPending.ResultValue;
    Payload := VFSBrokerPending.Payload;
    if Status <> 0 then
    begin
      VFSBrokerPending.Active := 0;
      Result := Status;
      Exit;
    end;
    case Operation of
      VFS_BROKER_OP_OPEN:
        begin
          FD := VFSAllocateRemoteFD(ProcessTable[ProcessIndex].Files,
            Payload, LongWord(VFSBrokerPending.Flags));
          if FD < 0 then
          begin
            VFSQueueRemoteClose(Payload);
            Result := QWord(-24);
          end
          else
            Result := QWord(FD);
        end;
      VFS_BROKER_OP_READ:
        begin
          Result := Payload;
          if Result > VFSBrokerPending.ByteCount then Result := VFSBrokerPending.ByteCount;
          if Result <> 0 then
          begin
            ServerRoot := ProcessTable[VFSBrokerServerIndex].AddressSpaceRoot;
            if not AddressSpaceCopyFrom(ServerRoot, VFS_BROKER_SHARED_BASE,
                 @VFSBrokerScratch[0], Result) or
               not AddressSpaceCopyTo(ProcessTable[ProcessIndex].AddressSpaceRoot,
                 VFSBrokerPending.UserBuffer, @VFSBrokerScratch[0], Result) then
              Result := QWord(-14);
          end;
        end;
      VFS_BROKER_OP_WRITE:
        if Payload > VFSBrokerPending.ByteCount then
          Result := QWord(-71) { -EPROTO }
        else
          Result := Payload;
      VFS_BROKER_OP_CLOSE:
        Result := 0;
    else
      Result := QWord(-22);
    end;
    VFSBrokerPending.Active := 0;
    VFSBrokerPending.Ready := 0;
    Exit;
  end;

  Result := QWord(-19); { -ENODEV }
  if not VFSBrokerRegistered or not VFSBrokerServerAlive then Exit;
  if (ProcessIndex >= PROCESS_MAX_COUNT) or
     (ProcessTable[ProcessIndex].State <> psRunning) then
  begin
    Result := QWord(-14);
    Exit;
  end;
  if (Operation = VFS_BROKER_OP_READ) or (Operation = VFS_BROKER_OP_WRITE) then
  begin
    Result := QWord(-14);
    if (ByteCount <> 0) and not AddressSpaceValidateUserRange(
         ProcessTable[ProcessIndex].AddressSpaceRoot, UserBuffer, ByteCount) then Exit;
    if Operation = VFS_BROKER_OP_WRITE then
    begin
      if ByteCount <> 0 then
      begin
        if not AddressSpaceCopyFrom(ProcessTable[ProcessIndex].AddressSpaceRoot,
             UserBuffer, @VFSBrokerScratch[0], ByteCount) then Exit;
        ServerRoot := ProcessTable[VFSBrokerServerIndex].AddressSpaceRoot;
        if not AddressSpaceCopyTo(ServerRoot, VFS_BROKER_SHARED_BASE,
             @VFSBrokerScratch[0], ByteCount) then Exit;
      end;
    end;
  end
  else if Operation = VFS_BROKER_OP_OPEN then
  begin
    PathLength := VFSStringLength(Path);
    Result := QWord(-36); { -ENAMETOOLONG }
    if (PathLength = 0) or (PathLength > VFS_PATH_MAX) then Exit;
    for I := 0 to High(LocalPath) do LocalPath[I] := 0;
    for I := 0 to PathLength - 1 do LocalPath[I] := Ord(Path[I]);
    ServerRoot := ProcessTable[VFSBrokerServerIndex].AddressSpaceRoot;
    Result := QWord(-14);
    if not AddressSpaceCopyTo(ServerRoot, VFS_BROKER_SHARED_BASE,
         @LocalPath[0], SizeOf(LocalPath)) then Exit;
  end;

  Message.MessageId := 0;
  Message.WordCount := 4;
  for I := 0 to 6 do Message.Reserved[I] := 0;
  for I := 0 to IPC_MAX_WORDS - 1 do Message.Words[I] := 0;
  case Operation of
    VFS_BROKER_OP_OPEN: Message.MessageId := VFS_BROKER_OPEN;
    VFS_BROKER_OP_READ: Message.MessageId := VFS_BROKER_READ;
    VFS_BROKER_OP_WRITE: Message.MessageId := VFS_BROKER_WRITE;
    VFS_BROKER_OP_CLOSE: Message.MessageId := VFS_BROKER_CLOSE;
  else
    begin
      Result := QWord(-22);
      Exit;
    end;
  end;
  if VFSBrokerNextTransaction = 0 then VFSBrokerNextTransaction := 1;
  Message.Words[0] := VFSBrokerNextTransaction;
  Message.Words[1] := ProcessTable[ProcessIndex].PID;
  if Operation = VFS_BROKER_OP_OPEN then
  begin
    Message.Words[2] := Flags;
    Message.Words[3] := 0;
  end
  else
  begin
    Message.Words[2] := RemoteHandle;
    Message.Words[3] := ByteCount;
  end;
  if not VFSBrokerQueueRequest(Message) then
  begin
    Result := QWord(-11);
    Exit;
  end;

  VFSBrokerPending.Active := 1;
  VFSBrokerPending.Ready := 0;
  VFSBrokerPending.Operation := Operation;
  VFSBrokerPending.OwnerIndex := ProcessIndex;
  VFSBrokerPending.OwnerPID := ProcessTable[ProcessIndex].PID;
  VFSBrokerPending.TransactionID := VFSBrokerNextTransaction;
  Inc(VFSBrokerNextTransaction);
  VFSBrokerPending.RemoteHandle := RemoteHandle;
  VFSBrokerPending.UserBuffer := UserBuffer;
  VFSBrokerPending.ByteCount := ByteCount;
  VFSBrokerPending.Flags := Flags;
  VFSBrokerPending.ResultValue := QWord(-11);
  VFSBrokerPending.Payload := 0;
  for I := 0 to VFS_PATH_MAX do
    if (Operation = VFS_BROKER_OP_OPEN) and (I <= LongWord(High(Path))) then
      VFSBrokerPending.RequestPath[I] := Path[I]
    else
      VFSBrokerPending.RequestPath[I] := #0;
  Result := QWord(-11);
end;

function SyscallOpenUser(Process: PKernelProcess; PathAddress,
  Flags: QWord): QWord;
var
  UserPath, CanonicalPath: array[0..VFS_PATH_MAX] of Char;
  PathLength, I: LongWord;
  IsDevice: Boolean;
begin
  if (Flags and VFS_O_DIRECTORY) <> 0 then
  begin
    Result := VFSOpenUser(Process^.Files, Process^.AddressSpaceRoot,
      PathAddress, Flags);
    Exit;
  end;
  if not VFSBrokerRegistered then
  begin
    Result := VFSOpenUser(Process^.Files, Process^.AddressSpaceRoot,
      PathAddress, Flags);
    Exit;
  end;
  if not VFSCopyUserString(Process^.AddressSpaceRoot, PathAddress, UserPath) then
  begin
    Result := QWord(-14);
    Exit;
  end;
  if not VFSNormalizePath(Process^.Files.CurrentDirectory, UserPath,
       CanonicalPath) then
  begin
    Result := QWord(-36);
    Exit;
  end;
  IsDevice := (CanonicalPath[0] = '/') and
    (CanonicalPath[1] = 'd') and (CanonicalPath[2] = 'e') and
    (CanonicalPath[3] = 'v') and (CanonicalPath[4] = '/');
  if IsDevice then
  begin
    Result := VFSOpenUser(Process^.Files, Process^.AddressSpaceRoot,
      PathAddress, Flags and not QWord(VFS_O_CREAT));
    Exit;
  end;
  PathLength := VFSStringLength(CanonicalPath);
  if PathLength = 0 then
  begin
    Result := QWord(-2);
    Exit;
  end;
  Result := VFSBrokerCall(CurrentProcessIndex, VFS_BROKER_OP_OPEN,
    0, 0, 0, Flags, CanonicalPath);
end;

function SyscallReadUserBuffer(Process: PKernelProcess;
  FileDescriptor, BufferAddress, ByteCount: QWord): QWord;
var
  RemoteHandle: QWord;
  RemoteFlags: LongWord;
  DescriptionIndex: LongInt;
  EmptyPath: array[0..0] of Char;
begin
  if VFSIsRemoteFD(Process^.Files, FileDescriptor, RemoteHandle,
       RemoteFlags, DescriptionIndex) then
  begin
    if (RemoteFlags and 3) = VFS_O_WRONLY then
    begin
      Result := QWord(-9);
      Exit;
    end;
    EmptyPath[0] := #0;
    Result := VFSBrokerCall(CurrentProcessIndex, VFS_BROKER_OP_READ,
      RemoteHandle, BufferAddress, ByteCount, 0, EmptyPath);
    Exit;
  end;
  Result := VFSReadUser(Process^.Files, Process^.AddressSpaceRoot,
    FileDescriptor, BufferAddress, ByteCount);
end;

function SyscallWriteUserBuffer(Process: PKernelProcess;
  FileDescriptor, BufferAddress, ByteCount: QWord): QWord;
var
  RemoteHandle: QWord;
  RemoteFlags: LongWord;
  DescriptionIndex: LongInt;
  EmptyPath: array[0..0] of Char;
begin
  if VFSIsRemoteFD(Process^.Files, FileDescriptor, RemoteHandle,
       RemoteFlags, DescriptionIndex) then
  begin
    if (RemoteFlags and 3) = VFS_O_RDONLY then
    begin
      Result := QWord(-9);
      Exit;
    end;
    EmptyPath[0] := #0;
    Result := VFSBrokerCall(CurrentProcessIndex, VFS_BROKER_OP_WRITE,
      RemoteHandle, BufferAddress, ByteCount, 0, EmptyPath);
    Exit;
  end;
  Result := VFSWriteUser(Process^.Files, Process^.AddressSpaceRoot,
    FileDescriptor, BufferAddress, ByteCount);
end;

function SyscallCloseUser(Process: PKernelProcess; FileDescriptor: QWord): QWord;
begin
  { VFSReleaseDescription queues a remote CLOSE when the final local
    reference disappears, including exit and dup2 replacement paths. }
  Result := VFSCloseUser(Process^.Files, FileDescriptor);
  if Result = 0 then VFSBrokerFlushRemoteCloses;
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
      ReturnValue := SyscallOpenUser(Process, Frame^.SavedRDI, Frame^.SavedRSI);

    MISTIX_SYSCALL_READ:
      ReturnValue := SyscallReadUserBuffer(Process, Frame^.SavedRDI,
        Frame^.SavedRSI, Frame^.SavedRDX);

    MISTIX_SYSCALL_CLOSE:
      ReturnValue := SyscallCloseUser(Process, Frame^.SavedRDI);

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

    MISTIX_SYSCALL_VFS_REGISTER:
      ReturnValue := SyscallVFSRegister(Frame^.SavedRDI,
        Frame^.SavedRSI, Frame^.SavedRDX);
  end;

  Frame^.SavedRAX := ReturnValue;
end;
