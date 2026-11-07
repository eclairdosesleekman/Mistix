type
  TGDTRegister = packed record
    Limit: Word;
    Base: QWord;
  end;

  TIDTRegister = packed record
    Limit: Word;
    Base: QWord;
  end;

  TTSS64 = packed record
    Reserved0: LongWord;
    RSP0: QWord;
    RSP1: QWord;
    RSP2: QWord;
    Reserved1: QWord;
    IST1: QWord;
    IST2: QWord;
    IST3: QWord;
    IST4: QWord;
    IST5: QWord;
    IST6: QWord;
    IST7: QWord;
    Reserved2: QWord;
    Reserved3: Word;
    IOPBOffset: Word;
  end;

  TIDTGate = packed record
    OffsetLow: Word;
    Selector: Word;
    IST: Byte;
    Attributes: Byte;
    OffsetMiddle: Word;
    OffsetHigh: LongWord;
    Reserved: LongWord;
  end;

  TInterruptFrame = packed record
    SavedR15: QWord;
    SavedR14: QWord;
    SavedR13: QWord;
    SavedR12: QWord;
    SavedR11: QWord;
    SavedR10: QWord;
    SavedR9: QWord;
    SavedR8: QWord;
    SavedRDI: QWord;
    SavedRSI: QWord;
    SavedRBP: QWord;
    SavedRDX: QWord;
    SavedRCX: QWord;
    SavedRBX: QWord;
    SavedRAX: QWord;
    Vector: QWord;
    ErrorCode: QWord;
    RIP: QWord;
    CS: QWord;
    RFLAGS: QWord;
  end;
  PInterruptFrame = ^TInterruptFrame;

var
  GDT: array[0..10] of QWord;
  IDT: array[0..255] of TIDTGate;
  GDTRegister: TGDTRegister;
  IDTRegister: TIDTRegister;
  TaskState: TTSS64;
  KernelTransitionStack: array[0..16383] of Byte;
  DoubleFaultStack: array[0..8191] of Byte;
  CPUHasNXSupport: Boolean;

function ReadCR2: QWord; assembler; nostackframe;
asm
  mov rax, cr2
end;

function ReadCR0: QWord; assembler; nostackframe;
asm
  mov rax, cr0
end;

procedure WriteCR0(Value: QWord); assembler; nostackframe;
asm
  mov cr0, rdi
end;

function CPUEnableWriteProtect: Boolean;
var
  ControlRegister: QWord;
begin
  ControlRegister := ReadCR0;
  ControlRegister := ControlRegister or (QWord(1) shl 16); { CR0.WP }
  WriteCR0(ControlRegister);
  Result := (ReadCR0 and (QWord(1) shl 16)) <> 0;
end;

function ReadCR3: QWord; assembler; nostackframe;
asm
  mov rax, cr3
end;

function ReadRSP: QWord; assembler; nostackframe;
asm
  mov rax, rsp
end;

function ReadMSR(Index: LongWord): QWord; assembler; nostackframe;
asm
  mov ecx, edi
  rdmsr
  shl rdx, 32
  or rax, rdx
end;

procedure WriteMSR(Index: LongWord; Value: QWord); assembler; nostackframe;
asm
  mov ecx, edi
  mov eax, esi
  mov rdx, rsi
  shr rdx, 32
  wrmsr
end;

function CPUID(Leaf, Subleaf: LongWord;
  var A, B, C, D: LongWord): Boolean; assembler; nostackframe;
asm
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov r12, rdx
  mov r13, rcx
  mov r14, r8
  mov r15, r9
  mov eax, edi
  mov ecx, esi
  cpuid
  mov [r12], eax
  mov [r13], ebx
  mov [r14], ecx
  mov [r15], edx
  mov eax, 1
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
end;

function CPUHasNX: Boolean;
var
  A, B, C, D: LongWord;
begin
  Result := False;
  if not CPUID($80000000, 0, A, B, C, D) then
    Exit;
  if A < $80000001 then
    Exit;
  if not CPUID($80000001, 0, A, B, C, D) then
    Exit;
  Result := (D and (1 shl 20)) <> 0;
end;

function CPUEnableNX: Boolean;
var
  EFER: QWord;
begin
  Result := False;
  if not CPUHasNX then
    Exit;
  EFER := ReadMSR($C0000080);
  EFER := EFER or (QWord(1) shl 11);
  WriteMSR($C0000080, EFER);
  Result := True;
end;

procedure LoadGDT(Descriptor: Pointer); assembler; nostackframe;
asm
  lgdt [rdi]
end;

procedure LoadIDT(Descriptor: Pointer); assembler; nostackframe;
asm
  lidt [rdi]
end;

procedure LoadTaskRegister; assembler; nostackframe;
asm
  mov ax, $48
  ltr ax
end;

procedure InvalidatePage(VirtualAddress: QWord); assembler; nostackframe;
asm
  invlpg [rdi]
end;

procedure HaltForever; assembler; nostackframe;
asm
@halt_forever:
  cli
  hlt
  jmp @halt_forever
end;

function StackTopAligned(BaseAddress: PtrUInt; StackSize: PtrUInt): QWord;
var
  Top: PtrUInt;
begin
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] StackTopAligned: entered.'#13#10);
{$endif}
  Top := (BaseAddress + StackSize) and not PtrUInt(15);
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] StackTopAligned: arithmetic complete.'#13#10);
{$endif}
  Result := QWord(Top);
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] StackTopAligned: result assigned.'#13#10);
{$endif}
end;

procedure BuildTSSDescriptor;
var
  BaseAddress: QWord;
  Limit: QWord;
begin
  BaseAddress := PtrUInt(@TaskState);
  Limit := SizeOf(TTSS64) - 1;
  GDT[9] :=
    (Limit and $FFFF) or
    ((BaseAddress and $FFFF) shl 16) or
    (((BaseAddress shr 16) and $FF) shl 32) or
    (QWord($89) shl 40) or
    (((Limit shr 16) and $F) shl 48) or
    (((BaseAddress shr 24) and $FF) shl 56);
  GDT[10] := BaseAddress shr 32;
end;

procedure SetIDTGate(Vector: LongWord; HandlerAddress: QWord; ISTIndex: Byte);
var
  Gate: ^TIDTGate;
begin
  Gate := @IDT[Vector];
  Gate^.OffsetLow := Word(HandlerAddress and $FFFF);
  Gate^.Selector := $28; { Preserve Limine's 64-bit kernel code selector. }
  Gate^.IST := ISTIndex and $07;
  Gate^.Attributes := $8E; { Present, DPL 0, 64-bit interrupt gate. }
  Gate^.OffsetMiddle := Word((HandlerAddress shr 16) and $FFFF);
  Gate^.OffsetHigh := LongWord((HandlerAddress shr 32) and $FFFFFFFF);
  Gate^.Reserved := 0;
end;

function TrapDispatch(Frame: PInterruptFrame): QWord; cdecl; forward;
procedure ISRCommon; assembler; nostackframe; forward;

procedure ISR0; assembler; nostackframe; public name 'mistix_isr_0';
asm
  push 0
  push 0
  jmp ISRCommon
end;

procedure ISR1; assembler; nostackframe; public name 'mistix_isr_1';
asm
  push 0
  push 1
  jmp ISRCommon
end;

procedure ISR2; assembler; nostackframe; public name 'mistix_isr_2';
asm
  push 0
  push 2
  jmp ISRCommon
end;

procedure ISR3; assembler; nostackframe; public name 'mistix_isr_3';
asm
  push 0
  push 3
  jmp ISRCommon
end;

procedure ISR4; assembler; nostackframe; public name 'mistix_isr_4';
asm
  push 0
  push 4
  jmp ISRCommon
end;

procedure ISR5; assembler; nostackframe; public name 'mistix_isr_5';
asm
  push 0
  push 5
  jmp ISRCommon
end;

procedure ISR6; assembler; nostackframe; public name 'mistix_isr_6';
asm
  push 0
  push 6
  jmp ISRCommon
end;

procedure ISR7; assembler; nostackframe; public name 'mistix_isr_7';
asm
  push 0
  push 7
  jmp ISRCommon
end;

procedure ISR8; assembler; nostackframe; public name 'mistix_isr_8';
asm
  push 8
  jmp ISRCommon
end;

procedure ISR9; assembler; nostackframe; public name 'mistix_isr_9';
asm
  push 0
  push 9
  jmp ISRCommon
end;

procedure ISR10; assembler; nostackframe; public name 'mistix_isr_10';
asm
  push 10
  jmp ISRCommon
end;

procedure ISR11; assembler; nostackframe; public name 'mistix_isr_11';
asm
  push 11
  jmp ISRCommon
end;

procedure ISR12; assembler; nostackframe; public name 'mistix_isr_12';
asm
  push 12
  jmp ISRCommon
end;

procedure ISR13; assembler; nostackframe; public name 'mistix_isr_13';
asm
  push 13
  jmp ISRCommon
end;

procedure ISR14; assembler; nostackframe; public name 'mistix_isr_14';
asm
  push 14
  jmp ISRCommon
end;

procedure ISR15; assembler; nostackframe; public name 'mistix_isr_15';
asm
  push 0
  push 15
  jmp ISRCommon
end;

procedure ISR16; assembler; nostackframe; public name 'mistix_isr_16';
asm
  push 0
  push 16
  jmp ISRCommon
end;

procedure ISR17; assembler; nostackframe; public name 'mistix_isr_17';
asm
  push 17
  jmp ISRCommon
end;

procedure ISR18; assembler; nostackframe; public name 'mistix_isr_18';
asm
  push 0
  push 18
  jmp ISRCommon
end;

procedure ISR19; assembler; nostackframe; public name 'mistix_isr_19';
asm
  push 0
  push 19
  jmp ISRCommon
end;

procedure ISR20; assembler; nostackframe; public name 'mistix_isr_20';
asm
  push 0
  push 20
  jmp ISRCommon
end;

procedure ISR21; assembler; nostackframe; public name 'mistix_isr_21';
asm
  push 21
  jmp ISRCommon
end;

procedure ISR22; assembler; nostackframe; public name 'mistix_isr_22';
asm
  push 0
  push 22
  jmp ISRCommon
end;

procedure ISR23; assembler; nostackframe; public name 'mistix_isr_23';
asm
  push 0
  push 23
  jmp ISRCommon
end;

procedure ISR24; assembler; nostackframe; public name 'mistix_isr_24';
asm
  push 0
  push 24
  jmp ISRCommon
end;

procedure ISR25; assembler; nostackframe; public name 'mistix_isr_25';
asm
  push 0
  push 25
  jmp ISRCommon
end;

procedure ISR26; assembler; nostackframe; public name 'mistix_isr_26';
asm
  push 0
  push 26
  jmp ISRCommon
end;

procedure ISR27; assembler; nostackframe; public name 'mistix_isr_27';
asm
  push 0
  push 27
  jmp ISRCommon
end;

procedure ISR28; assembler; nostackframe; public name 'mistix_isr_28';
asm
  push 0
  push 28
  jmp ISRCommon
end;

procedure ISR29; assembler; nostackframe; public name 'mistix_isr_29';
asm
  push 29
  jmp ISRCommon
end;

procedure ISR30; assembler; nostackframe; public name 'mistix_isr_30';
asm
  push 30
  jmp ISRCommon
end;

procedure ISR31; assembler; nostackframe; public name 'mistix_isr_31';
asm
  push 0
  push 31
  jmp ISRCommon
end;

procedure ISRSpurious; assembler; nostackframe; public name 'mistix_isr_spurious';
asm
  push 0
  push 255
  jmp ISRCommon
end;

procedure ISRCommon; assembler; nostackframe; public name 'mistix_isr_common';
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
  call TrapDispatch
  mov rsp, rax
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

procedure ExceptionDispatch(Frame: PInterruptFrame); cdecl;
  public name 'mistix_exception_dispatch';
begin
  WriteText(#13#10'[FATAL] CPU exception vector=');
  WriteDecimal(Frame^.Vector);
  WriteText(' error=');
  WriteHex64(Frame^.ErrorCode);
  WriteText(' rip=');
  WriteHex64(Frame^.RIP);
  WriteText(#13#10);
  if Frame^.Vector = 14 then
  begin
    WriteText('[FATAL] page-fault linear address CR2=');
    WriteHex64(ReadCR2);
    WriteText(#13#10);
  end;
  WriteText('[FATAL] Mistix halted; interrupts remain disabled.'#13#10);
  HaltForever;
end;

procedure CPUInitialize;
var
  I: LongWord;
begin
  { Limine Base Revision 5+ guarantees the initial selectors used here:
      CS = 0x28, DS/ES/SS = 0x30.
    Preserve those descriptor slots while adding user descriptors and TSS. }
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] CPU init: clearing GDT.'#13#10);
{$endif}
  for I := 0 to 10 do
    GDT[I] := 0;
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] CPU init: GDT descriptors written.'#13#10);
{$endif}
  GDT[1] := $00009A000000FFFF; { 16-bit code }
  GDT[2] := $000092000000FFFF; { 16-bit data }
  GDT[3] := $00CF9A000000FFFF; { 32-bit code }
  GDT[4] := $00CF92000000FFFF; { 32-bit data }
  GDT[5] := $00AF9A000000FFFF; { 64-bit ring-0 code, selector 0x28 }
  GDT[6] := $00CF92000000FFFF; { ring-0 data, selector 0x30 }
  GDT[7] := $00CFF2000000FFFF; { ring-3 data, selector 0x3B }
  GDT[8] := $00AFFA000000FFFF; { ring-3 code, selector 0x43 }
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] CPU init: GDT descriptors initialized.'#13#10);
{$endif}

  TaskState.Reserved0 := 0;
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] CPU init: TSS Reserved0 set.'#13#10);
{$endif}
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] CPU init: computing RSP0.'#13#10);
{$endif}
  TaskState.RSP0 := StackTopAligned(PtrUInt(@KernelTransitionStack[0]),
                                    SizeOf(KernelTransitionStack));
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] CPU init: RSP0 assigned.'#13#10);
{$endif}
  TaskState.RSP1 := 0;
  TaskState.RSP2 := 0;
  TaskState.Reserved1 := 0;
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] CPU init: RSP1/RSP2/reserved set.'#13#10);
  WriteText('[DEBUG] CPU init: computing IST1.'#13#10);
{$endif}
  TaskState.IST1 := StackTopAligned(PtrUInt(@DoubleFaultStack[0]),
                                    SizeOf(DoubleFaultStack));
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] CPU init: IST1 assigned.'#13#10);
{$endif}
  TaskState.IST2 := 0;
  TaskState.IST3 := 0;
  TaskState.IST4 := 0;
  TaskState.IST5 := 0;
  TaskState.IST6 := 0;
  TaskState.IST7 := 0;
  TaskState.Reserved2 := 0;
  TaskState.Reserved3 := 0;
  TaskState.IOPBOffset := SizeOf(TTSS64); { no I/O bitmap: ring 3 I/O is denied }
  BuildTSSDescriptor;
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] CPU init: TSS descriptor built.'#13#10);
{$endif}

  GDTRegister.Limit := SizeOf(GDT) - 1;
  GDTRegister.Base := PtrUInt(@GDT[0]);
  LoadGDT(@GDTRegister);
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] CPU init: GDT loaded.'#13#10);
{$endif}
  LoadTaskRegister;
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] CPU init: task register loaded.'#13#10);
{$endif}

  for I := 0 to 255 do
    SetIDTGate(I, PtrUInt(@ISRSpurious), 0);
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] CPU init: IDT default gates installed.'#13#10);
{$endif}

  SetIDTGate(0,  PtrUInt(@ISR0), 0);
  SetIDTGate(1,  PtrUInt(@ISR1), 0);
  SetIDTGate(2,  PtrUInt(@ISR2), 0);
  SetIDTGate(3,  PtrUInt(@ISR3), 0);
  SetIDTGate(4,  PtrUInt(@ISR4), 0);
  SetIDTGate(5,  PtrUInt(@ISR5), 0);
  SetIDTGate(6,  PtrUInt(@ISR6), 0);
  SetIDTGate(7,  PtrUInt(@ISR7), 0);
  SetIDTGate(8,  PtrUInt(@ISR8), 1); { dedicated IST stack for double fault }
  SetIDTGate(9,  PtrUInt(@ISR9), 0);
  SetIDTGate(10, PtrUInt(@ISR10), 0);
  SetIDTGate(11, PtrUInt(@ISR11), 0);
  SetIDTGate(12, PtrUInt(@ISR12), 0);
  SetIDTGate(13, PtrUInt(@ISR13), 0);
  SetIDTGate(14, PtrUInt(@ISR14), 0);
  SetIDTGate(15, PtrUInt(@ISR15), 0);
  SetIDTGate(16, PtrUInt(@ISR16), 0);
  SetIDTGate(17, PtrUInt(@ISR17), 0);
  SetIDTGate(18, PtrUInt(@ISR18), 0);
  SetIDTGate(19, PtrUInt(@ISR19), 0);
  SetIDTGate(20, PtrUInt(@ISR20), 0);
  SetIDTGate(21, PtrUInt(@ISR21), 0);
  SetIDTGate(22, PtrUInt(@ISR22), 0);
  SetIDTGate(23, PtrUInt(@ISR23), 0);
  SetIDTGate(24, PtrUInt(@ISR24), 0);
  SetIDTGate(25, PtrUInt(@ISR25), 0);
  SetIDTGate(26, PtrUInt(@ISR26), 0);
  SetIDTGate(27, PtrUInt(@ISR27), 0);
  SetIDTGate(28, PtrUInt(@ISR28), 0);
  SetIDTGate(29, PtrUInt(@ISR29), 0);
  SetIDTGate(30, PtrUInt(@ISR30), 0);
  SetIDTGate(31, PtrUInt(@ISR31), 0);
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] CPU init: exception gates installed.'#13#10);
{$endif}

  IDTRegister.Limit := SizeOf(IDT) - 1;
  IDTRegister.Base := PtrUInt(@IDT[0]);
  LoadIDT(@IDTRegister);
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] CPU init: IDT loaded.'#13#10);
  WriteText('[DEBUG] CPU init: enabling NX.'#13#10);
{$endif}

  CPUHasNXSupport := CPUEnableNX;
{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] CPU init: NX check complete.'#13#10);
{$endif}
  WriteText('[ OK ] GDT loaded; Limine selectors preserved; 64-bit TSS installed.'#13#10);
  WriteText('[ OK ] IDT loaded with exception gates 0..31 and double-fault IST.'#13#10);
  if CPUHasNXSupport then
    WriteText('[ OK ] CPU NX support enabled.'#13#10)
  else
    WriteText('[WARN] CPU has no NX support; execute-disable flags unavailable.'#13#10);

  if CPUEnableWriteProtect then
    WriteText('[ OK ] CR0.WP enabled; supervisor writes respect read-only PTEs.'#13#10)
  else
  begin
    WriteText('[FAIL] Could not enable CR0.WP.'#13#10);
    HaltForever;
  end;
end;
