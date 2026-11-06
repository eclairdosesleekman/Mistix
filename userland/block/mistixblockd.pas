unit MistixBlockd;

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
  BLOCK_RPC_INFO  = $4D424C4B00000001;
  BLOCK_RPC_READ  = $4D424C4B00000002;
  BLOCK_RPC_WRITE = $4D424C4B00000003;
  BLOCK_RPC_SECTOR_SIZE = 512;
  BLOCK_RAMDISK_BYTES = 65536;
  BLOCK_RPC_READ_MAX = BLOCK_RAMDISK_BYTES;
  BLOCK_RPC_WRITE_MAX = BLOCK_RAMDISK_BYTES;
  BLOCK_SHARED_BASE = MISTIX_SHM_DEFAULT_BASE;
  BLOCK_ERR_INVAL = QWord(-22);
  BLOCK_ERR_RANGE = QWord(-34);

type
  TRAMBlockDevice = array[0..BLOCK_RAMDISK_BYTES - 1] of Byte;

var
  RAMDisk: TRAMBlockDevice;
  RequestEndpoint: QWord;
  ReplyEndpoint: QWord;
  SharedHandle: QWord;
  SharedBuffer: PByte;

procedure ClearMessage(var Message: TMistixIPCMessage);
var
  I: LongWord;
begin
  Message.MessageId := 0;
  Message.WordCount := 0;
  for I := 0 to 6 do Message.Reserved[I] := 0;
  for I := 0 to 5 do Message.Words[I] := 0;
end;

function ReceiveOneCapability(out Handle: QWord): Boolean;
var
  Retry: LongWord;
begin
  Result := False;
  Handle := 0;
  Retry := 0;
  while Retry < 1000000 do
  begin
    Handle := SysCapabilityReceive;
    if not IsError(Handle) then
    begin
      Result := Handle <> 0;
      Exit;
    end;
    if Handle <> MISTIX_EAGAIN then Exit;
    SysYield;
    Inc(Retry);
  end;
end;

function ReceiveCapabilities: Boolean;
var
  A, B, C: QWord;
begin
  Result := False;
  if not ReceiveOneCapability(A) then Exit;
  if not ReceiveOneCapability(B) then Exit;
  if not ReceiveOneCapability(C) then Exit;
  RequestEndpoint := A;
  ReplyEndpoint := B;
  SharedHandle := C;
  if IsError(SysShmMap(SharedHandle, BLOCK_SHARED_BASE, 1)) then Exit;
  SharedBuffer := PByte(PtrUInt(BLOCK_SHARED_BASE));
  Result := True;
end;

procedure HandleRequest(const Request: TMistixIPCMessage;
  var Response: TMistixIPCMessage);
var
  Offset, Count: QWord;
  I: LongWord;
  Source, Dest: PByte;
begin
  ClearMessage(Response);
  Response.MessageId := BLOCK_ERR_INVAL;
  case Request.MessageId of
    BLOCK_RPC_INFO:
      begin
        Response.MessageId := 0;
        Response.WordCount := 3;
        Response.Words[0] := BLOCK_RAMDISK_BYTES div BLOCK_RPC_SECTOR_SIZE;
        Response.Words[1] := BLOCK_RPC_SECTOR_SIZE;
        Response.Words[2] := BLOCK_RAMDISK_BYTES;
      end;
    BLOCK_RPC_READ:
      begin
        if Request.WordCount < 2 then Exit;
        Offset := Request.Words[0];
        Count := Request.Words[1];
        if (Count = 0) or (Count > BLOCK_RPC_READ_MAX) or
           (Offset > BLOCK_RAMDISK_BYTES) or
           (Count > BLOCK_RAMDISK_BYTES - Offset) then
        begin
          Response.MessageId := BLOCK_ERR_RANGE;
          Exit;
        end;
        Dest := SharedBuffer;
        for I := 0 to LongWord(Count) - 1 do
          Dest[I] := RAMDisk[Offset + I];
        Response.MessageId := 0;
        Response.WordCount := 1;
        Response.Words[0] := Count;
      end;
    BLOCK_RPC_WRITE:
      begin
        if Request.WordCount < 2 then Exit;
        Offset := Request.Words[0];
        Count := Request.Words[1];
        if (Count = 0) or (Count > BLOCK_RPC_WRITE_MAX) or
           (Offset > BLOCK_RAMDISK_BYTES) or
           (Count > BLOCK_RAMDISK_BYTES - Offset) then
        begin
          Response.MessageId := BLOCK_ERR_RANGE;
          Exit;
        end;
        Source := SharedBuffer;
        for I := 0 to LongWord(Count) - 1 do
          RAMDisk[Offset + I] := Source[I];
        Response.MessageId := 0;
        Response.WordCount := 1;
        Response.Words[0] := Count;
      end;
  end;
end;

procedure MistixBlockdMain; cdecl; public name 'mistix_user_main';
var
  Request, Response: TMistixIPCMessage;
  Status: QWord;
begin
  if not ReceiveCapabilities then SysExit(20);
  WriteCString('blockd: 128-sector RAM disk with shared-memory bulk-transfer window online'#13#10);
  while True do
  begin
    Status := SysIPCReceive(RequestEndpoint, PtrUInt(@Request));
    if IsError(Status) then
    begin
      SysYield;
      Continue;
    end;
    HandleRequest(Request, Response);
    repeat
      Status := SysIPCSend(ReplyEndpoint, PtrUInt(@Response));
      if Status = MISTIX_EAGAIN then SysYield
      else if IsError(Status) then SysExit(12);
    until not IsError(Status);
  end;
end;

procedure MistixBlockdEntry; assembler; nostackframe; public name '_start';
asm
  and rsp, -16
  call mistix_user_main
  xor edi, edi
  mov eax, 2
  int $80
  ud2
end;

end.
