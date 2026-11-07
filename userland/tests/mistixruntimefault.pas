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
  ReadProbeByte: Byte;

procedure ExecuteProbeAddress(Address: PtrUInt); cdecl; assembler; nostackframe;
asm
  call rdi
end;

procedure MistixRuntimeFaultMain; cdecl; public name 'mistix_user_main';
var
  Mode: array[0..15] of Char;
  I: LongWord;
  CommandLength: QWord;
  CodeByte: Byte;
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
{$ifdef MISTIX_DEBUG_CHECKS}
        WriteCString('[DEBUG] overflow probe bytes=');
        WriteNumber(Ord(Mode[0]));
        WriteCString(',');
        WriteNumber(Ord(Mode[1]));
        WriteCString(#13#10);
        WriteCString('[DEBUG] overflow probe setting left operand.'#13#10);
{$endif}
        LeftValue := High(LongInt);
{$ifdef MISTIX_DEBUG_CHECKS}
        WriteCString('[DEBUG] overflow probe setting right operand.'#13#10);
{$endif}
        RightValue := Ord(Mode[1]);
{$ifdef MISTIX_DEBUG_CHECKS}
        WriteCString('[DEBUG] overflow probe adding operands.'#13#10);
{$endif}
        ArithmeticResult := LeftValue + RightValue;
{$ifdef MISTIX_DEBUG_CHECKS}
        WriteCString('[DEBUG] overflow probe unexpectedly completed.'#13#10);
{$endif}
      end;
    'd':
      begin
        Numerator := Ord(Mode[1]);
        Denominator := Ord(Mode[2]) - Ord(Mode[2]);
        DivisionResult := Numerator div Denominator;
      end;
    'p':
      case Mode[1] of
        'r':
          ReadProbeByte := PByte(PtrUInt($0000600000000000))^;
        'w':
          PByte(PtrUInt($0000600000000000))^ := $A5;
        't':
          PByte(PtrUInt(@MistixRuntimeFaultMain))^ := $90;
        'x':
          begin
            CodeByte := $C3;
            ExecuteProbeAddress(PtrUInt(@CodeByte));
          end;
      else
        SysExit(2);
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
  call MistixRuntimeFaultMain
  xor edi, edi
  mov eax, 2
  int $80
  ud2
end;

end.
