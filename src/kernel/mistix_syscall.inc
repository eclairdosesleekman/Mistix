{ Mistix Phase 3: ring-3 software interrupt gate and checked syscall ABI.
  ABI: RAX=number; RDI, RSI, RDX are arguments; RAX is the result.
  This bootstrap ABI is intentionally tiny: write, exit, getpid, yield. }

const
  MISTIX_SYSCALL_WRITE = 1;
  MISTIX_SYSCALL_EXIT = 2;
  MISTIX_SYSCALL_GETPID = 3;
  MISTIX_SYSCALL_YIELD = 4;
  MISTIX_SYSCALL_MAX_WRITE = 4096;

  MISTIX_ERR_BADF = $FFFFFFFFFFFFFFF7;
  MISTIX_ERR_FAULT = $FFFFFFFFFFFFFFF2;
  MISTIX_ERR_INVAL = $FFFFFFFFFFFFFFEA;
  MISTIX_ERR_NOSYS = $FFFFFFFFFFFFFFDA;

procedure SyscallISR; assembler; nostackframe;
  public name 'mistix_syscall_isr';
asm
  push 0
  push 128
  jmp mistix_syscall_common
end;

procedure SyscallCommon; assembler; nostackframe;
  public name 'mistix_syscall_common';
asm
  cld
  push rax
  push rbx
  push rcx
  push rdx
  push rbp
  push rsi
  push rdi
  push r8
  push r9
  push r10
  push r11
  push r12
  push r13
  push r14
  push r15
  mov r12, rsp
  mov rdi, r12
  and rsp, -16
  call mistix_syscall_dispatch
  mov rsp, r12
  pop r15
  pop r14
  pop r13
  pop r12
  pop r11
  pop r10
  pop r9
  pop r8
  pop rdi
  pop rsi
  pop rbp
  pop rdx
  pop rcx
  pop rbx
  pop rax
  add rsp, 16
  iretq
end;

procedure SyscallInitialize;
begin
  SetIDTGate(128, PtrUInt(@SyscallISR), 0);
  IDT[128].Attributes := $EE; { Present, DPL 3, 64-bit interrupt gate. }
  LoadIDT(@IDTRegister);
  WriteText('[ OK ] DPL-3 int 0x80 syscall gate installed.'#13#10);
end;

procedure SyscallWriteUserBuffer(Process: PKernelProcess;
  FileDescriptor: QWord; BufferAddress, ByteCount: QWord;
  var ReturnValue: QWord);
var
  I: QWord;
  Source: PByte;
begin
  if (FileDescriptor <> 1) and (FileDescriptor <> 2) then
  begin
    ReturnValue := MISTIX_ERR_BADF;
    Exit;
  end;
  if ByteCount = 0 then
  begin
    ReturnValue := 0;
    Exit;
  end;
  if ByteCount > MISTIX_SYSCALL_MAX_WRITE then
  begin
    ReturnValue := MISTIX_ERR_INVAL;
    Exit;
  end;
  if not AddressSpaceValidateUserRange(Process^.AddressSpaceRoot,
                                        BufferAddress, ByteCount) then
  begin
    ReturnValue := MISTIX_ERR_FAULT;
    Exit;
  end;
  Source := PByte(PtrUInt(BufferAddress));
  for I := 0 to ByteCount - 1 do
    SerialPutChar(Char(Source[I]));
  ReturnValue := ByteCount;
end;

procedure SyscallDispatch(Frame: PInterruptFrame); cdecl;
  public name 'mistix_syscall_dispatch';
var
  Process: PKernelProcess;
  ReturnValue: QWord;
begin
  ReturnValue := MISTIX_ERR_NOSYS;
  if (Frame = nil) or (CurrentProcessIndex >= PROCESS_MAX_COUNT) then
  begin
    if Frame <> nil then Frame^.SavedRAX := MISTIX_ERR_FAULT;
    Exit;
  end;
  { The DPL-3 gate is the intended entry; reject kernel-originated calls. }
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
      SyscallWriteUserBuffer(Process, Frame^.SavedRDI,
        Frame^.SavedRSI, Frame^.SavedRDX, ReturnValue);

    MISTIX_SYSCALL_EXIT:
      begin
        Process^.ExitCode := Frame^.SavedRDI;
        Process^.State := psTerminated;
        WriteText(#13#10'[PROCESS] user process pid=');
        WriteDecimal(Process^.PID);
        WriteText(' exited with code ');
        WriteDecimal(Process^.ExitCode);
        WriteText('.'#13#10);
        WriteText('[NOTE] Phase 3 has one foreground process; exit halts the bootstrap kernel.'#13#10);
        HaltForever;
      end;

    MISTIX_SYSCALL_GETPID:
      ReturnValue := Process^.PID;

    MISTIX_SYSCALL_YIELD:
      ReturnValue := 0; { Cooperative scheduling is deferred to Phase 4. }
  end;

  Frame^.SavedRAX := ReturnValue;
end;

{ Build a minimal ring-3 RFLAGS image: reserved bit 1 set; IF/IOPL/DF/TF clear. }
procedure EnterUserMode(EntryPoint, UserStack: QWord);
  assembler; nostackframe;
asm
  cli
  mov ax, $3B
  mov ds, ax
  mov es, ax
  xor eax, eax
  mov fs, ax
  mov gs, ax
  push $3B
  push rsi
  push 2
  push $43
  push rdi
  iretq
end;
