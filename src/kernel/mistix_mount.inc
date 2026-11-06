{ Small mount namespace. The Phase 6 Ext2 driver is read-only; this table is
  separate from the Phase 5 InitFS VFS and is not yet wired to open(2). }

const
  MOUNT_MAX = 8;
  MOUNT_PATH_MAX = 63;
  MOUNT_FS_NONE = 0;
  MOUNT_FS_EXT2 = 1;

type
  TMountRecord = record
    Active: Byte;
    FileSystemKind: Byte;
    Ext2Index: Word;
    Path: array[0..MOUNT_PATH_MAX] of Char;
  end;

var
  MountTable: array[0..MOUNT_MAX - 1] of TMountRecord;

function MountStringLength(const S: array of Char): LongWord;
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

function MountRegisterExt2(const Path: array of Char; DeviceIndex: LongWord;
  StartLBA, SectorCount: QWord; out MountIndex: LongWord): Boolean;
var
  I, J, L, FreeSlot: LongWord;
  Ext2Index: LongWord;
  Match: Boolean;
begin
  Result := False;
  MountIndex := High(LongWord);
  L := MountStringLength(Path);
  if (L = 0) or (L > MOUNT_PATH_MAX) or (Path[0] <> '/') then Exit;
  FreeSlot := MOUNT_MAX;
  for I := 0 to MOUNT_MAX - 1 do
  begin
    if MountTable[I].Active = 0 then
    begin
      if FreeSlot = MOUNT_MAX then FreeSlot := I;
      Continue;
    end;
    if MountStringLength(MountTable[I].Path) <> L then Continue;
    Match := True;
    for J := 0 to L - 1 do
      if MountTable[I].Path[J] <> Path[J] then
      begin
        Match := False;
        Break;
      end;
    if Match then Exit;
  end;
  if FreeSlot = MOUNT_MAX then Exit;
  if not Ext2Mount(DeviceIndex, StartLBA, SectorCount, Ext2Index) then Exit;
  MountTable[FreeSlot].Active := 1;
  MountTable[FreeSlot].FileSystemKind := MOUNT_FS_EXT2;
  MountTable[FreeSlot].Ext2Index := Ext2Index;
  for L := 0 to MOUNT_PATH_MAX do
  begin
    if L < MountStringLength(Path) then MountTable[FreeSlot].Path[L] := Path[L]
    else MountTable[FreeSlot].Path[L] := #0;
  end;
  MountIndex := FreeSlot;
  Result := True;
end;

function MountFindLongestPrefix(const Path: array of Char): LongInt;
var
  I, J, PathLength, MountLength: LongWord;
  Match, BoundaryMatch: Boolean;
begin
  Result := -1;
  PathLength := MountStringLength(Path);
  if (PathLength = 0) or (Path[0] <> '/') then Exit;
  for I := 0 to MOUNT_MAX - 1 do
  begin
    if MountTable[I].Active = 0 then Continue;
    MountLength := MountStringLength(MountTable[I].Path);
    if (MountLength = 0) or (MountLength > PathLength) then Continue;
    Match := True;
    for J := 0 to MountLength - 1 do
      if MountTable[I].Path[J] <> Path[J] then
      begin
        Match := False;
        Break;
      end;
    if not Match then Continue;
    BoundaryMatch := MountLength = PathLength;
    if not BoundaryMatch then
    begin
      if (MountLength = 1) and (MountTable[I].Path[0] = '/') then
        BoundaryMatch := True
      else if MountTable[I].Path[MountLength - 1] = '/' then
        BoundaryMatch := True
      else
        BoundaryMatch := Path[MountLength] = '/';
    end;
    if BoundaryMatch then
    begin
      if Result < 0 then
        Result := LongInt(I)
      else if MountLength > MountStringLength(MountTable[Result].Path) then
        Result := LongInt(I);
    end;
  end;
end;

procedure MountInitialize;
var
  I, J: LongWord;
begin
  for I := 0 to MOUNT_MAX - 1 do
  begin
    MountTable[I].Active := 0;
    MountTable[I].FileSystemKind := MOUNT_FS_NONE;
    MountTable[I].Ext2Index := 0;
    for J := 0 to MOUNT_PATH_MAX do MountTable[I].Path[J] := #0;
  end;
end;
