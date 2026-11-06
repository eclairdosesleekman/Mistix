function ValidateBootInformation: Boolean;
var
  Memmap: PLimineMemmapResponse;
  HHDM: PLimineHHDMResponse;
  Paging: PLiminePagingModeResponse;
begin
  Result := False;
  if LimineRequests.BaseRevision[2] <> 0 then
  begin
    WriteText('[FAIL] Limine base revision 6 was not accepted.'#13#10);
    Exit;
  end;
  WriteText('[ OK ] Limine base revision 6 accepted.'#13#10);

  Paging := LimineRequests.PagingModeRequest.Response;
  if (Paging = nil) or
     (Paging^.Mode <> LIMINE_PAGING_MODE_X86_64_4LVL) then
  begin
    WriteText('[FAIL] Mistix requires Limine x86-64 four-level paging.'#13#10);
    Exit;
  end;
  WriteText('[ OK ] Limine selected x86-64 4-level paging.'#13#10);

  HHDM := LimineRequests.HHDMRequest.Response;
  if HHDM = nil then
  begin
    WriteText('[FAIL] Limine did not return an HHDM response.'#13#10);
    Exit;
  end;
  WriteText('HHDM offset: ');
  WriteHex64(HHDM^.Offset);
  WriteText(#13#10);

  Memmap := LimineRequests.MemmapRequest.Response;
  if (Memmap = nil) or (Memmap^.EntryCount > LIMINE_MAX_MEMMAP_ENTRIES) or
     ((Memmap^.EntryCount <> 0) and (Memmap^.Entries = nil)) then
  begin
    WriteText('[FAIL] Limine memory-map structure failed validation.'#13#10);
    Exit;
  end;
  WriteText('Memory map entries: ');
  WriteDecimal(Memmap^.EntryCount);
  WriteText(#13#10);
  Result := True;
end;

procedure GetEmbeddedInitFS(var ImageStart, ImageEnd: Pointer);
begin
  ImageStart := @EmbeddedInitFSStart;
  ImageEnd := @EmbeddedInitFSEnd;
end;

procedure KernelMain; cdecl; public name 'mistix_kernel_main';
var
  Memmap: PLimineMemmapResponse;
  HHDM: PLimineHHDMResponse;
  IPCTestOK: Boolean;
  NotificationTestOK: Boolean;
  MemoryObjectTestOK: Boolean;
  ProcessCapTestOK: Boolean;
  VFSTestOK: Boolean;
  UserImageStart: Pointer;
  UserImageEnd: Pointer;
  UserImageSize: QWord;
  InitFSStart: Pointer;
  InitFSEnd: Pointer;
  InitialPID: QWord;
begin
  SerialInit;
  WriteText(#13#10'============================================================'#13#10);
  WriteText(' Mistix - Phase 5: UNIX foundation / Pascal userland        '#13#10);
  WriteText(' Pascal freestanding kernel / VFS / pipes / shell           '#13#10);
  WriteText('============================================================'#13#10);

  if not ValidateBootInformation then
  begin
    WriteText('[FAIL] Boot information validation failed.'#13#10);
    HaltForever;
  end;

  CPUInitialize;
  Memmap := LimineRequests.MemmapRequest.Response;
  HHDM := LimineRequests.HHDMRequest.Response;
  if not MemoryInitialize(Memmap, HHDM^.Offset) then
  begin
    WriteText('[FAIL] Memory subsystem initialisation failed.'#13#10);
    HaltForever;
  end;

  ThreadsInitialize;
  CapabilityInitialize;
  IPCInitialize;
  NotificationInitialize;
  MemoryObjectInitialize;
  COWInitialize;
  ProcessManagerInitialize;
  GetEmbeddedInitFS(InitFSStart, InitFSEnd);
  VFSInitialize(InitFSStart, InitFSEnd);
  VFSTestOK := VFSSelfTest;
  if not VFSTestOK then
  begin
    WriteText('[FAIL] VFS self-test failed.'#13#10);
    HaltForever;
  end;

  WriteText(#13#10'[PHASE 5] UNIX foundation initialised.'#13#10);
  IPCTestOK := IPCRendezvousSelfTest;
  NotificationTestOK := NotificationSelfTest;
  MemoryObjectTestOK := MemoryObjectAliasSelfTest;
  ProcessCapTestOK := ProcessCapabilityIsolationSelfTest;
  if not IPCTestOK or not NotificationTestOK or
     not MemoryObjectTestOK or not ProcessCapTestOK then
  begin
    WriteText('[FAIL] A kernel subsystem self-test failed.'#13#10);
    HaltForever;
  end;

  GetEmbeddedInitImage(UserImageStart, UserImageEnd);
  if (UserImageStart = nil) or
     (PtrUInt(UserImageEnd) <= PtrUInt(UserImageStart)) then
  begin
    WriteText('[FAIL] Embedded Pascal user ELF image is missing.'#13#10);
    HaltForever;
  end;
  UserImageSize := QWord(PtrUInt(UserImageEnd) - PtrUInt(UserImageStart));
  WriteText('[ OK ] Embedded Pascal init image size=');
  WriteDecimal(UserImageSize);
  WriteText(' bytes.'#13#10);

  if not ProcessCreateFromELF(UserImageStart, UserImageSize, 0, InitialPID) then
  begin
    WriteText('[FAIL] Could not create initial user process.'#13#10);
    HaltForever;
  end;

  SyscallInitialize;
  TimerInitialize;
  WriteText(#13#10'[PASS] Phase 5 VFS, InitFS, DevFS, descriptors, pipes and Pascal userland initialised.'#13#10);
  WriteText('[NOTE] VFS is a bootstrap in-kernel core; move to userland service later.'#13#10);
  WriteText('[NOTE] Single CPU; legacy PIC/PIT; raw serial TTY; no SMP yet.'#13#10);
  WriteText('[NOTE] User syscalls: process/IPC, open/read/write/close, dup2, pipe,'#13#10);
  WriteText('       getdents, chdir, getcwd, spawn_path and getcmdline.'#13#10);

  ProcessLaunch(InitialPID);
  HaltForever;
end;

procedure MistixEntry; assembler; nostackframe; public name '_start';
asm
  cli
  cld
  xor rbp, rbp
  and rsp, -16
  call mistix_kernel_main
@entry_halt:
  cli
  hlt
  jmp @entry_halt
end;
