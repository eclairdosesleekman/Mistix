{ Unified interrupt/syscall/exception return path. The dispatcher may return
  a frame on another process's kernel stack; ISRCommon restores that frame. }

function TrapDispatch(Frame: PInterruptFrame): QWord; cdecl;
  public name 'mistix_trap_dispatch';
var
  Vector: QWord;
  FaultAddress: QWord;
  ExitCode: QWord;
begin
  Result := PtrUInt(Frame);
  if Frame = nil then
  begin
    WriteText('[FATAL] Null interrupt frame.'#13#10);
    HaltForever;
  end;
  Vector := Frame^.Vector;

  if Vector = 32 then
  begin
    TimerPicAcknowledge;
    Inc(TimerTickCount);
    Inc(VFSBrokerTickCounter);
    if ((Frame^.CS and 3) = 3) and
       (CurrentProcessIndex < PROCESS_MAX_COUNT) and
       ((TimerTickCount mod 3) = 0) then
      Result := ProcessScheduleFromFrame(Frame);
    Exit;
  end;

  if Vector = 128 then
  begin
    Result := SyscallDispatchFrame(Frame);
    Exit;
  end;

  if Vector < 32 then
  begin
    if (Frame^.CS and 3) = 3 then
    begin
      if (Vector = 14) and (CurrentProcessIndex < PROCESS_MAX_COUNT) then
      begin
        FaultAddress := ReadCR2;
        if COWHandlePageFault(ProcessTable[CurrentProcessIndex].AddressSpaceRoot,
             FaultAddress, Frame^.ErrorCode) then
          Exit;
      end;
      ExitCode := QWord($8000000000000000) or (Vector shl 32) or
                  (Frame^.ErrorCode and $FFFFFFFF);
      WriteText('[PROCESS] user exception vector=');
      WriteDecimal(Vector);
      WriteText(' error=');
      WriteHex64(Frame^.ErrorCode);
      if Vector = 14 then
      begin
        WriteText(' cr2=');
        WriteHex64(ReadCR2);
      end;
      WriteText(#13#10);
      ProcessTerminateCurrent(Frame, ExitCode);
      Result := ProcessScheduleFromFrame(Frame);
      Exit;
    end;

    WriteText(#13#10'[FATAL] Kernel exception vector=');
    WriteDecimal(Vector);
    WriteText(' error=');
    WriteHex64(Frame^.ErrorCode);
    WriteText(' rip=');
    WriteHex64(Frame^.RIP);
    if Vector = 14 then
    begin
      WriteText(' cr2=');
      WriteHex64(ReadCR2);
    end;
    WriteText(#13#10);
    HaltForever;
  end;

  { A vector not installed by Mistix is treated as a fatal kernel event. }
  WriteText('[FATAL] Unexpected interrupt vector=');
  WriteDecimal(Vector);
  WriteText(#13#10);
  HaltForever;
end;
