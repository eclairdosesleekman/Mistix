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
  VFS_RPC_MOUNT_EXT2 = $4D56534600000009;
  VFS_RPC_READDIR = $4D5653460000000A;
  VFS_RPC_BROKER_OPEN = $4D56534600000011;
  VFS_RPC_BROKER_READ = $4D56534600000012;
  VFS_RPC_BROKER_WRITE = $4D56534600000013;
  VFS_RPC_BROKER_CLOSE = $4D56534600000014;
  VFS10_DIR_OPEN = $4D56534600000021;
  VFS10_DIR_READ = $4D56534600000022;
  VFS10_CHDIR = $4D56534600000024;
  VFS10_CANCEL = $4D56534600000026;
  VFS_BROKER_PENDING_MAX = 8;
  VFS_BROKER_SLOT_SIZE = 8192;
  VFS_O_RDONLY = 0;
  VFS_O_WRONLY = 1;
  VFS_O_RDWR = 2;
  VFS_O_CREAT = $40;
  VFS_O_DIRECTORY = $100;
  VFS_OPEN_MAX = 8;
  VFS_RPC_PATH_BYTES = 40;
  VFS_BROKER_PATH_BYTES = 128;
  VFS_RPC_READ_MAX = 65536;
  VFS_RPC_IO_MAX = 4096;
  VFS_TEMP_MAX = 8;
  VFS_TEMP_FILE_MAX = 4096;
  VFS_SHARED_BASE = MISTIX_SHM_DEFAULT_BASE;
  VFS_ERR_NOENT = QWord(-2);
  VFS_ERR_BADF = QWord(-9);
  VFS_ERR_IO = QWord(-5);
  VFS_ERR_NOTDIR = QWord(-20);
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
    SourceKind: Byte; { 0=MFS1, 1=tmpfs, 2=read-only Ext2 }
    TempIndex: SmallInt;
    DataOffset: QWord;
    DataSize: QWord;
    Position: QWord;
    Flags: LongWord;
    Path: array[0..VFS_BROKER_PATH_BYTES - 1] of Char;
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

{$I mistix_ext2_types.inc}

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
  TempIndex: SmallInt; Flags: LongWord): QWord;
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
      OpenSlots[I].Flags := Flags;
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

{$I mistix_ext2_client.inc}

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


function CopySharedBrokerPath(var Path: array of Char): Boolean;
var
  I: LongWord;
begin
  Result := False;
  if (SharedBuffer = nil) or (Length(Path) < VFS_BROKER_PATH_BYTES) then Exit;
  for I := 0 to High(Path) do Path[I] := #0;
  for I := 0 to VFS_BROKER_PATH_BYTES - 1 do
  begin
    Path[I] := Char(SharedBuffer[I]);
    if SharedBuffer[I] = 0 then
    begin
      Result := Path[0] = '/';
      Exit;
    end;
  end;
end;

procedure DispatchBrokerRequest(const Request: TMistixIPCMessage;
  var Response: TMistixIPCMessage);
var
  Path: array[0..VFS_BROKER_PATH_BYTES - 1] of Char;
  ValidPath: Boolean;
  DataOffset, DataSize, Requested, Available: QWord;
  Slot, I, DirectoryEntryCount: LongWord;
  TempIndex: LongInt;
  Ext2InodeNumber: LongWord;
  Ext2Inode: TExt2UserInode;
  Ext2BytesRead: LongWord;
  Dest, Source: PByte;
  Flags: LongWord;
  DirectoryPosition, EntryBytes: QWord;
begin
  ClearMessage(Response);
  Response.MessageId := VFS_ERR_INVAL;
  Response.WordCount := 4;
  Response.Words[0] := Request.Words[0]; { transaction ID }
  Response.Words[1] := Request.Words[1]; { requester PID }
  Response.Words[2] := 0;
  Response.Words[3] := 0; { VFS broker server generation }
  if Request.MessageId = VFS10_CANCEL then
  begin
    if (Request.WordCount < 4) or (Request.Words[0] = 0) or
       (Request.Words[2] = 0) or
       (Request.Words[3] >= VFS_BROKER_PENDING_MAX) then
    begin
      Response.MessageId := VFS_ERR_PROTO;
      Exit;
    end;
    Response.MessageId := VFS10_CANCEL;
    Response.Words[2] := 1; { cancel notification observed; late replies remain harmless }
    Response.Words[3] := Request.Words[2];
    Exit;
  end;
  if Request.Words[0] <> 0 then
  begin
    if Request.WordCount < 6 then
    begin
      Response.MessageId := VFS_ERR_PROTO;
      Exit;
    end;
    Response.Words[3] := Request.Words[5];
  end;
  case Request.MessageId of
    VFS_RPC_BROKER_OPEN:
      begin
        if Request.WordCount < 4 then Exit;
        if not CopySharedBrokerPath(Path) then
        begin
          Response.MessageId := VFS_ERR_INVAL;
          Exit;
        end;
        Flags := LongWord(Request.Words[2]);
        if (Request.Words[2] > High(LongWord)) or
           ((Flags and not LongWord(VFS_O_WRONLY or VFS_O_RDWR or VFS_O_CREAT)) <> 0) or
           ((Flags and 3) = 3) then
        begin
          Response.MessageId := VFS_ERR_INVAL;
          Exit;
        end;
        TempIndex := TempFind(Path);
        if (TempIndex < 0) and ((Flags and VFS_O_CREAT) <> 0) and
           TempPathAllowed(Path) then
          TempIndex := TempCreate(Path);
        Ext2InodeNumber := 0;
        DataOffset := 0;
        DataSize := 0;
        if TempIndex >= 0 then
        begin
          DataSize := TempFiles[TempIndex].Size;
          if ((Flags and 3) <> VFS_O_RDONLY) and not TempPathAllowed(Path) then
          begin
            Response.MessageId := VFS_ERR_ROFS;
            Exit;
          end;
        end
        else if Ext2UPathIsMounted(Path) then
        begin
          if not Ext2Mounted then
          begin
            Response.MessageId := QWord(-19);
            Exit;
          end;
          if not Ext2ULookupPath(Path, Ext2InodeNumber, Ext2Inode) or
             ((Ext2Inode.Mode and EXT2_U_MODE_MASK) <> EXT2_U_MODE_REGULAR) then
          begin
            Response.MessageId := VFS_ERR_NOENT;
            Exit;
          end;
          if (Flags and 3) <> VFS_O_RDONLY then
          begin
            Response.MessageId := VFS_ERR_ROFS;
            Exit;
          end;
          DataOffset := Ext2InodeNumber;
          DataSize := Ext2Inode.Size;
        end
        else if not ArchiveFind(Path, DataOffset, DataSize) then
        begin
          Response.MessageId := VFS_ERR_NOENT;
          Exit;
        end
        else if (Flags and 3) <> VFS_O_RDONLY then
        begin
          Response.MessageId := VFS_ERR_ROFS;
          Exit;
        end;
        if TempIndex >= 0 then
          Response.Words[2] := AllocateOpen(0, DataSize, 1, TempIndex, Flags)
        else if Ext2InodeNumber <> 0 then
          Response.Words[2] := AllocateOpen(DataOffset, DataSize, 2, -1, Flags)
        else
          Response.Words[2] := AllocateOpen(DataOffset, DataSize, 0, -1, Flags);
        if IsError(Response.Words[2]) then
          Response.MessageId := Response.Words[2]
        else
          Response.MessageId := 0;
      end;

    VFS_RPC_BROKER_READ:
      begin
        if Request.WordCount < 4 then Exit;
        if not LookupOpen(Request.Words[2], Slot) then
        begin
          Response.MessageId := VFS_ERR_BADF;
          Exit;
        end;
        if (OpenSlots[Slot].Flags and 3) = VFS_O_WRONLY then
        begin
          Response.MessageId := VFS_ERR_BADF;
          Exit;
        end;
        Requested := Request.Words[3];
        if Requested > VFS_TEMP_FILE_MAX then Requested := VFS_TEMP_FILE_MAX;
        if OpenSlots[Slot].Position >= OpenSlots[Slot].DataSize then
          Available := 0
        else
          Available := OpenSlots[Slot].DataSize - OpenSlots[Slot].Position;
        if Requested > Available then Requested := Available;
        if Requested <> 0 then
        begin
          Dest := SharedBuffer;
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
          else if OpenSlots[Slot].SourceKind = 2 then
          begin
            if not Ext2UReadFile(LongWord(OpenSlots[Slot].DataOffset),
                 OpenSlots[Slot].Position, @Ext2FileScratch[0],
                 LongWord(Requested), Ext2BytesRead) then
            begin
              Response.MessageId := VFS_ERR_IO;
              Exit;
            end;
            Requested := Ext2BytesRead;
            for I := 0 to LongWord(Requested) - 1 do
              SharedBuffer[I] := Ext2FileScratch[I];
          end
          else
            for I := 0 to LongWord(Requested) - 1 do
              Dest[I] := ByteAt(ImageBase,
                OpenSlots[Slot].DataOffset + OpenSlots[Slot].Position + I);
        end;
        Inc(OpenSlots[Slot].Position, Requested);
        Response.MessageId := 0;
        Response.Words[2] := Requested;
      end;

    VFS10_DIR_OPEN:
      begin
        if Request.WordCount < 4 then Exit;
        if not CopySharedBrokerPath(Path) then
        begin
          Response.MessageId := VFS_ERR_INVAL;
          Exit;
        end;
        if (Request.Words[2] > High(LongWord)) or
           ((LongWord(Request.Words[2]) and not LongWord(VFS_O_DIRECTORY)) <> 0) or
           ((LongWord(Request.Words[2]) and 3) <> VFS_O_RDONLY) or
           ((LongWord(Request.Words[2]) and VFS_O_DIRECTORY) = 0) then
        begin
          Response.MessageId := VFS_ERR_INVAL;
          Exit;
        end;
        if not Ext2UPathIsMounted(Path) then
        begin
          Response.MessageId := VFS_ERR_NOENT;
          Exit;
        end;
        if not Ext2Mounted then
        begin
          Response.MessageId := QWord(-19);
          Exit;
        end;
        if not Ext2ULookupPath(Path, Ext2InodeNumber, Ext2Inode) then
        begin
          Response.MessageId := VFS_ERR_NOENT;
          Exit;
        end;
        if (Ext2Inode.Mode and EXT2_U_MODE_MASK) <> EXT2_U_MODE_DIRECTORY then
        begin
          Response.MessageId := VFS_ERR_NOTDIR;
          Exit;
        end;
        Response.Words[2] := AllocateOpen(0, 0, 3, -1, VFS_O_RDONLY);
        if IsError(Response.Words[2]) then
          Response.MessageId := Response.Words[2]
        else
        begin
          Slot := LongWord(Response.Words[2]) and $FFFF;
          Dec(Slot);
          for I := 0 to VFS_BROKER_PATH_BYTES - 1 do
            OpenSlots[Slot].Path[I] := Path[I];
          Response.MessageId := 0;
        end;
      end;

    VFS10_DIR_READ:
      begin
        if Request.WordCount < 4 then Exit;
        if not LookupOpen(Request.Words[2], Slot) then
        begin
          Response.MessageId := VFS_ERR_BADF;
          Exit;
        end;
        if OpenSlots[Slot].SourceKind <> 3 then
        begin
          Response.MessageId := VFS_ERR_NOTDIR;
          Exit;
        end;
        if not Ext2UReadDirectory(OpenSlots[Slot].Path,
             DirectoryEntryCount) then
        begin
          Response.MessageId := VFS_ERR_IO;
          Exit;
        end;
        Requested := Request.Words[3];
        if Requested > 16 then Requested := 16;
        DirectoryPosition := OpenSlots[Slot].Position;
        if DirectoryPosition > DirectoryEntryCount then
        begin
          Response.MessageId := VFS_ERR_PROTO;
          Exit;
        end;
        Available := DirectoryEntryCount - DirectoryPosition;
        if Requested > Available then Requested := Available;
        if Requested <> 0 then
        begin
          EntryBytes := Requested * SizeOf(TMistixDirent);
          I := LongWord(EntryBytes);
          while I <> 0 do
          begin
            Dec(I);
            SharedBuffer[I] := SharedBuffer[
              DirectoryPosition * SizeOf(TMistixDirent) + I];
          end;
        end;
        Inc(OpenSlots[Slot].Position, Requested);
        Response.MessageId := 0;
        Response.Words[2] := Requested;
      end;

    VFS10_CHDIR:
      begin
        if Request.WordCount < 4 then Exit;
        if not CopySharedBrokerPath(Path) then
        begin
          Response.MessageId := VFS_ERR_INVAL;
          Exit;
        end;
        if not Ext2UPathIsMounted(Path) then
        begin
          Response.MessageId := VFS_ERR_NOENT;
          Exit;
        end;
        if not Ext2Mounted then
        begin
          Response.MessageId := QWord(-19);
          Exit;
        end;
        if not Ext2ULookupPath(Path, Ext2InodeNumber, Ext2Inode) then
        begin
          Response.MessageId := VFS_ERR_NOENT;
          Exit;
        end;
        if (Ext2Inode.Mode and EXT2_U_MODE_MASK) <> EXT2_U_MODE_DIRECTORY then
        begin
          Response.MessageId := VFS_ERR_NOTDIR;
          Exit;
        end;
        Response.MessageId := 0;
        Response.Words[2] := 0;
      end;

    VFS_RPC_BROKER_WRITE:
      begin
        if Request.WordCount < 4 then Exit;
        if not LookupOpen(Request.Words[2], Slot) then
        begin
          Response.MessageId := VFS_ERR_BADF;
          Exit;
        end;
        if (OpenSlots[Slot].Flags and 3) = VFS_O_RDONLY then
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
        Requested := Request.Words[3];
        if Requested > VFS_RPC_IO_MAX then Requested := VFS_RPC_IO_MAX;
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
        Response.Words[2] := Requested;
      end;

    VFS_RPC_BROKER_CLOSE:
      begin
        if Request.WordCount < 3 then Exit;
        if not LookupOpen(Request.Words[2], Slot) then
        begin
          Response.MessageId := VFS_ERR_BADF;
          Exit;
        end;
        OpenSlots[Slot].Active := 0;
        OpenSlots[Slot].DataOffset := 0;
        OpenSlots[Slot].DataSize := 0;
        OpenSlots[Slot].Position := 0;
        OpenSlots[Slot].TempIndex := -1;
        OpenSlots[Slot].Flags := 0;
        for I := 0 to VFS_BROKER_PATH_BYTES - 1 do
          OpenSlots[Slot].Path[I] := #0;
        Response.MessageId := 0;
      end;
  else
    Response.MessageId := VFS_ERR_PROTO;
  end;
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
  Ext2InodeNumber: LongWord;
  Ext2Inode: TExt2UserInode;
  Ext2BytesRead: LongWord;
  DirectoryEntryCount: LongWord;
begin
  if ((Request.MessageId >= VFS_RPC_BROKER_OPEN) and
      (Request.MessageId <= VFS_RPC_BROKER_CLOSE)) or
     ((Request.MessageId >= VFS10_DIR_OPEN) and
      (Request.MessageId <= VFS10_CHDIR)) or
     (Request.MessageId = VFS10_CANCEL) then
  begin
    DispatchBrokerRequest(Request, Response);
    Exit;
  end;
  ClearMessage(Response);
  Response.MessageId := VFS_ERR_INVAL;
  case Request.MessageId of
    VFS_RPC_MOUNT_EXT2:
      begin
        if Ext2UMount then
        begin
          Response.MessageId := 0;
          Response.WordCount := 3;
          Response.Words[0] := Ext2BlockSize;
          Response.Words[1] := Ext2BlocksCount;
          Response.Words[2] := Ext2InodesCount;
        end
        else if Ext2DiskBytes = 0 then
          Response.MessageId := QWord(-19)
        else
          Response.MessageId := QWord(-22);
      end;

    VFS_RPC_PING:
      begin
        Response.MessageId := 0;
        Response.WordCount := 3;
        Response.Words[0] := 3; { RPC protocol version }
        Response.Words[1] := ReadLE32(ImageBase, 8);
        Response.Words[2] := VFS_TEMP_MAX;
      end;

    VFS_RPC_READDIR:
      begin
        if Request.WordCount < 5 then Exit;
        CopyRequestPath(Request, Path, ValidPath);
        if not ValidPath then Exit;
        if not Ext2UPathIsMounted(Path) then
        begin
          Response.MessageId := VFS_ERR_NOENT;
          Exit;
        end;
        if not Ext2Mounted then
        begin
          Response.MessageId := QWord(-19);
          Exit;
        end;
        if not Ext2UReadDirectory(Path, DirectoryEntryCount) then
        begin
          Response.MessageId := VFS_ERR_NOTDIR;
          Exit;
        end;
        Response.MessageId := 0;
        Response.WordCount := 1;
        Response.Words[0] := DirectoryEntryCount;
      end;

    VFS_RPC_OPEN, VFS_RPC_STAT:
      begin
        if Request.WordCount < 5 then Exit;
        CopyRequestPath(Request, Path, ValidPath);
        if not ValidPath then Exit;
        TempIndex := TempFind(Path);
        Ext2InodeNumber := 0;
        if TempIndex >= 0 then
        begin
          DataOffset := 0;
          DataSize := TempFiles[TempIndex].Size;
        end
        else if Ext2UPathIsMounted(Path) then
        begin
          if not Ext2Mounted then
          begin
            Response.MessageId := QWord(-19); { -ENODEV: not mounted }
            Exit;
          end;
          if not Ext2ULookupPath(Path, Ext2InodeNumber, Ext2Inode) or
             ((Ext2Inode.Mode and EXT2_U_MODE_MASK) <> EXT2_U_MODE_REGULAR) then
          begin
            Response.MessageId := VFS_ERR_NOENT;
            Exit;
          end;
          DataOffset := Ext2InodeNumber;
          DataSize := Ext2Inode.Size;
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
            Response.Words[0] := AllocateOpen(0, DataSize, 1, TempIndex, VFS_O_RDWR)
          else if Ext2InodeNumber <> 0 then
            Response.Words[0] := AllocateOpen(DataOffset, DataSize, 2, -1, VFS_O_RDONLY)
          else
            Response.Words[0] := AllocateOpen(DataOffset, DataSize, 0, -1, VFS_O_RDONLY);
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
          else if OpenSlots[Slot].SourceKind = 2 then
          begin
            if not Ext2UReadFile(LongWord(OpenSlots[Slot].DataOffset),
                 OpenSlots[Slot].Position, @Ext2FileScratch[0],
                 LongWord(Requested), Ext2BytesRead) then
            begin
              Response.MessageId := VFS_ERR_IO;
              Exit;
            end;
            Requested := Ext2BytesRead;
            for I := 0 to LongWord(Requested) - 1 do
              SharedBuffer[I] := Ext2FileScratch[I];
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
  BlockSendHandle, BlockReceiveHandle: QWord;
begin
  Result := False;
  if not ReceiveOneCapability(FirstHandle) then Exit;
  if not ReceiveOneCapability(SecondHandle) then Exit;
  if not ReceiveOneCapability(SharedHandle) then Exit;
  if not ReceiveOneCapability(BlockSendHandle) then Exit;
  if not ReceiveOneCapability(BlockReceiveHandle) then Exit;
  RequestEndpoint := FirstHandle;
  ReplyEndpoint := SecondHandle;
  BlockRequestEndpoint := BlockSendHandle;
  BlockReplyEndpoint := BlockReceiveHandle;
  if IsError(SysShmMap(SharedHandle, VFS_SHARED_BASE, 1)) then Exit;
  SharedBuffer := PByte(PtrUInt(VFS_SHARED_BASE));
  Result := True;
end;

function VFSBrokerStageRequest(const Request: TMistixIPCMessage): Boolean;
var
  SlotIndex, I: LongWord;
  BufferOffset: QWord;
begin
  Result := False;
  if Request.MessageId = VFS10_CANCEL then
  begin
    Result := True;
    Exit;
  end;
  if (Request.WordCount < 6) or
     (Request.Words[4] >= VFS_BROKER_PENDING_MAX) or
     (Request.Words[5] = 0) then Exit;
  SlotIndex := LongWord(Request.Words[4]);
  BufferOffset := QWord(SlotIndex) * VFS_BROKER_SLOT_SIZE;
  { The broker owns one 8 KiB lane per outstanding transaction. Stage that
    lane into the legacy base window only while this single-threaded server
    dispatches the request. blockd continues to use the base window. }
  if SlotIndex <> 0 then
    for I := 0 to VFS_BROKER_SLOT_SIZE - 1 do
      SharedBuffer[I] := SharedBuffer[BufferOffset + I];
  Result := True;
end;

procedure VFSBrokerSaveResponse(const Request, Response: TMistixIPCMessage);
var
  SlotIndex, I: LongWord;
  BufferOffset, CopyBytes: QWord;
begin
  if (Request.MessageId = VFS10_CANCEL) or (Request.Words[0] = 0) or
     (Response.MessageId <> 0) or (Request.WordCount < 6) or
     (Request.Words[4] >= VFS_BROKER_PENDING_MAX) then Exit;
  SlotIndex := LongWord(Request.Words[4]);
  BufferOffset := QWord(SlotIndex) * VFS_BROKER_SLOT_SIZE;
  CopyBytes := 0;
  if Request.MessageId = VFS_RPC_BROKER_READ then
  begin
    CopyBytes := Response.Words[2];
    if CopyBytes > VFS_RPC_IO_MAX then Exit;
  end
  else if Request.MessageId = VFS10_DIR_READ then
  begin
    if Response.Words[2] > 16 then Exit;
    CopyBytes := Response.Words[2] * SizeOf(TMistixDirent);
  end
  else
    Exit;
  if CopyBytes > VFS_BROKER_SLOT_SIZE then Exit;
  for I := 0 to LongWord(CopyBytes) - 1 do
    SharedBuffer[BufferOffset + I] := SharedBuffer[I];
end;

procedure MistixVFSdMain; cdecl; public name 'mistix_user_main';
var
  Request, Response: TMistixIPCMessage;
  Status: QWord;
  IsBrokerTransaction: Boolean;
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
  Ext2Mounted := False;
  WriteCString('vfsd: userland VFS online (MFS1 + /tmp + Ext2 read-only mount over blockd RPC)'#13#10);
  while True do
  begin
    Status := SysIPCReceive(RequestEndpoint, PtrUInt(@Request));
    if IsError(Status) then
    begin
      SysYield;
      Continue;
    end;
    IsBrokerTransaction :=
      (Request.Words[0] <> 0) and
      (((Request.MessageId >= VFS_RPC_BROKER_OPEN) and
        (Request.MessageId <= VFS_RPC_BROKER_CLOSE)) or
       ((Request.MessageId >= VFS10_DIR_OPEN) and
        (Request.MessageId <= VFS10_CHDIR)));
    if IsBrokerTransaction then
    begin
      if VFSBrokerStageRequest(Request) then
      begin
        DispatchRequest(Request, Response);
        VFSBrokerSaveResponse(Request, Response);
      end
      else
      begin
        ClearMessage(Response);
        Response.MessageId := VFS_ERR_PROTO;
        Response.WordCount := 4;
        Response.Words[0] := Request.Words[0];
        Response.Words[1] := Request.Words[1];
        Response.Words[2] := 0;
        if Request.WordCount >= 6 then
          Response.Words[3] := Request.Words[5];
      end;
    end
    else
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
