{ Phase 1 cooperative kernel threads, extended in Phase 2 with wait queues.
  This remains a single-CPU scheduler. No locks are required while IF=0. }

type
  TThreadEntry = procedure;
  TThreadState = (tsFree, tsRunnable, tsRunning, tsBlocked, tsTerminated);

  TKernelThread = record
    Id: LongWord;
    State: TThreadState;
    Entry: TThreadEntry;
    SavedStackPointer: QWord;
    WaitKey: QWord;
  end;

var
  ThreadTable: array[0..THREAD_MAX_COUNT - 1] of TKernelThread;
  ThreadStacks: array[1..THREAD_MAX_COUNT - 1, 0..THREAD_STACK_SIZE - 1] of Byte;
  CurrentThreadIndex: LongWord;
  NextThreadId: LongWord;

procedure ThreadYield; forward;
procedure ThreadBlockCurrent(WaitKey: QWord); forward;
function ThreadWakeWaiters(WaitKey: QWord; WakeAll: Boolean): LongWord; forward;

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
  ThreadId: LongWord;
begin
  Entry := ThreadTable[CurrentThreadIndex].Entry;
  ThreadId := ThreadTable[CurrentThreadIndex].Id;
  if Assigned(Entry) then
    Entry();

  WriteText('[THREAD] thread ');
  WriteDecimal(ThreadId);
  WriteText(' terminated.'#13#10);
  ThreadTable[CurrentThreadIndex].State := tsTerminated;
  ThreadTable[CurrentThreadIndex].WaitKey := 0;
  ThreadYield;
  HaltForever; { no runnable context may return to a terminated thread }
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
    ThreadTable[I].WaitKey := 0;
  end;

  NextThreadId := 1;
  CurrentThreadIndex := 0;
  ThreadTable[0].Id := NextThreadId;
  Inc(NextThreadId);
  ThreadTable[0].State := tsRunning;
  ThreadTable[0].Entry := nil;
  ThreadTable[0].WaitKey := 0;
  WriteText('[ OK ] Cooperative kernel-thread scheduler initialised (' );
  WriteDecimal(THREAD_MAX_COUNT);
  WriteText(' slots).'#13#10);
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
    StackWords[0] := 0; { r15 }
    StackWords[1] := 0; { r14 }
    StackWords[2] := 0; { r13 }
    StackWords[3] := 0; { r12 }
    StackWords[4] := 0; { rbp }
    StackWords[5] := 0; { rbx }
    StackWords[6] := PtrUInt(@ThreadBootstrap);

    ThreadTable[I].Id := NextThreadId;
    Inc(NextThreadId);
    if NextThreadId = 0 then
      NextThreadId := 1;
    ThreadTable[I].State := tsRunnable;
    ThreadTable[I].Entry := Entry;
    ThreadTable[I].SavedStackPointer := InitialSP;
    ThreadTable[I].WaitKey := 0;
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

procedure ThreadSchedule;
var
  OldIndex: LongWord;
  NextIndex: LongWord;
begin
  OldIndex := CurrentThreadIndex;
  if not FindNextRunnable(OldIndex, NextIndex) then
  begin
    if ThreadTable[OldIndex].State = tsRunnable then
    begin
      ThreadTable[OldIndex].State := tsRunning;
      Exit;
    end;
    { Phase 2 has no interrupt-driven wake source yet. If every thread is
      blocked/terminated, there is no event that can make progress. }
    HaltForever;
    Exit;
  end;

  ThreadTable[NextIndex].State := tsRunning;
  CurrentThreadIndex := NextIndex;
  ContextSwitch(@ThreadTable[OldIndex].SavedStackPointer,
                ThreadTable[NextIndex].SavedStackPointer);
end;

procedure ThreadYield;
begin
  if ThreadTable[CurrentThreadIndex].State = tsRunning then
    ThreadTable[CurrentThreadIndex].State := tsRunnable;
  ThreadSchedule;
end;

procedure ThreadBlockCurrent(WaitKey: QWord);
begin
  if WaitKey = 0 then
    Exit;
  ThreadTable[CurrentThreadIndex].WaitKey := WaitKey;
  ThreadTable[CurrentThreadIndex].State := tsBlocked;
  ThreadSchedule;
end;

function ThreadWakeWaiters(WaitKey: QWord; WakeAll: Boolean): LongWord;
var
  I: LongWord;
begin
  Result := 0;
  if WaitKey = 0 then
    Exit;
  for I := 0 to THREAD_MAX_COUNT - 1 do
    if (ThreadTable[I].State = tsBlocked) and
       (ThreadTable[I].WaitKey = WaitKey) then
    begin
      ThreadTable[I].WaitKey := 0;
      ThreadTable[I].State := tsRunnable;
      Inc(Result);
      if not WakeAll then
        Exit;
    end;
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
