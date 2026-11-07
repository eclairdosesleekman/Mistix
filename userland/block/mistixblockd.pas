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
  BLOCK_RPC_READ_MAX = 65536;
  BLOCK_RPC_WRITE_MAX = BLOCK_RAMDISK_BYTES;
  BLOCK_SHARED_BASE = MISTIX_SHM_DEFAULT_BASE;
  BLOCK_ERR_INVAL = QWord(-22);
  BLOCK_ERR_RANGE = QWord(-34);
  BLOCK_ERR_NODEV = QWord(-19);
  BLOCK_ERR_ROFS = QWord(-30);
  BLOCK_ERR_IO = QWord(-5);

type
  TRAMBlockDevice = array[0..BLOCK_RAMDISK_BYTES - 1] of Byte;
  THardwareScratch = array[0..65535 + 511] of Byte;

var
  RAMDisk: TRAMBlockDevice;
  HardwareScratch: THardwareScratch;
  RequestEndpoint: QWord;
  ReplyEndpoint: QWord;
  SharedHandle: QWord;
  SharedBuffer: PByte;
  HardwareOnline: Boolean;
  HardwareInfo: TMistixBlockDeviceInfo;

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
  A, B, C, R: QWord;
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
  { Registration is restricted by the kernel to PID 1's direct child. }
  R := SysBlockRegister;
  if IsError(R) then Exit;
  HardwareOnline := not IsError(SysBlockInfo(PtrUInt(@HardwareInfo)));
  Result := True;
end;

function HardwareReadBytes(Offset, Count: QWord): Boolean;
var
  StartLBA, Prefix, SectorCount, ByteCount: QWord;
  CursorLBA, RemainingSectors, BatchSectors, ScratchOffset: QWord;
  I: QWord;
  Status: QWord;
begin
  Result := False;
  if not HardwareOnline then Exit;
  if (Count = 0) or (Count > BLOCK_RPC_READ_MAX) then Exit;
  StartLBA := Offset div BLOCK_RPC_SECTOR_SIZE;
  Prefix := Offset mod BLOCK_RPC_SECTOR_SIZE;
  SectorCount := (Prefix + Count + BLOCK_RPC_SECTOR_SIZE - 1) div BLOCK_RPC_SECTOR_SIZE;
  if (SectorCount = 0) or (SectorCount > 128) then Exit;
  if (StartLBA > HardwareInfo.SectorCount) or
     (SectorCount > HardwareInfo.SectorCount - StartLBA) then Exit;
  { A 64 KiB unaligned byte range can span 129 sectors. Split it into
    bounded kernel calls (each syscall permits at most 128 sectors). }
  CursorLBA := StartLBA;
  RemainingSectors := SectorCount;
  ScratchOffset := 0;
  while RemainingSectors <> 0 do
  begin
    BatchSectors := RemainingSectors;
    if BatchSectors > 128 then BatchSectors := 128;
    Status := SysBlockRead(CursorLBA, BatchSectors,
      PtrUInt(@HardwareScratch[ScratchOffset]));
    ByteCount := BatchSectors * BLOCK_RPC_SECTOR_SIZE;
    if IsError(Status) or (Status <> ByteCount) then Exit;
    Inc(CursorLBA, BatchSectors);
    Inc(ScratchOffset, ByteCount);
    Dec(RemainingSectors, BatchSectors);
  end;
  for I := 0 to Count - 1 do
    SharedBuffer[I] := HardwareScratch[Prefix + I];
  Result := True;
end;

procedure HandleRequest(const Request: TMistixIPCMessage;
  var Response: TMistixIPCMessage);
var
  Offset, Count, I: QWord;
  Source, Dest: PByte;
begin
  ClearMessage(Response);
  Response.MessageId := BLOCK_ERR_INVAL;
  case Request.MessageId of
    BLOCK_RPC_INFO:
      begin
        Response.MessageId := 0;
        Response.WordCount := 4;
        if HardwareOnline then
        begin
          Response.Words[0] := HardwareInfo.SectorCount;
          Response.Words[1] := HardwareInfo.SectorSize;
          Response.Words[2] := HardwareInfo.SectorCount * HardwareInfo.SectorSize;
          Response.Words[3] := 1; { ATA PIO read-only backend }
        end
        else
        begin
          Response.Words[0] := BLOCK_RAMDISK_BYTES div BLOCK_RPC_SECTOR_SIZE;
          Response.Words[1] := BLOCK_RPC_SECTOR_SIZE;
          Response.Words[2] := BLOCK_RAMDISK_BYTES;
          Response.Words[3] := 0; { volatile RAM fallback }
        end;
      end;

    BLOCK_RPC_READ:
      begin
        if Request.WordCount < 2 then Exit;
        Offset := Request.Words[0];
        Count := Request.Words[1];
        if (Count = 0) or (Count > BLOCK_RPC_READ_MAX) then
        begin
          Response.MessageId := BLOCK_ERR_RANGE;
          Exit;
        end;
        if HardwareOnline then
        begin
          if (Offset > HardwareInfo.SectorCount * BLOCK_RPC_SECTOR_SIZE) or
             (Count > HardwareInfo.SectorCount * BLOCK_RPC_SECTOR_SIZE - Offset) then
          begin
            Response.MessageId := BLOCK_ERR_RANGE;
            Exit;
          end;
          if not HardwareReadBytes(Offset, Count) then
          begin
            Response.MessageId := BLOCK_ERR_IO;
            Exit;
          end;
        end
        else
        begin
          if (Offset > BLOCK_RAMDISK_BYTES) or
             (Count > BLOCK_RAMDISK_BYTES - Offset) then
          begin
            Response.MessageId := BLOCK_ERR_RANGE;
            Exit;
          end;
          Dest := SharedBuffer;
          for I := 0 to Count - 1 do
            Dest[I] := RAMDisk[Offset + I];
        end;
        Response.MessageId := 0;
        Response.WordCount := 1;
        Response.Words[0] := Count;
      end;

    BLOCK_RPC_WRITE:
      begin
        if Request.WordCount < 2 then Exit;
        Offset := Request.Words[0];
        Count := Request.Words[1];
        if HardwareOnline then
        begin
          { Raw disk writes remain disabled until a block-device capability and
            durable filesystem transaction layer are implemented. }
          Response.MessageId := BLOCK_ERR_ROFS;
          Exit;
        end;
        if (Count = 0) or (Count > BLOCK_RPC_WRITE_MAX) or
           (Offset > BLOCK_RAMDISK_BYTES) or
           (Count > BLOCK_RAMDISK_BYTES - Offset) then
        begin
          Response.MessageId := BLOCK_ERR_RANGE;
          Exit;
        end;
        Source := SharedBuffer;
        for I := 0 to Count - 1 do
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
  if HardwareOnline then
    WriteCString('blockd: ATA PIO read-only backend online; shared-memory RPC enabled'#13#10)
  else
    WriteCString('blockd: 64 KiB volatile RAM block fallback online'#13#10);
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
  call MistixBlockdMain
  xor edi, edi
  mov eax, 2
  int $80
  ud2
end;

end.
