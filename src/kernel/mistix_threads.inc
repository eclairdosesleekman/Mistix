{ Phase 20 scheduler foundation. UP-only priority Round-Robin kernel threads. }
const
  THREAD_PRIORITY_LEVELS = 32;
  THREAD_PRIORITY_DEFAULT = 16;
  THREAD_PRIORITY_LOWEST = 0;
  THREAD_PRIORITY_HIGHEST = THREAD_PRIORITY_LEVELS - 1;
  THREAD_IDLE_INDEX = THREAD_MAX_COUNT - 1;
type
  TThreadEntry = procedure;
  TThreadInitialStack = array[0..6] of QWord;
  PThreadInitialStack = ^TThreadInitialStack;
  TThreadState = (tsFree, tsRunnable, tsRunning, tsBlocked, tsTerminated);
  TKernelThread = record
    Id: LongWord;
    State: TThreadState;
    Entry: TThreadEntry;
    SavedStackPointer: QWord;
    WaitKey: QWord;
    Priority: Byte;
    ReadySequence: QWord;
  end;
var
  ThreadTable: array[0..THREAD_MAX_COUNT - 1] of TKernelThread;
  ThreadStacks: array[1..THREAD_IDLE_INDEX - 1, 0..THREAD_STACK_SIZE - 1] of Byte;
  ThreadIdleStack: array[0..THREAD_STACK_SIZE - 1] of Byte;
  CurrentThreadIndex: LongWord;
  NextThreadId: LongWord;
  ThreadReadySequence: QWord;
procedure ThreadYield; forward;
procedure ThreadBlockCurrent(WaitKey: QWord); forward;
function ThreadWakeWaiters(WaitKey: QWord; WakeAll: Boolean): LongWord; forward;
procedure ThreadIdleHalt; assembler; nostackframe;
asm
  sti
  hlt
end;
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
var Entry: TThreadEntry; ThreadId: LongWord;
begin
  Entry := ThreadTable[CurrentThreadIndex].Entry;
  ThreadId := ThreadTable[CurrentThreadIndex].Id;
  if Assigned(Entry) then Entry();
  WriteText('[THREAD] thread '); WriteDecimal(ThreadId);
  WriteText(' terminated.'#13#10);
  ThreadTable[CurrentThreadIndex].State := tsTerminated;
  ThreadTable[CurrentThreadIndex].WaitKey := 0;
  ThreadYield;
  HaltForever;
end;
procedure ThreadPrepareContext(Index: LongWord; StackBase: PtrUInt);
var Top, InitialSP: QWord; StackWords: PThreadInitialStack;
begin
  Top := StackTopAligned(StackBase, THREAD_STACK_SIZE);
  Dec(Top, 8);
  InitialSP := Top - (7 * SizeOf(QWord));
  StackWords := PThreadInitialStack(PtrUInt(InitialSP));
  StackWords^[0] := 0; StackWords^[1] := 0; StackWords^[2] := 0;
  StackWords^[3] := 0; StackWords^[4] := 0; StackWords^[5] := 0;
  StackWords^[6] := PtrUInt(@ThreadBootstrap);
  ThreadTable[Index].SavedStackPointer := InitialSP;
end;
procedure ThreadIdleEntry;
begin
  while True do
  begin
    ThreadIdleHalt;
    ThreadSchedule;
  end;
end;
procedure ThreadsInitialize;
var I: LongWord;
begin
  for I := 0 to THREAD_MAX_COUNT - 1 do
  begin
    ThreadTable[I].Id := 0; ThreadTable[I].State := tsFree;
    ThreadTable[I].Entry := nil; ThreadTable[I].SavedStackPointer := 0;
    ThreadTable[I].WaitKey := 0; ThreadTable[I].Priority := THREAD_PRIORITY_DEFAULT;
    ThreadTable[I].ReadySequence := 0;
  end;
  NextThreadId := 1; ThreadReadySequence := 1; CurrentThreadIndex := 0;
  ThreadTable[0].Id := NextThreadId; Inc(NextThreadId);
  ThreadTable[0].State := tsRunning;
  ThreadTable[THREAD_IDLE_INDEX].State := tsRunnable;
  ThreadTable[THREAD_IDLE_INDEX].Entry := @ThreadIdleEntry;
  ThreadTable[THREAD_IDLE_INDEX].Priority := THREAD_PRIORITY_LOWEST;
  ThreadPrepareContext(THREAD_IDLE_INDEX, PtrUInt(@ThreadIdleStack[0]));
  WriteText('[ OK ] Phase 20 UP scheduler initialised (32 priorities, idle thread, ');
  WriteDecimal(THREAD_MAX_COUNT - 2); WriteText(' worker slots).'#13#10);
end;
function ThreadCreatePriority(Entry: TThreadEntry; Priority: LongInt;
  var ThreadId: LongWord): Boolean;
var I: LongWord;
begin
  Result := False; ThreadId := 0;
  if not Assigned(Entry) or (Priority < THREAD_PRIORITY_LOWEST) or
     (Priority > THREAD_PRIORITY_HIGHEST) then Exit;
  for I := 1 to THREAD_IDLE_INDEX - 1 do
  begin
    if ThreadTable[I].State <> tsFree then Continue;
    ThreadPrepareContext(I, PtrUInt(@ThreadStacks[I, 0]));
    ThreadTable[I].Id := NextThreadId; Inc(NextThreadId);
    if NextThreadId = 0 then NextThreadId := 1;
    ThreadTable[I].State := tsRunnable; ThreadTable[I].Entry := Entry;
    ThreadTable[I].WaitKey := 0; ThreadTable[I].Priority := Priority;
    ThreadTable[I].ReadySequence := ThreadReadySequence; Inc(ThreadReadySequence);
    if ThreadReadySequence = 0 then ThreadReadySequence := 1;
    ThreadId := ThreadTable[I].Id; Result := True; Exit;
  end;
end;
function ThreadCreate(Entry: TThreadEntry; var ThreadId: LongWord): Boolean;
begin Result := ThreadCreatePriority(Entry, THREAD_PRIORITY_DEFAULT, ThreadId); end;
function ThreadSetPriority(ThreadId: LongWord; Priority: LongInt): Boolean;
var I: LongWord;
begin
  Result := False;
  if (Priority < THREAD_PRIORITY_LOWEST) or (Priority > THREAD_PRIORITY_HIGHEST) then Exit;
  for I := 0 to THREAD_IDLE_INDEX - 1 do
    if (ThreadTable[I].State <> tsFree) and (ThreadTable[I].Id = ThreadId) then
    begin
      ThreadTable[I].Priority := Priority;
      if ThreadTable[I].State = tsRunnable then
      begin
        ThreadTable[I].ReadySequence := ThreadReadySequence; Inc(ThreadReadySequence);
      end;
      Result := True; Exit;
    end;
end;
function FindNextRunnable(FromIndex: LongWord; var NextIndex: LongWord): Boolean;
var I: LongWord; BestPriority: LongInt; BestSequence: QWord;
begin
  Result := False; NextIndex := THREAD_IDLE_INDEX;
  BestPriority := -1; BestSequence := High(QWord);
  for I := 0 to THREAD_IDLE_INDEX - 1 do
    if (I <> FromIndex) and (ThreadTable[I].State = tsRunnable) then
      if (LongInt(ThreadTable[I].Priority) > BestPriority) or
         ((LongInt(ThreadTable[I].Priority) = BestPriority) and
          (ThreadTable[I].ReadySequence < BestSequence)) then
      begin
        BestPriority := ThreadTable[I].Priority;
        BestSequence := ThreadTable[I].ReadySequence;
        NextIndex := I; Result := True;
      end;
  if (not Result) and (FromIndex < THREAD_IDLE_INDEX) and
     (ThreadTable[FromIndex].State = tsRunnable) then
  begin NextIndex := FromIndex; Result := True; end;
end;
procedure ThreadSchedule;
var OldIndex, NextIndex: LongWord; Found: Boolean;
begin
  OldIndex := CurrentThreadIndex;
  repeat
    Found := FindNextRunnable(OldIndex, NextIndex);
    if Found then Break;
    if OldIndex = THREAD_IDLE_INDEX then ThreadIdleHalt
    else begin NextIndex := THREAD_IDLE_INDEX; Break; end;
  until False;
  if NextIndex = OldIndex then
  begin
    if OldIndex = THREAD_IDLE_INDEX then ThreadIdleHalt;
    if ThreadTable[OldIndex].State = tsRunnable then ThreadTable[OldIndex].State := tsRunning;
    Exit;
  end;
  if NextIndex <> THREAD_IDLE_INDEX then ThreadTable[NextIndex].State := tsRunning;
  CurrentThreadIndex := NextIndex;
  ContextSwitch(@ThreadTable[OldIndex].SavedStackPointer,
                ThreadTable[NextIndex].SavedStackPointer);
end;
procedure ThreadYield;
begin
  if CurrentThreadIndex >= THREAD_MAX_COUNT then Exit;
  if CurrentThreadIndex <> THREAD_IDLE_INDEX then
  begin
    if ThreadTable[CurrentThreadIndex].State = tsRunning then
      ThreadTable[CurrentThreadIndex].State := tsRunnable;
    if ThreadTable[CurrentThreadIndex].State = tsRunnable then
    begin
      ThreadTable[CurrentThreadIndex].ReadySequence := ThreadReadySequence;
      Inc(ThreadReadySequence);
    end;
  end;
  ThreadSchedule;
end;
procedure ThreadBlockCurrent(WaitKey: QWord);
begin
  if (WaitKey = 0) or (CurrentThreadIndex >= THREAD_IDLE_INDEX) then Exit;
  ThreadTable[CurrentThreadIndex].WaitKey := WaitKey;
  ThreadTable[CurrentThreadIndex].State := tsBlocked;
  ThreadSchedule;
end;
function ThreadWakeWaiters(WaitKey: QWord; WakeAll: Boolean): LongWord;
var I: LongWord;
begin
  Result := 0; if WaitKey = 0 then Exit;
  for I := 0 to THREAD_IDLE_INDEX - 1 do
    if (ThreadTable[I].State = tsBlocked) and (ThreadTable[I].WaitKey = WaitKey) then
    begin
      ThreadTable[I].WaitKey := 0; ThreadTable[I].State := tsRunnable;
      ThreadTable[I].ReadySequence := ThreadReadySequence; Inc(ThreadReadySequence);
      Inc(Result); if not WakeAll then Exit;
    end;
end;
function ThreadWake(ThreadId: LongWord): Boolean;
var I: LongWord;
begin
  Result := False;
  for I := 0 to THREAD_IDLE_INDEX - 1 do
    if (ThreadTable[I].Id = ThreadId) and (ThreadTable[I].State = tsBlocked) then
    begin
      ThreadTable[I].WaitKey := 0; ThreadTable[I].State := tsRunnable;
      ThreadTable[I].ReadySequence := ThreadReadySequence; Inc(ThreadReadySequence);
      Result := True; Exit;
    end;
end;
function HasRunnableWorker: Boolean;
var I: LongWord;
begin
  Result := False;
  for I := 0 to THREAD_IDLE_INDEX - 1 do
    if ThreadTable[I].State = tsRunnable then begin Result := True; Exit; end;
end;
procedure ThreadsRunUntilIdle;
begin while HasRunnableWorker do ThreadYield; end;
