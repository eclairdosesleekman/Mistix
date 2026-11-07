unit MistixInit;

{$mode objfpc}
{$H-}
{$R-}
{$Q-}
{$S-}
{$I-}
{$ALIGN 8}
{$ASMMODE intel}

interface

implementation

{$I mistixlib.inc}

const
  ShellPath: array[0..7] of Char = ('/','b','i','n','/','s','h',#0);
  VFSdPath: array[0..10] of Char = ('/','s','b','i','n','/','v','f','s','d',#0);
  BlockdPath: array[0..12] of Char = ('/','s','b','i','n','/','b','l','o','c','k','d',#0);
  DevdPath: array[0..10] of Char = ('/','s','b','i','n','/','d','e','v','d',#0);
  NetdPath: array[0..10] of Char = ('/','s','b','i','n','/','n','e','t','d',#0);
  Ext2RootPath: array[0..9] of Char = ('/','m','n','t','/','d','i','s','k',#0);
  EmptyArgs: array[0..0] of Char = (#0);
  RuntimeFaultPath: array[0..17] of Char = '/bin/runtimefault'#0;
  RuntimeRangeArgs: array[0..5] of Char = 'range'#0;
  RuntimeOverflowArgs: array[0..8] of Char = 'overflow'#0;
  RuntimeDivideArgs: array[0..6] of Char = 'divide'#0;
  RuntimeRangeName: array[0..5] of Char = 'range'#0;
  RuntimeOverflowName: array[0..8] of Char = 'overflow'#0;
  RuntimeDivideName: array[0..6] of Char = 'divide'#0;

  VFS_RPC_PING  = $4D56534600000001;
  VFS_RPC_OPEN  = $4D56534600000002;
  VFS_RPC_READ  = $4D56534600000003;
  VFS_RPC_CLOSE = $4D56534600000004;
  VFS_RPC_CREATE = $4D56534600000006;
  VFS_RPC_WRITE = $4D56534600000007;
  VFS_RPC_UNLINK = $4D56534600000008;
  VFS_RPC_MOUNT_EXT2 = $4D56534600000009;
  BLOCK_RPC_INFO  = $4D424C4B00000001;
  BLOCK_RPC_READ  = $4D424C4B00000002;
  BLOCK_RPC_WRITE = $4D424C4B00000003;
  DEV_RPC_REGISTER = $4D44455600000001;
  DEV_RPC_LIST = $4D44455600000002;
  NET_RPC_IFINFO = $4D4E455400000001;
  NET_RPC_ARP_LOOKUP = $4D4E455400000002;
  NET_RPC_UDP_ECHO = $4D4E455400000003;
  SHARED_BYTES = 65536;

var
  SharedHandle: QWord;
  SharedReady: Boolean;
  SharedBuffer: PByte;
  BlockPID: QWord;
  BlockRequestHandle: QWord;
  BlockReplyHandle: QWord;
  BlockServiceReady: Boolean;

procedure ClearMessage(var Message: TMistixIPCMessage);
var
  I: LongWord;
begin
  Message.MessageId := 0;
  Message.WordCount := 0;
  for I := 0 to 6 do Message.Reserved[I] := 0;
  for I := 0 to 5 do Message.Words[I] := 0;
end;

procedure PackPath(var Message: TMistixIPCMessage; const Path: array of Char);
var
  I, L: LongWord;
  Dest: PByte;
begin
  Dest := PByte(@Message.Words[0]);
  L := CStrLength(Path);
  if L > 39 then L := 39;
  for I := 0 to 39 do Dest[I] := 0;
  for I := 0 to L - 1 do Dest[I] := Ord(Path[I]);
  Message.WordCount := 5;
end;

function RPCSend(RequestHandle: QWord; const Request: TMistixIPCMessage): Boolean;
var
  R, Retry: QWord;
begin
  Result := False;
  Retry := 0;
  repeat
    R := SysIPCSend(RequestHandle, PtrUInt(@Request));
    if not IsError(R) then
    begin
      Result := True;
      Exit;
    end;
    if R <> MISTIX_EAGAIN then Exit;
    SysYield;
    Inc(Retry);
  until Retry >= 1000000;
end;

function RPCReceive(ReplyHandle: QWord; var Response: TMistixIPCMessage): Boolean;
var
  R: QWord;
  Retry: LongWord;
begin
  Result := False;
  Retry := 0;
  while Retry < 1000000 do
  begin
    R := SysIPCReceive(ReplyHandle, PtrUInt(@Response));
    if not IsError(R) then
    begin
      Result := True;
      Exit;
    end;
    if R <> MISTIX_EAGAIN then Exit;
    SysYield;
    Inc(Retry);
  end;
end;

function LaunchService(const Path: array of Char; out PID,
  RequestHandle, ReplyHandle: QWord): Boolean;
var
  R: QWord;
begin
  Result := False;
  PID := 0;
  RequestHandle := SysEndpointCreate;
  if IsError(RequestHandle) then Exit;
  ReplyHandle := SysEndpointCreate;
  if IsError(ReplyHandle) then Exit;
  PID := SysSpawnPath(PtrUInt(@Path[0]), PtrUInt(@EmptyArgs[0]), 0, 1);
  if IsError(PID) then Exit;
  R := SysCapabilityTransfer(PID, RequestHandle, MISTIX_CAP_RECEIVE);
  if IsError(R) then Exit;
  R := SysCapabilityTransfer(PID, ReplyHandle, MISTIX_CAP_SEND);
  if IsError(R) then Exit;
  Result := True;
end;

function EnsureSharedMemory: Boolean;
begin
  Result := SharedReady;
  if Result then Exit;
  SharedHandle := SysShmCreate(16);
  if IsError(SharedHandle) then Exit(False);
  if IsError(SysShmMap(SharedHandle, MISTIX_SHM_DEFAULT_BASE, 1)) then Exit(False);
  SharedBuffer := PByte(PtrUInt(MISTIX_SHM_DEFAULT_BASE));
  SharedReady := True;
  Result := True;
end;

function ShareMemoryWith(PID: QWord): Boolean;
var
  R: QWord;
begin
  Result := False;
  if not EnsureSharedMemory then Exit;
  R := SysCapabilityTransfer(PID, SharedHandle,
    MISTIX_CAP_MAP or MISTIX_CAP_READ or MISTIX_CAP_WRITE or MISTIX_CAP_USER_MAP);
  Result := not IsError(R);
end;

function StartBlockService: Boolean;
begin
  Result := BlockServiceReady;
  if Result then Exit;
  if not EnsureSharedMemory then Exit(False);
  if not LaunchService(BlockdPath, BlockPID, BlockRequestHandle,
       BlockReplyHandle) then Exit(False);
  if not ShareMemoryWith(BlockPID) then Exit(False);
  BlockServiceReady := True;
  WriteCString('[ OK ] blockd launched with restricted ATA/RAM backend capability.'#13#10);
  Result := True;
end;

function TestVFSService: Boolean;
var
  PID, RequestHandle, ReplyHandle, FileHandle, Count: QWord;
  Request, Response: TMistixIPCMessage;
  MotdPath: array[0..9] of Char;
  TempPath: array[0..15] of Char;
  Payload: array[0..24] of Char;
  ProxyBuffer: array[0..127] of Byte;
  ProxyLength: QWord;
  I: LongWord;
begin
  Result := False;
  MotdPath[0] := '/'; MotdPath[1] := 'e'; MotdPath[2] := 't';
  MotdPath[3] := 'c'; MotdPath[4] := '/'; MotdPath[5] := 'm';
  MotdPath[6] := 'o'; MotdPath[7] := 't'; MotdPath[8] := 'd'; MotdPath[9] := #0;
  if not EnsureSharedMemory then Exit;
  if not LaunchService(VFSdPath, PID, RequestHandle, ReplyHandle) then
  begin
    WriteCString('[FAIL] Could not start userland VFS server.'#13#10);
    Exit;
  end;
  if not ShareMemoryWith(PID) then Exit;
  if not BlockServiceReady then Exit;
  if IsError(SysCapabilityTransfer(PID, BlockRequestHandle, MISTIX_CAP_SEND)) then Exit;
  if IsError(SysCapabilityTransfer(PID, BlockReplyHandle, MISTIX_CAP_RECEIVE)) then Exit;
  WriteCString('[ OK ] vfsd received shared-memory and blockd RPC capabilities.'#13#10);

  ClearMessage(Request);
  Request.MessageId := VFS_RPC_PING;
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) or
     (Response.Words[0] <> 3) then Exit;
  WriteCString('[ OK ] VFS RPC v3 handshake passed.'#13#10);

  ClearMessage(Request);
  Request.MessageId := VFS_RPC_MOUNT_EXT2;
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) then Exit;
  if Response.MessageId = 0 then
  begin
    WriteCString('[ OK ] Ext2 mounted read-only at /mnt/disk through blockd RPC.'#13#10);
    ClearMessage(Request);
    Request.MessageId := VFS_RPC_READDIR;
    PackPath(Request, Ext2RootPath);
    if RPCSend(RequestHandle, Request) and RPCReceive(ReplyHandle, Response) and
       (Response.MessageId = 0) then
      WriteCString('[ OK ] Ext2 root directory enumeration returned entries.'#13#10)
    else
      WriteCString('[WARN] Ext2 root directory enumeration was unavailable.'#13#10);
  end
  else if Response.MessageId = QWord(-19) then
    WriteCString('[INFO] No physical Ext2 disk is available; /mnt/disk remains unmounted.'#13#10)
  else
    WriteCString('[WARN] Disk found but Ext2 mount was rejected.'#13#10);

  ClearMessage(Request);
  Request.MessageId := VFS_RPC_OPEN;
  PackPath(Request, MotdPath);
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) then Exit;
  FileHandle := Response.Words[0];
  WriteCString('[VFS] /etc/motd via shared-memory READ:'#13#10);

  repeat
    ClearMessage(Request);
    Request.MessageId := VFS_RPC_READ;
    Request.WordCount := 2;
    Request.Words[0] := FileHandle;
    Request.Words[1] := SHARED_BYTES;
    if not RPCSend(RequestHandle, Request) or
       not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) then Exit;
    Count := Response.Words[0];
    if Count = 0 then Break;
    if not SysWriteAll(1, PtrUInt(SharedBuffer), Count) then Exit;
  until False;

  ClearMessage(Request);
  Request.MessageId := VFS_RPC_CLOSE;
  Request.WordCount := 1;
  Request.Words[0] := FileHandle;
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) then Exit;

  TempPath[0] := '/'; TempPath[1] := 't'; TempPath[2] := 'm'; TempPath[3] := 'p';
  TempPath[4] := '/'; TempPath[5] := 'p'; TempPath[6] := 'h'; TempPath[7] := 'a';
  TempPath[8] := 's'; TempPath[9] := 'e'; TempPath[10] := '9'; TempPath[11] := '.';
  TempPath[12] := 't'; TempPath[13] := 'x'; TempPath[14] := 't'; TempPath[15] := #0;
  ClearMessage(Request);
  Request.MessageId := VFS_RPC_CREATE;
  PackPath(Request, TempPath);
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) then Exit;

  ClearMessage(Request);
  Request.MessageId := VFS_RPC_OPEN;
  PackPath(Request, TempPath);
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) then Exit;
  FileHandle := Response.Words[0];
  for I := 0 to High(Payload) do Payload[I] := #0;
  Payload[0] := 'M'; Payload[1] := 'i'; Payload[2] := 's'; Payload[3] := 't';
  Payload[4] := 'i'; Payload[5] := 'x'; Payload[6] := ' '; Payload[7] := 'P';
  Payload[8] := 'h'; Payload[9] := 'a'; Payload[10] := 's'; Payload[11] := 'e';
  Payload[12] := ' '; Payload[13] := '9'; Payload[14] := #13; Payload[15] := #10;
  for I := 0 to 15 do SharedBuffer[I] := Ord(Payload[I]);
  ClearMessage(Request);
  Request.MessageId := VFS_RPC_WRITE;
  Request.WordCount := 2;
  Request.Words[0] := FileHandle;
  Request.Words[1] := 16;
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) or
     (Response.Words[0] <> 16) then Exit;
  ClearMessage(Request);
  Request.MessageId := VFS_RPC_CLOSE;
  Request.WordCount := 1;
  Request.Words[0] := FileHandle;
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) then Exit;

  ClearMessage(Request);
  Request.MessageId := VFS_RPC_OPEN;
  PackPath(Request, TempPath);
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) then Exit;
  FileHandle := Response.Words[0];
  ClearMessage(Request);
  Request.MessageId := VFS_RPC_READ;
  Request.WordCount := 2;
  Request.Words[0] := FileHandle;
  Request.Words[1] := 16;
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) or
     (Response.Words[0] <> 16) then Exit;
  for I := 0 to 15 do
    if SharedBuffer[I] <> Ord(Payload[I]) then Exit;
  WriteCString('[ OK ] Writable tmpfs overlay create/write/read passed.'#13#10);
  ClearMessage(Request);
  Request.MessageId := VFS_RPC_CLOSE;
  Request.WordCount := 1;
  Request.Words[0] := FileHandle;
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) then Exit;

  { Register the same endpoint pair with the kernel. From this point onward,
    ordinary open/read/write/close syscalls are brokered to this vfsd. }
  if IsError(SysVFSRegister(PID, RequestHandle, ReplyHandle)) then Exit;
  WriteCString('[ OK ] Standard file syscalls now route through userland vfsd.'#13#10);
  FileHandle := SysOpen(PtrUInt(@MotdPath[0]), MISTIX_O_RDONLY);
  if IsError(FileHandle) then Exit;
  ProxyLength := SysRead(FileHandle, PtrUInt(@ProxyBuffer[0]), SizeOf(ProxyBuffer));
  if IsError(ProxyLength) or (ProxyLength = 0) then Exit;
  if not SysWriteAll(1, PtrUInt(@ProxyBuffer[0]), ProxyLength) then Exit;
  if IsError(SysClose(FileHandle)) then Exit;
  WriteCString('[ OK ] open/read/close syscall proxy round-trip passed.'#13#10);

  FileHandle := SysOpen(PtrUInt(@TempPath[0]), MISTIX_O_RDWR or MISTIX_O_CREAT);
  if IsError(FileHandle) then Exit;
  if not SysWriteAll(FileHandle, PtrUInt(@Payload[0]), 16) then Exit;
  if IsError(SysClose(FileHandle)) then Exit;
  FileHandle := SysOpen(PtrUInt(@TempPath[0]), MISTIX_O_RDONLY);
  if IsError(FileHandle) then Exit;
  ProxyLength := SysRead(FileHandle, PtrUInt(@ProxyBuffer[0]), 16);
  if IsError(ProxyLength) or (ProxyLength <> 16) then Exit;
  for I := 0 to 15 do
    if ProxyBuffer[I] <> Ord(Payload[I]) then Exit;
  if IsError(SysClose(FileHandle)) then Exit;
  WriteCString('[ OK ] standard syscall O_CREAT/write/read/close proxy round-trip passed.'#13#10);
  Result := True;
end;

function TestBlockService: Boolean;
var
  Request, Response: TMistixIPCMessage;
  I: LongWord;
  TestText: array[0..7] of Byte;
begin
  Result := False;
  if not BlockServiceReady or not EnsureSharedMemory then Exit;
  ClearMessage(Request);
  Request.MessageId := BLOCK_RPC_INFO;
  if not RPCSend(BlockRequestHandle, Request) or
     not RPCReceive(BlockReplyHandle, Response) or (Response.MessageId <> 0) or
     (Response.Words[1] <> 512) then Exit;

  if Response.Words[3] = 1 then
  begin
    { Never write to an attached physical disk during a boot self-test. }
    ClearMessage(Request);
    Request.MessageId := BLOCK_RPC_READ;
    Request.WordCount := 2;
    Request.Words[0] := 0;
    Request.Words[1] := 512;
    if not RPCSend(BlockRequestHandle, Request) or
       not RPCReceive(BlockReplyHandle, Response) or
       (Response.MessageId <> 0) or (Response.Words[0] <> 512) then Exit;
    WriteCString('[ OK ] blockd physical ATA read-only sector RPC passed.'#13#10);
  end
  else
  begin
    if Response.Words[2] <> SHARED_BYTES then Exit;
    TestText[0] := Ord('M'); TestText[1] := Ord('I');
    TestText[2] := Ord('S'); TestText[3] := Ord('T');
    TestText[4] := Ord('I'); TestText[5] := Ord('X');
    TestText[6] := Ord('8'); TestText[7] := Ord('!');
    for I := 0 to High(TestText) do SharedBuffer[I] := TestText[I];
    ClearMessage(Request);
    Request.MessageId := BLOCK_RPC_WRITE;
    Request.WordCount := 2;
    Request.Words[0] := 0;
    Request.Words[1] := SizeOf(TestText);
    if not RPCSend(BlockRequestHandle, Request) or
       not RPCReceive(BlockReplyHandle, Response) or (Response.MessageId <> 0) or
       (Response.Words[0] <> SizeOf(TestText)) then Exit;
    for I := 0 to High(TestText) do SharedBuffer[I] := 0;
    ClearMessage(Request);
    Request.MessageId := BLOCK_RPC_READ;
    Request.WordCount := 2;
    Request.Words[0] := 0;
    Request.Words[1] := SizeOf(TestText);
    if not RPCSend(BlockRequestHandle, Request) or
       not RPCReceive(BlockReplyHandle, Response) or (Response.MessageId <> 0) or
       (Response.Words[0] <> SizeOf(TestText)) then Exit;
    for I := 0 to High(TestText) do
      if SharedBuffer[I] <> TestText[I] then Exit;
    WriteCString('[ OK ] blockd shared-memory RAM READ/WRITE round-trip passed.'#13#10);
  end;
  Result := True;
end;

function TestDeviceManager: Boolean;
var
  PID, RequestHandle, ReplyHandle: QWord;
  Request, Response: TMistixIPCMessage;
  NameBytes: PByte;
begin
  Result := False;
  if not LaunchService(DevdPath, PID, RequestHandle, ReplyHandle) then Exit;
  ClearMessage(Request);
  Request.MessageId := DEV_RPC_REGISTER;
  Request.WordCount := 6;
  Request.Words[0] := 1; { class: character device }
  Request.Words[1] := 1; { state: online }
  NameBytes := PByte(@Request.Words[2]);
  NameBytes[0] := Ord('s'); NameBytes[1] := Ord('e'); NameBytes[2] := Ord('r');
  NameBytes[3] := Ord('i'); NameBytes[4] := Ord('a'); NameBytes[5] := Ord('l');
  NameBytes[6] := Ord('0'); NameBytes[7] := 0;
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) then Exit;
  ClearMessage(Request);
  Request.MessageId := DEV_RPC_LIST;
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) or
     (Response.Words[0] = 0) then Exit;
  WriteCString('[ OK ] Userland device-manager register/list RPC passed.'#13#10);
  Result := True;
end;

function TestNetworkService: Boolean;
var
  PID, RequestHandle, ReplyHandle: QWord;
  Request, Response: TMistixIPCMessage;
  I: LongWord;
  TestPayload: array[0..11] of Byte;
begin
  Result := False;
  if not EnsureSharedMemory then Exit;
  if not LaunchService(NetdPath, PID, RequestHandle, ReplyHandle) then Exit;
  if not ShareMemoryWith(PID) then Exit;
  ClearMessage(Request);
  Request.MessageId := NET_RPC_IFINFO;
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) or
     (Response.Words[0] <> $7F000001) then Exit;
  ClearMessage(Request);
  Request.MessageId := NET_RPC_ARP_LOOKUP;
  Request.WordCount := 1;
  Request.Words[0] := $7F000001;
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) then Exit;
  for I := 0 to High(TestPayload) do
  begin
    TestPayload[I] := Ord('A') + I;
    SharedBuffer[I] := TestPayload[I];
  end;
  ClearMessage(Request);
  Request.MessageId := NET_RPC_UDP_ECHO;
  Request.WordCount := 2;
  Request.Words[0] := 7000;
  Request.Words[1] := SizeOf(TestPayload);
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) or
     (Response.Words[0] <> SizeOf(TestPayload)) then Exit;
  for I := 0 to High(TestPayload) do
    if SharedBuffer[I] <> TestPayload[I] then Exit;
  WriteCString('[ OK ] Loopback IPv4/UDP + ARP-cache RPC self-test passed.'#13#10);
  Result := True;
end;


{$ifdef MISTIX_DEBUG_CHECKS}
function RunRuntimeFaultCase(const Args, CaseName: array of Char;
  ExpectedStatus: QWord): Boolean;
var
  PID, WaitedPID, Status: QWord;
begin
  Result := False;
  PID := SysSpawnPath(PtrUInt(@RuntimeFaultPath[0]),
    PtrUInt(@Args[0]), 0, 1);
  if IsError(PID) then
  begin
    WriteCString('[FAIL] Could not spawn Phase 17 runtime-fault child: ');
    WriteCString(CaseName);
    WriteCString(#13#10);
    Exit;
  end;

  Status := High(QWord);
  WaitedPID := SysWaitPID(PID, PtrUInt(@Status));
  if WaitedPID <> PID then
  begin
    WriteCString('[FAIL] waitpid did not reap Phase 17 child: ');
    WriteCString(CaseName);
    WriteCString(#13#10);
    Exit;
  end;
  if Status <> ExpectedStatus then
  begin
    WriteCString('[FAIL] Phase 17 runtime-fault status mismatch for ');
    WriteCString(CaseName);
    WriteCString(': expected ');
    WriteNumber(ExpectedStatus);
    WriteCString(', got ');
    WriteNumber(Status);
    WriteCString(#13#10);
    Exit;
  end;

  WriteCString('[ OK ] Phase 17 isolated runtime fault: ');
  WriteCString(CaseName);
  WriteCString(' returned the expected exit status.'#13#10);
  Result := True;
end;

function TestRuntimeFaultIsolation: Boolean;
begin
  Result := False;
  if not RunRuntimeFaultCase(RuntimeRangeArgs, RuntimeRangeName, 201) then Exit;
  if not RunRuntimeFaultCase(RuntimeOverflowArgs, RuntimeOverflowName, 215) then Exit;
  if not RunRuntimeFaultCase(RuntimeDivideArgs, RuntimeDivideName, 200) then Exit;
  WriteCString('[ OK ] Phase 17 runtime fault isolation tests passed.'#13#10);
  Result := True;
end;
{$endif}

procedure MistixInitMain; cdecl; public name 'mistix_user_main';
var
  PID, Status: QWord;
begin
  WriteCString(#13#10'Mistix Phase 9: userland VFS syscall broker'#13#10);
  if not StartBlockService then
    WriteCString('[WARN] Could not start blockd; disk and Ext2 services unavailable.'#13#10);
  if not TestBlockService then
    WriteCString('[WARN] Block service test did not complete.'#13#10);
  if not TestVFSService then
    WriteCString('[WARN] VFS service RPC test did not complete.'#13#10);
  if not TestDeviceManager then
    WriteCString('[WARN] Device-manager RPC test did not complete.'#13#10);
  if not TestNetworkService then
    WriteCString('[WARN] Network service RPC test did not complete.'#13#10);
{$ifdef MISTIX_DEBUG_CHECKS}
  if not TestRuntimeFaultIsolation then
  begin
    WriteCString('[FAIL] Phase 17 runtime fault isolation tests failed.'#13#10);
    SysExit(1);
  end;
{$endif}
  WriteCString(#13#10'Starting Mistix shell.'#13#10);
  repeat
    PID := SysSpawnPath(PtrUInt(@ShellPath[0]), PtrUInt(@EmptyArgs[0]), 0, 1);
    if not IsError(PID) then
    begin
      Status := 0;
      SysWaitPID(PID, PtrUInt(@Status));
    end;
    SysYield;
  until False;
end;

procedure MistixInitEntry; assembler; nostackframe; public name '_start';
asm
  and rsp, -16
  call mistix_user_main
  xor edi, edi
  mov eax, 2
  int $80
  ud2
end;

end.
