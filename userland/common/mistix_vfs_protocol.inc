{ Mistix Phase 10 VFS protocol ABI.
  Additive directory/CWD and asynchronous transaction definitions. }

const
  MISTIX_VFS10_ABI_VERSION = 1;
  MISTIX_VFS10_REQUEST_MAX = 32;
  MISTIX_VFS10_NAME_MAX = 63;
  MISTIX_VFS10_PATH_MAX = 127;
  MISTIX_VFS10_DIR_OPEN = QWord($4D56534600000021);
  MISTIX_VFS10_DIR_READ = QWord($4D56534600000022);
  MISTIX_VFS10_DIR_CLOSE = QWord($4D56534600000023);
  MISTIX_VFS10_CHDIR = QWord($4D56534600000024);
  MISTIX_VFS10_GETCWD = QWord($4D56534600000025);
  MISTIX_VFS10_CANCEL = QWord($4D56534600000026);
  MISTIX_VFS10_PING = QWord($4D56534600000027);
  MISTIX_VFS10_KIND_FILE = 1;
  MISTIX_VFS10_KIND_DIR = 2;
  MISTIX_VFS10_KIND_DEV = 3;
  MISTIX_VFS10_STATE_FREE = 0;
  MISTIX_VFS10_STATE_SENT = 1;
  MISTIX_VFS10_STATE_DONE = 2;
  MISTIX_VFS10_STATE_CANCELLED = 3;

type
  TMistixVFS10Dirent = packed record
    Inode: QWord;
    Size: QWord;
    Kind: LongWord;
    NameLength: Word;
    Reserved: Word;
    Name: array[0..MISTIX_VFS10_NAME_MAX] of Char;
  end;
  TMistixVFS10Request = packed record
    Version: LongWord;
    Operation: LongWord;
    RequestID: QWord;
    OwnerPID: QWord;
    Handle: QWord;
    Argument: QWord;
    BufferOffset: LongWord;
    BufferLength: LongWord;
    PathLength: Word;
    Reserved: Word;
    Path: array[0..MISTIX_VFS10_PATH_MAX] of Char;
  end;
  TMistixVFS10Response = packed record
    Version: LongWord;
    State: LongWord;
    RequestID: QWord;
    OwnerPID: QWord;
    ResultValue: QWord;
    BytesTransferred: LongWord;
    Reserved: LongWord;
  end;

function MistixVFS10ValidPath(const Path: array of Char;
  PathLength: LongWord): Boolean;
var I: LongWord;
begin
  Result := False;
  if (PathLength = 0) or (PathLength > MISTIX_VFS10_PATH_MAX) then Exit;
  if Path[0] <> '/' then Exit;
  for I := 0 to PathLength - 1 do
    if (Path[I] = #0) or (Path[I] < ' ') then Exit;
  Result := True;
end;

function MistixVFS10ValidDirent(const Entry: TMistixVFS10Dirent): Boolean;
var I: LongWord;
begin
  Result := False;
  if (Entry.NameLength = 0) or
     (Entry.NameLength > MISTIX_VFS10_NAME_MAX) then Exit;
  if (Entry.Kind < MISTIX_VFS10_KIND_FILE) or
     (Entry.Kind > MISTIX_VFS10_KIND_DEV) then Exit;
  for I := 0 to Entry.NameLength - 1 do
    if (Entry.Name[I] = #0) or (Entry.Name[I] = '/') then Exit;
  Result := Entry.Name[Entry.NameLength] = #0;
end;

function MistixVFS10ResponseMatches(const Request: TMistixVFS10Request;
  const Response: TMistixVFS10Response): Boolean;
begin
  Result := (Request.Version = MISTIX_VFS10_ABI_VERSION) and
    (Response.Version = MISTIX_VFS10_ABI_VERSION) and
    (Request.RequestID <> 0) and
    (Response.RequestID = Request.RequestID) and
    (Response.OwnerPID = Request.OwnerPID);
end;
