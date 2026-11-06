unit MistixShell;

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
  Prompt: array[0..8] of Char = 'Mistix> '#0;
  HelpText: array[0..62] of Char = (66, 117, 105, 108, 116, 105, 110, 115, 58, 32, 104, 101, 108, 112, 44, 32, 99, 100, 32, 80, 65, 84, 72, 44, 32, 112, 119, 100, 44, 32, 101, 120, 105, 116, 46, 32, 67, 111, 109, 109, 97, 110, 100, 115, 58, 32, 108, 115, 44, 32, 99, 97, 116, 44, 32, 101, 99, 104, 111, 46, 13, 10, 0);
  NewLine: array[0..1] of Char = (#13,#10);
  BackspaceSequence: array[0..2] of Byte = (8,32,8);

function EqualText(const A, B: array of Char): Boolean;
var I: LongWord;
begin
  Result := False;
  for I := 0 to High(B) do
  begin
    if I > High(A) then Exit;
    if A[I] <> B[I] then Exit;
    if B[I] = #0 then begin Result := True; Exit; end;
  end;
end;

procedure BuildProgramPath(const Command: array of Char; var Path: array of Char);
var I, P: LongWord;
begin
  for I := 0 to High(Path) do Path[I] := #0;
  Path[0] := '/'; Path[1] := 'b'; Path[2] := 'i'; Path[3] := 'n'; Path[4] := '/';
  P := 5;
  I := 0;
  while (I < Length(Command)) and (Command[I] <> #0) and (Command[I] <> ' ') do
  begin
    if P >= High(Path) then Exit;
    Path[P] := Command[I]; Inc(P); Inc(I);
  end;
  Path[P] := #0;
end;

procedure ExtractCommand(const Line: array of Char; var Command, Args: array of Char);
var I, J: LongWord;
begin
  for I := 0 to High(Command) do Command[I] := #0;
  for I := 0 to High(Args) do Args[I] := #0;
  I := 0;
  while (I < Length(Line)) and (Line[I] = ' ') do Inc(I);
  J := 0;
  while (I < Length(Line)) and (Line[I] <> #0) and (Line[I] <> ' ') do
  begin
    if J < High(Command) then begin Command[J] := Line[I]; Inc(J); end;
    Inc(I);
  end;
  while (I < Length(Line)) and (Line[I] = ' ') do Inc(I);
  J := 0;
  while (I < Length(Line)) and (Line[I] <> #0) do
  begin
    if J < High(Args) then begin Args[J] := Line[I]; Inc(J); end;
    Inc(I);
  end;
end;

function FindPipe(const Line: array of Char): LongInt;
var I: LongWord;
begin
  Result := -1;
  for I := 0 to High(Line) do
  begin
    if Line[I] = #0 then Exit;
    if Line[I] = '|' then begin Result := I; Exit; end;
  end;
end;

procedure CopyRange(const Source: array of Char; StartAt, Count: LongWord;
  var Destination: array of Char);
var I: LongWord;
begin
  for I := 0 to High(Destination) do
  begin
    Destination[I] := #0;
    if I >= Count then Exit;
    Destination[I] := Source[StartAt + I];
  end;
end;

procedure RunExternal(const Line: array of Char);
var
  Command, Args, Path: array[0..127] of Char;
  PID, WaitStatus: QWord;
begin
  ExtractCommand(Line, Command, Args);
  if Command[0] = #0 then Exit;
  BuildProgramPath(Command, Path);
  PID := SysSpawnPath(PtrUInt(@Path[0]), PtrUInt(@Args[0]), 0, 1);
  if IsError(PID) then
  begin
    WriteCString(['m','i','s','t','i','x',':',' ','c','o','m','m','a','n','d',' ','n','o','t',' ','f','o','u','n','d',':',' ',#0]);
    WriteCString(Command);
    SysWrite(1, PtrUInt(@NewLine[0]), 2);
    Exit;
  end;
  WaitStatus := 0;
  SysWaitPID(PID, PtrUInt(@WaitStatus));
end;

procedure RunPipeline(const Line: array of Char; PipeAt: LongInt);
var
  LeftLine, RightLine: array[0..127] of Char;
  LeftCommand, LeftArgs, LeftPath: array[0..127] of Char;
  RightCommand, RightArgs, RightPath: array[0..127] of Char;
  Pair: array[0..1] of QWord;
  LeftPID, RightPID, Status: QWord;
  I, LeftLength, RightLength: LongWord;
begin
  LeftLength := PipeAt;
  while (LeftLength > 0) and (Line[LeftLength-1] = ' ') do Dec(LeftLength);
  I := PipeAt + 1;
  while (I < Length(Line)) and (Line[I] = ' ') do Inc(I);
  RightLength := CStrLength(Line) - I;
  CopyRange(Line, 0, LeftLength, LeftLine);
  CopyRange(Line, I, RightLength, RightLine);
  ExtractCommand(LeftLine, LeftCommand, LeftArgs);
  ExtractCommand(RightLine, RightCommand, RightArgs);
  if (LeftCommand[0] = #0) or (RightCommand[0] = #0) then Exit;
  if IsError(SysPipe(PtrUInt(@Pair[0]))) then Exit;
  BuildProgramPath(LeftCommand, LeftPath);
  BuildProgramPath(RightCommand, RightPath);
  LeftPID := SysSpawnPath(PtrUInt(@LeftPath[0]), PtrUInt(@LeftArgs[0]), 0, Pair[1]);
  RightPID := SysSpawnPath(PtrUInt(@RightPath[0]), PtrUInt(@RightArgs[0]), Pair[0], 1);
  SysClose(Pair[0]);
  SysClose(Pair[1]);
  if not IsError(LeftPID) then begin Status := 0; SysWaitPID(LeftPID, PtrUInt(@Status)); end;
  if not IsError(RightPID) then begin Status := 0; SysWaitPID(RightPID, PtrUInt(@Status)); end;
end;

procedure ExecuteLine(const Line: array of Char);
var
  Command, Args: array[0..127] of Char;
  CWD: array[0..127] of Char;
  PipeAt: LongInt;
begin
  if CStrLength(Line) = 0 then Exit;
  PipeAt := FindPipe(Line);
  if PipeAt >= 0 then
  begin
    RunPipeline(Line, PipeAt);
    Exit;
  end;
  ExtractCommand(Line, Command, Args);
  if EqualText(Command, ['h','e','l','p',#0]) then
    WriteCString(HelpText)
  else if EqualText(Command, ['e','x','i','t',#0]) then
    SysExit(0)
  else if EqualText(Command, ['p','w','d',#0]) then
  begin
    SysGetCWD(PtrUInt(@CWD[0]), SizeOf(CWD));
    WriteCString(CWD);
    SysWrite(1, PtrUInt(@NewLine[0]), 2);
  end
  else if EqualText(Command, ['c','d',#0]) then
  begin
    if Args[0] = #0 then Args[0] := '/';
    if IsError(SysChdir(PtrUInt(@Args[0]))) then
      SysWrite(1, PtrUInt(@NewLine[0]), 2);
  end
  else
    RunExternal(Line);
end;

procedure MistixShellMain; cdecl; public name 'mistix_user_main';
var
  Line: array[0..127] of Char;
  Count: QWord;
  C: Char;
  Used: LongWord;
begin
  while True do
  begin
    WriteCString(Prompt);
    Used := 0;
    while True do
    begin
      Count := SysRead(0, PtrUInt(@C), 1);
      if IsError(Count) then
      begin
        if Count = MISTIX_EAGAIN then SysYield else Break;
        Continue;
      end;
      if Count = 0 then begin SysYield; Continue; end;
      if (C = #13) or (C = #10) then
      begin
        SysWrite(1, PtrUInt(@NewLine[0]), 2);
        Break;
      end;
      if (C = #8) or (C = #127) then
      begin
        if Used > 0 then
        begin
          Dec(Used);
          SysWrite(1, PtrUInt(@BackspaceSequence[0]), 3);
        end;
        Continue;
      end;
      if (Ord(C) >= 32) and (Used < High(Line)) then
      begin
        Line[Used] := C;
        Inc(Used);
        SysWrite(1, PtrUInt(@C), 1);
      end;
    end;
    Line[Used] := #0;
    ExecuteLine(Line);
  end;
end;

procedure MistixShellEntry; assembler; nostackframe; public name '_start';
asm
  and rsp, -16
  call mistix_user_main
  xor edi, edi
  mov eax, 2
  int $80
  ud2
end;

end.
