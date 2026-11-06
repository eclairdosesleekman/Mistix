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
  VFS_OPEN_MAX = 8;
  VFS_RPC_PATH_BYTES = 40;
  VFS_RPC_READ_MAX = 40;
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
    DataOffset: QWord;
    DataSize: QWord;
    Position: QWord;
  end;

var
  VFSBaseImageStart: Byte; external name '_binary_vfs_base_img_start';
  VFSBaseImageEnd: Byte; external name '_binary_vfs_base_img_end';
  OpenSlots: array[0..VFS_OPEN_MAX - 1] of TVFSOpenSlot;
  RequestEndpoint: QWord;
  ReplyEndpoint: QWord;
  ImageBase: Pointer;
  ImageSize: QWord;

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

function AllocateOpen(DataOffset, DataSize: QWord): QWord;
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
      OpenSlots[I].DataOffset := DataOffset;
      OpenSlots[I].DataSize := DataSize;
      OpenSlots[I].Position := 0;
      Result := (QWord(OpenSlots[I].Generation) shl 16) or (I + 1);
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
  Dest: PByte;
begin
  ClearMessage(Response);
  Response.MessageId := VFS_ERR_INVAL;
  case Request.MessageId of
    VFS_RPC_PING:
      begin
        Response.MessageId := 0;
        Response.WordCount := 2;
        Response.Words[0] := 1; { protocol version }
        Response.Words[1] := ReadLE32(ImageBase, 8); { file count }
      end;

    VFS_RPC_OPEN, VFS_RPC_STAT:
      begin
        if Request.WordCount < 5 then Exit;
        CopyRequestPath(Request, Path, ValidPath);
        if not ValidPath then Exit;
        if not ArchiveFind(Path, DataOffset, DataSize) then
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
          Response.Words[0] := AllocateOpen(DataOffset, DataSize);
          if IsError(Response.Words[0]) then
            Response.MessageId := Response.Words[0];
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
        Available := OpenSlots[Slot].DataSize - OpenSlots[Slot].Position;
        if Requested > Available then Requested := Available;
        Response.MessageId := 0;
        Response.WordCount := 6;
        Response.Words[0] := Requested;
        Dest := PByte(@Response.Words[1]);
        if Requested <> 0 then
          for I := 0 to LongWord(Requested) - 1 do
            Dest[I] := ByteAt(ImageBase,
              OpenSlots[Slot].DataOffset + OpenSlots[Slot].Position + I);
        Inc(OpenSlots[Slot].Position, Requested);
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
        Response.MessageId := 0;
      end;
  else
    Response.MessageId := VFS_ERR_PROTO;
  end;
end;

function ReceiveCapabilities: Boolean;
var
  FirstHandle, SecondHandle: QWord;
  Retry: LongWord;
begin
  Result := False;
  FirstHandle := 0;
  SecondHandle := 0;
  Retry := 0;
  while (IsError(FirstHandle) or (FirstHandle = 0)) and (Retry < 1000000) do
  begin
    FirstHandle := SysCapabilityReceive;
    if IsError(FirstHandle) then
    begin
      if FirstHandle <> MISTIX_EAGAIN then Exit;
      SysYield;
    end;
    Inc(Retry);
  end;
  Retry := 0;
  while (IsError(SecondHandle) or (SecondHandle = 0)) and (Retry < 1000000) do
  begin
    SecondHandle := SysCapabilityReceive;
    if IsError(SecondHandle) then
    begin
      if SecondHandle <> MISTIX_EAGAIN then Exit;
      SysYield;
    end;
    Inc(Retry);
  end;
  if IsError(FirstHandle) or IsError(SecondHandle) then Exit;
  RequestEndpoint := FirstHandle;
  ReplyEndpoint := SecondHandle;
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
  WriteCString('vfsd: userland VFS service online (MFS1 read-only)'#13#10);
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
