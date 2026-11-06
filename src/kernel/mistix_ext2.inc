{ Mistix Phase 6 read-only Ext2 reader. Direct blocks are supported in this
  bring-up version; indirect blocks, htree lookup, extents and journal replay
  are rejected/documented rather than silently misinterpreted. }

const
  EXT2_MOUNT_MAX = 4;
  EXT2_BLOCK_MAX = 4096;
  EXT2_INODE_MAX = 256;
  EXT2_MAGIC = $EF53;
  EXT2_FEATURE_INCOMPAT_FILETYPE = $00000002;
  EXT2_FEATURE_COMPAT_HAS_JOURNAL = $00000004;
  EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER = $00000001;
  EXT2_FEATURE_RO_COMPAT_LARGE_FILE = $00000002;
  EXT2_MODE_TYPE_MASK = $F000;
  EXT2_MODE_DIRECTORY = $4000;
  EXT2_MODE_REGULAR = $8000;

type
  TExt2MountRecord = record
    Active: Byte;
    DeviceIndex: Word;
    StartLBA: QWord;
    SectorCount: QWord;
    BlockSize: LongWord;
    BlocksCount: LongWord;
    InodesCount: LongWord;
    FirstDataBlock: LongWord;
    BlocksPerGroup: LongWord;
    InodesPerGroup: LongWord;
    InodeSize: Word;
    GroupCount: LongWord;
    FeatureCompat: LongWord;
    FeatureIncompat: LongWord;
    FeatureROCompat: LongWord;
  end;

  TExt2Inode = record
    Mode: Word;
    Size: QWord;
    Blocks: array[0..14] of LongWord;
  end;

var
  Ext2Mounts: array[0..EXT2_MOUNT_MAX - 1] of TExt2MountRecord;
  Ext2BlockBuffer: array[0..EXT2_BLOCK_MAX - 1] of Byte;
  Ext2InodeBuffer: array[0..EXT2_INODE_MAX - 1] of Byte;
  Ext2SectorBuffer: array[0..BLOCK_SECTOR_SIZE - 1] of Byte;

function Ext2ReadLE16(Buffer: Pointer; Offset: LongWord): Word; inline;
begin
  Result := Word(PByte(PtrUInt(Buffer) + Offset)^) or
    (Word(PByte(PtrUInt(Buffer) + Offset + 1)^) shl 8);
end;

function Ext2ReadLE32(Buffer: Pointer; Offset: LongWord): LongWord; inline;
begin
  Result := LongWord(PByte(PtrUInt(Buffer) + Offset)^) or
    (LongWord(PByte(PtrUInt(Buffer) + Offset + 1)^) shl 8) or
    (LongWord(PByte(PtrUInt(Buffer) + Offset + 2)^) shl 16) or
    (LongWord(PByte(PtrUInt(Buffer) + Offset + 3)^) shl 24);
end;

function Ext2ReadBytes(DeviceIndex: LongWord; StartLBA, PartitionSectors,
  ByteOffset: QWord; Buffer: Pointer; ByteCount: LongWord): Boolean;
var
  AbsoluteSector, InSector, CopyCount: QWord;
  Done: LongWord;
  Dest: PByte;
  I: LongWord;
begin
  Result := False;
  if (Buffer = nil) or (DeviceIndex >= BLOCK_DEVICE_MAX) then Exit;
  if (ByteOffset > PartitionSectors * BLOCK_SECTOR_SIZE) or
     (QWord(ByteCount) > PartitionSectors * BLOCK_SECTOR_SIZE - ByteOffset) then Exit;
  Done := 0;
  Dest := PByte(Buffer);
  while Done < ByteCount do
  begin
    AbsoluteSector := ByteOffset div BLOCK_SECTOR_SIZE;
    InSector := ByteOffset mod BLOCK_SECTOR_SIZE;
    if AbsoluteSector >= PartitionSectors then Exit;
    if not BlockReadSectors(DeviceIndex, StartLBA + AbsoluteSector, 1,
      @Ext2SectorBuffer[0]) then Exit;
    CopyCount := BLOCK_SECTOR_SIZE - InSector;
    if CopyCount > ByteCount - Done then CopyCount := ByteCount - Done;
    for I := 0 to LongWord(CopyCount) - 1 do
      Dest[Done + I] := Ext2SectorBuffer[InSector + I];
    Inc(Done, LongWord(CopyCount));
    Inc(ByteOffset, CopyCount);
  end;
  Result := True;
end;

function Ext2ReadBlock(MountIndex: LongWord; BlockNumber: LongWord;
  Buffer: Pointer): Boolean;
var
  Offset: QWord;
begin
  Result := False;
  if (MountIndex >= EXT2_MOUNT_MAX) or (Ext2Mounts[MountIndex].Active = 0) then Exit;
  if (BlockNumber >= Ext2Mounts[MountIndex].BlocksCount) then Exit;
  Offset := QWord(BlockNumber) * Ext2Mounts[MountIndex].BlockSize;
  Result := Ext2ReadBytes(Ext2Mounts[MountIndex].DeviceIndex,
    Ext2Mounts[MountIndex].StartLBA, Ext2Mounts[MountIndex].SectorCount,
    Offset, Buffer, Ext2Mounts[MountIndex].BlockSize);
end;

function Ext2Mount(DeviceIndex: LongWord; StartLBA,
  PartitionSectors: QWord; out MountIndex: LongWord): Boolean;
var
  Superblock: array[0..1023] of Byte;
  I, FreeIndex: LongWord;
  LogBlockSize, Revision: LongWord;
  BlockSize: LongWord;
  FeatureCompat, FeatureIncompat, FeatureRO: LongWord;
  BlocksCount, InodesCount, FirstDataBlock: LongWord;
  BlocksPerGroup, InodesPerGroup: LongWord;
  InodeSize: Word;
  GroupCount: QWord;
begin
  Result := False;
  MountIndex := High(LongWord);
  if (DeviceIndex >= BLOCK_DEVICE_MAX) or
     (BlockDevices[DeviceIndex].Active = 0) then Exit;
  if (StartLBA > BlockDevices[DeviceIndex].SectorCount) or
     (PartitionSectors > BlockDevices[DeviceIndex].SectorCount - StartLBA) then Exit;
  if not Ext2ReadBytes(DeviceIndex, StartLBA, PartitionSectors,
       1024, @Superblock[0], SizeOf(Superblock)) then Exit;
  if Ext2ReadLE16(@Superblock[0], 56) <> EXT2_MAGIC then Exit;

  InodesCount := Ext2ReadLE32(@Superblock[0], 0);
  BlocksCount := Ext2ReadLE32(@Superblock[0], 4);
  FirstDataBlock := Ext2ReadLE32(@Superblock[0], 20);
  LogBlockSize := Ext2ReadLE32(@Superblock[0], 24);
  BlocksPerGroup := Ext2ReadLE32(@Superblock[0], 32);
  InodesPerGroup := Ext2ReadLE32(@Superblock[0], 40);
  Revision := Ext2ReadLE32(@Superblock[0], 76);
  if Revision = 0 then InodeSize := 128
  else InodeSize := Ext2ReadLE16(@Superblock[0], 88);
  FeatureCompat := Ext2ReadLE32(@Superblock[0], 92);
  FeatureIncompat := Ext2ReadLE32(@Superblock[0], 96);
  FeatureRO := Ext2ReadLE32(@Superblock[0], 100);

  if (LogBlockSize > 2) or (BlocksCount = 0) or (InodesCount = 0) or
     (BlocksCount <= FirstDataBlock) or
     (BlocksPerGroup = 0) or (InodesPerGroup = 0) then Exit;
  BlockSize := 1024 shl LogBlockSize;
  if (BlockSize <> 1024) and (BlockSize <> 2048) and (BlockSize <> 4096) then Exit;
  if (InodeSize < 128) or (InodeSize > EXT2_INODE_MAX) or
     ((InodeSize and 3) <> 0) or (InodeSize > BlockSize) then Exit;
  if (FeatureCompat and EXT2_FEATURE_COMPAT_HAS_JOURNAL) <> 0 then Exit;
  if (FeatureIncompat and not EXT2_FEATURE_INCOMPAT_FILETYPE) <> 0 then Exit;
  if (FeatureRO and not (EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER or
       EXT2_FEATURE_RO_COMPAT_LARGE_FILE)) <> 0 then Exit;
  if QWord(BlocksCount) * BlockSize > PartitionSectors * BLOCK_SECTOR_SIZE then Exit;

  FreeIndex := EXT2_MOUNT_MAX;
  for I := 0 to EXT2_MOUNT_MAX - 1 do
    if Ext2Mounts[I].Active = 0 then
    begin
      FreeIndex := I;
      Break;
    end;
  if FreeIndex = EXT2_MOUNT_MAX then Exit;

  GroupCount := (QWord(BlocksCount - FirstDataBlock) + BlocksPerGroup - 1) div BlocksPerGroup;
  if GroupCount > High(LongWord) then Exit;
  Ext2Mounts[FreeIndex].Active := 1;
  Ext2Mounts[FreeIndex].DeviceIndex := DeviceIndex;
  Ext2Mounts[FreeIndex].StartLBA := StartLBA;
  Ext2Mounts[FreeIndex].SectorCount := PartitionSectors;
  Ext2Mounts[FreeIndex].BlockSize := BlockSize;
  Ext2Mounts[FreeIndex].BlocksCount := BlocksCount;
  Ext2Mounts[FreeIndex].InodesCount := InodesCount;
  Ext2Mounts[FreeIndex].FirstDataBlock := FirstDataBlock;
  Ext2Mounts[FreeIndex].BlocksPerGroup := BlocksPerGroup;
  Ext2Mounts[FreeIndex].InodesPerGroup := InodesPerGroup;
  Ext2Mounts[FreeIndex].InodeSize := InodeSize;
  Ext2Mounts[FreeIndex].GroupCount := LongWord(GroupCount);
  Ext2Mounts[FreeIndex].FeatureCompat := FeatureCompat;
  Ext2Mounts[FreeIndex].FeatureIncompat := FeatureIncompat;
  Ext2Mounts[FreeIndex].FeatureROCompat := FeatureRO;
  MountIndex := FreeIndex;
  Result := True;
end;

function Ext2ReadInode(MountIndex, InodeNumber: LongWord;
  out Inode: TExt2Inode): Boolean;
var
  Group, IndexInGroup, DescriptorBlock, DescriptorOffset: QWord;
  GroupDescriptorOffset, InodeTableBlock, InodeOffset: QWord;
  I: LongWord;
begin
  Result := False;
  if (MountIndex >= EXT2_MOUNT_MAX) or
     (Ext2Mounts[MountIndex].Active = 0) or
     (InodeNumber = 0) or
     (InodeNumber > Ext2Mounts[MountIndex].InodesCount) then Exit;
  Group := (InodeNumber - 1) div Ext2Mounts[MountIndex].InodesPerGroup;
  IndexInGroup := (InodeNumber - 1) mod Ext2Mounts[MountIndex].InodesPerGroup;
  if Group >= Ext2Mounts[MountIndex].GroupCount then Exit;
  DescriptorBlock := Ext2Mounts[MountIndex].FirstDataBlock + 1;
  GroupDescriptorOffset := (DescriptorBlock * Ext2Mounts[MountIndex].BlockSize) +
    (Group * 32);
  if not Ext2ReadBytes(Ext2Mounts[MountIndex].DeviceIndex,
      Ext2Mounts[MountIndex].StartLBA, Ext2Mounts[MountIndex].SectorCount,
      GroupDescriptorOffset, @Ext2InodeBuffer[0], 32) then Exit;
  InodeTableBlock := Ext2ReadLE32(@Ext2InodeBuffer[0], 8);
  if (InodeTableBlock = 0) or
     (InodeTableBlock >= Ext2Mounts[MountIndex].BlocksCount) then Exit;
  InodeOffset := InodeTableBlock * Ext2Mounts[MountIndex].BlockSize +
    IndexInGroup * Ext2Mounts[MountIndex].InodeSize;
  if not Ext2ReadBytes(Ext2Mounts[MountIndex].DeviceIndex,
      Ext2Mounts[MountIndex].StartLBA, Ext2Mounts[MountIndex].SectorCount,
      InodeOffset, @Ext2InodeBuffer[0], Ext2Mounts[MountIndex].InodeSize) then Exit;
  Inode.Mode := Ext2ReadLE16(@Ext2InodeBuffer[0], 0);
  Inode.Size := Ext2ReadLE32(@Ext2InodeBuffer[0], 4);
  if ((Inode.Mode and EXT2_MODE_TYPE_MASK) = EXT2_MODE_REGULAR) and
     ((Ext2Mounts[MountIndex].FeatureROCompat and EXT2_FEATURE_RO_COMPAT_LARGE_FILE) <> 0) then
    Inode.Size := Inode.Size or (QWord(Ext2ReadLE32(@Ext2InodeBuffer[0], 108)) shl 32);
  for I := 0 to 14 do
    Inode.Blocks[I] := Ext2ReadLE32(@Ext2InodeBuffer[0], 40 + I * 4);
  Result := True;
end;

function Ext2LookupChild(MountIndex: LongWord; const Directory: TExt2Inode;
  const Name: array of Char; out InodeNumber: LongWord): Boolean;
var
  BlockIndex, Offset, RecordLength, NameLength, I, NameLengthInput: LongWord;
  EntryInode: LongWord;
  EntryNameMatches: Boolean;
  Buffer: Pointer;
begin
  Result := False;
  InodeNumber := 0;
  NameLengthInput := 0;
  while (NameLengthInput < LongWord(Length(Name))) and
        (Name[NameLengthInput] <> #0) do Inc(NameLengthInput);
  if (NameLengthInput = 0) or (NameLengthInput > 255) then Exit;
  if (Directory.Mode and EXT2_MODE_TYPE_MASK) <> EXT2_MODE_DIRECTORY then Exit;
  Buffer := @Ext2BlockBuffer[0];
  for BlockIndex := 0 to 11 do
  begin
    if Directory.Blocks[BlockIndex] = 0 then Continue;
    if not Ext2ReadBlock(MountIndex, Directory.Blocks[BlockIndex], Buffer) then Exit;
    Offset := 0;
    while Offset + 8 <= Ext2Mounts[MountIndex].BlockSize do
    begin
      EntryInode := Ext2ReadLE32(Buffer, Offset);
      RecordLength := Ext2ReadLE16(Buffer, Offset + 4);
      if (RecordLength < 8) or ((RecordLength and 3) <> 0) or
         (RecordLength > Ext2Mounts[MountIndex].BlockSize - Offset) then Exit;
      if (Ext2Mounts[MountIndex].FeatureIncompat and EXT2_FEATURE_INCOMPAT_FILETYPE) <> 0 then
        NameLength := PByte(PtrUInt(Buffer) + Offset + 6)^
      else
        NameLength := Ext2ReadLE16(Buffer, Offset + 6);
      if NameLength > RecordLength - 8 then Exit;
      if (EntryInode <> 0) and (NameLength <> 0) and
         (NameLength = NameLengthInput) then
      begin
        EntryNameMatches := True;
        for I := 0 to NameLength - 1 do
          if Char(PByte(PtrUInt(Buffer) + Offset + 8 + I)^) <> Name[I] then
          begin
            EntryNameMatches := False;
            Break;
          end;
        if EntryNameMatches then
        begin
          InodeNumber := EntryInode;
          Result := True;
          Exit;
        end;
      end;
      Inc(Offset, RecordLength);
    end;
  end;
end;

function Ext2LookupPath(MountIndex: LongWord; const Path: array of Char;
  out InodeNumber: LongWord): Boolean;
var
  CurrentInode: LongWord;
  Position, StartPosition, ComponentLength, PathLength, I: LongWord;
  Component: array[0..255] of Char;
  DirectoryInode: TExt2Inode;
begin
  Result := False;
  InodeNumber := 0;
  if (MountIndex >= EXT2_MOUNT_MAX) or (Ext2Mounts[MountIndex].Active = 0) then Exit;
  PathLength := 0;
  while (PathLength < LongWord(Length(Path))) and (Path[PathLength] <> #0) do
    Inc(PathLength);
  if (PathLength = 0) or (Path[0] <> '/') then Exit;
  if PathLength >= LongWord(Length(Path)) then Exit;
  CurrentInode := 2;
  Position := 1;
  while Position < PathLength do
  begin
    while (Position < PathLength) and (Path[Position] = '/') do Inc(Position);
    if Position >= PathLength then Break;
    StartPosition := Position;
    while (Position < PathLength) and (Path[Position] <> '/') do Inc(Position);
    ComponentLength := Position - StartPosition;
    if (ComponentLength = 0) or (ComponentLength > 255) then Exit;
    if not Ext2ReadInode(MountIndex, CurrentInode, DirectoryInode) then Exit;
    for I := 0 to ComponentLength - 1 do
      Component[I] := Path[StartPosition + I];
    Component[ComponentLength] := #0;
    if not Ext2LookupChild(MountIndex, DirectoryInode, Component, CurrentInode) then Exit;
  end;
  InodeNumber := CurrentInode;
  Result := True;
end;

function Ext2ReadFile(MountIndex, InodeNumber: LongWord; Offset: QWord;
  Buffer: Pointer; ByteCount: LongWord; out BytesRead: LongWord): Boolean;
var
  Inode: TExt2Inode;
  LogicalBlock, BlockOffset, Chunk, Remaining, I: QWord;
  Dest: PByte;
begin
  Result := False;
  BytesRead := 0;
  if (Buffer = nil) or not Ext2ReadInode(MountIndex, InodeNumber, Inode) then Exit;
  if (Inode.Mode and EXT2_MODE_TYPE_MASK) <> EXT2_MODE_REGULAR then Exit;
  if Offset >= Inode.Size then
  begin
    Result := True;
    Exit;
  end;
  Remaining := Inode.Size - Offset;
  if Remaining > ByteCount then Remaining := ByteCount;
  Dest := PByte(Buffer);
  while Remaining <> 0 do
  begin
    LogicalBlock := Offset div Ext2Mounts[MountIndex].BlockSize;
    BlockOffset := Offset mod Ext2Mounts[MountIndex].BlockSize;
    if LogicalBlock >= 12 then Exit; { Indirect blocks intentionally unsupported. }
    Chunk := Ext2Mounts[MountIndex].BlockSize - BlockOffset;
    if Chunk > Remaining then Chunk := Remaining;
    if Inode.Blocks[LogicalBlock] = 0 then
      for I := 0 to Chunk - 1 do Dest[BytesRead + I] := 0
    else
    begin
      if not Ext2ReadBlock(MountIndex, Inode.Blocks[LogicalBlock],
        @Ext2BlockBuffer[0]) then Exit;
      for I := 0 to Chunk - 1 do
        Dest[BytesRead + I] := Ext2BlockBuffer[BlockOffset + I];
    end;
    Inc(BytesRead, LongWord(Chunk));
    Inc(Offset, Chunk);
    Dec(Remaining, Chunk);
  end;
  Result := True;
end;

procedure Ext2Initialize;
var
  I: LongWord;
begin
  for I := 0 to EXT2_MOUNT_MAX - 1 do Ext2Mounts[I].Active := 0;
end;
