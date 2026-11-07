unit MistixLS;
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
  NewLine: array[0..1] of Char = (#13, #10);

procedure MistixLSMain; cdecl; public name 'mistix_user_main';
var
  Args: array[0..127] of Char;
  Path: array[0..127] of Char;
  Entries: array[0..7] of TMistixDirent;
  FD, Count, I, J, Len: QWord;
  Slash: array[0..0] of Char;
begin
  SysGetCommandLine(PtrUInt(@Args[0]), SizeOf(Args));
  Slash[0] := '/';
  Path[0] := '.'; Path[1] := #0;
  if Args[0] <> #0 then
  begin
    J := 0;
    while (Args[J] <> #0) and (J < High(Path)) do
    begin
      Path[J] := Args[J];
      Inc(J);
    end;
    Path[J] := #0;
  end;
  FD := SysOpen(PtrUInt(@Path[0]), MISTIX_O_RDONLY or MISTIX_O_DIRECTORY);
  if IsError(FD) then begin SysWrite(1, PtrUInt(@Path[0]), CStrLength(Path)); SysWrite(1, PtrUInt(@NewLine[0]), 2); SysExit(1); end;
  repeat
    Count := SysGetDents(FD, PtrUInt(@Entries[0]), 8);
    if IsError(Count) or (Count = 0) then Break;
    for I := 0 to Count - 1 do
    begin
      Len := CStrLength(Entries[I].Name);
      if (Len <> 0) and not SysWriteAll(1, PtrUInt(@Entries[I].Name[0]), Len) then SysExit(1);
      if (Entries[I].EntryType = 2) and not SysWriteAll(1, PtrUInt(@Slash[0]), 1) then SysExit(1);
      if not SysWriteAll(1, PtrUInt(@NewLine[0]), 2) then SysExit(1);
    end;
  until Count < 8;
  SysClose(FD);
  SysExit(0);
end;

procedure MistixLSEntry; assembler; nostackframe; public name '_start';
asm
  and rsp, -16
  call MistixLSMain
  xor edi, edi
  mov eax, 2
  int $80
  ud2
end;
end.
