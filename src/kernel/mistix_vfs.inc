{ Mistix Phase 5 VFS bootstrap core.
  The ABI is intentionally service-shaped; this first implementation is a
  kernel-resident bridge until the VFS server is moved to userland. }

const
  VFS_NODE_MAX = 64;
  VFS_FD_MAX = 16;
  VFS_OPEN_MAX = 64;
  VFS_PIPE_MAX = 8;
  VFS_PIPE_CAPACITY = 4096;
  VFS_PATH_MAX = 127;
  VFS_IO_MAX = 4096;
  VFS_DIRENT_NAME_MAX = 64;

  VFS_O_RDONLY = 0;
  VFS_O_WRONLY = 1;
  VFS_O_RDWR = 2;
  VFS_O_CREAT = $40;
  VFS_O_DIRECTORY = $100;

  VFS_DIRENT_FILE = 1;
  VFS_DIRENT_DIRECTORY = 2;
  VFS_DIRENT_DEVICE = 3;

  VFS_ERR_PERM = QWord(-1);
  VFS_ERR_NOENT = QWord(-2);
  VFS_ERR_IO = QWord(-5);
  VFS_ERR_BADF = QWord(-9);
  VFS_ERR_AGAIN = QWord(-11);
  VFS_ERR_NOMEM = QWord(-12);
  VFS_ERR_FAULT = QWord(-14);
  VFS_ERR_BUSY = QWord(-16);
  VFS_ERR_EXIST = QWord(-17);
  VFS_ERR_NOTDIR = QWord(-20);
  VFS_ERR_ISDIR = QWord(-21);
  VFS_ERR_INVAL = QWord(-22);
  VFS_ERR_MFILE = QWord(-24);
  VFS_ERR_NOSPC = QWord(-28);
  VFS_ERR_ROFS = QWord(-30);
  VFS_ERR_PIPE = QWord(-32);
  VFS_ERR_NAMETOOLONG = QWord(-36);
  VFS_ERR_NOTEMPTY = QWord(-39);

  MISTIX_ERR_NOENT = VFS_ERR_NOENT;

type
  TVFSNodeKind = (vnDirectory, vnRegular, vnConsole, vnNull, vnSerial);
  TVFSNode = record
    Active: Byte;
    Kind: TVFSNodeKind;
    Parent: LongInt;
    Name: array[0..63] of Char;
    Data: Pointer;
    Size: QWord;
  end;

  TVFSOpenKind = (voFree, voRegular, voDirectory, voConsole, voNull,
                  voSerial, voPipeRead, voPipeWrite, voRemote);
  TVFSOpenDescription = record
    Active: Byte;
    Kind: TVFSOpenKind;
    RefCount: Word;
    NodeIndex: LongInt;
    PipeIndex: LongInt;
    Position: QWord;
    Flags: LongWord;
    RemoteHandle: QWord;
  end;

  TVFSPipe = record
    Active: Byte;
    Readers: Word;
    Writers: Word;
    Head: LongWord;
    Tail: LongWord;
    Count: LongWord;
    Buffer: array[0..VFS_PIPE_CAPACITY - 1] of Byte;
  end;

  TVFSFDEntry = packed record
    Active: Byte;
    Description: SmallInt;
  end;

  TVFSProcessFiles = record
    FDs: array[0..VFS_FD_MAX - 1] of TVFSFDEntry;
    CurrentDirectory: array[0..VFS_PATH_MAX] of Char;
  end;

  TVFSDirent = packed record
    Inode: QWord;
    EntryType: Byte;
    Name: array[0..VFS_DIRENT_NAME_MAX - 1] of Char;
    Size: QWord;
  end;
  PVFSDirent = ^TVFSDirent;

var
  VFSNodes: array[0..VFS_NODE_MAX - 1] of TVFSNode;
  VFSNodeCount: LongWord;
  VFSOpenDescriptions: array[0..VFS_OPEN_MAX - 1] of TVFSOpenDescription;
  VFSPipes: array[0..VFS_PIPE_MAX - 1] of TVFSPipe;
  VFSIsInitialized: Boolean;

var
  EmbeddedInitFSStart: Byte; external name '_binary_initfs_img_start';
  EmbeddedInitFSEnd: Byte; external name '_binary_initfs_img_end';

function VFSByteAt(Base: Pointer; Offset: QWord): Byte;
begin
  Result := PByte(PtrUInt(Base) + Offset)^;
end;

function VFSReadLE16(Base: Pointer; Offset: QWord): Word;
begin
  Result := Word(VFSByteAt(Base, Offset)) or
            (Word(VFSByteAt(Base, Offset + 1)) shl 8);
end;

function VFSReadLE32(Base: Pointer; Offset: QWord): LongWord;
begin
  Result := LongWord(VFSByteAt(Base, Offset)) or
    (LongWord(VFSByteAt(Base, Offset + 1)) shl 8) or
    (LongWord(VFSByteAt(Base, Offset + 2)) shl 16) or
    (LongWord(VFSByteAt(Base, Offset + 3)) shl 24);
end;

function VFSReadLE64(Base: Pointer; Offset: QWord): QWord;
begin
  Result := QWord(VFSReadLE32(Base, Offset)) or
            (QWord(VFSReadLE32(Base, Offset + 4)) shl 32);
end;


function VFSNodeNameEquals(NodeIndex: LongInt; const Name: array of Char): Boolean;
var
  I: LongWord;
begin
  Result := False;
  if (NodeIndex < 0) or (NodeIndex >= LongInt(VFSNodeCount)) then Exit;
  for I := 0 to High(Name) do
  begin
    if VFSNodes[NodeIndex].Name[I] <> Name[I] then Exit;
    if Name[I] = #0 then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function VFSFindChild(ParentIndex: LongInt; const Name: array of Char): LongInt;
var
  I: LongWord;
begin
  Result := -1;
  for I := 0 to VFSNodeCount - 1 do
    if (VFSNodes[I].Active <> 0) and
       (VFSNodes[I].Parent = ParentIndex) and
       VFSNodeNameEquals(I, Name) then
    begin
      Result := LongInt(I);
      Exit;
    end;
end;

function VFSAddNode(ParentIndex: LongInt; const Name: array of Char;
  Kind: TVFSNodeKind; Data: Pointer; Size: QWord; out NodeIndex: LongInt): Boolean;
var
  I: LongWord;
begin
  Result := False;
  NodeIndex := -1;
  if VFSNodeCount >= VFS_NODE_MAX then Exit;
  if (ParentIndex < 0) or (ParentIndex >= LongInt(VFSNodeCount)) then Exit;
  if VFSFindChild(ParentIndex, Name) >= 0 then Exit;
  NodeIndex := LongInt(VFSNodeCount);
  Inc(VFSNodeCount);
  VFSNodes[NodeIndex].Active := 1;
  VFSNodes[NodeIndex].Kind := Kind;
  VFSNodes[NodeIndex].Parent := ParentIndex;
  for I := 0 to 63 do
  begin
    if I <= High(Name) then VFSNodes[NodeIndex].Name[I] := Name[I]
    else VFSNodes[NodeIndex].Name[I] := #0;
  end;
  VFSNodes[NodeIndex].Data := Data;
  VFSNodes[NodeIndex].Size := Size;
  Result := True;
end;

function VFSAddArchivePath(PathBytes: Pointer; PathLength: Word;
  Data: Pointer; DataSize: QWord): Boolean;
var
  Path: array[0..VFS_PATH_MAX] of Char;
  Component: array[0..63] of Char;
  I, StartPos, ComponentLength: LongWord;
  ParentIndex, NodeIndex: LongInt;
  IsLast: Boolean;
begin
  Result := False;
  if (PathLength = 0) or (PathLength > VFS_PATH_MAX) then Exit;
  if PByte(PathBytes)^ <> Ord('/') then Exit;
  for I := 0 to PathLength do Path[I] := #0;
  for I := 0 to PathLength - 1 do
  begin
    Path[I] := Char(PByte(PtrUInt(PathBytes) + I)^);
    if Path[I] = #0 then Exit;
  end;

  ParentIndex := 0;
  I := 1;
  while I < PathLength do
  begin
    StartPos := I;
    while (I < PathLength) and (Path[I] <> '/') do Inc(I);
    ComponentLength := I - StartPos;
    if ComponentLength = 0 then Exit;
    if (ComponentLength > 63) or (ComponentLength = 1) and
       (Path[StartPos] = '.') then Exit;
    if (ComponentLength = 2) and (Path[StartPos] = '.') and
       (Path[StartPos + 1] = '.') then Exit;
    for NodeIndex := 0 to 63 do Component[NodeIndex] := #0;
    for NodeIndex := 0 to LongInt(ComponentLength) - 1 do
      Component[NodeIndex] := Path[StartPos + NodeIndex];
    IsLast := I >= PathLength;
    if not IsLast then
    begin
      NodeIndex := VFSFindChild(ParentIndex, Component);
      if NodeIndex < 0 then
      begin
        if not VFSAddNode(ParentIndex, Component, vnDirectory, nil, 0,
                          NodeIndex) then Exit;
      end
      else if VFSNodes[NodeIndex].Kind <> vnDirectory then Exit;
      ParentIndex := NodeIndex;
      Inc(I);
    end
    else
    begin
      if not VFSAddNode(ParentIndex, Component, vnRegular, Data, DataSize,
                        NodeIndex) then Exit;
    end;
  end;
  Result := True;
end;

function VFSAddDevice(ParentIndex: LongInt; const Name: array of Char;
  Kind: TVFSNodeKind): Boolean;
var
  NodeIndex: LongInt;
begin
  Result := VFSAddNode(ParentIndex, Name, Kind, nil, 0, NodeIndex);
end;

function VFSParseInitFS(Image: Pointer; ImageSize: QWord): Boolean;
var
  Count, I: LongWord;
  Offset: QWord;
  PathLength: Word;
  DataSize: QWord;
  PathPtr, DataPtr: Pointer;
  Magic: array[0..3] of Byte;
begin
  Result := False;
  if (Image = nil) or (ImageSize < 12) then Exit;
  Magic[0] := VFSByteAt(Image, 0);
  Magic[1] := VFSByteAt(Image, 1);
  Magic[2] := VFSByteAt(Image, 2);
  Magic[3] := VFSByteAt(Image, 3);
  if (Magic[0] <> Ord('M')) or (Magic[1] <> Ord('F')) or
     (Magic[2] <> Ord('S')) or (Magic[3] <> Ord('1')) then Exit;
  if VFSReadLE32(Image, 4) <> 1 then Exit;
  Count := VFSReadLE32(Image, 8);
  if (Count = 0) or (Count > 48) then Exit;
  Offset := 12;
  for I := 0 to Count - 1 do
  begin
    if Offset > ImageSize - 12 then Exit;
    PathLength := VFSReadLE16(Image, Offset);
    DataSize := VFSReadLE64(Image, Offset + 4);
    Inc(Offset, 12);
    if (PathLength = 0) or (PathLength > VFS_PATH_MAX) then Exit;
    if Offset > ImageSize then Exit;
    if QWord(PathLength) > ImageSize - Offset then Exit;
    PathPtr := Pointer(PtrUInt(Image) + Offset);
    Inc(Offset, PathLength);
    if DataSize > ImageSize - Offset then Exit;
    DataPtr := Pointer(PtrUInt(Image) + Offset);
    if not VFSAddArchivePath(PathPtr, PathLength, DataPtr, DataSize) then Exit;
    Inc(Offset, DataSize);
  end;
  if Offset <> ImageSize then Exit;
  Result := True;
end;

procedure VFSInitialize(ImageStart, ImageEnd: Pointer);
var
  I: LongWord;
  ImageSize: QWord;
  DevIndex: LongInt;
begin
  VFSIsInitialized := False;
  VFSNodeCount := 0;
  for I := 0 to VFS_NODE_MAX - 1 do
  begin
    VFSNodes[I].Active := 0;
    VFSNodes[I].Parent := -1;
    VFSNodes[I].Data := nil;
    VFSNodes[I].Size := 0;
  end;
  for I := 0 to VFS_OPEN_MAX - 1 do
  begin
    VFSOpenDescriptions[I].Active := 0;
    VFSOpenDescriptions[I].RefCount := 0;
    VFSOpenDescriptions[I].Kind := voFree;
  end;
  for I := 0 to VFS_PIPE_MAX - 1 do VFSPipes[I].Active := 0;

  VFSNodeCount := 1;
  VFSNodes[0].Active := 1;
  VFSNodes[0].Kind := vnDirectory;
  VFSNodes[0].Parent := -1;
  VFSNodes[0].Data := nil;
  VFSNodes[0].Size := 0;
  for I := 0 to 63 do VFSNodes[0].Name[I] := #0;

  if (ImageStart = nil) or (ImageEnd = nil) or
     (PtrUInt(ImageEnd) <= PtrUInt(ImageStart)) then HaltForever;
  ImageSize := QWord(PtrUInt(ImageEnd) - PtrUInt(ImageStart));
  if not VFSParseInitFS(ImageStart, ImageSize) then
  begin
    WriteText('[FAIL] InitFS archive is invalid.'#13#10);
    HaltForever;
  end;

  DevIndex := VFSFindChild(0, ['d','e','v',#0]);
  if DevIndex < 0 then
  begin
    if not VFSAddNode(0, ['d','e','v',#0], vnDirectory, nil, 0, DevIndex) then
      HaltForever;
  end;
  if not VFSAddDevice(DevIndex, ['c','o','n','s','o','l','e',#0], vnConsole) then HaltForever;
  if not VFSAddDevice(DevIndex, ['n','u','l','l',#0], vnNull) then HaltForever;
  if not VFSAddDevice(DevIndex, ['s','e','r','i','a','l',#0], vnSerial) then HaltForever;
  VFSIsInitialized := True;
  WriteText('[ OK ] InitFS mounted read-only; DevFS nodes registered.'#13#10);
  WriteText('[ OK ] VFS node count: ');
  WriteDecimal(VFSNodeCount);
  WriteText('.'#13#10);
end;

function VFSStringLength(const S: array of Char): LongWord;
var
  I: LongWord;
begin
  Result := 0;
  for I := 0 to High(S) do
  begin
    if S[I] = #0 then Exit;
    Inc(Result);
  end;
end;

function VFSCopyUserString(RootPhysical, UserAddress: QWord;
  var Destination: array of Char): Boolean;
var
  I: LongWord;
  C: Char;
begin
  Result := False;
  if (UserAddress = 0) or (Length(Destination) = 0) then Exit;
  for I := 0 to High(Destination) do
  begin
    if not AddressSpaceCopyFrom(RootPhysical, UserAddress + I, @C, 1) then Exit;
    Destination[I] := C;
    if C = #0 then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function VFSNormalizePath(const CurrentDirectory: array of Char;
  const InputPath: array of Char; var OutputPath: array of Char): Boolean;
var
  Temp: array[0..255] of Char;
  Components: array[0..15, 0..63] of Char;
  ComponentCount: LongWord;
  InputLength, I, StartPos, LengthValue, J, OutPos: LongWord;
  IsAbsolute: Boolean;
begin
  Result := False;
  for I := 0 to 255 do Temp[I] := #0;
  InputLength := VFSStringLength(InputPath);
  if (InputLength = 0) or (InputLength > VFS_PATH_MAX) then Exit;
  IsAbsolute := InputPath[0] = '/';
  if IsAbsolute then
  begin
    for I := 0 to InputLength - 1 do Temp[I] := InputPath[I];
  end
  else
  begin
    LengthValue := VFSStringLength(CurrentDirectory);
    if LengthValue = 0 then
    begin
      Temp[0] := '/';
      LengthValue := 1;
    end
    else
      for I := 0 to LengthValue - 1 do Temp[I] := CurrentDirectory[I];
    if LengthValue + InputLength + 2 > 255 then Exit;
    if Temp[LengthValue - 1] <> '/' then
    begin
      Temp[LengthValue] := '/';
      Inc(LengthValue);
    end;
    for I := 0 to InputLength - 1 do Temp[LengthValue + I] := InputPath[I];
  end;

  for I := 0 to 15 do
    for J := 0 to 63 do Components[I, J] := #0;
  ComponentCount := 0;
  LengthValue := VFSStringLength(Temp);
  I := 0;
  while I < LengthValue do
  begin
    while (I < LengthValue) and (Temp[I] = '/') do Inc(I);
    StartPos := I;
    while (I < LengthValue) and (Temp[I] <> '/') do Inc(I);
    J := I - StartPos;
    if J = 0 then Continue;
    if (J = 1) and (Temp[StartPos] = '.') then Continue;
    if (J = 2) and (Temp[StartPos] = '.') and (Temp[StartPos + 1] = '.') then
    begin
      if ComponentCount > 0 then Dec(ComponentCount);
      Continue;
    end;
    if (J > 63) or (ComponentCount >= 16) then Exit;
    for OutPos := 0 to J - 1 do
      Components[ComponentCount, OutPos] := Temp[StartPos + OutPos];
    Inc(ComponentCount);
  end;

  for I := 0 to High(OutputPath) do OutputPath[I] := #0;
  OutPos := 0;
  OutputPath[OutPos] := '/';
  Inc(OutPos);
  if ComponentCount <> 0 then
  for I := 0 to ComponentCount - 1 do
  begin
    J := 0;
    while (J < 63) and (Components[I, J] <> #0) do
    begin
      if OutPos >= VFS_PATH_MAX then Exit;
      OutputPath[OutPos] := Components[I, J];
      Inc(OutPos);
      Inc(J);
    end;
    if I + 1 < ComponentCount then
    begin
      if OutPos >= VFS_PATH_MAX then Exit;
      OutputPath[OutPos] := '/';
      Inc(OutPos);
    end;
  end;
  OutputPath[OutPos] := #0;
  Result := True;
end;

function VFSFindPath(const Path: array of Char): LongInt;
var
  I, J, ParentIndex: LongInt;
  Component: array[0..63] of Char;
  PosIndex, PathLength, ComponentLength: LongWord;
begin
  Result := -1;
  PathLength := VFSStringLength(Path);
  if (PathLength = 0) or (Path[0] <> '/') then Exit;
  if PathLength = 1 then
  begin
    Result := 0;
    Exit;
  end;
  ParentIndex := 0;
  PosIndex := 1;
  while PosIndex < PathLength do
  begin
    ComponentLength := 0;
    while (PosIndex < PathLength) and (Path[PosIndex] <> '/') do
    begin
      if ComponentLength >= 63 then Exit;
      Component[ComponentLength] := Path[PosIndex];
      Inc(ComponentLength);
      Inc(PosIndex);
    end;
    Component[ComponentLength] := #0;
    if ComponentLength = 0 then Exit;
    I := VFSFindChild(ParentIndex, Component);
    if I < 0 then Exit;
    ParentIndex := I;
    if PosIndex < PathLength then Inc(PosIndex);
  end;
  Result := ParentIndex;
end;

function VFSAllocateDescription(Kind: TVFSOpenKind; NodeIndex,
  PipeIndex: LongInt; Flags: LongWord): LongInt;
var
  I: LongWord;
begin
  Result := -1;
  for I := 0 to VFS_OPEN_MAX - 1 do
    if VFSOpenDescriptions[I].Active = 0 then
    begin
      VFSOpenDescriptions[I].Active := 1;
      VFSOpenDescriptions[I].Kind := Kind;
      VFSOpenDescriptions[I].RefCount := 1;
      VFSOpenDescriptions[I].NodeIndex := NodeIndex;
      VFSOpenDescriptions[I].PipeIndex := PipeIndex;
      VFSOpenDescriptions[I].Position := 0;
      VFSOpenDescriptions[I].Flags := Flags;
      VFSOpenDescriptions[I].RemoteHandle := 0;
      Result := LongInt(I);
      Exit;
    end;
end;

procedure VFSQueueRemoteClose(RemoteHandle: QWord); forward;

procedure VFSReleaseDescription(DescriptionIndex: LongInt);
var
  Kind: TVFSOpenKind;
  PipeIndex: LongInt;
  RemoteHandle: QWord;
begin
  if (DescriptionIndex < 0) or (DescriptionIndex >= VFS_OPEN_MAX) then Exit;
  if VFSOpenDescriptions[DescriptionIndex].Active = 0 then Exit;
  if VFSOpenDescriptions[DescriptionIndex].RefCount > 1 then
  begin
    Dec(VFSOpenDescriptions[DescriptionIndex].RefCount);
    Exit;
  end;
  Kind := VFSOpenDescriptions[DescriptionIndex].Kind;
  PipeIndex := VFSOpenDescriptions[DescriptionIndex].PipeIndex;
  RemoteHandle := VFSOpenDescriptions[DescriptionIndex].RemoteHandle;
  if (Kind = voRemote) and (RemoteHandle <> 0) then
    VFSQueueRemoteClose(RemoteHandle);
  VFSOpenDescriptions[DescriptionIndex].Active := 0;
  VFSOpenDescriptions[DescriptionIndex].RefCount := 0;
  VFSOpenDescriptions[DescriptionIndex].Kind := voFree;
  VFSOpenDescriptions[DescriptionIndex].RemoteHandle := 0;
  if (PipeIndex >= 0) and (PipeIndex < VFS_PIPE_MAX) then
  begin
    if (Kind = voPipeRead) and (VFSPipes[PipeIndex].Readers > 0) then
      Dec(VFSPipes[PipeIndex].Readers);
    if (Kind = voPipeWrite) and (VFSPipes[PipeIndex].Writers > 0) then
      Dec(VFSPipes[PipeIndex].Writers);
    if (VFSPipes[PipeIndex].Readers = 0) and (VFSPipes[PipeIndex].Writers = 0) then
      VFSPipes[PipeIndex].Active := 0;
  end;
end;

function VFSAllocateFD(var Files: TVFSProcessFiles; DescriptionIndex: LongInt): LongInt;
var
  I: LongWord;
begin
  Result := -1;
  if (DescriptionIndex < 0) or (DescriptionIndex >= VFS_OPEN_MAX) then Exit;
  for I := 0 to VFS_FD_MAX - 1 do
    if Files.FDs[I].Active = 0 then
    begin
      Files.FDs[I].Active := 1;
      Files.FDs[I].Description := DescriptionIndex;
      Result := LongInt(I);
      Exit;
    end;
end;

procedure VFSCloseFD(var Files: TVFSProcessFiles; FD: LongInt);
var
  DescriptionIndex: LongInt;
begin
  if (FD < 0) or (FD >= VFS_FD_MAX) then Exit;
  if Files.FDs[FD].Active = 0 then Exit;
  DescriptionIndex := Files.FDs[FD].Description;
  Files.FDs[FD].Active := 0;
  Files.FDs[FD].Description := -1;
  VFSReleaseDescription(DescriptionIndex);
end;

function VFSFDDescription(var Files: TVFSProcessFiles; FD: QWord): LongInt;
begin
  Result := -1;
  if FD >= VFS_FD_MAX then Exit;
  if Files.FDs[FD].Active = 0 then Exit;
  Result := Files.FDs[FD].Description;
  if (Result < 0) or (Result >= VFS_OPEN_MAX) then Result := -1;
  if (Result >= 0) and (VFSOpenDescriptions[Result].Active = 0) then Result := -1;
end;


function VFSAllocateRemoteFD(var Files: TVFSProcessFiles;
  RemoteHandle: QWord; Flags: LongWord): LongInt;
var
  DescriptionIndex: LongInt;
begin
  Result := -1;
  if RemoteHandle = 0 then Exit;
  DescriptionIndex := VFSAllocateDescription(voRemote, -1, -1, Flags);
  if DescriptionIndex < 0 then Exit;
  VFSOpenDescriptions[DescriptionIndex].RemoteHandle := RemoteHandle;
  Result := VFSAllocateFD(Files, DescriptionIndex);
  if Result < 0 then VFSReleaseDescription(DescriptionIndex);
end;

function VFSIsRemoteFD(var Files: TVFSProcessFiles; FD: QWord;
  out RemoteHandle: QWord; out Flags: LongWord;
  out DescriptionIndex: LongInt): Boolean;
begin
  Result := False;
  RemoteHandle := 0;
  Flags := 0;
  DescriptionIndex := VFSFDDescription(Files, FD);
  if DescriptionIndex < 0 then Exit;
  if VFSOpenDescriptions[DescriptionIndex].Kind <> voRemote then Exit;
  RemoteHandle := VFSOpenDescriptions[DescriptionIndex].RemoteHandle;
  Flags := VFSOpenDescriptions[DescriptionIndex].Flags;
  Result := RemoteHandle <> 0;
end;

procedure VFSProcessCloseAll(var Files: TVFSProcessFiles); forward;

procedure VFSProcessInitialize(var Files: TVFSProcessFiles);
var
  I: LongWord;
  ConsoleDescription: LongInt;
begin
  for I := 0 to VFS_FD_MAX - 1 do
  begin
    Files.FDs[I].Active := 0;
    Files.FDs[I].Description := -1;
  end;
  for I := 0 to VFS_PATH_MAX do Files.CurrentDirectory[I] := #0;
  Files.CurrentDirectory[0] := '/';
  ConsoleDescription := VFSAllocateDescription(voConsole, -1, -1, 3);
  if ConsoleDescription < 0 then Exit;
  Files.FDs[0].Active := 1;
  Files.FDs[0].Description := ConsoleDescription;
  Inc(VFSOpenDescriptions[ConsoleDescription].RefCount);
  Files.FDs[1].Active := 1;
  Files.FDs[1].Description := ConsoleDescription;
  Inc(VFSOpenDescriptions[ConsoleDescription].RefCount);
  Files.FDs[2].Active := 1;
  Files.FDs[2].Description := ConsoleDescription;
end;

function VFSProcessClone(var Destination: TVFSProcessFiles;
  const Source: TVFSProcessFiles): Boolean;
var
  I: LongWord;
  DescriptionIndex: LongInt;
begin
  Result := False;
  VFSProcessCloseAll(Destination);
  for I := 0 to VFS_FD_MAX - 1 do
  begin
    Destination.FDs[I].Active := 0;
    Destination.FDs[I].Description := -1;
  end;
  for I := 0 to VFS_PATH_MAX do Destination.CurrentDirectory[I] := Source.CurrentDirectory[I];
  for I := 0 to VFS_FD_MAX - 1 do
  begin
    if Source.FDs[I].Active = 0 then Continue;
    DescriptionIndex := Source.FDs[I].Description;
    if (DescriptionIndex < 0) or (DescriptionIndex >= VFS_OPEN_MAX) then
    begin
      VFSProcessCloseAll(Destination);
      Exit;
    end;
    if VFSOpenDescriptions[DescriptionIndex].Active = 0 then
    begin
      VFSProcessCloseAll(Destination);
      Exit;
    end;
    if VFSOpenDescriptions[DescriptionIndex].RefCount = High(Word) then
    begin
      VFSProcessCloseAll(Destination);
      Exit;
    end;
    Inc(VFSOpenDescriptions[DescriptionIndex].RefCount);
    Destination.FDs[I].Active := 1;
    Destination.FDs[I].Description := DescriptionIndex;
  end;
  Result := True;
end;

procedure VFSProcessCloseAll(var Files: TVFSProcessFiles);
var
  I: LongInt;
begin
  for I := 0 to VFS_FD_MAX - 1 do VFSCloseFD(Files, I);
end;

function VFSProcessSetFD(var Files: TVFSProcessFiles;
  SourceFD, DestinationFD: QWord): Boolean;
var
  DescriptionIndex: LongInt;
begin
  Result := False;
  if (SourceFD >= VFS_FD_MAX) or (DestinationFD >= VFS_FD_MAX) then Exit;
  DescriptionIndex := VFSFDDescription(Files, SourceFD);
  if DescriptionIndex < 0 then Exit;
  if SourceFD = DestinationFD then
  begin
    Result := True;
    Exit;
  end;
  if VFSOpenDescriptions[DescriptionIndex].RefCount = High(Word) then Exit;
  Inc(VFSOpenDescriptions[DescriptionIndex].RefCount);
  VFSCloseFD(Files, DestinationFD);
  Files.FDs[DestinationFD].Active := 1;
  Files.FDs[DestinationFD].Description := DescriptionIndex;
  Result := True;
end;


function VFSGetCanonicalNode(var Files: TVFSProcessFiles;
  RootPhysical, UserPathAddress: QWord; var Canonical: array of Char;
  out NodeIndex: LongInt): QWord;
var
  UserPath: array[0..VFS_PATH_MAX] of Char;
begin
  NodeIndex := -1;
  if not VFSCopyUserString(RootPhysical, UserPathAddress, UserPath) then
  begin
    Result := VFS_ERR_FAULT;
    Exit;
  end;
  if not VFSNormalizePath(Files.CurrentDirectory, UserPath, Canonical) then
  begin
    Result := VFS_ERR_NAMETOOLONG;
    Exit;
  end;
  NodeIndex := VFSFindPath(Canonical);
  if NodeIndex < 0 then Result := VFS_ERR_NOENT else Result := 0;
end;

function VFSOpenUser(var Files: TVFSProcessFiles; RootPhysical,
  UserPathAddress, Flags: QWord): QWord;
var
  Canonical: array[0..VFS_PATH_MAX] of Char;
  NodeIndex, DescriptionIndex, FD: LongInt;
  Status: QWord;
  Kind: TVFSOpenKind;
begin
  Result := VFS_ERR_INVAL;
  if (Flags and not QWord(VFS_O_WRONLY or VFS_O_RDWR or VFS_O_DIRECTORY)) <> 0 then Exit;
  if ((Flags and 3) = 3) then Exit;
  Status := VFSGetCanonicalNode(Files, RootPhysical, UserPathAddress,
    Canonical, NodeIndex);
  if Status <> 0 then
  begin
    Result := Status;
    Exit;
  end;
  if ((Flags and VFS_O_DIRECTORY) <> 0) and
     (VFSNodes[NodeIndex].Kind <> vnDirectory) then
  begin
    Result := VFS_ERR_NOTDIR;
    Exit;
  end;
  if (VFSNodes[NodeIndex].Kind = vnDirectory) then
  begin
    if (Flags and 3) <> VFS_O_RDONLY then
    begin
      Result := VFS_ERR_ISDIR;
      Exit;
    end;
    Kind := voDirectory;
  end
  else
    case VFSNodes[NodeIndex].Kind of
      vnRegular: begin
        if (Flags and 3) <> VFS_O_RDONLY then
        begin
          Result := VFS_ERR_ROFS;
          Exit;
        end;
        Kind := voRegular;
      end;
      vnConsole: Kind := voConsole;
      vnNull: Kind := voNull;
      vnSerial: Kind := voSerial;
    else
      begin
        Result := VFS_ERR_INVAL;
        Exit;
      end;
    end;
  DescriptionIndex := VFSAllocateDescription(Kind, NodeIndex, -1, LongWord(Flags));
  if DescriptionIndex < 0 then
  begin
    Result := VFS_ERR_MFILE;
    Exit;
  end;
  FD := VFSAllocateFD(Files, DescriptionIndex);
  if FD < 0 then
  begin
    VFSReleaseDescription(DescriptionIndex);
    Result := VFS_ERR_MFILE;
    Exit;
  end;
  Result := FD;
end;

function VFSReadUser(var Files: TVFSProcessFiles; RootPhysical,
  FD, UserBuffer, Count: QWord): QWord;
var
  DescriptionIndex: LongInt;
  Kind: TVFSOpenKind;
  Temp: array[0..VFS_IO_MAX - 1] of Byte;
  Amount, I: QWord;
  NodeIndex, PipeIndex: LongInt;
  PipeStart: LongWord;
  C: Char;
  Source: PByte;
  Pipe: ^TVFSPipe;
begin
  Result := VFS_ERR_BADF;
  DescriptionIndex := VFSFDDescription(Files, FD);
  if DescriptionIndex < 0 then Exit;
  Kind := VFSOpenDescriptions[DescriptionIndex].Kind;
  if VFSOpenDescriptions[DescriptionIndex].Flags = VFS_O_WRONLY then
  begin
    Result := VFS_ERR_BADF;
    Exit;
  end;
  if Count = 0 then
  begin
    Result := 0;
    Exit;
  end;
  if Count > VFS_IO_MAX then Count := VFS_IO_MAX;
  if not AddressSpaceValidateUserRange(RootPhysical, UserBuffer, Count) then
  begin
    Result := VFS_ERR_FAULT;
    Exit;
  end;
  if (Kind = voDirectory) or (Kind = voPipeWrite) then
  begin
    Result := VFS_ERR_INVAL;
    Exit;
  end;
  Amount := 0;
  case Kind of
    voRegular:
      begin
        NodeIndex := VFSOpenDescriptions[DescriptionIndex].NodeIndex;
        if (NodeIndex < 0) or (NodeIndex >= LongInt(VFSNodeCount)) then Exit;
        if VFSOpenDescriptions[DescriptionIndex].Position >= VFSNodes[NodeIndex].Size then
        begin
          Result := 0;
          Exit;
        end;
        Amount := VFSNodes[NodeIndex].Size - VFSOpenDescriptions[DescriptionIndex].Position;
        if Amount > Count then Amount := Count;
        Source := PByte(PtrUInt(VFSNodes[NodeIndex].Data) +
                  VFSOpenDescriptions[DescriptionIndex].Position);
        for I := 0 to Amount - 1 do Temp[I] := Source[I];
      end;
    voConsole, voSerial:
      begin
        while Amount < Count do
        begin
          if not SerialTryGetChar(C) then Break;
          Temp[Amount] := Ord(C);
          Inc(Amount);
          if C = #10 then Break;
        end;
        if Amount = 0 then
        begin
          Result := VFS_ERR_AGAIN;
          Exit;
        end;
      end;
    voNull:
      begin
        Result := 0;
        Exit;
      end;
    voPipeRead:
      begin
        PipeIndex := VFSOpenDescriptions[DescriptionIndex].PipeIndex;
        if (PipeIndex < 0) or (PipeIndex >= VFS_PIPE_MAX) then Exit;
        Pipe := @VFSPipes[PipeIndex];
        if Pipe^.Count = 0 then
        begin
          if Pipe^.Writers = 0 then Result := 0 else Result := VFS_ERR_AGAIN;
          Exit;
        end;
        Amount := Pipe^.Count;
        if Amount > Count then Amount := Count;
        PipeStart := Pipe^.Head;
        for I := 0 to Amount - 1 do
          Temp[I] := Pipe^.Buffer[(PipeStart + I) mod VFS_PIPE_CAPACITY];
      end;
  else
    begin
      Result := VFS_ERR_BADF;
      Exit;
    end;
  end;
  if not AddressSpaceCopyTo(RootPhysical, UserBuffer, @Temp[0], Amount) then
  begin
    Result := VFS_ERR_FAULT;
    Exit;
  end;
  if Kind = voRegular then
    Inc(VFSOpenDescriptions[DescriptionIndex].Position, Amount);
  if Kind = voPipeRead then
  begin
    Pipe := @VFSPipes[PipeIndex];
    Pipe^.Head := (PipeStart + Amount) mod VFS_PIPE_CAPACITY;
    Dec(Pipe^.Count, Amount);
  end;
  Result := Amount;
end;

function VFSWriteUser(var Files: TVFSProcessFiles; RootPhysical,
  FD, UserBuffer, Count: QWord): QWord;
var
  DescriptionIndex: LongInt;
  Kind: TVFSOpenKind;
  Temp: array[0..VFS_IO_MAX - 1] of Byte;
  Amount, I: QWord;
  PipeIndex: LongInt;
  Pipe: ^TVFSPipe;
begin
  Result := VFS_ERR_BADF;
  DescriptionIndex := VFSFDDescription(Files, FD);
  if DescriptionIndex < 0 then Exit;
  Kind := VFSOpenDescriptions[DescriptionIndex].Kind;
  if VFSOpenDescriptions[DescriptionIndex].Flags = VFS_O_RDONLY then
  begin
    Result := VFS_ERR_BADF;
    Exit;
  end;
  if Count = 0 then
  begin
    Result := 0;
    Exit;
  end;
  if Count > VFS_IO_MAX then Count := VFS_IO_MAX;
  if (Kind = voRegular) or (Kind = voDirectory) then
  begin
    Result := VFS_ERR_ROFS;
    Exit;
  end;
  if not AddressSpaceCopyFrom(RootPhysical, UserBuffer, @Temp[0], Count) then
  begin
    Result := VFS_ERR_FAULT;
    Exit;
  end;
  case Kind of
    voConsole, voSerial:
      begin
        for I := 0 to Count - 1 do SerialPutChar(Char(Temp[I]));
        Result := Count;
      end;
    voNull:
      Result := Count;
    voPipeWrite:
      begin
        PipeIndex := VFSOpenDescriptions[DescriptionIndex].PipeIndex;
        if (PipeIndex < 0) or (PipeIndex >= VFS_PIPE_MAX) then Exit;
        Pipe := @VFSPipes[PipeIndex];
        if Pipe^.Readers = 0 then
        begin
          Result := VFS_ERR_PIPE;
          Exit;
        end;
        Amount := VFS_PIPE_CAPACITY - Pipe^.Count;
        if Amount = 0 then
        begin
          Result := VFS_ERR_AGAIN;
          Exit;
        end;
        if Amount > Count then Amount := Count;
        for I := 0 to Amount - 1 do
        begin
          Pipe^.Buffer[Pipe^.Tail] := Temp[I];
          Pipe^.Tail := (Pipe^.Tail + 1) mod VFS_PIPE_CAPACITY;
        end;
        Inc(Pipe^.Count, Amount);
        Result := Amount;
      end;
  else
    Result := VFS_ERR_BADF;
  end;
end;

function VFSCloseUser(var Files: TVFSProcessFiles; FD: QWord): QWord;
begin
  Result := VFS_ERR_BADF;
  if (FD >= VFS_FD_MAX) or (Files.FDs[FD].Active = 0) then Exit;
  VFSCloseFD(Files, FD);
  Result := 0;
end;

function VFSDup2User(var Files: TVFSProcessFiles; OldFD, NewFD: QWord): QWord;
begin
  Result := VFS_ERR_BADF;
  if not VFSProcessSetFD(Files, OldFD, NewFD) then Exit;
  Result := NewFD;
end;

function VFSPipeUser(var Files: TVFSProcessFiles; RootPhysical,
  UserPairAddress: QWord): QWord;
var
  PipeIndex: LongInt;
  I: LongWord;
  ReadDesc, WriteDesc, ReadFD, WriteFD: LongInt;
  Pair: array[0..1] of QWord;
begin
  Result := VFS_ERR_NOSPC;
  if not AddressSpaceValidateUserRange(RootPhysical, UserPairAddress, SizeOf(Pair)) then
  begin
    Result := VFS_ERR_FAULT;
    Exit;
  end;
  PipeIndex := -1;
  for I := 0 to VFS_PIPE_MAX - 1 do
    if VFSPipes[I].Active = 0 then
    begin
      PipeIndex := I;
      Break;
    end;
  if PipeIndex < 0 then Exit;
  VFSPipes[PipeIndex].Active := 1;
  VFSPipes[PipeIndex].Readers := 1;
  VFSPipes[PipeIndex].Writers := 1;
  VFSPipes[PipeIndex].Head := 0;
  VFSPipes[PipeIndex].Tail := 0;
  VFSPipes[PipeIndex].Count := 0;
  ReadDesc := VFSAllocateDescription(voPipeRead, -1, PipeIndex, VFS_O_RDONLY);
  if ReadDesc < 0 then
  begin
    VFSPipes[PipeIndex].Active := 0;
    Exit;
  end;
  WriteDesc := VFSAllocateDescription(voPipeWrite, -1, PipeIndex, VFS_O_WRONLY);
  if WriteDesc < 0 then
  begin
    VFSPipes[PipeIndex].Writers := 0;
    VFSReleaseDescription(ReadDesc);
    Exit;
  end;
  ReadFD := VFSAllocateFD(Files, ReadDesc);
  WriteFD := VFSAllocateFD(Files, WriteDesc);
  if (ReadFD < 0) or (WriteFD < 0) then
  begin
    if ReadFD >= 0 then VFSCloseFD(Files, ReadFD) else VFSReleaseDescription(ReadDesc);
    if WriteFD >= 0 then VFSCloseFD(Files, WriteFD) else VFSReleaseDescription(WriteDesc);
    Exit;
  end;
  Pair[0] := ReadFD;
  Pair[1] := WriteFD;
  if not AddressSpaceCopyTo(RootPhysical, UserPairAddress, @Pair[0], SizeOf(Pair)) then
  begin
    VFSCloseFD(Files, ReadFD);
    VFSCloseFD(Files, WriteFD);
    Result := VFS_ERR_FAULT;
    Exit;
  end;
  Result := 0;
end;

function VFSGetDentsUser(var Files: TVFSProcessFiles; RootPhysical,
  FD, UserBuffer, MaximumEntries: QWord): QWord;
var
  DescriptionIndex: LongInt;
  NodeIndex, I: LongInt;
  Position, Written: QWord;
  Entry: TVFSDirent;
  Target: PVFSDirent;
begin
  Result := VFS_ERR_BADF;
  DescriptionIndex := VFSFDDescription(Files, FD);
  if DescriptionIndex < 0 then Exit;
  if VFSOpenDescriptions[DescriptionIndex].Kind <> voDirectory then
  begin
    Result := VFS_ERR_NOTDIR;
    Exit;
  end;
  if MaximumEntries = 0 then
  begin
    Result := 0;
    Exit;
  end;
  if MaximumEntries > 16 then MaximumEntries := 16;
  Position := VFSOpenDescriptions[DescriptionIndex].Position;
  Written := 0;
  for I := 0 to VFSNodeCount - 1 do
  begin
    if VFSNodes[I].Active = 0 then Continue;
    if VFSNodes[I].Parent <> VFSOpenDescriptions[DescriptionIndex].NodeIndex then Continue;
    if Position <> 0 then
    begin
      Dec(Position);
      Continue;
    end;
    Entry.Inode := I + 1;
    case VFSNodes[I].Kind of
      vnDirectory: Entry.EntryType := VFS_DIRENT_DIRECTORY;
      vnRegular: Entry.EntryType := VFS_DIRENT_FILE;
    else Entry.EntryType := VFS_DIRENT_DEVICE;
    end;
    for NodeIndex := 0 to VFS_DIRENT_NAME_MAX - 1 do
      Entry.Name[NodeIndex] := VFSNodes[I].Name[NodeIndex];
    Entry.Size := VFSNodes[I].Size;
    Target := PVFSDirent(PtrUInt(UserBuffer) + Written * SizeOf(TVFSDirent));
    if not AddressSpaceCopyTo(RootPhysical, PtrUInt(Target), @Entry, SizeOf(Entry)) then
    begin
      Result := VFS_ERR_FAULT;
      Exit;
    end;
    Inc(Written);
    Inc(VFSOpenDescriptions[DescriptionIndex].Position);
    if Written >= MaximumEntries then Break;
  end;
  Result := Written;
end;

function VFSChdirUser(var Files: TVFSProcessFiles; RootPhysical,
  UserPathAddress: QWord): QWord;
var
  Canonical: array[0..VFS_PATH_MAX] of Char;
  NodeIndex: LongInt;
  Status: QWord;
  I: LongWord;
begin
  Status := VFSGetCanonicalNode(Files, RootPhysical, UserPathAddress,
    Canonical, NodeIndex);
  if Status <> 0 then
  begin
    Result := Status;
    Exit;
  end;
  if VFSNodes[NodeIndex].Kind <> vnDirectory then
  begin
    Result := VFS_ERR_NOTDIR;
    Exit;
  end;
  for I := 0 to VFS_PATH_MAX do Files.CurrentDirectory[I] := Canonical[I];
  Result := 0;
end;

function VFSGetCWDUser(var Files: TVFSProcessFiles; RootPhysical,
  UserBuffer, Capacity: QWord): QWord;
var
  LengthValue: LongWord;
begin
  LengthValue := VFSStringLength(Files.CurrentDirectory) + 1;
  if Capacity < LengthValue then
  begin
    Result := VFS_ERR_NAMETOOLONG;
    Exit;
  end;
  if not AddressSpaceCopyTo(RootPhysical, UserBuffer,
      @Files.CurrentDirectory[0], LengthValue) then
  begin
    Result := VFS_ERR_FAULT;
    Exit;
  end;
  Result := LengthValue - 1;
end;

function VFSLookupExecutable(const Path: array of Char;
  out Image: Pointer; out ImageSize: QWord): Boolean;
var
  NodeIndex: LongInt;
begin
  Result := False;
  Image := nil;
  ImageSize := 0;
  NodeIndex := VFSFindPath(Path);
  if NodeIndex < 0 then Exit;
  if VFSNodes[NodeIndex].Kind <> vnRegular then Exit;
  Image := VFSNodes[NodeIndex].Data;
  ImageSize := VFSNodes[NodeIndex].Size;
  Result := (Image <> nil) and (ImageSize <> 0);
end;

function VFSValidateSpawnFDs(const Files: TVFSProcessFiles;
  InputFD, OutputFD: QWord): Boolean;
begin
  Result := False;
  if (InputFD >= VFS_FD_MAX) or (OutputFD >= VFS_FD_MAX) then Exit;
  Result := (Files.FDs[InputFD].Active <> 0) and
            (Files.FDs[OutputFD].Active <> 0);
end;

function VFSPrepareSpawnFromParent(var ChildFiles: TVFSProcessFiles;
  const ParentFiles: TVFSProcessFiles; InputFD, OutputFD: QWord): Boolean;
var
  I: LongWord;
begin
  Result := False;
  if not VFSValidateSpawnFDs(ParentFiles, InputFD, OutputFD) then Exit;
  if not VFSProcessClone(ChildFiles, ParentFiles) then Exit;
  if not VFSProcessSetFD(ChildFiles, InputFD, 0) then
  begin
    VFSProcessCloseAll(ChildFiles);
    Exit;
  end;
  if not VFSProcessSetFD(ChildFiles, OutputFD, 1) then
  begin
    VFSProcessCloseAll(ChildFiles);
    Exit;
  end;
  for I := 3 to VFS_FD_MAX - 1 do VFSCloseFD(ChildFiles, I);
  Result := True;
end;

function VFSSetCommandLine(var Destination: array of Char;
  const Source: array of Char): Boolean;
var
  I: LongWord;
begin
  Result := False;
  for I := 0 to High(Destination) do Destination[I] := #0;
  for I := 0 to High(Destination) do
  begin
    Destination[I] := Source[I];
    if Source[I] = #0 then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function VFSSelfTest: Boolean;
var
  Path: array[0..VFS_PATH_MAX] of Char;
  NodeIndex: LongInt;
begin
  Result := False;
  Path[0] := '/'; Path[1] := 'e'; Path[2] := 't'; Path[3] := 'c';
  Path[4] := '/'; Path[5] := 'm'; Path[6] := 'o'; Path[7] := 't';
  Path[8] := 'd'; Path[9] := #0;
  NodeIndex := VFSFindPath(Path);
  if (NodeIndex < 0) or (VFSNodes[NodeIndex].Kind <> vnRegular) then Exit;
  Path[0] := '/'; Path[1] := 'b'; Path[2] := 'i'; Path[3] := 'n';
  Path[4] := '/'; Path[5] := 's'; Path[6] := 'h'; Path[7] := #0;
  NodeIndex := VFSFindPath(Path);
  if (NodeIndex < 0) or (VFSNodes[NodeIndex].Kind <> vnRegular) then Exit;
  if VFSNodes[NodeIndex].Size < 4 then Exit;
  if (VFSByteAt(VFSNodes[NodeIndex].Data, 0) <> $7F) or
     (VFSByteAt(VFSNodes[NodeIndex].Data, 1) <> Ord('E')) or
     (VFSByteAt(VFSNodes[NodeIndex].Data, 2) <> Ord('L')) or
     (VFSByteAt(VFSNodes[NodeIndex].Data, 3) <> Ord('F')) then Exit;
  Path[0] := '/'; Path[1] := 'd'; Path[2] := 'e'; Path[3] := 'v';
  Path[4] := '/'; Path[5] := 'c'; Path[6] := 'o'; Path[7] := 'n';
  Path[8] := 's'; Path[9] := 'o'; Path[10] := 'l'; Path[11] := 'e';
  Path[12] := #0;
  NodeIndex := VFSFindPath(Path);
  if (NodeIndex < 0) or (VFSNodes[NodeIndex].Kind <> vnConsole) then Exit;
  Path[0] := '/'; Path[1] := 'd'; Path[2] := 'e'; Path[3] := 'v';
  Path[4] := '/'; Path[5] := 'n'; Path[6] := 'u'; Path[7] := 'l';
  Path[8] := 'l'; Path[9] := #0;
  NodeIndex := VFSFindPath(Path);
  if (NodeIndex < 0) or (VFSNodes[NodeIndex].Kind <> vnNull) then Exit;
  Path[0] := '/'; Path[1] := 'd'; Path[2] := 'e'; Path[3] := 'v';
  Path[4] := '/'; Path[5] := 's'; Path[6] := 'e'; Path[7] := 'r';
  Path[8] := 'i'; Path[9] := 'a'; Path[10] := 'l'; Path[11] := #0;
  NodeIndex := VFSFindPath(Path);
  if (NodeIndex < 0) or (VFSNodes[NodeIndex].Kind <> vnSerial) then Exit;
  Result := True;
  WriteText('[ OK ] VFS InitFS and DevFS path lookup self-test passed.'#13#10);
end;
