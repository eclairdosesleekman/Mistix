unit MistixEcho;
{$mode objfpc}
{$H-}
{$R-}
{$Q-}
{$S-}
{$I-}
{$ALIGN 8}
{$ASMMODE intel}
interface
implementation
{$I mistixlib.inc}

procedure MistixEchoMain; cdecl; public name 'mistix_user_main';
var
  Args: array[0..127] of Char;
  J: LongWord;
  NewLine: array[0..1] of Byte;
begin
  SysGetCommandLine(PtrUInt(@Args[0]), SizeOf(Args));
  J := CStrLength(Args);
  if (J <> 0) and not SysWriteAll(1, PtrUInt(@Args[0]), J) then SysExit(1);
  NewLine[0] := 13; NewLine[1] := 10;
  SysWriteAll(1, PtrUInt(@NewLine[0]), 2);
  SysExit(0);
end;

procedure MistixEchoEntry; assembler; nostackframe; public name '_start';
asm
  and rsp, -16
  call MistixEchoMain
  xor edi, edi
  mov eax, 2
  int $80
  ud2
end;
end.
