{ 8259 PIC + PIT timer source. IRQ0 is mapped to IDT vector 32 at 100 Hz. }

const
  PIC1_COMMAND = $20;
  PIC1_DATA = $21;
  PIC2_COMMAND = $A0;
  PIC2_DATA = $A1;
  PIT_CHANNEL0 = $40;
  PIT_COMMAND = $43;
  PIT_INPUT_HZ = 1193182;
  TIMER_FREQUENCY_HZ = 100;

var
  TimerTickCount: QWord;

procedure TimerISR; assembler; nostackframe; public name 'mistix_timer_isr';
asm
  push 0
  push 32
  jmp ISRCommon
end;

procedure TimerPicAcknowledge;
begin
  Out8(PIC1_COMMAND, $20);
end;

procedure TimerInitialize;
var
  Divisor: Word;
begin
  TimerTickCount := 0;
  { Remap legacy PIC vectors away from CPU exceptions. Keep only IRQ0 unmasked. }
  Out8(PIC1_COMMAND, $11);
  Out8(PIC2_COMMAND, $11);
  Out8(PIC1_DATA, $20);
  Out8(PIC2_DATA, $28);
  Out8(PIC1_DATA, $04);
  Out8(PIC2_DATA, $02);
  Out8(PIC1_DATA, $01);
  Out8(PIC2_DATA, $01);
  Out8(PIC1_DATA, $FE);
  Out8(PIC2_DATA, $FF);

  SetIDTGate(32, PtrUInt(@TimerISR), 0);
  LoadIDT(@IDTRegister);
  Divisor := Word(PIT_INPUT_HZ div TIMER_FREQUENCY_HZ);
  Out8(PIT_COMMAND, $36);
  Out8(PIT_CHANNEL0, Divisor and $FF);
  Out8(PIT_CHANNEL0, Divisor shr 8);
  WriteText('[ OK ] PIT configured at ');
  WriteDecimal(TIMER_FREQUENCY_HZ);
  WriteText(' Hz; PIC IRQ0 mapped to vector 32.'#13#10);
end;
