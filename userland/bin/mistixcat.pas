unit MistixCat;
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

procedure MistixCatMain; cdecl; public name 'mistix_user_main';
var
  Args, Path: array[0..127] of Char;
  Buffer: array[0..511] of Byte;
  J, FD, Count, TargetFD: QWord;
begin
  SysGetCommandLine(PtrUInt(@Args[0]), SizeOf(Args));
  TargetFD := 0;
  if Args[0] <> #0 then
  begin
    J := 0;
    while (Args[J] <> #0) and (J < High(Path)) do
    begin
      Path[J] := Args[J];
      Inc(J);
    end;
    Path[J] := #0;
    FD := SysOpen(PtrUInt(@Path[0]), MISTIX_O_RDONLY);
    if IsError(FD) then SysExit(1);
    TargetFD := FD;
  end;
  repeat
    Count := SysRead(TargetFD, PtrUInt(@Buffer[0]), SizeOf(Buffer));
    if IsError(Count) then
    begin
      if Count = MISTIX_EAGAIN then begin SysYield; Continue; end;
      Break;
    end;
    if Count = 0 then Break;
    if not SysWriteAll(1, PtrUInt(@Buffer[0]), Count) then SysExit(1);
  until False;
  if TargetFD <> 0 then SysClose(TargetFD);
  SysExit(0);
end;

procedure MistixCatEntry; assembler; nostackframe; public name '_start';
asm
  and rsp, -16
  call MistixCatMain
  xor edi, edi
  mov eax, 2
  int $80
  ud2
end;
end.
