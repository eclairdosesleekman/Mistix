{ Mistix Phase 4 syscall ABI. RAX=number/result, args in RDI/RSI/RDX. }

const
  MISTIX_SYSCALL_WRITE = 1;
  MISTIX_SYSCALL_EXIT = 2;
  MISTIX_SYSCALL_GETPID = 3;
  MISTIX_SYSCALL_YIELD = 4;
  MISTIX_SYSCALL_FORK = 5;
  MISTIX_SYSCALL_SPAWN = 6;
  MISTIX_SYSCALL_WAITPID = 7;
  MISTIX_SYSCALL_ENDPOINT_CREATE = 8;
  MISTIX_SYSCALL_IPC_SEND = 9;
  MISTIX_SYSCALL_IPC_RECEIVE = 10;
  MISTIX_SYSCALL_CAP_TRANSFER = 11;
  MISTIX_SYSCALL_CAP_RECEIVE = 12;
  MISTIX_SYSCALL_CAP_CLOSE = 13;
  MISTIX_SYSCALL_KILL = 14;

  MISTIX_SYSCALL_MAX_WRITE = 4096;
  MISTIX_ERR_BADF = $FFFFFFFFFFFFFFF7;
  MISTIX_ERR_FAULT = $FFFFFFFFFFFFFFF2;
  MISTIX_ERR_INVAL = $FFFFFFFFFFFFFFEA;
  MISTIX_ERR_NOSYS = $FFFFFFFFFFFFFFDA;
  MISTIX_ERR_NOMEM = $FFFFFFFFFFFFFFF4;

procedure SyscallISR; assembler; nostackframe; public name 'mistix_syscall_isr';
asm
  push 0
  push 128
  jmp mistix_isr_common
end;

procedure SyscallInitialize;
begin
  SetIDTGate(128, PtrUInt(@SyscallISR), 0);
  IDT[128].Attributes := $EE; { Present, DPL 3, 64-bit interrupt gate. }
  LoadIDT(@IDTRegister);
  WriteText('[ OK ] DPL-3 int 0x80 syscall gate installed.'#13#10);
end;

function SyscallWriteUserBuffer(Process: PKernelProcess;
  FileDescriptor, BufferAddress, ByteCount: QWord): QWord;
var
  Buffer: array[0..MISTIX_SYSCALL_MAX_WRITE - 1] of Byte;
  I: QWord;
begin
  Result := MISTIX_ERR_BADF;
  if (FileDescriptor <> 1) and (FileDescriptor <> 2) then Exit;
  if ByteCount = 0 then
  begin
    Result := 0;
    Exit;
  end;
  if ByteCount > MISTIX_SYSCALL_MAX_WRITE then
  begin
    Result := MISTIX_ERR_INVAL;
    Exit;
  end;
  if not AddressSpaceCopyFrom(Process^.AddressSpaceRoot, BufferAddress,
       @Buffer[0], ByteCount) then
  begin
    Result := MISTIX_ERR_FAULT;
    Exit;
  end;
  for I := 0 to ByteCount - 1 do
    SerialPutChar(Char(Buffer[I]));
  Result := ByteCount;
end;

function SyscallDispatchFrame(Frame: PInterruptFrame): QWord;
var
  Process: PKernelProcess;
  ReturnValue: QWord;
  ChildPID: QWord;
  Handle: LongWord;
begin
  Result := PtrUInt(Frame);
  ReturnValue := MISTIX_ERR_NOSYS;
  if (Frame = nil) or (CurrentProcessIndex >= PROCESS_MAX_COUNT) then
  begin
    if Frame <> nil then Frame^.SavedRAX := MISTIX_ERR_FAULT;
    Exit;
  end;
  if (Frame^.CS and 3) <> 3 then
  begin
    Frame^.SavedRAX := MISTIX_ERR_FAULT;
    Exit;
  end;
  Process := @ProcessTable[CurrentProcessIndex];
  if Process^.State <> psRunning then
  begin
    Frame^.SavedRAX := MISTIX_ERR_FAULT;
    Exit;
  end;

  case Frame^.SavedRAX of
    MISTIX_SYSCALL_WRITE:
      ReturnValue := SyscallWriteUserBuffer(Process, Frame^.SavedRDI,
        Frame^.SavedRSI, Frame^.SavedRDX);

    MISTIX_SYSCALL_EXIT:
      begin
        ProcessTerminateCurrent(Frame, Frame^.SavedRDI);
        Result := ProcessScheduleFromFrame(Frame);
        Exit;
      end;

    MISTIX_SYSCALL_GETPID:
      ReturnValue := Process^.PID;

    MISTIX_SYSCALL_YIELD:
      begin
        Frame^.SavedRAX := 0;
        Result := ProcessScheduleFromFrame(Frame);
        Exit;
      end;

    MISTIX_SYSCALL_FORK:
      begin
        if ProcessForkCurrent(Frame, ChildPID) then
          ReturnValue := ChildPID
        else
          ReturnValue := MISTIX_ERR_NOMEM;
      end;

    MISTIX_SYSCALL_SPAWN:
      begin
        if ProcessSpawnFromUser(Frame^.SavedRDI, Frame^.SavedRSI, ChildPID) then
          ReturnValue := ChildPID
        else
          ReturnValue := MISTIX_ERR_NOMEM;
      end;

    MISTIX_SYSCALL_WAITPID:
      begin
        Result := ProcessWaitCurrent(Frame, Frame^.SavedRDI,
          Frame^.SavedRSI, ReturnValue);
        if Result <> PtrUInt(Frame) then Exit;
      end;

    MISTIX_SYSCALL_ENDPOINT_CREATE:
      ReturnValue := ProcessEndpointCreate(Process^, Handle);

    MISTIX_SYSCALL_IPC_SEND:
      ReturnValue := ProcessIPCSend(Process^, Frame^.SavedRDI,
                                    Frame^.SavedRSI);

    MISTIX_SYSCALL_IPC_RECEIVE:
      ReturnValue := ProcessIPCReceive(Process^, Frame^.SavedRDI,
                                       Frame^.SavedRSI);

    MISTIX_SYSCALL_CAP_TRANSFER:
      begin
        if (Frame^.SavedRSI > High(LongWord)) or
           (Frame^.SavedRDX > High(LongWord)) then
          ReturnValue := MISTIX_ERR_INVAL
        else
          ReturnValue := ProcessCapabilityTransfer(CurrentProcessIndex,
            Frame^.SavedRDI, LongWord(Frame^.SavedRSI), LongWord(Frame^.SavedRDX));
      end;

    MISTIX_SYSCALL_CAP_RECEIVE:
      ReturnValue := ProcessCapabilityReceive(CurrentProcessIndex);

    MISTIX_SYSCALL_CAP_CLOSE:
      begin
        if (Frame^.SavedRDI <= High(LongWord)) and
           (ProcessCapabilityClose(Process^.Capabilities,
             LongWord(Frame^.SavedRDI)) = krSuccess) then
          ReturnValue := 0
        else
          ReturnValue := MISTIX_ERR_BADF;
      end;

    MISTIX_SYSCALL_KILL:
      begin
        ReturnValue := ProcessKill(CurrentProcessIndex,
          Frame^.SavedRDI, Frame^.SavedRSI);
        if (Frame^.SavedRDI = Process^.PID) and (ReturnValue = 0) then
        begin
          Result := ProcessScheduleFromFrame(Frame);
          Exit;
        end;
      end;
  end;

  Frame^.SavedRAX := ReturnValue;
end;
