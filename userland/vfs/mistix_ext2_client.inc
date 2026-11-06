function Ext2UReadLE16(Base: Pointer; Offset: LongWord): Word; inline;
begin
  Result := Word(PByte(PtrUInt(Base) + Offset)^) or
    (Word(PByte(PtrUInt(Base) + Offset + 1)^) shl 8);
end;

function Ext2UReadLE32(Base: Pointer; Offset: LongWord): LongWord; inline;
begin
  Result := LongWord(PByte(PtrUInt(Base) + Offset)^) or
    (LongWord(PByte(PtrUInt(Base) + Offset + 1)^) shl 8) or
    (LongWord(PByte(PtrUInt(Base) + Offset + 2)^) shl 16) or
    (LongWord(PByte(PtrUInt(Base) + Offset + 3)^) shl 24);
end;

function Ext2UBlockRPCRead(Offset, Count: QWord): Boolean;
var
  Request, Response: TMistixIPCMessage;
  Status: QWord;
  Retry: LongWord;
begin
  Result := False;
  if (BlockRequestEndpoint = 0) or (BlockReplyEndpoint = 0) or
     (Count = 0) or (Count > 4096) then Exit;
  ClearMessage(Request);
  Request.MessageId := VFS_BLOCK_RPC_READ;
  Request.WordCount := 2;
  Request.Words[0] := Offset;
  Request.Words[1] := Count;
  Retry := 0;
  repeat
    Status := SysIPCSend(BlockRequestEndpoint, PtrUInt(@Request));
    if not IsError(Status) then Break;
    if Status <> MISTIX_EAGAIN then Exit;
    SysYield;
    Inc(Retry);
  until Retry >= 100000;
  if Retry >= 100000 then Exit;
  Retry := 0;
  repeat
    Status := SysIPCReceive(BlockReplyEndpoint, PtrUInt(@Response));
    if not IsError(Status) then
    begin
      Result := (Response.MessageId = 0) and (Response.WordCount >= 1) and
                (Response.Words[0] = Count);
      Exit;
    end;
    if Status <> MISTIX_EAGAIN then Exit;
    SysYield;
    Inc(Retry);
  until Retry >= 100000;
end;

function Ext2UReadDisk(Offset: QWord; Buffer: Pointer;
  Count: LongWord): Boolean;
var
  Dest: PByte;
  I: LongWord;
begin
  Result := False;
  if (Buffer = nil) or (Count = 0) or (Count > 4096) then Exit;
  if (Offset > Ext2DiskBytes) or (QWord(Count) > Ext2DiskBytes - Offset) then Exit;
  if not Ext2UBlockRPCRead(Offset, Count) then Exit;
  Dest := PByte(Buffer);
  for I := 0 to Count - 1 do Dest[I] := SharedBuffer[I];
  Result := True;
end;

function Ext2UReadFS(Offset: QWord; Buffer: Pointer;
  Count: LongWord): Boolean;
begin
  Result := False;
  if (Offset > Ext2PartitionBytes) or
     (QWord(Count) > Ext2PartitionBytes - Offset) then Exit;
  if Ext2BaseBytes > High(QWord) - Offset then Exit;
  Result := Ext2UReadDisk(Ext2BaseBytes + Offset, Buffer, Count);
end;

function Ext2UReadBlock(BlockNumber: LongWord; Buffer: Pointer): Boolean;
begin
  Result := False;
  if (BlockNumber = 0) or (BlockNumber >= Ext2BlocksCount) then Exit;
  if QWord(BlockNumber) * Ext2BlockSize > High(QWord) then Exit;
  Result := Ext2UReadFS(QWord(BlockNumber) * Ext2BlockSize,
    Buffer, Ext2BlockSize);
end;

function Ext2UReadInode(InodeNumber: LongWord; out Inode: TExt2UserInode): Boolean; forward;

function Ext2UBlockRPCInfo(out Info: TMistixBlockDeviceInfo): Boolean;
var
  Request, Response: TMistixIPCMessage;
  Status: QWord;
  Retry: LongWord;
begin
  Result := False;
  ClearMessage(Request);
  Request.MessageId := VFS_BLOCK_RPC_INFO;
  Retry := 0;
  repeat
    Status := SysIPCSend(BlockRequestEndpoint, PtrUInt(@Request));
    if not IsError(Status) then Break;
    if Status <> MISTIX_EAGAIN then Exit;
    SysYield;
    Inc(Retry);
  until Retry >= 100000;
  if Retry >= 100000 then Exit;
  Retry := 0;
  repeat
    Status := SysIPCReceive(BlockReplyEndpoint, PtrUInt(@Response));
    if not IsError(Status) then
    begin
      if (Response.MessageId <> 0) or (Response.WordCount < 4) then Exit;
      Info.SectorCount := Response.Words[0];
      Info.SectorSize := LongWord(Response.Words[1]);
      Info.Flags := LongWord(Response.Words[3]);
      { Words[2] is byte capacity and is validated by the mount routine. }
      Ext2DiskBytes := Response.Words[2];
      Result := True;
      Exit;
    end;
    if Status <> MISTIX_EAGAIN then Exit;
    SysYield;
    Inc(Retry);
  until Retry >= 100000;
end;

function Ext2UMount: Boolean;
var
  Info: TMistixBlockDeviceInfo;
  I, EntryOffset: LongWord;
  PartitionStart, PartitionSectors: LongWord;
  LogBlockSize, Revision: LongWord;
  FeatureCompat, FeatureIncompat, FeatureRO: LongWord;
  GroupCount64: QWord;
  FoundPartition: Boolean;
  RootInode: TExt2UserInode;
begin
  Result := False;
  Ext2Mounted := False;
  Ext2DiskBytes := 0;
  Ext2BaseBytes := 0;
  Ext2PartitionBytes := 0;
  if (BlockRequestEndpoint = 0) or (BlockReplyEndpoint = 0) then Exit;

  if not Ext2UBlockRPCInfo(Info) then Exit;
  if (Info.SectorSize <> 512) or (Info.SectorCount = 0) or
     (Info.SectorCount > High(QWord) div 512) then Exit;
  if Ext2DiskBytes <> Info.SectorCount * 512 then Exit;
  { The blockd RAM fallback is intentionally not considered persistent media. }
  if Info.Flags <> 1 then
  begin
    Ext2DiskBytes := 0;
    Exit;
  end;
  if Ext2DiskBytes < 4096 then Exit;

  if not Ext2UReadDisk(0, @Ext2SectorScratch[0], 512) then Exit;
  FoundPartition := False;
  if (Ext2SectorScratch[510] = $55) and (Ext2SectorScratch[511] = $AA) then
  begin
    for I := 0 to 3 do
    begin
      EntryOffset := 446 + I * 16;
      if Ext2SectorScratch[EntryOffset + 4] <> $83 then Continue;
      PartitionStart := LongWord(Ext2SectorScratch[EntryOffset + 8]) or
        (LongWord(Ext2SectorScratch[EntryOffset + 9]) shl 8) or
        (LongWord(Ext2SectorScratch[EntryOffset + 10]) shl 16) or
        (LongWord(Ext2SectorScratch[EntryOffset + 11]) shl 24);
      PartitionSectors := LongWord(Ext2SectorScratch[EntryOffset + 12]) or
        (LongWord(Ext2SectorScratch[EntryOffset + 13]) shl 8) or
        (LongWord(Ext2SectorScratch[EntryOffset + 14]) shl 16) or
        (LongWord(Ext2SectorScratch[EntryOffset + 15]) shl 24);
      if (PartitionSectors = 0) or
         (QWord(PartitionStart) * 512 > Ext2DiskBytes) or
         (QWord(PartitionSectors) * 512 > Ext2DiskBytes - QWord(PartitionStart) * 512) then
        Continue;
      Ext2BaseBytes := QWord(PartitionStart) * 512;
      Ext2PartitionBytes := QWord(PartitionSectors) * 512;
      FoundPartition := True;
      Break;
    end;
  end;
  if not FoundPartition then
  begin
    Ext2BaseBytes := 0;
    Ext2PartitionBytes := Ext2DiskBytes;
  end;
  if Ext2PartitionBytes < 2048 then Exit;
  if not Ext2UReadFS(1024, @Ext2SuperScratch[0], 1024) then Exit;
  if Ext2UReadLE16(@Ext2SuperScratch[0], 56) <> EXT2_U_MAGIC then Exit;

  Ext2InodesCount := Ext2UReadLE32(@Ext2SuperScratch[0], 0);
  Ext2BlocksCount := Ext2UReadLE32(@Ext2SuperScratch[0], 4);
  Ext2FirstDataBlock := Ext2UReadLE32(@Ext2SuperScratch[0], 20);
  LogBlockSize := Ext2UReadLE32(@Ext2SuperScratch[0], 24);
  Ext2BlocksPerGroup := Ext2UReadLE32(@Ext2SuperScratch[0], 32);
  Ext2InodesPerGroup := Ext2UReadLE32(@Ext2SuperScratch[0], 40);
  Revision := Ext2UReadLE32(@Ext2SuperScratch[0], 76);
  if Revision > 1 then Exit;
  if Revision = 0 then Ext2InodeSize := 128
  else Ext2InodeSize := Ext2UReadLE16(@Ext2SuperScratch[0], 88);
  FeatureCompat := Ext2UReadLE32(@Ext2SuperScratch[0], 92);
  FeatureIncompat := Ext2UReadLE32(@Ext2SuperScratch[0], 96);
  FeatureRO := Ext2UReadLE32(@Ext2SuperScratch[0], 100);

  if (LogBlockSize > 2) or (Ext2BlocksCount = 0) or
     (Ext2InodesCount = 0) or (Ext2BlocksPerGroup = 0) or
     (Ext2InodesPerGroup = 0) then Exit;
  Ext2BlockSize := 1024 shl LogBlockSize;
  if ((Ext2BlockSize = 1024) and (Ext2FirstDataBlock <> 1)) or
     ((Ext2BlockSize <> 1024) and (Ext2FirstDataBlock <> 0)) then Exit;
  if (Ext2InodeSize < 128) or (Ext2InodeSize > 256) or
     ((Ext2InodeSize and 3) <> 0) or (Ext2InodeSize > Ext2BlockSize) then Exit;
  if (FeatureCompat and EXT2_U_FEATURE_COMPAT_HAS_JOURNAL) <> 0 then Exit;
  if (FeatureCompat and EXT2_U_FEATURE_COMPAT_DIR_INDEX) <> 0 then Exit;
  if (FeatureIncompat and not EXT2_U_FEATURE_INCOMPAT_FILETYPE) <> 0 then Exit;
  if (FeatureRO and not EXT2_U_FEATURE_RO_SUPPORTED) <> 0 then Exit;
  if QWord(Ext2BlocksCount) * Ext2BlockSize > Ext2PartitionBytes then Exit;
  if Ext2BlocksCount <= Ext2FirstDataBlock then Exit;
  GroupCount64 := (QWord(Ext2BlocksCount - Ext2FirstDataBlock) +
    Ext2BlocksPerGroup - 1) div Ext2BlocksPerGroup;
  if (GroupCount64 = 0) or (GroupCount64 > High(LongWord)) then Exit;
  Ext2GroupCount := LongWord(GroupCount64);
  Ext2FeatureCompat := FeatureCompat;
  Ext2FeatureIncompat := FeatureIncompat;
  Ext2FeatureRO := FeatureRO;
  Ext2Mounted := True;
  if not Ext2UReadInode(2, RootInode) then
  begin
    Ext2Mounted := False;
    Exit;
  end;
  if (RootInode.Mode and EXT2_U_MODE_MASK) <> EXT2_U_MODE_DIRECTORY then
  begin
    Ext2Mounted := False;
    Exit;
  end;
  Result := True;
end;

function Ext2UReadInode(InodeNumber: LongWord; out Inode: TExt2UserInode): Boolean;
var
  Group, IndexInGroup: QWord;
  DescriptorOffset, InodeTableBlock, InodeOffset: QWord;
  I: LongWord;
begin
  Result := False;
  if not Ext2Mounted or (InodeNumber = 0) or
     (InodeNumber > Ext2InodesCount) then Exit;
  Group := (InodeNumber - 1) div Ext2InodesPerGroup;
  IndexInGroup := (InodeNumber - 1) mod Ext2InodesPerGroup;
  if Group >= Ext2GroupCount then Exit;
  DescriptorOffset := QWord(Ext2FirstDataBlock + 1) * Ext2BlockSize + Group * 32;
  if not Ext2UReadFS(DescriptorOffset, @Ext2MetaScratch[0], 32) then Exit;
  InodeTableBlock := Ext2UReadLE32(@Ext2MetaScratch[0], 8);
  if (InodeTableBlock = 0) or (InodeTableBlock >= Ext2BlocksCount) then Exit;
  InodeOffset := QWord(InodeTableBlock) * Ext2BlockSize +
    IndexInGroup * Ext2InodeSize;
  if not Ext2UReadFS(InodeOffset, @Ext2MetaScratch[0], 128) then Exit;
  Inode.Mode := Ext2UReadLE16(@Ext2MetaScratch[0], 0);
  Inode.Size := Ext2UReadLE32(@Ext2MetaScratch[0], 4);
  if ((Inode.Mode and EXT2_U_MODE_MASK) = EXT2_U_MODE_REGULAR) and
     ((Ext2FeatureRO and $00000002) <> 0) then
    Inode.Size := Inode.Size or
      (QWord(Ext2UReadLE32(@Ext2MetaScratch[0], 108)) shl 32);
  for I := 0 to 14 do Inode.Blocks[I] := Ext2UReadLE32(@Ext2MetaScratch[0], 40 + I * 4);
  Result := True;
end;

function Ext2UResolveBlock(const Inode: TExt2UserInode; LogicalBlock: QWord;
  out PhysicalBlock: LongWord): Boolean;
var
  Index: QWord;
begin
  Result := False;
  PhysicalBlock := 0;
  if LogicalBlock < 12 then
  begin
    PhysicalBlock := Inode.Blocks[LogicalBlock];
    Result := True;
    Exit;
  end;
  Index := LogicalBlock - 12;
  if Index >= Ext2BlockSize div 4 then Exit;
  if Inode.Blocks[12] = 0 then
  begin
    PhysicalBlock := 0; { sparse hole in the single-indirect range }
    Result := True;
    Exit;
  end;
  if not Ext2UReadBlock(Inode.Blocks[12], @Ext2IndirectScratch[0]) then Exit;
  PhysicalBlock := Ext2UReadLE32(@Ext2IndirectScratch[0], LongWord(Index * 4));
  Result := True;
end;

function Ext2ULookupChild(const Directory: TExt2UserInode;
  const Name: array of Char; out ChildInode: LongWord): Boolean;
var
  NameLength, BlockIndex, EntryOffset, RecordLength, EntryNameLength: LongWord;
  LogicalBlocks, PhysicalBlock, I: QWord;
  EntryInode: LongWord;
  Match: Boolean;
begin
  Result := False;
  ChildInode := 0;
  if (Directory.Mode and EXT2_U_MODE_MASK) <> EXT2_U_MODE_DIRECTORY then Exit;
  NameLength := 0;
  while (NameLength < LongWord(Length(Name))) and (Name[NameLength] <> #0) do
    Inc(NameLength);
  if (NameLength = 0) or (NameLength > 255) then Exit;
  LogicalBlocks := Directory.Size div Ext2BlockSize;
  if (Directory.Size mod Ext2BlockSize) <> 0 then Inc(LogicalBlocks);
  if (LogicalBlocks = 0) or (LogicalBlocks > High(LongWord)) then Exit;
  for BlockIndex := 0 to LongWord(LogicalBlocks) - 1 do
  begin
    if not Ext2UResolveBlock(Directory, BlockIndex, PhysicalBlock) then Exit;
    if PhysicalBlock = 0 then Continue;
    if not Ext2UReadBlock(PhysicalBlock, @Ext2BlockScratch[0]) then Exit;
    EntryOffset := 0;
    while EntryOffset + 8 <= Ext2BlockSize do
    begin
      EntryInode := Ext2UReadLE32(@Ext2BlockScratch[0], EntryOffset);
      RecordLength := Ext2UReadLE16(@Ext2BlockScratch[0], EntryOffset + 4);
      if (RecordLength < 8) or ((RecordLength and 3) <> 0) or
         (RecordLength > Ext2BlockSize - EntryOffset) then Exit;
      if (Ext2FeatureIncompat and EXT2_U_FEATURE_INCOMPAT_FILETYPE) <> 0 then
        EntryNameLength := Ext2BlockScratch[EntryOffset + 6]
      else
        EntryNameLength := Ext2UReadLE16(@Ext2BlockScratch[0], EntryOffset + 6);
      if EntryNameLength > RecordLength - 8 then Exit;
      if (EntryInode <> 0) and (EntryNameLength = NameLength) then
      begin
        Match := True;
        for I := 0 to NameLength - 1 do
          if Char(Ext2BlockScratch[EntryOffset + 8 + I]) <> Name[I] then
          begin
            Match := False;
            Break;
          end;
        if Match then
        begin
          ChildInode := EntryInode;
          Result := True;
          Exit;
        end;
      end;
      Inc(EntryOffset, RecordLength);
    end;
  end;
end;

function Ext2ULookupPath(const Path: array of Char;
  out InodeNumber: LongWord; out Inode: TExt2UserInode): Boolean;
var
  PathLength, Position, StartPosition, ComponentLength, I: LongWord;
  Component: array[0..255] of Char;
  CurrentInode: LongWord;
  DirectoryInode: TExt2UserInode;
begin
  Result := False;
  InodeNumber := 0;
  if not Ext2Mounted then Exit;
  PathLength := 0;
  while (PathLength < LongWord(Length(Path))) and (Path[PathLength] <> #0) do
    Inc(PathLength);
  if (PathLength < 9) or (PathLength > 127) then Exit;
  for I := 0 to 8 do
    if Path[I] <> EXT2_U_MOUNT_PREFIX[I + 1] then Exit;
  if PathLength > 9 then
    if Path[9] <> '/' then Exit;
  CurrentInode := 2;
  Position := 9;
  while Position < PathLength do
  begin
    while (Position < PathLength) and (Path[Position] = '/') do Inc(Position);
    if Position >= PathLength then Break;
    StartPosition := Position;
    while (Position < PathLength) and (Path[Position] <> '/') do Inc(Position);
    ComponentLength := Position - StartPosition;
    if (ComponentLength = 0) or (ComponentLength > 255) then Exit;
    if not Ext2UReadInode(CurrentInode, DirectoryInode) then Exit;
    for I := 0 to ComponentLength - 1 do Component[I] := Path[StartPosition + I];
    Component[ComponentLength] := #0;
    if not Ext2ULookupChild(DirectoryInode, Component, CurrentInode) then Exit;
  end;
  if not Ext2UReadInode(CurrentInode, Inode) then Exit;
  InodeNumber := CurrentInode;
  Result := True;
end;

function Ext2UReadDirectory(const Path: array of Char;
  out EntryCount: LongWord): Boolean;
var
  DirectoryNumber, ChildNumber: LongWord;
  Directory, Child: TExt2UserInode;
  LogicalBlocks, BlockIndex: QWord;
  PhysicalBlock: LongWord;
  EntryOffset, RecordLength, NameLength, I, J: LongWord;
  EntryInode: LongWord;
  EntryType: Byte;
  Source: PByte;
  SkipEntry: Boolean;
begin
  Result := False;
  EntryCount := 0;
  if not Ext2ULookupPath(Path, DirectoryNumber, Directory) then Exit;
  if (Directory.Mode and EXT2_U_MODE_MASK) <> EXT2_U_MODE_DIRECTORY then Exit;
  LogicalBlocks := Directory.Size div Ext2BlockSize;
  if (Directory.Size mod Ext2BlockSize) <> 0 then Inc(LogicalBlocks);
  if LogicalBlocks = 0 then
  begin
    Result := True;
    Exit;
  end;
  if LogicalBlocks > High(LongWord) then Exit;
  for BlockIndex := 0 to LogicalBlocks - 1 do
  begin
    if not Ext2UResolveBlock(Directory, BlockIndex, PhysicalBlock) then Exit;
    if PhysicalBlock = 0 then Continue;
    if not Ext2UReadBlock(PhysicalBlock, @Ext2BlockScratch[0]) then Exit;
    EntryOffset := 0;
    while EntryOffset + 8 <= Ext2BlockSize do
    begin
      EntryInode := Ext2UReadLE32(@Ext2BlockScratch[0], EntryOffset);
      RecordLength := Ext2UReadLE16(@Ext2BlockScratch[0], EntryOffset + 4);
      if (RecordLength < 8) or ((RecordLength and 3) <> 0) or
         (RecordLength > Ext2BlockSize - EntryOffset) then Exit;
      if (Ext2FeatureIncompat and EXT2_U_FEATURE_INCOMPAT_FILETYPE) <> 0 then
      begin
        NameLength := Ext2BlockScratch[EntryOffset + 6];
        EntryType := Ext2BlockScratch[EntryOffset + 7];
      end
      else
      begin
        NameLength := Ext2UReadLE16(@Ext2BlockScratch[0], EntryOffset + 6);
        EntryType := 0;
      end;
      if NameLength > RecordLength - 8 then Exit;
      { TMistixDirent reserves 64 bytes including a terminating NUL. }
      if NameLength > 63 then Exit;
      SkipEntry := False;
      if (NameLength = 1) and
         (Ext2BlockScratch[EntryOffset + 8] = Ord('.')) then
        SkipEntry := True;
      if (NameLength = 2) and
         (Ext2BlockScratch[EntryOffset + 8] = Ord('.')) and
         (Ext2BlockScratch[EntryOffset + 9] = Ord('.')) then
        SkipEntry := True;
      if NameLength <> 0 then
        for I := 0 to NameLength - 1 do
          if (Ext2BlockScratch[EntryOffset + 8 + I] = 0) or
             (Ext2BlockScratch[EntryOffset + 8 + I] = Ord('/')) or
             (Ext2BlockScratch[EntryOffset + 8 + I] < 32) or
             (Ext2BlockScratch[EntryOffset + 8 + I] = 127) then Exit;
      if (EntryInode <> 0) and (NameLength <> 0) and not SkipEntry then
      begin
        if EntryCount >= 128 then Exit;
        Ext2DirEntries[EntryCount].Inode := EntryInode;
        case EntryType of
          2: Ext2DirEntries[EntryCount].EntryType := 2;
          1: Ext2DirEntries[EntryCount].EntryType := 1;
        else
          Ext2DirEntries[EntryCount].EntryType := 0;
        end;
        for I := 0 to 63 do
          if I < NameLength then
            Ext2DirEntries[EntryCount].Name[I] :=
              Char(Ext2BlockScratch[EntryOffset + 8 + I])
          else
            Ext2DirEntries[EntryCount].Name[I] := #0;
        Ext2DirEntries[EntryCount].Size := 0;
        if Ext2UReadInode(EntryInode, Child) then
          Ext2DirEntries[EntryCount].Size := Child.Size;
        Inc(EntryCount);
      end;
      Inc(EntryOffset, RecordLength);
    end;
  end;
  if EntryCount <> 0 then
  begin
    Source := PByte(@Ext2DirEntries[0]);
    for J := 0 to EntryCount * SizeOf(TMistixDirent) - 1 do
      SharedBuffer[J] := Source[J];
  end;
  Result := True;
end;

function Ext2UReadFile(InodeNumber: LongWord; Offset: QWord;
  Buffer: Pointer; ByteCount: LongWord; out BytesRead: LongWord): Boolean;
var
  Inode: TExt2UserInode;
  LogicalBlock, BlockOffset, Chunk, Remaining, I: QWord;
  PhysicalBlock: LongWord;
  Dest: PByte;
begin
  Result := False;
  BytesRead := 0;
  if (Buffer = nil) or not Ext2UReadInode(InodeNumber, Inode) then Exit;
  if (Inode.Mode and EXT2_U_MODE_MASK) <> EXT2_U_MODE_REGULAR then Exit;
  if (ByteCount = 0) or (Offset >= Inode.Size) then
  begin
    Result := True;
    Exit;
  end;
  Remaining := Inode.Size - Offset;
  if Remaining > ByteCount then Remaining := ByteCount;
  Dest := PByte(Buffer);
  while Remaining <> 0 do
  begin
    LogicalBlock := Offset div Ext2BlockSize;
    BlockOffset := Offset mod Ext2BlockSize;
    Chunk := Ext2BlockSize - BlockOffset;
    if Chunk > Remaining then Chunk := Remaining;
    if not Ext2UResolveBlock(Inode, LogicalBlock, PhysicalBlock) then Exit;
    if PhysicalBlock = 0 then
      for I := 0 to Chunk - 1 do Dest[BytesRead + I] := 0
    else
    begin
      if not Ext2UReadBlock(PhysicalBlock, @Ext2BlockScratch[0]) then Exit;
      for I := 0 to Chunk - 1 do
        Dest[BytesRead + I] := Ext2BlockScratch[BlockOffset + I];
    end;
    Inc(BytesRead, LongWord(Chunk));
    Inc(Offset, Chunk);
    Dec(Remaining, Chunk);
  end;
  Result := True;
end;

function Ext2UPathIsMounted(const Path: array of Char): Boolean;
var
  I, L: LongWord;
begin
  Result := False;
  L := 0;
  while (L < LongWord(Length(Path))) and (Path[L] <> #0) do Inc(L);
  if L < 9 then Exit;
  for I := 0 to 8 do
    if Path[I] <> EXT2_U_MOUNT_PREFIX[I + 1] then Exit;
  Result := (L = 9);
  if L > 9 then Result := Path[9] = '/';
end;
