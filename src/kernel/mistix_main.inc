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

  if LimineRequests.StackSizeRequest.Response <> nil then
    WriteText('[ OK ] 256 KiB boot stack request acknowledged.'#13#10)
  else
    WriteText('[WARN] Stack-size response missing; protocol default is in use.'#13#10);

  Paging := LimineRequests.PagingModeRequest.Response;
  if Paging = nil then
  begin
    WriteText('[FAIL] Limine did not return the paging-mode response.'#13#10);
    Exit;
  end;
  if Paging^.Mode <> LIMINE_PAGING_MODE_X86_64_4LVL then
  begin
    WriteText('[FAIL] Mistix requires 4-level paging; actual mode=');
    WriteDecimal(Paging^.Mode);
    WriteText(#13#10);
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
  if Memmap = nil then
  begin
    WriteText('[FAIL] Limine did not return a memory map.'#13#10);
    Exit;
  end;
  if (Memmap^.EntryCount > LIMINE_MAX_MEMMAP_ENTRIES) or
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
  WriteText(#13#10'=================================================='#13#10);
  WriteText(' Mistix - Phase 3: user processes and syscall ABI  '#13#10);
  WriteText(' Pascal freestanding microkernel / x86-64          '#13#10);
  WriteText('=================================================='#13#10);

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
  ProcessManagerInitialize;

  WriteText(#13#10'[PHASE 3] Re-running kernel IPC and memory self-tests.'#13#10);
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
  WriteText('[ OK ] Embedded Pascal user ELF image size=');
  WriteDecimal(UserImageSize);
  WriteText(' bytes.'#13#10);

  if not ProcessCreateFromELF(UserImageStart, UserImageSize, InitialPID) then
  begin
    WriteText('[FAIL] Could not create initial user process.'#13#10);
    HaltForever;
  end;

  SyscallInitialize;
  WriteText(#13#10'[PASS] Phase 3 kernel address spaces, ELF loader, per-process');
  WriteText(#13#10'       capabilities and ring-3 syscall entry are initialised.'#13#10);
  WriteText('[NOTE] Single foreground process; no timer preemption or SMP yet.'#13#10);
  WriteText('[NOTE] User syscalls: write(1/2), exit, getpid, yield.'#13#10);

  ProcessRun(InitialPID);
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
