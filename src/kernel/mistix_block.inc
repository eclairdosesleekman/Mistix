{ Mistix Phase 6 block layer: polling ATA PIO + MBR partition discovery.
  The driver is deliberately single-channel and single-CPU. It is a bring-up
  backend; AHCI/NVMe and interrupt-driven DMA are not part of this phase. }

const
  BLOCK_DEVICE_MAX = 4;
  BLOCK_PARTITION_MAX = 4;
  BLOCK_SECTOR_SIZE = 512;
  BLOCK_ERR_IO = QWord(-5);
  BLOCK_ERR_NODEV = QWord(-19);
  BLOCK_ERR_BUSY = QWord(-16);
  BLOCK_ERR_RANGE = QWord(-34);

  ATA_PRIMARY_IO = $1F0;
  ATA_PRIMARY_CTRL = $3F6;
  ATA_REG_DATA = 0;
  ATA_REG_ERROR = 1;
  ATA_REG_FEATURES = 1;
  ATA_REG_SECCOUNT0 = 2;
  ATA_REG_LBA0 = 3;
  ATA_REG_LBA1 = 4;
  ATA_REG_LBA2 = 5;
  ATA_REG_HDDEVSEL = 6;
  ATA_REG_COMMAND = 7;
  ATA_REG_STATUS = 7;
  ATA_CMD_IDENTIFY = $EC;
  ATA_CMD_READ_SECTORS = $20;
  ATA_CMD_WRITE_SECTORS = $30;
  ATA_CMD_CACHE_FLUSH = $E7;
  ATA_STATUS_ERR = $01;
  ATA_STATUS_DRQ = $08;
  ATA_STATUS_DF = $20;
  ATA_STATUS_DRDY = $40;
  ATA_STATUS_BSY = $80;

type
  TBlockDeviceKind = (bdNone, bdATA_PIO);
  TBlockDevice = record
    Active: Byte;
    Kind: TBlockDeviceKind;
    BasePort: Word;
    ControlPort: Word;
    SectorCount: QWord;
    LBA48: Byte;
    Name: array[0..15] of Char;
  end;

  TBlockPartition = record
    Active: Byte;
    DeviceIndex: Word;
    TypeCode: Byte;
    StartLBA: QWord;
    SectorCount: QWord;
  end;

var
  BlockDevices: array[0..BLOCK_DEVICE_MAX - 1] of TBlockDevice;
  BlockPartitions: array[0..BLOCK_PARTITION_MAX - 1] of TBlockPartition;
  BlockBusy: Byte;
  BlockInitialized: Boolean;
  BlockServerPID: QWord; { only the registered blockd may use raw read syscalls }
  BlockSectorScratch: array[0..BLOCK_SECTOR_SIZE - 1] of Byte;

function ATAReadDataWord(Port: Word): Word; cdecl; assembler; nostackframe;
asm
  mov dx, di
  xor eax, eax
  in ax, dx
end;

procedure ATAWriteDataWord(Port, Value: Word); cdecl; assembler; nostackframe;
asm
  mov dx, di
  mov ax, si
  out dx, ax
end;

procedure ATADelay400ns;
begin
  In8(ATA_PRIMARY_CTRL);
  In8(ATA_PRIMARY_CTRL);
  In8(ATA_PRIMARY_CTRL);
  In8(ATA_PRIMARY_CTRL);
end;

function ATAWait(NeedDRQ: Boolean; Timeout: LongWord): Boolean;
var
  Status: Word;
  I: LongWord;
begin
  Result := False;
  for I := 0 to Timeout - 1 do
  begin
    Status := In8(ATA_PRIMARY_IO + ATA_REG_STATUS);
    if (Status = 0) or (Status = $FF) then Exit;
    if (Status and ATA_STATUS_BSY) <> 0 then Continue;
    if (Status and (ATA_STATUS_ERR or ATA_STATUS_DF)) <> 0 then Exit;
    if NeedDRQ and ((Status and ATA_STATUS_DRQ) = 0) then Continue;
    Result := True;
    Exit;
  end;
end;

function ATAIdentifyPrimaryMaster(out SectorCount: QWord): Boolean;
var
  I: LongWord;
  WordValue: Word;
  Capacity28: QWord;
begin
  Result := False;
  SectorCount := 0;
  Out8(ATA_PRIMARY_IO + ATA_REG_HDDEVSEL, $A0);
  ATADelay400ns;
  Out8(ATA_PRIMARY_IO + ATA_REG_SECCOUNT0, 0);
  Out8(ATA_PRIMARY_IO + ATA_REG_LBA0, 0);
  Out8(ATA_PRIMARY_IO + ATA_REG_LBA1, 0);
  Out8(ATA_PRIMARY_IO + ATA_REG_LBA2, 0);
  Out8(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
  if In8(ATA_PRIMARY_IO + ATA_REG_STATUS) = 0 then Exit;
  if not ATAWait(True, 2000000) then Exit;
  { ATAPI signatures are intentionally not accepted by this ATA-only path. }
  if (In8(ATA_PRIMARY_IO + ATA_REG_LBA1) <> 0) or
     (In8(ATA_PRIMARY_IO + ATA_REG_LBA2) <> 0) then Exit;
  Capacity28 := 0;
  for I := 0 to 255 do
  begin
    WordValue := ATAReadDataWord(ATA_PRIMARY_IO + ATA_REG_DATA);
    if (I = 60) then Capacity28 := WordValue;
    if (I = 61) then Capacity28 := Capacity28 or (QWord(WordValue) shl 16);
  end;
  if Capacity28 = 0 then Exit;
  SectorCount := Capacity28;
  Result := True;
end;

function BlockReadSectors(DeviceIndex: LongWord; LBA: QWord;
  SectorCount: LongWord; Buffer: Pointer): Boolean;
var
  I, J: LongWord;
  Address: QWord;
  Dest: PByte;
  Value: Word;
begin
  Result := False;
  if (DeviceIndex >= BLOCK_DEVICE_MAX) or (Buffer = nil) or
     (SectorCount = 0) or (SectorCount > 255) then Exit;
  if BlockBusy <> 0 then Exit;
  if BlockDevices[DeviceIndex].Active = 0 then Exit;
  if (LBA > BlockDevices[DeviceIndex].SectorCount) or
     (QWord(SectorCount) > BlockDevices[DeviceIndex].SectorCount - LBA) then Exit;
  if LBA + SectorCount > $10000000 then Exit; { LBA28 boundary }
  BlockBusy := 1;
  Dest := PByte(Buffer);
  for I := 0 to SectorCount - 1 do
  begin
    Address := LBA + I;
    Out8(ATA_PRIMARY_IO + ATA_REG_HDDEVSEL,
      $E0 or Byte((Address shr 24) and $0F));
    ATADelay400ns;
    Out8(ATA_PRIMARY_IO + ATA_REG_FEATURES, 0);
    Out8(ATA_PRIMARY_IO + ATA_REG_SECCOUNT0, 1);
    Out8(ATA_PRIMARY_IO + ATA_REG_LBA0, Byte(Address));
    Out8(ATA_PRIMARY_IO + ATA_REG_LBA1, Byte(Address shr 8));
    Out8(ATA_PRIMARY_IO + ATA_REG_LBA2, Byte(Address shr 16));
    Out8(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);
    if not ATAWait(True, 2000000) then
    begin
      BlockBusy := 0;
      Exit;
    end;
    for J := 0 to 255 do
    begin
      Value := ATAReadDataWord(ATA_PRIMARY_IO + ATA_REG_DATA);
      Dest^ := Byte(Value);
      Inc(Dest);
      Dest^ := Byte(Value shr 8);
      Inc(Dest);
    end;
  end;
  BlockBusy := 0;
  Result := True;
end;

function BlockWriteSectors(DeviceIndex: LongWord; LBA: QWord;
  SectorCount: LongWord; Buffer: Pointer): Boolean;
var
  I, J: LongWord;
  Address: QWord;
  Source: PByte;
  Value: Word;
begin
  Result := False;
  if (DeviceIndex >= BLOCK_DEVICE_MAX) or (Buffer = nil) or
     (SectorCount = 0) or (SectorCount > 255) then Exit;
  if BlockBusy <> 0 then Exit;
  if BlockDevices[DeviceIndex].Active = 0 then Exit;
  if (LBA > BlockDevices[DeviceIndex].SectorCount) or
     (QWord(SectorCount) > BlockDevices[DeviceIndex].SectorCount - LBA) then Exit;
  if LBA + SectorCount > $10000000 then Exit;
  BlockBusy := 1;
  Source := PByte(Buffer);
  for I := 0 to SectorCount - 1 do
  begin
    Address := LBA + I;
    Out8(ATA_PRIMARY_IO + ATA_REG_HDDEVSEL,
      $E0 or Byte((Address shr 24) and $0F));
    ATADelay400ns;
    Out8(ATA_PRIMARY_IO + ATA_REG_FEATURES, 0);
    Out8(ATA_PRIMARY_IO + ATA_REG_SECCOUNT0, 1);
    Out8(ATA_PRIMARY_IO + ATA_REG_LBA0, Byte(Address));
    Out8(ATA_PRIMARY_IO + ATA_REG_LBA1, Byte(Address shr 8));
    Out8(ATA_PRIMARY_IO + ATA_REG_LBA2, Byte(Address shr 16));
    Out8(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);
    if not ATAWait(True, 2000000) then
    begin
      BlockBusy := 0;
      Exit;
    end;
    for J := 0 to 255 do
    begin
      Value := Word(Source^) or (Word(PByte(PtrUInt(Source) + 1)^) shl 8);
      ATAWriteDataWord(ATA_PRIMARY_IO + ATA_REG_DATA, Value);
      Inc(Source, 2);
    end;
    if not ATAWait(False, 2000000) then
    begin
      BlockBusy := 0;
      Exit;
    end;
  end;
  Out8(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
  Result := ATAWait(False, 2000000);
  BlockBusy := 0;
end;

function BlockScanMBR(DeviceIndex: LongWord): LongWord;
var
  I: LongWord;
  EntryOffset: LongWord;
  StartLBA, SectorCount: LongWord;
  TypeCode: Byte;
  PartitionIndex: LongWord;
begin
  Result := 0;
  if not BlockReadSectors(DeviceIndex, 0, 1, @BlockSectorScratch[0]) then Exit;
  if (BlockSectorScratch[510] <> $55) or
     (BlockSectorScratch[511] <> $AA) then Exit;
  for I := 0 to 3 do
  begin
    EntryOffset := 446 + (I * 16);
    TypeCode := BlockSectorScratch[EntryOffset + 4];
    StartLBA := LongWord(BlockSectorScratch[EntryOffset + 8]) or
      (LongWord(BlockSectorScratch[EntryOffset + 9]) shl 8) or
      (LongWord(BlockSectorScratch[EntryOffset + 10]) shl 16) or
      (LongWord(BlockSectorScratch[EntryOffset + 11]) shl 24);
    SectorCount := LongWord(BlockSectorScratch[EntryOffset + 12]) or
      (LongWord(BlockSectorScratch[EntryOffset + 13]) shl 8) or
      (LongWord(BlockSectorScratch[EntryOffset + 14]) shl 16) or
      (LongWord(BlockSectorScratch[EntryOffset + 15]) shl 24);
    if (TypeCode = 0) or (SectorCount = 0) then Continue;
    if Result >= BLOCK_PARTITION_MAX then Break;
    PartitionIndex := Result;
    BlockPartitions[PartitionIndex].Active := 1;
    BlockPartitions[PartitionIndex].DeviceIndex := DeviceIndex;
    BlockPartitions[PartitionIndex].TypeCode := TypeCode;
    BlockPartitions[PartitionIndex].StartLBA := StartLBA;
    BlockPartitions[PartitionIndex].SectorCount := SectorCount;
    Inc(Result);
  end;
end;

procedure BlockInitialize;
var
  I: LongWord;
  Capacity: QWord;
  Partitions: LongWord;
begin
  BlockBusy := 0;
  BlockServerPID := 0;
  BlockInitialized := False;
  for I := 0 to BLOCK_DEVICE_MAX - 1 do
  begin
    BlockDevices[I].Active := 0;
    BlockDevices[I].Kind := bdNone;
    BlockDevices[I].SectorCount := 0;
    BlockDevices[I].Name[0] := #0;
  end;
  for I := 0 to BLOCK_PARTITION_MAX - 1 do
    BlockPartitions[I].Active := 0;

  if ATAIdentifyPrimaryMaster(Capacity) then
  begin
    BlockDevices[0].Active := 1;
    BlockDevices[0].Kind := bdATA_PIO;
    BlockDevices[0].BasePort := ATA_PRIMARY_IO;
    BlockDevices[0].ControlPort := ATA_PRIMARY_CTRL;
    BlockDevices[0].SectorCount := Capacity;
    BlockDevices[0].LBA48 := 0;
    BlockDevices[0].Name[0] := 'a';
    BlockDevices[0].Name[1] := 't';
    BlockDevices[0].Name[2] := 'a';
    BlockDevices[0].Name[3] := '0';
    BlockDevices[0].Name[4] := #0;
    Partitions := BlockScanMBR(0);
    WriteText('[ OK ] ATA PIO primary-master detected; sectors=');
    WriteDecimal(Capacity);
    WriteText(' partitions=');
    WriteDecimal(Partitions);
    WriteText('.'#13#10);
  end
  else
    WriteText('[INFO] No ATA PIO primary-master device detected.'#13#10);
  BlockInitialized := True;
end;
