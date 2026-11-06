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
  EmptyArgs: array[0..0] of Char = (#0);

  VFS_RPC_PING  = $4D56534600000001;
  VFS_RPC_OPEN  = $4D56534600000002;
  VFS_RPC_READ  = $4D56534600000003;
  VFS_RPC_CLOSE = $4D56534600000004;
  BLOCK_RPC_INFO  = $4D424C4B00000001;
  BLOCK_RPC_READ  = $4D424C4B00000002;
  BLOCK_RPC_WRITE = $4D424C4B00000003;

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

function TestVFSService: Boolean;
var
  PID, RequestHandle, ReplyHandle, FileHandle, Count: QWord;
  Request, Response: TMistixIPCMessage;
  MotdPath: array[0..9] of Char;
begin
  Result := False;
  MotdPath[0] := '/'; MotdPath[1] := 'e'; MotdPath[2] := 't';
  MotdPath[3] := 'c'; MotdPath[4] := '/'; MotdPath[5] := 'm';
  MotdPath[6] := 'o'; MotdPath[7] := 't'; MotdPath[8] := 'd'; MotdPath[9] := #0;
  if not LaunchService(VFSdPath, PID, RequestHandle, ReplyHandle) then
  begin
    WriteCString('[FAIL] Could not start userland VFS server.'#13#10);
    Exit;
  end;
  WriteCString('[ OK ] vfsd spawned; IPC capabilities transferred.'#13#10);

  ClearMessage(Request);
  Request.MessageId := VFS_RPC_PING;
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) or
     (Response.Words[0] <> 1) then Exit;
  WriteCString('[ OK ] VFS RPC handshake passed.'#13#10);

  ClearMessage(Request);
  Request.MessageId := VFS_RPC_OPEN;
  PackPath(Request, MotdPath);
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) then Exit;
  FileHandle := Response.Words[0];
  WriteCString('[VFS] /etc/motd served by userland vfsd:'#13#10);

  repeat
    ClearMessage(Request);
    Request.MessageId := VFS_RPC_READ;
    Request.WordCount := 2;
    Request.Words[0] := FileHandle;
    Request.Words[1] := 40;
    if not RPCSend(RequestHandle, Request) or
       not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) then Exit;
    Count := Response.Words[0];
    if Count = 0 then Break;
    SysWriteAll(1, PtrUInt(@Response.Words[1]), Count);
  until False;

  ClearMessage(Request);
  Request.MessageId := VFS_RPC_CLOSE;
  Request.WordCount := 1;
  Request.Words[0] := FileHandle;
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) then Exit;
  WriteCString('[ OK ] VFS OPEN/READ/CLOSE RPC passed.'#13#10);
  Result := True;
end;

function TestBlockService: Boolean;
var
  PID, RequestHandle, ReplyHandle: QWord;
  Request, Response: TMistixIPCMessage;
  I: LongWord;
  Data: PByte;
  TestText: array[0..7] of Byte;
begin
  Result := False;
  TestText[0] := Ord('M'); TestText[1] := Ord('I');
  TestText[2] := Ord('S'); TestText[3] := Ord('T');
  TestText[4] := Ord('I'); TestText[5] := Ord('X');
  TestText[6] := Ord('6'); TestText[7] := Ord('!');
  if not LaunchService(BlockdPath, PID, RequestHandle, ReplyHandle) then
  begin
    WriteCString('[FAIL] Could not start block server.'#13#10);
    Exit;
  end;
  WriteCString('[ OK ] blockd spawned; IPC capabilities transferred.'#13#10);

  ClearMessage(Request);
  Request.MessageId := BLOCK_RPC_INFO;
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) or
     (Response.Words[1] <> 512) then Exit;
  WriteCString('[ OK ] blockd reports 512-byte sectors.'#13#10);

  ClearMessage(Request);
  Request.MessageId := BLOCK_RPC_WRITE;
  Request.WordCount := 3;
  Request.Words[0] := 0;
  Request.Words[1] := SizeOf(TestText);
  Data := PByte(@Request.Words[2]);
  for I := 0 to High(TestText) do Data[I] := TestText[I];
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) or
     (Response.Words[0] <> SizeOf(TestText)) then Exit;

  ClearMessage(Request);
  Request.MessageId := BLOCK_RPC_READ;
  Request.WordCount := 2;
  Request.Words[0] := 0;
  Request.Words[1] := SizeOf(TestText);
  if not RPCSend(RequestHandle, Request) or
     not RPCReceive(ReplyHandle, Response) or (Response.MessageId <> 0) or
     (Response.Words[0] <> SizeOf(TestText)) then Exit;
  Data := PByte(@Response.Words[1]);
  for I := 0 to High(TestText) do
    if Data[I] <> TestText[I] then Exit;
  WriteCString('[ OK ] RAM block server read/write round-trip passed.'#13#10);
  Result := True;
end;

procedure MistixInitMain; cdecl; public name 'mistix_user_main';
var
  PID, Status: QWord;
begin
  WriteCString(#13#10'Mistix Phase 6: userland storage services'#13#10);
  if not TestVFSService then
    WriteCString('[WARN] VFS service RPC test did not complete.'#13#10);
  if not TestBlockService then
    WriteCString('[WARN] Block service RPC test did not complete.'#13#10);
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
