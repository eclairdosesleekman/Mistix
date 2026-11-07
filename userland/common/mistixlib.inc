{$ifdef MISTIX_DEBUG_CHECKS}
{$R+}
{$Q+}
{$else}
{$R-}
{$Q-}
{$endif}

{ Minimal Pascal-only syscall ABI; Phase 9 retains restricted block-service calls. }
const
  MISTIX_ERRNO_BASE = QWord($FFFFFFFFFFFFF000);
  MISTIX_EAGAIN = QWord($FFFFFFFFFFFFFFF5);
  MISTIX_O_RDONLY = 0;
  MISTIX_O_WRONLY = 1;
  MISTIX_O_RDWR = 2;
  MISTIX_O_CREAT = $40;
  MISTIX_O_DIRECTORY = $100;
  MISTIX_SEEK_SET = 0;
  MISTIX_SEEK_CUR = 1;
  MISTIX_SEEK_END = 2;
  MISTIX_SYSCALL_WRITE = 1;
  MISTIX_SYSCALL_EXIT = 2;
  MISTIX_SYSCALL_GETPID = 3;
  MISTIX_SYSCALL_YIELD = 4;
  MISTIX_SYSCALL_WAITPID = 7;
  MISTIX_SYSCALL_OPEN = 15;
  MISTIX_SYSCALL_READ = 16;
  MISTIX_SYSCALL_CLOSE = 17;
  MISTIX_SYSCALL_DUP2 = 18;
  MISTIX_SYSCALL_PIPE = 19;
  MISTIX_SYSCALL_GETDENTS = 20;
  MISTIX_SYSCALL_CHDIR = 21;
  MISTIX_SYSCALL_GETCWD = 22;
  MISTIX_SYSCALL_SPAWN_PATH = 23;
  MISTIX_SYSCALL_GETCMDLINE = 24;
  MISTIX_SYSCALL_ENDPOINT_CREATE = 8;
  MISTIX_SYSCALL_IPC_SEND = 9;
  MISTIX_SYSCALL_IPC_RECEIVE = 10;
  MISTIX_SYSCALL_CAP_TRANSFER = 11;
  MISTIX_SYSCALL_CAP_RECEIVE = 12;
  MISTIX_SYSCALL_CAP_CLOSE = 13;
  MISTIX_CAP_SEND = $00000001;
  MISTIX_CAP_RECEIVE = $00000002;
  MISTIX_CAP_MAP = $00000010;
  MISTIX_CAP_READ = $00000020;
  MISTIX_CAP_WRITE = $00000040;
  MISTIX_CAP_USER_MAP = $00000100;
  MISTIX_CAP_TRANSFER = $00000400;
  MISTIX_SYSCALL_SHM_CREATE = 25;
  MISTIX_SYSCALL_SHM_MAP = 26;
  MISTIX_SYSCALL_SHM_UNMAP = 27;
  MISTIX_SYSCALL_BLOCK_REGISTER = 28;
  MISTIX_SYSCALL_BLOCK_INFO = 29;
  MISTIX_SYSCALL_BLOCK_READ = 30;
  MISTIX_SYSCALL_VFS_REGISTER = 31;
  MISTIX_SYSCALL_LSEEK = 32;
  MISTIX_SHM_DEFAULT_BASE = QWord($0000500000000000);

type
  TMistixIPCMessage = packed record
    MessageId: QWord;
    WordCount: Byte;
    Reserved: array[0..6] of Byte;
    Words: array[0..5] of QWord;
  end;

  TMistixBlockDeviceInfo = packed record
    SectorCount: QWord;
    SectorSize: LongWord;
    Flags: LongWord;
  end;

  TMistixDirent = packed record
    Inode: QWord;
    EntryType: Byte;
    Name: array[0..63] of Char;
    Size: QWord;
  end;

function SysWriteRaw(FD, Buffer, Count: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_WRITE
  int $80
end;

function SysReadRaw(FD, Buffer, Count: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_READ
  int $80
end;

function SysOpenRaw(Path, Flags: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_OPEN
  int $80
end;

function SysLseekRaw(FD: QWord; Offset: Int64; Whence: QWord): QWord;
  assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_LSEEK
  int $80
end;
function SysCloseRaw(FD: QWord): QWord; assembler; nostackframe;
asm
  mov rdi, rdi
  mov eax, MISTIX_SYSCALL_CLOSE
  int $80
end;

function SysDup2(OldFD, NewFD: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_DUP2
  int $80
end;

function SysPipe(PairAddress: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_PIPE
  int $80
end;

function SysGetDentsRaw(FD, Buffer, MaxEntries: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_GETDENTS
  int $80
end;

function SysChdirRaw(Path: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_CHDIR
  int $80
end;

function SysGetCWD(Buffer, Capacity: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_GETCWD
  int $80
end;

function SysSpawnPath(Path, Args, InputFD, OutputFD: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_SPAWN_PATH
  int $80
end;

function SysGetCommandLine(Buffer, Capacity: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_GETCMDLINE
  int $80
end;

function SysWaitPID(PID, StatusAddress: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_WAITPID
  int $80
end;

function SysEndpointCreate: QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_ENDPOINT_CREATE
  int $80
end;

function SysIPCSend(Endpoint, MessageAddress: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_IPC_SEND
  int $80
end;

function SysIPCReceive(Endpoint, MessageAddress: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_IPC_RECEIVE
  int $80
end;

function SysCapabilityTransfer(TargetPID, Handle, Rights: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_CAP_TRANSFER
  int $80
end;

function SysCapabilityReceive: QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_CAP_RECEIVE
  int $80
end;

function SysCapabilityClose(Handle: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_CAP_CLOSE
  int $80
end;

function SysVFSRegister(ServerPID, RequestHandle, ReplyHandle: QWord): QWord;
  assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_VFS_REGISTER
  int $80
end;

function SysShmCreate(PageCount: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_SHM_CREATE
  int $80
end;

function SysShmMap(Handle, BaseAddress, Writable: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_SHM_MAP
  int $80
end;

function SysBlockRegister: QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_BLOCK_REGISTER
  int $80
end;

function SysBlockInfo(InfoAddress: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_BLOCK_INFO
  int $80
end;

function SysBlockRead(LBA, SectorCount, BufferAddress: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_BLOCK_READ
  int $80
end;

function SysShmUnmap(Handle, BaseAddress: QWord): QWord; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_SHM_UNMAP
  int $80
end;

procedure SysYield; assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_YIELD
  int $80
end;

function SysWrite(FD, Buffer, Count: QWord): QWord;
var
  Retry: LongWord;
begin
  Retry := 0;
  repeat
    Result := SysWriteRaw(FD, Buffer, Count);
    if Result <> MISTIX_EAGAIN then Exit;
    SysYield;
    Inc(Retry);
  until Retry >= 1000000;
end;

function SysRead(FD, Buffer, Count: QWord): QWord;
var
  Retry: LongWord;
begin
  Retry := 0;
  repeat
    Result := SysReadRaw(FD, Buffer, Count);
    if Result <> MISTIX_EAGAIN then Exit;
    SysYield;
    Inc(Retry);
  until Retry >= 1000000;
end;

function SysOpen(Path, Flags: QWord): QWord;
var
  Retry: LongWord;
begin
  Retry := 0;
  repeat
    Result := SysOpenRaw(Path, Flags);
    if Result <> MISTIX_EAGAIN then Exit;
    SysYield;
    Inc(Retry);
  until Retry >= 1000000;
end;

function SysLseek(FD: QWord; Offset: Int64; Whence: QWord): QWord;
var
  Retry: LongWord;
begin
  Retry := 0;
  repeat
    Result := SysLseekRaw(FD, Offset, Whence);
    if Result <> MISTIX_EAGAIN then Exit;
    SysYield;
    Inc(Retry);
  until Retry >= 1000000;
end;
function SysClose(FD: QWord): QWord;
var
  Retry: LongWord;
begin
  Retry := 0;
  repeat
    Result := SysCloseRaw(FD);
    if Result <> MISTIX_EAGAIN then Exit;
    SysYield;
    Inc(Retry);
  until Retry >= 1000000;
end;

function SysGetDents(FD, Buffer, MaxEntries: QWord): QWord;
var
  Retry: LongWord;
begin
  Retry := 0;
  repeat
    Result := SysGetDentsRaw(FD, Buffer, MaxEntries);
    if Result <> MISTIX_EAGAIN then Exit;
    SysYield;
    Inc(Retry);
  until Retry >= 1000000;
end;

function SysChdir(Path: QWord): QWord;
var
  Retry: LongWord;
begin
  Retry := 0;
  repeat
    Result := SysChdirRaw(Path);
    if Result <> MISTIX_EAGAIN then Exit;
    SysYield;
    Inc(Retry);
  until Retry >= 1000000;
end;

procedure SysExit(Code: QWord); assembler; nostackframe;
asm
  mov eax, MISTIX_SYSCALL_EXIT
  int $80
  ud2
end;

function IsError(Value: QWord): Boolean;
begin
  Result := Value >= MISTIX_ERRNO_BASE;
end;


function SysWriteAll(FD, Buffer, Count: QWord): Boolean;
var
  Offset, Written: QWord;
begin
  Offset := 0;
  Result := False;
  while Offset < Count do
  begin
    Written := SysWrite(FD, Buffer + Offset, Count - Offset);
    if IsError(Written) then Exit;
    if Written = 0 then Exit;
    Inc(Offset, Written);
  end;
  Result := True;
end;

function CStrLength(const S: array of Char): LongWord;
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

procedure WriteCString(const S: array of Char);
var
  L: LongWord;
begin
  L := CStrLength(S);
  if L <> 0 then SysWrite(1, PtrUInt(@S[0]), L);
end;

procedure WriteNumber(Value: QWord);
var
  Digits: array[0..19] of Byte;
  Count: LongWord;
begin
  Count := 0;
  if Value = 0 then
  begin
    Digits[0] := Ord('0');
    Count := 1;
  end
  else
    while (Value <> 0) and (Count < 20) do
    begin
      Digits[Count] := Ord('0') + Byte(Value mod 10);
      Value := Value div 10;
      Inc(Count);
    end;
  while Count <> 0 do
  begin
    Dec(Count);
    SysWrite(1, PtrUInt(@Digits[Count]), 1);
  end;
end;


{ Phase 17: terminate only the faulting user process when checked code fails. }
procedure MistixFPCDivideByZero; cdecl; public name 'FPC_DIVBYZERO';
begin
  SysExit(200);
end;

procedure MistixFPCRangeError; cdecl; public name 'fpc_rangeerror';
begin
  SysExit(201);
end;

{ Keep the documented uppercase spelling as a compatibility alias. }
procedure MistixFPCRangeErrorCompat; cdecl; public name 'FPC_RANGEERROR';
begin
  SysExit(201);
end;

procedure MistixFPCOverflow; cdecl; public name 'FPC_OVERFLOW';
begin
  SysExit(215);
end;
