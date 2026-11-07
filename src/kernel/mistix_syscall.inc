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
  MISTIX_SYSCALL_LSEEK = 32;

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
  VFS_BROKER_DIR_OPEN = $4D56534600000021;
  VFS_BROKER_DIR_READ = $4D56534600000022;
  VFS_BROKER_CHDIR = $4D56534600000024;
  VFS_BROKER_SEEK = $4D56534600000015;
  VFS_BROKER_CANCEL = $4D56534600000026;
  VFS_BROKER_PENDING_MAX = 8;
  VFS_BROKER_SLOT_SIZE = 8192;
  VFS_BROKER_TIMEOUT_TICKS = 500;
  VFS_BROKER_CANCEL_GRACE_TICKS = 1000;
  VFS_BROKER_ERR_TIMEOUT = QWord(-110);
  VFS_BROKER_OP_OPEN = 1;
  VFS_BROKER_OP_READ = 2;
  VFS_BROKER_OP_WRITE = 3;
  VFS_BROKER_OP_CLOSE = 4;
  VFS_BROKER_OP_DIR_OPEN = 5;
  VFS_BROKER_OP_DIR_READ = 6;
  VFS_BROKER_OP_CHDIR = 7;
  VFS_BROKER_OP_SEEK = 8;
  VFS_BROKER_DIRENT_MAX = 16;
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
    Cancelled: Byte;
    CancelSent: Byte;
    TimeoutDelivered: Byte;
    Operation: Byte;
    OwnerIndex: LongWord;
    OwnerPID: QWord;
    TransactionID: QWord;
    ServerGeneration: QWord;
    DeadlineTick: QWord;
    CancelDeadlineTick: QWord;
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
  VFSBrokerServerGeneration: QWord;
  VFSBrokerTickCounter: QWord;
  VFSBrokerPending: array[0..VFS_BROKER_PENDING_MAX - 1] of TVFSBrokerPending;
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
  jmp ISRCommon
end;

procedure VFSBrokerInitialize; forward;
procedure VFSBrokerPumpReplies; forward;
procedure VFSBrokerFlushRemoteCloses; forward;
function VFSBrokerHasActiveRequests: Boolean; forward;

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
  I, J: LongWord;
begin
  VFSBrokerRegistered := False;
  VFSBrokerServerPID := 0;
  VFSBrokerServerIndex := PROCESS_INDEX_NONE;
  VFSBrokerRequestEndpointIndex := 0;
  VFSBrokerRequestEndpointGeneration := 0;
  VFSBrokerReplyEndpointIndex := 0;
  VFSBrokerReplyEndpointGeneration := 0;
  VFSBrokerNextTransaction := 1;
  VFSBrokerServerGeneration := 0;
  VFSBrokerTickCounter := 0;
  for I := 0 to VFS_BROKER_PENDING_MAX - 1 do
  begin
    VFSBrokerPending[I].Active := 0;
    VFSBrokerPending[I].Ready := 0;
    VFSBrokerPending[I].Cancelled := 0;
    VFSBrokerPending[I].CancelSent := 0;
    VFSBrokerPending[I].TimeoutDelivered := 0;
    VFSBrokerPending[I].Operation := 0;
    VFSBrokerPending[I].OwnerIndex := PROCESS_INDEX_NONE;
    VFSBrokerPending[I].OwnerPID := 0;
    VFSBrokerPending[I].TransactionID := 0;
    VFSBrokerPending[I].ServerGeneration := 0;
    VFSBrokerPending[I].DeadlineTick := 0;
    VFSBrokerPending[I].CancelDeadlineTick := 0;
    VFSBrokerPending[I].RemoteHandle := 0;
    VFSBrokerPending[I].UserBuffer := 0;
    VFSBrokerPending[I].ByteCount := 0;
    VFSBrokerPending[I].Flags := 0;
    VFSBrokerPending[I].ResultValue := QWord(-11);
    VFSBrokerPending[I].Payload := 0;
    for J := 0 to VFS_PATH_MAX do
      VFSBrokerPending[I].RequestPath[J] := #0;
  end;
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
  { Verify that PID 1 and vfsd mapped the same writable shared pages. }
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
  VFSBrokerPumpReplies;
  if VFSBrokerHasActiveRequests then
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
  Inc(VFSBrokerServerGeneration);
  if VFSBrokerServerGeneration = 0 then VFSBrokerServerGeneration := 1;
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
  { Handles from a dead server must not be replayed to its replacement. }
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
    Message.Words[2] := VFSBrokerCloseQueue[0];
    if not VFSBrokerQueueRequest(Message) then Exit;
    for I := 1 to VFSBrokerCloseCount - 1 do
      VFSBrokerCloseQueue[I - 1] := VFSBrokerCloseQueue[I];
    Dec(VFSBrokerCloseCount);
  end;
end;

function VFSBrokerHasActiveRequests: Boolean;
var
  I: LongWord;
begin
  Result := False;
  for I := 0 to VFS_BROKER_PENDING_MAX - 1 do
    if VFSBrokerPending[I].Active <> 0 then
    begin
      Result := True;
      Exit;
    end;
end;

procedure VFSBrokerTrySendCancel(Index: LongWord);
var
  Message: TIPCMessage;
  I: LongWord;
begin
  if (Index >= VFS_BROKER_PENDING_MAX) or
     (VFSBrokerPending[Index].Active = 0) or
     (VFSBrokerPending[Index].Cancelled = 0) or
     (VFSBrokerPending[Index].CancelSent <> 0) then Exit;
  if not VFSBrokerRegistered or not VFSBrokerServerAlive then Exit;
  Message.MessageId := VFS_BROKER_CANCEL;
  Message.WordCount := 4;
  for I := 0 to 6 do Message.Reserved[I] := 0;
  for I := 0 to IPC_MAX_WORDS - 1 do Message.Words[I] := 0;
  Message.Words[0] := VFSBrokerPending[Index].TransactionID;
  Message.Words[1] := VFSBrokerPending[Index].OwnerPID;
  Message.Words[2] := VFSBrokerPending[Index].ServerGeneration;
  Message.Words[3] := Index;
  if VFSBrokerQueueRequest(Message) then
    VFSBrokerPending[Index].CancelSent := 1;
end;

procedure VFSBrokerClearPending(Index: LongWord);
begin
  if Index >= VFS_BROKER_PENDING_MAX then Exit;
  VFSBrokerPending[Index].Active := 0;
  VFSBrokerPending[Index].Ready := 0;
  VFSBrokerPending[Index].Cancelled := 0;
  VFSBrokerPending[Index].CancelSent := 0;
  VFSBrokerPending[Index].TimeoutDelivered := 0;
  VFSBrokerPending[Index].Operation := 0;
  VFSBrokerPending[Index].OwnerIndex := PROCESS_INDEX_NONE;
  VFSBrokerPending[Index].OwnerPID := 0;
  VFSBrokerPending[Index].TransactionID := 0;
  VFSBrokerPending[Index].ServerGeneration := 0;
  VFSBrokerPending[Index].DeadlineTick := 0;
  VFSBrokerPending[Index].CancelDeadlineTick := 0;
  VFSBrokerPending[Index].RemoteHandle := 0;
  VFSBrokerPending[Index].UserBuffer := 0;
  VFSBrokerPending[Index].ByteCount := 0;
  VFSBrokerPending[Index].Flags := 0;
  VFSBrokerPending[Index].Payload := 0;
end;

procedure VFSBrokerPumpReplies;
var
  Endpoint: ^TEndpoint;
  Message: TIPCMessage;
  ObjectIndex: Word;
  ObjectGeneration: Word;
  I, MatchIndex: LongWord;
  OwnerAlive: Boolean;
begin
  if VFSBrokerRegistered and not VFSBrokerServerAlive then
  begin
    for I := 0 to VFS_BROKER_PENDING_MAX - 1 do
      if VFSBrokerPending[I].Active <> 0 then
      begin
        if VFSBrokerPending[I].Cancelled <> 0 then
          VFSBrokerClearPending(I)
        else
        begin
          VFSBrokerPending[I].Ready := 1;
          VFSBrokerPending[I].ResultValue := QWord(-5);
          VFSBrokerPending[I].Payload := 0;
        end;
      end;
    VFSBrokerRegistered := False;
    VFSBrokerCloseCount := 0;
    Exit;
  end;
  if VFSBrokerRegistered then
  begin
    ObjectIndex := VFSBrokerReplyEndpointIndex;
    ObjectGeneration := VFSBrokerReplyEndpointGeneration;
    if ObjectIndex >= ENDPOINT_MAX_COUNT then
    begin
      VFSBrokerRegistered := False;
      VFSBrokerCloseCount := 0;
      for I := 0 to VFS_BROKER_PENDING_MAX - 1 do
        if VFSBrokerPending[I].Active <> 0 then
        begin
          VFSBrokerPending[I].Ready := 1;
          VFSBrokerPending[I].ResultValue := QWord(-5);
          VFSBrokerPending[I].Payload := 0;
        end;
      Exit;
    end;
    Endpoint := @EndpointTable[ObjectIndex];
    if (Endpoint^.Active = 0) or
       (Endpoint^.Generation <> ObjectGeneration) then
    begin
      VFSBrokerRegistered := False;
      VFSBrokerCloseCount := 0;
      for I := 0 to VFS_BROKER_PENDING_MAX - 1 do
        if VFSBrokerPending[I].Active <> 0 then
        begin
          VFSBrokerPending[I].Ready := 1;
          VFSBrokerPending[I].ResultValue := QWord(-5);
          VFSBrokerPending[I].Payload := 0;
        end;
      Exit;
    end;
    while Endpoint^.Count <> 0 do
    begin
      Message := Endpoint^.Queue[Endpoint^.Head];
      Endpoint^.Head := (Endpoint^.Head + 1) mod IPC_QUEUE_DEPTH;
      Dec(Endpoint^.Count);
      MatchIndex := VFS_BROKER_PENDING_MAX;
      for I := 0 to VFS_BROKER_PENDING_MAX - 1 do
        if (VFSBrokerPending[I].Active <> 0) and
           (VFSBrokerPending[I].TransactionID = Message.Words[0]) then
        begin
          MatchIndex := I;
          Break;
        end;
      if MatchIndex >= VFS_BROKER_PENDING_MAX then Continue;
      if VFSBrokerPending[MatchIndex].Cancelled <> 0 then
      begin
        if (Message.WordCount >= 4) and
           (Message.Words[1] = VFSBrokerPending[MatchIndex].OwnerPID) and
           (Message.Words[3] = VFSBrokerPending[MatchIndex].ServerGeneration) then
        begin
          if (Message.MessageId = VFS_BROKER_CANCEL) then
          begin
            if (VFSBrokerPending[MatchIndex].OwnerIndex < PROCESS_MAX_COUNT) and
               (ProcessTable[VFSBrokerPending[MatchIndex].OwnerIndex].PID =
                 VFSBrokerPending[MatchIndex].OwnerPID) and
               (ProcessTable[VFSBrokerPending[MatchIndex].OwnerIndex].State <> psZombie) and
               (ProcessTable[VFSBrokerPending[MatchIndex].OwnerIndex].State <> psFree) and
               (VFSBrokerPending[MatchIndex].TimeoutDelivered = 0) then
            begin
              VFSBrokerPending[MatchIndex].Ready := 1;
              VFSBrokerPending[MatchIndex].ResultValue := VFS_BROKER_ERR_TIMEOUT;
              VFSBrokerPending[MatchIndex].Payload := 0;
            end
            else
              VFSBrokerClearPending(MatchIndex);
          end
          else
          begin
            if (Message.MessageId = 0) and
               ((VFSBrokerPending[MatchIndex].Operation = VFS_BROKER_OP_OPEN) or
                (VFSBrokerPending[MatchIndex].Operation = VFS_BROKER_OP_DIR_OPEN)) and
               (Message.Words[2] <> 0) then
              VFSQueueRemoteClose(Message.Words[2]);
            if (VFSBrokerPending[MatchIndex].OwnerIndex < PROCESS_MAX_COUNT) and
               (ProcessTable[VFSBrokerPending[MatchIndex].OwnerIndex].PID =
                 VFSBrokerPending[MatchIndex].OwnerPID) and
               (ProcessTable[VFSBrokerPending[MatchIndex].OwnerIndex].State <> psZombie) and
               (ProcessTable[VFSBrokerPending[MatchIndex].OwnerIndex].State <> psFree) and
               (VFSBrokerPending[MatchIndex].TimeoutDelivered = 0) then
            begin
              VFSBrokerPending[MatchIndex].Ready := 1;
              VFSBrokerPending[MatchIndex].ResultValue := VFS_BROKER_ERR_TIMEOUT;
              VFSBrokerPending[MatchIndex].Payload := 0;
            end
            else
              VFSBrokerClearPending(MatchIndex);
          end;
        end;
        Continue;
      end;
      if (Message.WordCount < 4) or
         (Message.Words[1] <> VFSBrokerPending[MatchIndex].OwnerPID) or
         (Message.Words[3] <> VFSBrokerPending[MatchIndex].ServerGeneration) then
      begin
        VFSBrokerPending[MatchIndex].ResultValue := QWord(-71);
        VFSBrokerPending[MatchIndex].Payload := 0;
        VFSBrokerPending[MatchIndex].Ready := 1;
        Continue;
      end;
      VFSBrokerPending[MatchIndex].ResultValue := Message.MessageId;
      VFSBrokerPending[MatchIndex].Payload := Message.Words[2];
      VFSBrokerPending[MatchIndex].Ready := 1;
    end;
    ThreadWakeWaiters(EndpointWaitKey(ObjectIndex, ObjectGeneration,
      WAIT_KEY_ENDPOINT_SEND), False);
  end;
  for I := 0 to VFS_BROKER_PENDING_MAX - 1 do
    if VFSBrokerPending[I].Active <> 0 then
    begin
      OwnerAlive := False;
      if VFSBrokerPending[I].OwnerIndex < PROCESS_MAX_COUNT then
        OwnerAlive :=
          (ProcessTable[VFSBrokerPending[I].OwnerIndex].PID =
            VFSBrokerPending[I].OwnerPID) and
          (ProcessTable[VFSBrokerPending[I].OwnerIndex].State <> psZombie) and
          (ProcessTable[VFSBrokerPending[I].OwnerIndex].State <> psFree);
      if not OwnerAlive then
      begin
        if (VFSBrokerPending[I].Ready <> 0) and
           (VFSBrokerPending[I].ResultValue <> VFS_BROKER_ERR_TIMEOUT) then
        begin
          if (VFSBrokerPending[I].ResultValue = 0) and
             ((VFSBrokerPending[I].Operation = VFS_BROKER_OP_OPEN) or
              (VFSBrokerPending[I].Operation = VFS_BROKER_OP_DIR_OPEN)) and
             (VFSBrokerPending[I].Payload <> 0) then
            VFSQueueRemoteClose(VFSBrokerPending[I].Payload);
          VFSBrokerClearPending(I);
          Continue;
        end;
        if VFSBrokerPending[I].Cancelled = 0 then
        begin
          VFSBrokerPending[I].Cancelled := 1;
          VFSBrokerPending[I].CancelDeadlineTick :=
            VFSBrokerTickCounter + VFS_BROKER_CANCEL_GRACE_TICKS;
        end;
      end
      else if (VFSBrokerPending[I].Ready = 0) and
              (VFSBrokerPending[I].Cancelled = 0) and
              (VFSBrokerTickCounter >= VFSBrokerPending[I].DeadlineTick) then
      begin
        VFSBrokerPending[I].Ready := 1;
        VFSBrokerPending[I].ResultValue := VFS_BROKER_ERR_TIMEOUT;
        VFSBrokerPending[I].Payload := 0;
        VFSBrokerPending[I].Cancelled := 1;
        VFSBrokerPending[I].CancelDeadlineTick :=
          VFSBrokerTickCounter + VFS_BROKER_CANCEL_GRACE_TICKS;
        VFSBrokerTrySendCancel(I);
      end;
      if VFSBrokerPending[I].Cancelled <> 0 then
      begin
        VFSBrokerTrySendCancel(I);
        if VFSBrokerTickCounter >= VFSBrokerPending[I].CancelDeadlineTick then
          VFSBrokerClearPending(I);
      end;
    end;
  VFSBrokerFlushRemoteCloses;
end;

function VFSBrokerCall(ProcessIndex, Operation: LongWord;
  RemoteHandle, UserBuffer, ByteCount, Flags: QWord;
  const Path: array of Char): QWord;
var
  Message: TIPCMessage;
  I, J, PathLength, SlotIndex: LongWord;
  PendingIndex: LongInt;
  ServerRoot, LaneAddress: QWord;
  Payload, Status, TransferBytes: QWord;
  FD: LongInt;
  Dirent: TVFSDirent;
  NameTerminated, ValidEntries, PathOperation: Boolean;
  LocalPath: array[0..VFS_PATH_MAX] of Byte;
begin
  Result := QWord(-19); { -ENODEV }
  PathOperation := (Operation = VFS_BROKER_OP_OPEN) or
    (Operation = VFS_BROKER_OP_DIR_OPEN) or
    (Operation = VFS_BROKER_OP_CHDIR);
  if ((Operation = VFS_BROKER_OP_READ) or
      (Operation = VFS_BROKER_OP_WRITE)) and (ByteCount > VFS_IO_MAX) then
    ByteCount := VFS_IO_MAX;
  if Operation = VFS_BROKER_OP_DIR_READ then
  begin
    if ByteCount > VFS_BROKER_DIRENT_MAX then
      ByteCount := VFS_BROKER_DIRENT_MAX;
    if ByteCount = 0 then
    begin
      Result := 0;
      Exit;
    end;
  end;
  if Operation = VFS_BROKER_OP_SEEK then
  begin
    Result := QWord(-22);
    if (RemoteHandle = 0) or (ByteCount > 2) then Exit;
  end;

  if ProcessIndex >= PROCESS_MAX_COUNT then
  begin
    Result := QWord(-14);
    Exit;
  end;
  if ProcessTable[ProcessIndex].State <> psRunning then
  begin
    Result := QWord(-14);
    Exit;
  end;

  VFSBrokerPumpReplies;
  PendingIndex := -1;
  for I := 0 to VFS_BROKER_PENDING_MAX - 1 do
    if (VFSBrokerPending[I].Active <> 0) and
       ((VFSBrokerPending[I].Cancelled = 0) or
        ((VFSBrokerPending[I].Ready <> 0) and
         (VFSBrokerPending[I].ResultValue = VFS_BROKER_ERR_TIMEOUT) and
         (VFSBrokerPending[I].TimeoutDelivered = 0))) and
       (VFSBrokerPending[I].OwnerIndex = ProcessIndex) and
       (VFSBrokerPending[I].OwnerPID = ProcessTable[ProcessIndex].PID) then
    begin
      PendingIndex := LongInt(I);
      Break;
    end;

  if PendingIndex >= 0 then
  begin
    SlotIndex := LongWord(PendingIndex);
    if (VFSBrokerPending[SlotIndex].Operation <> Operation) or
       (VFSBrokerPending[SlotIndex].RemoteHandle <> RemoteHandle) or
       (VFSBrokerPending[SlotIndex].UserBuffer <> UserBuffer) or
       (VFSBrokerPending[SlotIndex].ByteCount <> ByteCount) or
       (VFSBrokerPending[SlotIndex].Flags <> Flags) then
    begin
      Result := QWord(-16); { -EBUSY: request changed while transaction pending. }
      Exit;
    end;
    if PathOperation then
      for I := 0 to VFS_PATH_MAX do
        if VFSBrokerPending[SlotIndex].RequestPath[I] <> Path[I] then
        begin
          Result := QWord(-16);
          Exit;
        end;
    Result := QWord(-11);
    if VFSBrokerPending[SlotIndex].Ready = 0 then Exit;

    Status := VFSBrokerPending[SlotIndex].ResultValue;
    Payload := VFSBrokerPending[SlotIndex].Payload;
    if Status <> 0 then
    begin
      if Status = VFS_BROKER_ERR_TIMEOUT then
        VFSBrokerPending[SlotIndex].TimeoutDelivered := 1
      else
        VFSBrokerClearPending(SlotIndex);
      Result := Status;
      Exit;
    end;

    LaneAddress := VFS_BROKER_SHARED_BASE +
      QWord(SlotIndex) * VFS_BROKER_SLOT_SIZE;
    case Operation of
      VFS_BROKER_OP_OPEN:
        begin
          FD := VFSAllocateRemoteFD(ProcessTable[ProcessIndex].Files,
            Payload, LongWord(VFSBrokerPending[SlotIndex].Flags));
          if FD < 0 then
          begin
            VFSQueueRemoteClose(Payload);
            Result := QWord(-24);
          end
          else
            Result := QWord(FD);
        end;
      VFS_BROKER_OP_DIR_OPEN:
        begin
          FD := VFSAllocateRemoteDirectoryFD(ProcessTable[ProcessIndex].Files,
            Payload, LongWord(VFSBrokerPending[SlotIndex].Flags));
          if FD < 0 then
          begin
            VFSQueueRemoteClose(Payload);
            Result := QWord(-24);
          end
          else
            Result := QWord(FD);
        end;
      VFS_BROKER_OP_DIR_READ:
        begin
          Result := QWord(-71); { -EPROTO }
          if (Payload <= VFSBrokerPending[SlotIndex].ByteCount) and
             (Payload <= VFS_BROKER_DIRENT_MAX) then
          begin
            Result := Payload;
            if Payload <> 0 then
            begin
              TransferBytes := Payload * SizeOf(TVFSDirent);
              if TransferBytes > VFS_IO_MAX then
                Result := QWord(-71)
              else
              begin
                ServerRoot := ProcessTable[VFSBrokerServerIndex].AddressSpaceRoot;
                if not AddressSpaceCopyFrom(ServerRoot, LaneAddress,
                     @VFSBrokerScratch[0], TransferBytes) then
                  Result := QWord(-14)
                else
                begin
                  ValidEntries := True;
                  for I := 0 to LongWord(Payload) - 1 do
                  begin
                    Dirent := PVFSDirent(PtrUInt(@VFSBrokerScratch[0]) +
                      QWord(I) * SizeOf(TVFSDirent))^;
                    if (Dirent.Inode = 0) or
                       ((Dirent.EntryType <> VFS_DIRENT_FILE) and
                        (Dirent.EntryType <> VFS_DIRENT_DIRECTORY) and
                        (Dirent.EntryType <> 0)) then
                      ValidEntries := False;
                    NameTerminated := False;
                    for J := 0 to VFS_DIRENT_NAME_MAX - 1 do
                    begin
                      if Dirent.Name[J] = #0 then
                      begin
                        if J = 0 then ValidEntries := False;
                        if (J = 1) and (Dirent.Name[0] = '.') then
                          ValidEntries := False;
                        if (J = 2) and (Dirent.Name[0] = '.') and
                           (Dirent.Name[1] = '.') then
                          ValidEntries := False;
                        NameTerminated := True;
                        Break;
                      end;
                      if (Dirent.Name[J] = '/') or
                         (Dirent.Name[J] < ' ') or
                         (Dirent.Name[J] = #127) then
                        ValidEntries := False;
                    end;
                    if not NameTerminated then ValidEntries := False;
                  end;
                  if not ValidEntries then
                    Result := QWord(-71)
                  else if not AddressSpaceCopyTo(
                    ProcessTable[ProcessIndex].AddressSpaceRoot,
                    VFSBrokerPending[SlotIndex].UserBuffer,
                    @VFSBrokerScratch[0], TransferBytes) then
                    Result := QWord(-14);
                end;
              end;
            end;
          end;
        end;
      VFS_BROKER_OP_CHDIR:
        begin
          for I := 0 to VFS_PATH_MAX do
            ProcessTable[ProcessIndex].Files.CurrentDirectory[I] :=
              VFSBrokerPending[SlotIndex].RequestPath[I];
          Result := 0;
        end;
      VFS_BROKER_OP_READ:
        begin
          Result := Payload;
          if Result > VFSBrokerPending[SlotIndex].ByteCount then
            Result := VFSBrokerPending[SlotIndex].ByteCount;
          if Result <> 0 then
          begin
            ServerRoot := ProcessTable[VFSBrokerServerIndex].AddressSpaceRoot;
            if not AddressSpaceCopyFrom(ServerRoot, LaneAddress,
                 @VFSBrokerScratch[0], Result) or
               not AddressSpaceCopyTo(ProcessTable[ProcessIndex].AddressSpaceRoot,
                 VFSBrokerPending[SlotIndex].UserBuffer,
                 @VFSBrokerScratch[0], Result) then
              Result := QWord(-14);
          end;
        end;
      VFS_BROKER_OP_WRITE:
        if Payload > VFSBrokerPending[SlotIndex].ByteCount then
          Result := QWord(-71)
        else
          Result := Payload;
      VFS_BROKER_OP_SEEK:
        Result := Payload;
      VFS_BROKER_OP_CLOSE:
        Result := 0;
    else
      Result := QWord(-22);
    end;
    VFSBrokerClearPending(SlotIndex);
    Exit;
  end;

  Result := QWord(-19);
  if not VFSBrokerRegistered or not VFSBrokerServerAlive then Exit;

  if (Operation = VFS_BROKER_OP_READ) or
     (Operation = VFS_BROKER_OP_WRITE) then
  begin
    Result := QWord(-14);
    if (ByteCount <> 0) and not AddressSpaceValidateUserRange(
         ProcessTable[ProcessIndex].AddressSpaceRoot, UserBuffer, ByteCount) then Exit;
  end
  else if Operation = VFS_BROKER_OP_DIR_READ then
  begin
    Result := QWord(-14);
    TransferBytes := ByteCount * SizeOf(TVFSDirent);
    if (TransferBytes = 0) or (TransferBytes > VFS_IO_MAX) or
       not AddressSpaceValidateUserRange(
         ProcessTable[ProcessIndex].AddressSpaceRoot, UserBuffer,
         TransferBytes) then Exit;
  end
  else if PathOperation then
  begin
    PathLength := VFSStringLength(Path);
    Result := QWord(-36); { -ENAMETOOLONG }
    if (PathLength = 0) or (PathLength > VFS_PATH_MAX) then Exit;
    for I := 0 to High(LocalPath) do LocalPath[I] := 0;
    for I := 0 to PathLength - 1 do LocalPath[I] := Ord(Path[I]);
  end
  else if (Operation <> VFS_BROKER_OP_CLOSE) and
          (Operation <> VFS_BROKER_OP_SEEK) then
  begin
    Result := QWord(-22);
    Exit;
  end;

  PendingIndex := -1;
  for I := 0 to VFS_BROKER_PENDING_MAX - 1 do
    if VFSBrokerPending[I].Active = 0 then
    begin
      PendingIndex := LongInt(I);
      Break;
    end;
  Result := QWord(-11); { -EAGAIN: all bounded request slots are occupied. }
  if PendingIndex < 0 then Exit;
  SlotIndex := LongWord(PendingIndex);
  LaneAddress := VFS_BROKER_SHARED_BASE +
    QWord(SlotIndex) * VFS_BROKER_SLOT_SIZE;
  ServerRoot := ProcessTable[VFSBrokerServerIndex].AddressSpaceRoot;

  { Clear the complete private lane before staging this request. }
  for I := 0 to VFS_IO_MAX - 1 do VFSBrokerScratch[I] := 0;
  if not AddressSpaceCopyTo(ServerRoot, LaneAddress,
       @VFSBrokerScratch[0], VFS_IO_MAX) or
     not AddressSpaceCopyTo(ServerRoot, LaneAddress + VFS_IO_MAX,
       @VFSBrokerScratch[0], VFS_IO_MAX) then
  begin
    Result := QWord(-14);
    Exit;
  end;
  if Operation = VFS_BROKER_OP_WRITE then
  begin
    if (ByteCount <> 0) and not AddressSpaceCopyFrom(
         ProcessTable[ProcessIndex].AddressSpaceRoot, UserBuffer,
         @VFSBrokerScratch[0], ByteCount) then
    begin
      Result := QWord(-14);
      Exit;
    end;
    if (ByteCount <> 0) and not AddressSpaceCopyTo(
         ServerRoot, LaneAddress, @VFSBrokerScratch[0], ByteCount) then
    begin
      Result := QWord(-14);
      Exit;
    end;
  end
  else if PathOperation then
  begin
    if not AddressSpaceCopyTo(ServerRoot, LaneAddress,
         @LocalPath[0], SizeOf(LocalPath)) then
    begin
      Result := QWord(-14);
      Exit;
    end;
  end
  else if Operation = VFS_BROKER_OP_SEEK then
  begin
    for I := 0 to 7 do
      VFSBrokerScratch[I] := Byte(UserBuffer shr (I * 8));
    if not AddressSpaceCopyTo(ServerRoot, LaneAddress,
         @VFSBrokerScratch[0], 8) then
    begin
      Result := QWord(-14);
      Exit;
    end;
  end;

  Message.MessageId := 0;
  Message.WordCount := 6;
  for I := 0 to 6 do Message.Reserved[I] := 0;
  for I := 0 to IPC_MAX_WORDS - 1 do Message.Words[I] := 0;
  case Operation of
    VFS_BROKER_OP_OPEN: Message.MessageId := VFS_BROKER_OPEN;
    VFS_BROKER_OP_READ: Message.MessageId := VFS_BROKER_READ;
    VFS_BROKER_OP_WRITE: Message.MessageId := VFS_BROKER_WRITE;
    VFS_BROKER_OP_CLOSE: Message.MessageId := VFS_BROKER_CLOSE;
    VFS_BROKER_OP_DIR_OPEN: Message.MessageId := VFS_BROKER_DIR_OPEN;
    VFS_BROKER_OP_DIR_READ: Message.MessageId := VFS_BROKER_DIR_READ;
    VFS_BROKER_OP_CHDIR: Message.MessageId := VFS_BROKER_CHDIR;
    VFS_BROKER_OP_SEEK: Message.MessageId := VFS_BROKER_SEEK;
  end;
  if VFSBrokerNextTransaction = 0 then VFSBrokerNextTransaction := 1;
  Message.Words[0] := VFSBrokerNextTransaction;
  Message.Words[1] := ProcessTable[ProcessIndex].PID;
  if PathOperation then
  begin
    Message.Words[2] := Flags;
    Message.Words[3] := 0;
  end
  else
  begin
    Message.Words[2] := RemoteHandle;
    Message.Words[3] := ByteCount;
  end;
  Message.Words[4] := SlotIndex;
  Message.Words[5] := VFSBrokerServerGeneration;
  if not VFSBrokerQueueRequest(Message) then
  begin
    Result := QWord(-11);
    Exit;
  end;

  VFSBrokerPending[SlotIndex].Active := 1;
  VFSBrokerPending[SlotIndex].Ready := 0;
  VFSBrokerPending[SlotIndex].Cancelled := 0;
  VFSBrokerPending[SlotIndex].CancelSent := 0;
  VFSBrokerPending[SlotIndex].TimeoutDelivered := 0;
  VFSBrokerPending[SlotIndex].Operation := Operation;
  VFSBrokerPending[SlotIndex].OwnerIndex := ProcessIndex;
  VFSBrokerPending[SlotIndex].OwnerPID := ProcessTable[ProcessIndex].PID;
  VFSBrokerPending[SlotIndex].TransactionID := VFSBrokerNextTransaction;
  VFSBrokerPending[SlotIndex].ServerGeneration := VFSBrokerServerGeneration;
  VFSBrokerPending[SlotIndex].DeadlineTick :=
    VFSBrokerTickCounter + VFS_BROKER_TIMEOUT_TICKS;
  VFSBrokerPending[SlotIndex].CancelDeadlineTick := 0;
  VFSBrokerPending[SlotIndex].RemoteHandle := RemoteHandle;
  VFSBrokerPending[SlotIndex].UserBuffer := UserBuffer;
  VFSBrokerPending[SlotIndex].ByteCount := ByteCount;
  VFSBrokerPending[SlotIndex].Flags := Flags;
  VFSBrokerPending[SlotIndex].ResultValue := QWord(-11);
  VFSBrokerPending[SlotIndex].Payload := 0;
  for I := 0 to VFS_PATH_MAX do
    if PathOperation and (I <= LongWord(High(Path))) then
      VFSBrokerPending[SlotIndex].RequestPath[I] := Path[I]
    else
      VFSBrokerPending[SlotIndex].RequestPath[I] := #0;
  Inc(VFSBrokerNextTransaction);
  Result := QWord(-11);
end;

function VFSBrokerIsExt2Path(const Path: array of Char): Boolean;
begin
  Result := False;
  if Length(Path) < 10 then Exit;
  if (Path[0] <> '/') or (Path[1] <> 'm') or
     (Path[2] <> 'n') or (Path[3] <> 't') or
     (Path[4] <> '/') or (Path[5] <> 'd') or
     (Path[6] <> 'i') or (Path[7] <> 's') or
     (Path[8] <> 'k') then Exit;
  Result := (Path[9] = #0) or (Path[9] = '/');
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
    if VFSBrokerRegistered then
    begin
      if not VFSCopyUserString(Process^.AddressSpaceRoot, PathAddress,
           UserPath) then
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
      if VFSBrokerIsExt2Path(CanonicalPath) then
      begin
        Result := VFSBrokerCall(CurrentProcessIndex,
          VFS_BROKER_OP_DIR_OPEN, 0, 0, 0, Flags, CanonicalPath);
        Exit;
      end;
    end;
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

function SyscallGetDentsUser(Process: PKernelProcess; FileDescriptor,
  BufferAddress, MaximumEntries: QWord): QWord;
var
  RemoteHandle: QWord;
  EmptyPath: array[0..0] of Char;
begin
  if VFSIsRemoteDirectoryFD(Process^.Files, FileDescriptor, RemoteHandle) then
  begin
    EmptyPath[0] := #0;
    Result := VFSBrokerCall(CurrentProcessIndex, VFS_BROKER_OP_DIR_READ,
      RemoteHandle, BufferAddress, MaximumEntries, 0, EmptyPath);
    Exit;
  end;
  Result := VFSGetDentsUser(Process^.Files, Process^.AddressSpaceRoot,
    FileDescriptor, BufferAddress, MaximumEntries);
end;

function SyscallChdirUser(Process: PKernelProcess; PathAddress: QWord): QWord;
var
  UserPath, CanonicalPath: array[0..VFS_PATH_MAX] of Char;
begin
  if not VFSBrokerRegistered then
  begin
    Result := VFSChdirUser(Process^.Files, Process^.AddressSpaceRoot,
      PathAddress);
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
  if VFSBrokerIsExt2Path(CanonicalPath) then
  begin
    Result := VFSBrokerCall(CurrentProcessIndex, VFS_BROKER_OP_CHDIR,
      0, 0, 0, 0, CanonicalPath);
    Exit;
  end;
  Result := VFSChdirUser(Process^.Files, Process^.AddressSpaceRoot,
    PathAddress);
end;

function SyscallLseekUser(Process: PKernelProcess; FileDescriptor,
  Offset, Whence: QWord): QWord;
var
  RemoteHandle: QWord;
  RemoteFlags: LongWord;
  DescriptionIndex: LongInt;
  EmptyPath: array[0..0] of Char;
begin
  if Whence > 2 then
  begin
    Result := QWord(-22);
    Exit;
  end;
  if VFSIsRemoteFD(Process^.Files, FileDescriptor, RemoteHandle,
       RemoteFlags, DescriptionIndex) then
  begin
    EmptyPath[0] := #0;
    Result := VFSBrokerCall(CurrentProcessIndex, VFS_BROKER_OP_SEEK,
      RemoteHandle, Offset, Whence, 0, EmptyPath);
    Exit;
  end;
  Result := VFSSeekUser(Process^.Files, FileDescriptor, Offset, Whence);
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
      ReturnValue := SyscallGetDentsUser(Process, Frame^.SavedRDI,
        Frame^.SavedRSI, Frame^.SavedRDX);

    MISTIX_SYSCALL_CHDIR:
      ReturnValue := SyscallChdirUser(Process, Frame^.SavedRDI);

    MISTIX_SYSCALL_LSEEK:
      ReturnValue := SyscallLseekUser(Process, Frame^.SavedRDI,
        Frame^.SavedRSI, Frame^.SavedRDX);

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
