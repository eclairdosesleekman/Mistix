unit MistixRuntimeFault;

{$mode objfpc}
{$H-}
{$S-}
{$ALIGN 8}
{$ASMMODE intel}

interface

implementation

{$I mistixlib.inc}

type
  TProbeIndex = 0..3;

var
  ProbeTable: array[TProbeIndex] of Byte;
  IndexValue: LongInt;
  LeftValue: LongInt;
  RightValue: LongInt;
  ArithmeticResult: LongInt;
  Numerator: LongInt;
  Denominator: LongInt;
  DivisionResult: LongInt;

procedure MistixRuntimeFaultMain; cdecl; public name 'mistix_user_main';
var
  Mode: array[0..15] of Char;
  I: LongWord;
  CommandLength: QWord;
begin
  for I := 0 to High(Mode) do
    Mode[I] := #0;
  CommandLength := SysGetCommandLine(PtrUInt(@Mode[0]), SizeOf(Mode));
  if IsError(CommandLength) or (CommandLength = 0) then
    SysExit(2);

  case Mode[0] of
    'r':
      begin
        IndexValue := Ord(Mode[1]);
        ProbeTable[IndexValue] := Byte(IndexValue);
      end;
    'o':
      begin
        LeftValue := High(LongInt);
        RightValue := Ord(Mode[1]);
        ArithmeticResult := LeftValue + RightValue;
      end;
    'd':
      begin
        Numerator := Ord(Mode[1]);
        Denominator := Ord(Mode[2]) - Ord(Mode[2]);
        DivisionResult := Numerator div Denominator;
      end;
  else
    SysExit(2);
  end;

  { Reaching this point means the requested runtime check did not fire. }
  SysExit(99);
end;

procedure MistixRuntimeFaultEntry; assembler; nostackframe; public name '_start';
asm
  and rsp, -16
  call mistix_user_main
  xor edi, edi
  mov eax, 2
  int $80
  ud2
end;

end.
