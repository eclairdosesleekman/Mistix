unit MistixInit;

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

const
  MotdPath: array[0..12] of Char = ('/','e','t','c','/','m','o','t','d',#0,#0,#0,#0);
  ShellPath: array[0..7] of Char = ('/','b','i','n','/','s','h',#0);
  EmptyArgs: array[0..0] of Char = (#0);
  Banner: array[0..36] of Byte = (77, 105, 115, 116, 105, 120, 32, 80, 97, 115, 99, 97, 108, 32, 85, 78, 73, 88, 32, 115, 121, 115, 116, 101, 109, 32, 105, 115, 32, 114, 101, 97, 100, 121, 33, 13, 10);

procedure MistixInitMain; cdecl; public name 'mistix_user_main';
var
  FD: QWord;
  Count, PID, Status: QWord;
  Buffer: array[0..127] of Byte;
begin
  SysWriteAll(1, PtrUInt(@Banner[0]), SizeOf(Banner));
  FD := SysOpen(PtrUInt(@MotdPath[0]), 0);
  if not IsError(FD) then
  begin
    repeat
      Count := SysRead(FD, PtrUInt(@Buffer[0]), SizeOf(Buffer));
      if IsError(Count) then Break;
      if Count = 0 then Break;
      SysWriteAll(1, PtrUInt(@Buffer[0]), Count);
    until False;
    SysClose(FD);
  end;

  repeat
    PID := SysSpawnPath(PtrUInt(@ShellPath[0]), PtrUInt(@EmptyArgs[0]), 0, 1);
    if not IsError(PID) then
    begin
      Status := 0;
      SysWaitPID(PID, PtrUInt(@Status));
    end;
    SysYield;
  until False;
end;

procedure MistixInitEntry; assembler; nostackframe; public name '_start';
asm
  and rsp, -16
  call mistix_user_main
  xor edi, edi
  mov eax, 2
  int $80
  ud2
end;

end.
