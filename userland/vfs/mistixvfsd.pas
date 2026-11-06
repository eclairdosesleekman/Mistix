unit MistixVFSd;

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
  VFS_RPC_PING  = $4D56534600000001;
  VFS_RPC_OPEN  = $4D56534600000002;
  VFS_RPC_READ  = $4D56534600000003;
  VFS_RPC_CLOSE = $4D56534600000004;
  VFS_RPC_STAT  = $4D56534600000005;
  VFS_RPC_CREATE = $4D56534600000006;
  VFS_RPC_WRITE = $4D56534600000007;
  VFS_RPC_UNLINK = $4D56534600000008;
  VFS_OPEN_MAX = 8;
  VFS_RPC_PATH_BYTES = 40;
  VFS_RPC_READ_MAX = 65536;
  VFS_TEMP_MAX = 8;
  VFS_TEMP_FILE_MAX = 4096;
  VFS_SHARED_BASE = MISTIX_SHM_DEFAULT_BASE;
  VFS_ERR_NOENT = QWord(-2);
  VFS_ERR_BADF = QWord(-9);
  VFS_ERR_INVAL = QWord(-22);
  VFS_ERR_NOSPC = QWord(-28);
  VFS_ERR_ROFS = QWord(-30);
  VFS_ERR_PROTO = QWord(-71);

  MFS1_HEADER_SIZE = 12;
  MFS1_ENTRY_HEADER_SIZE = 12;
  MFS1_MAX_ENTRIES = 48;

  MFS1_MAGIC_0 = Ord('M');
  MFS1_MAGIC_1 = Ord('F');
  MFS1_MAGIC_2 = Ord('S');
  MFS1_MAGIC_3 = Ord('1');

type
  TVFSOpenSlot = record
    Active: Byte;
    Generation: Word;
    SourceKind: Byte; { 0=MFS1 archive, 1=tmpfs }
    TempIndex: SmallInt;
    DataOffset: QWord;
    DataSize: QWord;
    Position: QWord;
  end;

  TTempFile = record
    Active: Byte;
    Path: array[0..127] of Char;
    Data: array[0..VFS_TEMP_FILE_MAX - 1] of Byte;
    Size: QWord;
  end;

var
  VFSBaseImageStart: Byte; external name '_binary_vfs_base_img_start';
  VFSBaseImageEnd: Byte; external name '_binary_vfs_base_img_end';
  OpenSlots: array[0..VFS_OPEN_MAX - 1] of TVFSOpenSlot;
  RequestEndpoint: QWord;
  ReplyEndpoint: QWord;
  ImageBase: Pointer;
  ImageSize: QWord;
  SharedBuffer: PByte;
  TempFiles: array[0..VFS_TEMP_MAX - 1] of TTempFile;

function ByteAt(Base: Pointer; Offset: QWord): Byte; inline;
begin
  Result := PByte(PtrUInt(Base) + Offset)^;
end;

function ReadLE16(Base: Pointer; Offset: QWord): Word; inline;
begin
  Result := Word(ByteAt(Base, Offset)) or
            (Word(ByteAt(Base, Offset + 1)) shl 8);
end;

function ReadLE32(Base: Pointer; Offset: QWord): LongWord; inline;
begin
  Result := LongWord(ByteAt(Base, Offset)) or
    (LongWord(ByteAt(Base, Offset + 1)) shl 8) or
    (LongWord(ByteAt(Base, Offset + 2)) shl 16) or
    (LongWord(ByteAt(Base, Offset + 3)) shl 24);
end;

function ReadLE64(Base: Pointer; Offset: QWord): QWord; inline;
begin
  Result := QWord(ReadLE32(Base, Offset)) or
            (QWord(ReadLE32(Base, Offset + 4)) shl 32);
end;

function CLength(const S: array of Char): LongWord;
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

function ValidateArchive: Boolean;
var
  Count, I, PathLength: LongWord;
  Offset, DataSize: QWord;
begin
  Result := False;
  if (ImageBase = nil) or (ImageSize < MFS1_HEADER_SIZE) then Exit;
  if (ByteAt(ImageBase, 0) <> MFS1_MAGIC_0) or
     (ByteAt(ImageBase, 1) <> MFS1_MAGIC_1) or
     (ByteAt(ImageBase, 2) <> MFS1_MAGIC_2) or
     (ByteAt(ImageBase, 3) <> MFS1_MAGIC_3) then Exit;
  if ReadLE32(ImageBase, 4) <> 1 then Exit;
  Count := ReadLE32(ImageBase, 8);
  if Count > MFS1_MAX_ENTRIES then Exit;
  Offset := MFS1_HEADER_SIZE;
  if Count = 0 then
  begin
    Result := Offset = ImageSize;
    Exit;
  end;
  for I := 0 to Count - 1 do
  begin
    if (Offset > ImageSize) or
       (ImageSize - Offset < MFS1_ENTRY_HEADER_SIZE) then Exit;
    PathLength := ReadLE16(ImageBase, Offset);
    DataSize := ReadLE64(ImageBase, Offset + 4);
    Inc(Offset, MFS1_ENTRY_HEADER_SIZE);
    if (PathLength = 0) or (PathLength > 127) then Exit;
    if (Offset > ImageSize) or (QWord(PathLength) > ImageSize - Offset) then Exit;
    Inc(Offset, PathLength);
    if (Offset > ImageSize) or (DataSize > ImageSize - Offset) then Exit;
    Inc(Offset, DataSize);
  end;
  Result := Offset = ImageSize;
end;

function ArchiveFind(const Path: array of Char; out DataOffset,
  DataSize: QWord): Boolean;
var
  Count, I, PathLength, J: LongWord;
  Offset, EntrySize, CandidateOffset: QWord;
  Match: Boolean;
begin
  Result := False;
  DataOffset := 0;
  DataSize := 0;
  PathLength := CLength(Path);
  if (PathLength = 0) or (Path[0] <> '/') then Exit;
  Count := ReadLE32(ImageBase, 8);
  Offset := MFS1_HEADER_SIZE;
  if Count = 0 then Exit;
  for I := 0 to Count - 1 do
  begin
    EntrySize := ReadLE64(ImageBase, Offset + 4);
    J := ReadLE16(ImageBase, Offset);
    Inc(Offset, MFS1_ENTRY_HEADER_SIZE);
    CandidateOffset := Offset + J;
    Match := J = PathLength;
    if Match then
      for J := 0 to PathLength - 1 do
        if Char(ByteAt(ImageBase, Offset + J)) <> Path[J] then
        begin
          Match := False;
          Break;
        end;
    if Match then
    begin
      DataOffset := CandidateOffset;
      DataSize := EntrySize;
      Result := True;
      Exit;
    end;
    Offset := CandidateOffset + EntrySize;
  end;
end;

procedure ClearMessage(var Message: TMistixIPCMessage);
var
  I: LongWord;
begin
  Message.MessageId := 0;
  Message.WordCount := 0;
  for I := 0 to 6 do Message.Reserved[I] := 0;
  for I := 0 to 5 do Message.Words[I] := 0;
end;

procedure CopyRequestPath(const Message: TMistixIPCMessage;
  var Path: array of Char; out Valid: Boolean);
var
  I: LongWord;
  Source: PByte;
  Terminated: Boolean;
begin
  Valid := False;
  for I := 0 to High(Path) do Path[I] := #0;
  Source := PByte(@Message.Words[0]);
  Terminated := False;
  for I := 0 to VFS_RPC_PATH_BYTES - 1 do
  begin
    if I >= LongWord(Length(Path)) then Exit;
    Path[I] := Char(Source[I]);
    if Source[I] = 0 then
    begin
      Terminated := True;
      Break;
    end;
  end;
  Valid := Terminated and (Path[0] = '/');
end;

function AllocateOpen(DataOffset, DataSize: QWord; SourceKind: Byte;
  TempIndex: SmallInt): QWord;
var
  I: LongWord;
begin
  Result := VFS_ERR_NOSPC;
  for I := 0 to VFS_OPEN_MAX - 1 do
    if OpenSlots[I].Active = 0 then
    begin
      if OpenSlots[I].Generation = High(Word) then Continue;
      Inc(OpenSlots[I].Generation);
      OpenSlots[I].Active := 1;
      OpenSlots[I].SourceKind := SourceKind;
      OpenSlots[I].TempIndex := TempIndex;
      OpenSlots[I].DataOffset := DataOffset;
      OpenSlots[I].DataSize := DataSize;
      OpenSlots[I].Position := 0;
      Result := (QWord(OpenSlots[I].Generation) shl 16) or (I + 1);
      Exit;
    end;
end;

function TempFind(const Path: array of Char): LongInt;
var
  I, J, L: LongWord;
  Match: Boolean;
begin
  Result := -1;
  L := CLength(Path);
  for I := 0 to VFS_TEMP_MAX - 1 do
  begin
    if TempFiles[I].Active = 0 then Continue;
    Match := True;
    for J := 0 to L do
      if TempFiles[I].Path[J] <> Path[J] then
      begin
        Match := False;
        Break;
      end;
    if Match then
    begin
      Result := I;
      Exit;
    end;
  end;
end;

function TempPathAllowed(const Path: array of Char): Boolean;
begin
  { Writable overlay is deliberately restricted to /tmp. }
  Result := (Path[0] = '/') and (Path[1] = 't') and
    (Path[2] = 'm') and (Path[3] = 'p') and (Path[4] = '/') and
    (Path[5] <> #0);
end;

function TempCreate(const Path: array of Char): LongInt;
var
  I, J, L: LongWord;
  Existing: LongInt;
begin
  Result := -1;
  L := CLength(Path);
  if (L < 6) or (L > 127) or not TempPathAllowed(Path) then Exit;
  Existing := TempFind(Path);
  if Existing >= 0 then
  begin
    TempFiles[Existing].Size := 0;
    Result := Existing;
    Exit;
  end;
  for I := 0 to VFS_TEMP_MAX - 1 do
    if TempFiles[I].Active = 0 then
    begin
      TempFiles[I].Active := 1;
      TempFiles[I].Size := 0;
      for J := 0 to 127 do
        if J <= L then TempFiles[I].Path[J] := Path[J]
        else TempFiles[I].Path[J] := #0;
      Result := I;
      Exit;
    end;
end;

function LookupOpen(Handle: QWord; out Slot: LongWord): Boolean;
var
  EncodedSlot: LongWord;
  Generation: Word;
begin
  Result := False;
  Slot := 0;
  if (Handle shr 32) <> 0 then Exit;
  EncodedSlot := LongWord(Handle) and $FFFF;
  Generation := Word(Handle shr 16);
  if (EncodedSlot = 0) or (EncodedSlot > VFS_OPEN_MAX) or
     (Generation = 0) then Exit;
  Slot := EncodedSlot - 1;
  Result := (OpenSlots[Slot].Active <> 0) and
            (OpenSlots[Slot].Generation = Generation);
end;

procedure DispatchRequest(const Request: TMistixIPCMessage;
  var Response: TMistixIPCMessage);
var
  Path: array[0..39] of Char;
  ValidPath: Boolean;
  DataOffset, DataSize, Requested, Available: QWord;
  Slot: LongWord;
  I: LongWord;
  Dest, Source: PByte;
  TempIndex: LongInt;
begin
  ClearMessage(Response);
  Response.MessageId := VFS_ERR_INVAL;
  case Request.MessageId of
    VFS_RPC_PING:
      begin
        Response.MessageId := 0;
        Response.WordCount := 3;
        Response.Words[0] := 2; { RPC protocol version }
        Response.Words[1] := ReadLE32(ImageBase, 8);
        Response.Words[2] := VFS_TEMP_MAX;
      end;

    VFS_RPC_OPEN, VFS_RPC_STAT:
      begin
        if Request.WordCount < 5 then Exit;
        CopyRequestPath(Request, Path, ValidPath);
        if not ValidPath then Exit;
        TempIndex := TempFind(Path);
        if TempIndex >= 0 then
        begin
          DataOffset := 0;
          DataSize := TempFiles[TempIndex].Size;
        end
        else if not ArchiveFind(Path, DataOffset, DataSize) then
        begin
          Response.MessageId := VFS_ERR_NOENT;
          Exit;
        end;
        Response.MessageId := 0;
        Response.WordCount := 1;
        if Request.MessageId = VFS_RPC_STAT then
          Response.Words[0] := DataSize
        else
        begin
          if TempIndex >= 0 then
            Response.Words[0] := AllocateOpen(0, DataSize, 1, TempIndex)
          else
            Response.Words[0] := AllocateOpen(DataOffset, DataSize, 0, -1);
          if IsError(Response.Words[0]) then
            Response.MessageId := Response.Words[0];
        end;
      end;

    VFS_RPC_CREATE:
      begin
        if Request.WordCount < 5 then Exit;
        CopyRequestPath(Request, Path, ValidPath);
        if not ValidPath or not TempPathAllowed(Path) then
        begin
          Response.MessageId := VFS_ERR_ROFS;
          Exit;
        end;
        TempIndex := TempCreate(Path);
        if TempIndex < 0 then
          Response.MessageId := VFS_ERR_NOSPC
        else
        begin
          Response.MessageId := 0;
          Response.WordCount := 1;
          Response.Words[0] := TempIndex;
        end;
      end;

    VFS_RPC_READ:
      begin
        if Request.WordCount < 2 then Exit;
        if not LookupOpen(Request.Words[0], Slot) then
        begin
          Response.MessageId := VFS_ERR_BADF;
          Exit;
        end;
        Requested := Request.Words[1];
        if Requested > VFS_RPC_READ_MAX then Requested := VFS_RPC_READ_MAX;
        if OpenSlots[Slot].Position >= OpenSlots[Slot].DataSize then
          Available := 0
        else
          Available := OpenSlots[Slot].DataSize - OpenSlots[Slot].Position;
        if Requested > Available then Requested := Available;
        if Requested > 65536 then Requested := 65536;
        Dest := SharedBuffer;
        if Requested <> 0 then
        begin
          if OpenSlots[Slot].SourceKind = 1 then
          begin
            TempIndex := OpenSlots[Slot].TempIndex;
            if (TempIndex < 0) or (TempIndex >= VFS_TEMP_MAX) or
               (TempFiles[TempIndex].Active = 0) then
            begin
              Response.MessageId := VFS_ERR_BADF;
              Exit;
            end;
            Source := @TempFiles[TempIndex].Data[OpenSlots[Slot].Position];
            for I := 0 to LongWord(Requested) - 1 do Dest[I] := Source[I];
          end
          else
            for I := 0 to LongWord(Requested) - 1 do
              Dest[I] := ByteAt(ImageBase,
                OpenSlots[Slot].DataOffset + OpenSlots[Slot].Position + I);
        end;
        Inc(OpenSlots[Slot].Position, Requested);
        Response.MessageId := 0;
        Response.WordCount := 1;
        Response.Words[0] := Requested;
      end;

    VFS_RPC_WRITE:
      begin
        if Request.WordCount < 2 then Exit;
        if not LookupOpen(Request.Words[0], Slot) then
        begin
          Response.MessageId := VFS_ERR_BADF;
          Exit;
        end;
        if OpenSlots[Slot].SourceKind <> 1 then
        begin
          Response.MessageId := VFS_ERR_ROFS;
          Exit;
        end;
        TempIndex := OpenSlots[Slot].TempIndex;
        if (TempIndex < 0) or (TempIndex >= VFS_TEMP_MAX) or
           (TempFiles[TempIndex].Active = 0) then
        begin
          Response.MessageId := VFS_ERR_BADF;
          Exit;
        end;
        Requested := Request.Words[1];
        if Requested > 65536 then Requested := 65536;
        if (OpenSlots[Slot].Position > VFS_TEMP_FILE_MAX) or
           (Requested > VFS_TEMP_FILE_MAX - OpenSlots[Slot].Position) then
        begin
          Response.MessageId := VFS_ERR_NOSPC;
          Exit;
        end;
        Source := SharedBuffer;
        Dest := @TempFiles[TempIndex].Data[OpenSlots[Slot].Position];
        if Requested <> 0 then
          for I := 0 to LongWord(Requested) - 1 do Dest[I] := Source[I];
        Inc(OpenSlots[Slot].Position, Requested);
        if OpenSlots[Slot].Position > TempFiles[TempIndex].Size then
          TempFiles[TempIndex].Size := OpenSlots[Slot].Position;
        OpenSlots[Slot].DataSize := TempFiles[TempIndex].Size;
        Response.MessageId := 0;
        Response.WordCount := 1;
        Response.Words[0] := Requested;
      end;

    VFS_RPC_UNLINK:
      begin
        if Request.WordCount < 5 then Exit;
        CopyRequestPath(Request, Path, ValidPath);
        if not ValidPath then Exit;
        TempIndex := TempFind(Path);
        if TempIndex < 0 then
        begin
          Response.MessageId := VFS_ERR_ROFS;
          Exit;
        end;
        TempFiles[TempIndex].Active := 0;
        TempFiles[TempIndex].Size := 0;
        Response.MessageId := 0;
      end;

    VFS_RPC_CLOSE:
      begin
        if Request.WordCount < 1 then Exit;
        if not LookupOpen(Request.Words[0], Slot) then
        begin
          Response.MessageId := VFS_ERR_BADF;
          Exit;
        end;
        OpenSlots[Slot].Active := 0;
        OpenSlots[Slot].DataOffset := 0;
        OpenSlots[Slot].DataSize := 0;
        OpenSlots[Slot].Position := 0;
        OpenSlots[Slot].TempIndex := -1;
        Response.MessageId := 0;
      end;
  else
    Response.MessageId := VFS_ERR_PROTO;
  end;
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
  FirstHandle, SecondHandle, SharedHandle: QWord;
begin
  Result := False;
  if not ReceiveOneCapability(FirstHandle) then Exit;
  if not ReceiveOneCapability(SecondHandle) then Exit;
  if not ReceiveOneCapability(SharedHandle) then Exit;
  RequestEndpoint := FirstHandle;
  ReplyEndpoint := SecondHandle;
  if IsError(SysShmMap(SharedHandle, VFS_SHARED_BASE, 1)) then Exit;
  SharedBuffer := PByte(PtrUInt(VFS_SHARED_BASE));
  Result := True;
end;

procedure MistixVFSdMain; cdecl; public name 'mistix_user_main';
var
  Request, Response: TMistixIPCMessage;
  Status: QWord;
begin
  ImageBase := @VFSBaseImageStart;
  ImageSize := PtrUInt(@VFSBaseImageEnd) - PtrUInt(@VFSBaseImageStart);
  if not ValidateArchive then
  begin
    WriteCString('vfsd: invalid embedded MFS1 archive'#13#10);
    SysExit(10);
  end;
  if not ReceiveCapabilities then
  begin
    WriteCString('vfsd: missing IPC capabilities'#13#10);
    SysExit(11);
  end;
  WriteCString('vfsd: userland VFS online (MFS1 base + writable /tmp overlay)'#13#10);
  while True do
  begin
    Status := SysIPCReceive(RequestEndpoint, PtrUInt(@Request));
    if IsError(Status) then
    begin
      SysYield;
      Continue;
    end;
    DispatchRequest(Request, Response);
    repeat
      Status := SysIPCSend(ReplyEndpoint, PtrUInt(@Response));
      if Status = MISTIX_EAGAIN then SysYield
      else if IsError(Status) then SysExit(12);
    until not IsError(Status);
  end;
end;

procedure MistixVFSdEntry; assembler; nostackframe; public name '_start';
asm
  and rsp, -16
  call mistix_user_main
  xor edi, edi
  mov eax, 2
  int $80
  ud2
end;

end.
