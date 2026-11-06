type
  TThreadEntry = procedure;
  TThreadState = (tsFree, tsRunnable, tsRunning, tsTerminated);

  TKernelThread = record
    Id: LongWord;
    State: TThreadState;
    Entry: TThreadEntry;
    SavedStackPointer: QWord;
  end;

var
  ThreadTable: array[0..THREAD_MAX_COUNT - 1] of TKernelThread;
  ThreadStacks: array[1..THREAD_MAX_COUNT - 1, 0..THREAD_STACK_SIZE - 1] of Byte;
  CurrentThreadIndex: LongWord;
  NextThreadId: LongWord;

procedure ThreadYield; forward;

procedure ContextSwitch(OldStackPointer: PQWord; NewStackPointer: QWord);
  cdecl; assembler; nostackframe; public name 'mistix_context_switch';
asm
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  mov [rdi], rsp
  mov rsp, rsi
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret
end;

procedure ThreadBootstrap; cdecl;
var
  Entry: TThreadEntry;
begin
  Entry := ThreadTable[CurrentThreadIndex].Entry;
  if Assigned(Entry) then
    Entry();

  WriteText('[THREAD] thread ');
  WriteDecimal(ThreadTable[CurrentThreadIndex].Id);
  WriteText(' terminated.'#13#10);
  ThreadTable[CurrentThreadIndex].State := tsTerminated;
  ThreadYield;
  HaltForever; { unreachable unless scheduler invariants are broken }
end;

procedure ThreadsInitialize;
var
  I: LongWord;
begin
  for I := 0 to THREAD_MAX_COUNT - 1 do
  begin
    ThreadTable[I].Id := 0;
    ThreadTable[I].State := tsFree;
    ThreadTable[I].Entry := nil;
    ThreadTable[I].SavedStackPointer := 0;
  end;

  NextThreadId := 1;
  CurrentThreadIndex := 0;
  ThreadTable[0].Id := NextThreadId;
  Inc(NextThreadId);
  ThreadTable[0].State := tsRunning;
  ThreadTable[0].Entry := nil; { bootstrap/main thread uses Limine's stack }
  WriteText('[ OK ] Cooperative kernel-thread scheduler initialised.'#13#10);
end;

function ThreadCreate(Entry: TThreadEntry; var ThreadId: LongWord): Boolean;
var
  I: LongWord;
  Top: QWord;
  InitialSP: QWord;
  StackWords: PQWord;
begin
  Result := False;
  ThreadId := 0;
  if not Assigned(Entry) then
    Exit;

  for I := 1 to THREAD_MAX_COUNT - 1 do
  begin
    if ThreadTable[I].State <> tsFree then
      Continue;

    Top := StackTopAligned(PtrUInt(@ThreadStacks[I, 0]), THREAD_STACK_SIZE);
    Dec(Top, 8); { SysV AMD64 function-entry RSP must be 8 mod 16. }
    InitialSP := Top - (7 * SizeOf(QWord));
    StackWords := PQWord(PtrUInt(InitialSP));

    { ContextSwitch pops r15,r14,r13,r12,rbp,rbx and RETs to bootstrap. }
    StackWords^[0] := 0; { r15 }
    StackWords^[1] := 0; { r14 }
    StackWords^[2] := 0; { r13 }
    StackWords^[3] := 0; { r12 }
    StackWords^[4] := 0; { rbp }
    StackWords^[5] := 0; { rbx }
    StackWords^[6] := PtrUInt(@ThreadBootstrap);

    ThreadTable[I].Id := NextThreadId;
    Inc(NextThreadId);
    ThreadTable[I].State := tsRunnable;
    ThreadTable[I].Entry := Entry;
    ThreadTable[I].SavedStackPointer := InitialSP;
    ThreadId := ThreadTable[I].Id;
    Result := True;
    Exit;
  end;
end;

function FindNextRunnable(FromIndex: LongWord; var NextIndex: LongWord): Boolean;
var
  Step: LongWord;
  Candidate: LongWord;
begin
  Result := False;
  NextIndex := FromIndex;
  for Step := 1 to THREAD_MAX_COUNT - 1 do
  begin
    Candidate := (FromIndex + Step) mod THREAD_MAX_COUNT;
    if ThreadTable[Candidate].State = tsRunnable then
    begin
      NextIndex := Candidate;
      Result := True;
      Exit;
    end;
  end;
end;

procedure ThreadYield;
var
  OldIndex: LongWord;
  NextIndex: LongWord;
begin
  OldIndex := CurrentThreadIndex;
  if ThreadTable[OldIndex].State = tsRunning then
    ThreadTable[OldIndex].State := tsRunnable;

  if not FindNextRunnable(OldIndex, NextIndex) then
  begin
    if ThreadTable[OldIndex].State = tsRunnable then
      ThreadTable[OldIndex].State := tsRunning;
    Exit;
  end;

  ThreadTable[NextIndex].State := tsRunning;
  CurrentThreadIndex := NextIndex;
  ContextSwitch(@ThreadTable[OldIndex].SavedStackPointer,
                ThreadTable[NextIndex].SavedStackPointer);
end;

function HasRunnableWorker: Boolean;
var
  I: LongWord;
begin
  Result := False;
  for I := 1 to THREAD_MAX_COUNT - 1 do
    if ThreadTable[I].State = tsRunnable then
    begin
      Result := True;
      Exit;
    end;
end;

procedure ThreadsRunUntilIdle;
begin
  while HasRunnableWorker do
    ThreadYield;
end;
