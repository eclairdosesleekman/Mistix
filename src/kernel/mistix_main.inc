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
    WriteText('[FAIL] Mistix Phase 1 requires 4-level paging; actual mode=');
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

procedure WorkerAlpha;
begin
  WriteText('[THREAD] alpha: first dispatch.'#13#10);
  ThreadYield;
  WriteText('[THREAD] alpha: resumed after cooperative yield.'#13#10);
end;

procedure WorkerBeta;
begin
  WriteText('[THREAD] beta: first dispatch.'#13#10);
  ThreadYield;
  WriteText('[THREAD] beta: resumed after cooperative yield.'#13#10);
end;

procedure KernelMain; cdecl; public name 'mistix_kernel_main';
var
  Memmap: PLimineMemmapResponse;
  HHDM: PLimineHHDMResponse;
  AlphaId: LongWord;
  BetaId: LongWord;
begin
  SerialInit;
  WriteText(#13#10'=================================================='#13#10);
  WriteText(' Mistix - Phase 1: CPU and memory foundations     '#13#10);
  WriteText(' Pascal freestanding kernel / x86-64              '#13#10);
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
  if not ThreadCreate(@WorkerAlpha, AlphaId) then
  begin
    WriteText('[FAIL] Could not create alpha kernel thread.'#13#10);
    HaltForever;
  end;
  if not ThreadCreate(@WorkerBeta, BetaId) then
  begin
    WriteText('[FAIL] Could not create beta kernel thread.'#13#10);
    HaltForever;
  end;

  WriteText('[ OK ] Created cooperative kernel threads: ');
  WriteDecimal(AlphaId);
  WriteText(' and ');
  WriteDecimal(BetaId);
  WriteText(#13#10);
  ThreadsRunUntilIdle;

  WriteText('[PASS] Phase 1 CPU tables, exception IDT, PMM, VMM and kernel-thread context switching are alive.'#13#10);
  WriteText('[NOTE] Interrupts remain disabled; scheduling is cooperative, not timer-preemptive.'#13#10);
  WriteText('Mistix Phase 1 halted cleanly.'#13#10);
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
