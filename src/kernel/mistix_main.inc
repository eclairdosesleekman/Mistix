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

procedure KernelMain; cdecl; public name 'mistix_kernel_main';
var
  Memmap: PLimineMemmapResponse;
  HHDM: PLimineHHDMResponse;
  IPCTestOK: Boolean;
  NotificationTestOK: Boolean;
  MemoryObjectTestOK: Boolean;
  ProcessCapTestOK: Boolean;
  UserImageStart: Pointer;
  UserImageEnd: Pointer;
  UserImageSize: QWord;
  InitialPID: QWord;
begin
  SerialInit;
  WriteText(#13#10'============================================================'#13#10);
  WriteText(' Mistix - Phase 4: preemptive multi-process microkernel      '#13#10);
  WriteText(' Pascal freestanding kernel / x86-64 / COW / user IPC       '#13#10);
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

  WriteText(#13#10'[PHASE 4] Kernel service self-tests.'#13#10);
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
  WriteText(#13#10'[PASS] Phase 4 process scheduler, PIT preemption, fork/COW,'#13#10);
  WriteText('       spawn/wait, per-process capabilities and user IPC ready.'#13#10);
  WriteText('[NOTE] Single CPU; legacy 8259 PIC/PIT; no SMP or APIC yet.'#13#10);
  WriteText('[NOTE] User syscalls: write, exit, getpid, yield, fork, spawn,'#13#10);
  WriteText('       waitpid, endpoint_create, ipc_send/receive, cap_transfer.'#13#10);

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
