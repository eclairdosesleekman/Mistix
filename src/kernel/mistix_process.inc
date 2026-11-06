{ Mistix Phase 3: process table and per-process capability namespaces. }

const
  PROCESS_MAX_COUNT = 8;
  PROCESS_INDEX_NONE = PROCESS_MAX_COUNT;

type
  PKernelProcess = ^TKernelProcess;

  TProcessState = (psFree, psReady, psRunning, psTerminated);

  TProcessCapabilitySpace = packed record
    Entries: array[0..CAPABILITY_MAX_SLOTS - 1] of TCapabilityEntry;
  end;

  TKernelProcess = record
    PID: QWord;
    State: TProcessState;
    AddressSpaceRoot: QWord;
    EntryPoint: QWord;
    UserStackPointer: QWord;
    ExitCode: QWord;
    Capabilities: TProcessCapabilitySpace;
  end;

var
  ProcessTable: array[0..PROCESS_MAX_COUNT - 1] of TKernelProcess;
  CurrentProcessIndex: LongWord;
  NextProcessId: QWord;

procedure EnterUserMode(EntryPoint, UserStack: QWord);
  assembler; nostackframe; forward;

var
  EmbeddedUserInitStart: Byte; external name '_binary_user_init_elf_start';
  EmbeddedUserInitEnd: Byte; external name '_binary_user_init_elf_end';

procedure GetEmbeddedInitImage(var ImageStart, ImageEnd: Pointer);
begin
  ImageStart := @EmbeddedUserInitStart;
  ImageEnd := @EmbeddedUserInitEnd;
end;

procedure ProcessCapabilitySpaceInitialize(var Space: TProcessCapabilitySpace);
var
  I: LongWord;
begin
  for I := 0 to CAPABILITY_MAX_SLOTS - 1 do
  begin
    Space.Entries[I].Active := 0;
    Space.Entries[I].ObjectKind := Ord(okNone);
    Space.Entries[I].ObjectIndex := 0;
    Space.Entries[I].Generation := 0;
    Space.Entries[I].ObjectGeneration := 0;
    Space.Entries[I].Rights := 0;
  end;
end;

function ProcessCapabilityInstall(var Space: TProcessCapabilitySpace;
  Kind: TObjectKind; ObjectIndex, ObjectGeneration: Word;
  Rights: LongWord; out Handle: LongWord): TKernelResult;
var
  I: LongWord;
  NewGeneration: Word;
begin
  Handle := 0;
  if (Kind = okNone) or (Rights = 0) then
  begin
    Result := krInvalidArgument;
    Exit;
  end;
  for I := 0 to CAPABILITY_MAX_SLOTS - 1 do
  begin
    if Space.Entries[I].Active <> 0 then Continue;
    if Space.Entries[I].Generation = High(Word) then Continue;
    NewGeneration := Space.Entries[I].Generation + 1;
    Space.Entries[I].Active := 1;
    Space.Entries[I].ObjectKind := Ord(Kind);
    Space.Entries[I].ObjectIndex := ObjectIndex;
    Space.Entries[I].Generation := NewGeneration;
    Space.Entries[I].ObjectGeneration := ObjectGeneration;
    Space.Entries[I].Rights := Rights;
    Handle := (LongWord(NewGeneration) shl 16) or (I + 1);
    Result := krSuccess;
    Exit;
  end;
  Result := krNoResources;
end;

function ProcessCapabilityLookup(var Space: TProcessCapabilitySpace;
  Handle, RequiredRights: LongWord; ExpectedKind: TObjectKind;
  out ObjectIndex, ObjectGeneration: Word): TKernelResult;
var
  SlotEncoded: LongWord;
  Slot: LongWord;
  Generation: Word;
  Entry: ^TCapabilityEntry;
begin
  ObjectIndex := 0;
  ObjectGeneration := 0;
  SlotEncoded := Handle and $FFFF;
  Generation := Word(Handle shr 16);
  if (SlotEncoded = 0) or (SlotEncoded > CAPABILITY_MAX_SLOTS) or
     (Generation = 0) then
  begin
    Result := krInvalidHandle;
    Exit;
  end;
  Slot := SlotEncoded - 1;
  Entry := @Space.Entries[Slot];
  if (Entry^.Active = 0) or (Entry^.Generation <> Generation) or
     (Entry^.ObjectKind <> Ord(ExpectedKind)) then
  begin
    Result := krInvalidHandle;
    Exit;
  end;
  if (Entry^.Rights and RequiredRights) <> RequiredRights then
  begin
    Result := krPermissionDenied;
    Exit;
  end;
  ObjectIndex := Entry^.ObjectIndex;
  ObjectGeneration := Entry^.ObjectGeneration;
  Result := krSuccess;
end;

function ProcessCapabilityRestrict(var Space: TProcessCapabilitySpace;
  Handle, NewRights: LongWord; out RestrictedHandle: LongWord): TKernelResult;
var
  SlotEncoded: LongWord;
  Slot: LongWord;
  Generation: Word;
  Entry: ^TCapabilityEntry;
begin
  RestrictedHandle := 0;
  SlotEncoded := Handle and $FFFF;
  Generation := Word(Handle shr 16);
  if (SlotEncoded = 0) or (SlotEncoded > CAPABILITY_MAX_SLOTS) or
     (Generation = 0) then
  begin
    Result := krInvalidHandle;
    Exit;
  end;
  Slot := SlotEncoded - 1;
  Entry := @Space.Entries[Slot];
  if (Entry^.Active = 0) or (Entry^.Generation <> Generation) then
  begin
    Result := krInvalidHandle;
    Exit;
  end;
  if (NewRights = 0) or ((NewRights and not Entry^.Rights) <> 0) then
  begin
    Result := krPermissionDenied;
    Exit;
  end;
  Result := ProcessCapabilityInstall(Space, TObjectKind(Entry^.ObjectKind),
    Entry^.ObjectIndex, Entry^.ObjectGeneration, NewRights, RestrictedHandle);
end;

function ProcessCapabilityClose(var Space: TProcessCapabilitySpace;
  Handle: LongWord): TKernelResult;
var
  SlotEncoded: LongWord;
  Slot: LongWord;
  Generation: Word;
begin
  SlotEncoded := Handle and $FFFF;
  Generation := Word(Handle shr 16);
  if (SlotEncoded = 0) or (SlotEncoded > CAPABILITY_MAX_SLOTS) or
     (Generation = 0) then
  begin
    Result := krInvalidHandle;
    Exit;
  end;
  Slot := SlotEncoded - 1;
  if (Space.Entries[Slot].Active = 0) or
     (Space.Entries[Slot].Generation <> Generation) then
  begin
    Result := krInvalidHandle;
    Exit;
  end;
  Space.Entries[Slot].Active := 0;
  Space.Entries[Slot].Rights := 0;
  Result := krSuccess;
end;

procedure ProcessManagerInitialize;
var
  I: LongWord;
begin
  for I := 0 to PROCESS_MAX_COUNT - 1 do
  begin
    ProcessTable[I].PID := 0;
    ProcessTable[I].State := psFree;
    ProcessTable[I].AddressSpaceRoot := 0;
    ProcessTable[I].EntryPoint := 0;
    ProcessTable[I].UserStackPointer := 0;
    ProcessTable[I].ExitCode := 0;
    ProcessCapabilitySpaceInitialize(ProcessTable[I].Capabilities);
  end;
  CurrentProcessIndex := PROCESS_INDEX_NONE;
  NextProcessId := 1;
  WriteText('[ OK ] Process manager initialised (' );
  WriteDecimal(PROCESS_MAX_COUNT);
  WriteText(' process slots; per-process capability spaces).'#13#10);
end;

function ProcessCapabilityIsolationSelfTest: Boolean;
var
  SpaceA: TProcessCapabilitySpace;
  SpaceB: TProcessCapabilitySpace;
  HandleA: LongWord;
  ObjectIndex: Word;
  ObjectGeneration: Word;
begin
  Result := False;
  ProcessCapabilitySpaceInitialize(SpaceA);
  ProcessCapabilitySpaceInitialize(SpaceB);
  if ProcessCapabilityInstall(SpaceA, okEndpoint, 3, 1,
       CAP_RIGHT_SEND, HandleA) <> krSuccess then Exit;
  if ProcessCapabilityLookup(SpaceA, HandleA, CAP_RIGHT_SEND,
       okEndpoint, ObjectIndex, ObjectGeneration) <> krSuccess then Exit;
  if ProcessCapabilityLookup(SpaceB, HandleA, CAP_RIGHT_SEND,
       okEndpoint, ObjectIndex, ObjectGeneration) <> krInvalidHandle then Exit;
  Result := True;
  WriteText('[ OK ] Per-process capability isolation self-test passed.'#13#10);
end;

function ProcessCreateFromELF(Image: Pointer; ImageSize: QWord;
  out PID: QWord): Boolean;
var
  I: LongWord;
  Slot: LongWord;
  RootPhysical: QWord;
  Entry: QWord;
  UserStack: QWord;
begin
  Result := False;
  PID := 0;
  Slot := PROCESS_INDEX_NONE;
  for I := 0 to PROCESS_MAX_COUNT - 1 do
    if ProcessTable[I].State = psFree then
    begin
      Slot := I;
      Break;
    end;
  if Slot = PROCESS_INDEX_NONE then Exit;

  if not AddressSpaceCreate(RootPhysical) then Exit;
  if not AddressSpaceLoadELF64(RootPhysical, Image, ImageSize, Entry) then
  begin
    AddressSpaceDestroy(RootPhysical);
    WriteText('[FAIL] ELF64 loader rejected the initial user image.'#13#10);
    Exit;
  end;
  if not AddressSpaceCreateUserStack(RootPhysical, UserStack) then
  begin
    AddressSpaceDestroy(RootPhysical);
    WriteText('[FAIL] Could not create the initial user stack.'#13#10);
    Exit;
  end;

  ProcessTable[Slot].PID := NextProcessId;
  Inc(NextProcessId);
  if NextProcessId = 0 then NextProcessId := 1;
  ProcessTable[Slot].State := psReady;
  ProcessTable[Slot].AddressSpaceRoot := RootPhysical;
  ProcessTable[Slot].EntryPoint := Entry;
  ProcessTable[Slot].UserStackPointer := UserStack;
  ProcessTable[Slot].ExitCode := 0;
  ProcessCapabilitySpaceInitialize(ProcessTable[Slot].Capabilities);
  PID := ProcessTable[Slot].PID;
  WriteText('[ OK ] Loaded ELF64 user process pid=');
  WriteDecimal(PID);
  WriteText(' entry=');
  WriteHex64(Entry);
  WriteText(' CR3=');
  WriteHex64(RootPhysical);
  WriteText(#13#10);
  Result := True;
end;

function ProcessDestroy(PID: QWord): Boolean;
var
  I: LongWord;
  RootPhysical: QWord;
begin
  Result := False;
  for I := 0 to PROCESS_MAX_COUNT - 1 do
    if (ProcessTable[I].State <> psFree) and
       (ProcessTable[I].PID = PID) then
    begin
      if ProcessTable[I].State = psRunning then Exit;
      RootPhysical := ProcessTable[I].AddressSpaceRoot;
      if not AddressSpaceDestroy(RootPhysical) then Exit;
      ProcessTable[I].State := psFree;
      ProcessTable[I].PID := 0;
      ProcessTable[I].AddressSpaceRoot := 0;
      ProcessTable[I].EntryPoint := 0;
      ProcessTable[I].UserStackPointer := 0;
      ProcessCapabilitySpaceInitialize(ProcessTable[I].Capabilities);
      Result := True;
      Exit;
    end;
end;

function ProcessFindByPID(PID: QWord; out ProcessIndex: LongWord): Boolean;
var
  I: LongWord;
begin
  Result := False;
  ProcessIndex := PROCESS_INDEX_NONE;
  for I := 0 to PROCESS_MAX_COUNT - 1 do
    if (ProcessTable[I].State <> psFree) and
       (ProcessTable[I].PID = PID) then
    begin
      ProcessIndex := I;
      Result := True;
      Exit;
    end;
end;

function AddressSpaceActivate(RootPhysical: QWord): Boolean;
begin
  Result := False;
  if (RootPhysical = 0) or ((RootPhysical and (PAGE_SIZE - 1)) <> 0) then Exit;
  WriteCR3(RootPhysical);
  Result := (ReadCR3 and PTE_ADDRESS_MASK) = RootPhysical;
end;

procedure ProcessRunInternal(PID: QWord); cdecl;
  public name 'mistix_process_run_internal';
var
  Index: LongWord;
  RootPhysical: QWord;
  Entry: QWord;
  UserStack: QWord;
begin
  if not ProcessFindByPID(PID, Index) then
  begin
    WriteText('[FAIL] ProcessRun: unknown PID.'#13#10);
    HaltForever;
  end;
  if ProcessTable[Index].State <> psReady then
  begin
    WriteText('[FAIL] ProcessRun: process is not ready.'#13#10);
    HaltForever;
  end;

  RootPhysical := ProcessTable[Index].AddressSpaceRoot;
  Entry := ProcessTable[Index].EntryPoint;
  UserStack := ProcessTable[Index].UserStackPointer;
  ProcessTable[Index].State := psRunning;
  CurrentProcessIndex := Index;
  TaskState.RSP0 := StackTopAligned(PtrUInt(@KernelTransitionStack[0]),
                                    SizeOf(KernelTransitionStack));
  if not AddressSpaceActivate(RootPhysical) then
  begin
    ProcessTable[Index].State := psReady;
    CurrentProcessIndex := PROCESS_INDEX_NONE;
    WriteText('[FAIL] Could not activate process CR3.'#13#10);
    HaltForever;
  end;

  WriteText('[ OK ] Entering ring 3: pid=');
  WriteDecimal(PID);
  WriteText(' CS=0x43 SS=0x3B.'#13#10);
  EnterUserMode(Entry, UserStack);
  HaltForever;
end;

procedure ProcessRunOnDedicatedStack(NewStackTop, PID: QWord);
  assembler; nostackframe;
asm
  mov rsp, rdi
  and rsp, -16
  mov rdi, rsi
  call mistix_process_run_internal
@process_run_halt:
  cli
  hlt
  jmp @process_run_halt
end;

procedure ProcessRun(PID: QWord);
var
  StackTop: QWord;
begin
  StackTop := StackTopAligned(PtrUInt(@KernelTransitionStack[0]),
                              SizeOf(KernelTransitionStack));
  { Abandon Limine's entry stack before changing CR3. The current kernel
    stack and TSS.RSP0 now share this known, supervisor-mapped stack region;
    iretq abandons it before the first ring-3 syscall reuses its top. }
  ProcessRunOnDedicatedStack(StackTop, PID);
  HaltForever;
end;
