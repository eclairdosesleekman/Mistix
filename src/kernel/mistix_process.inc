{ Mistix Phase 4: independent process contexts, lifecycle, fork/spawn/wait,
  per-process capability spaces and user-visible endpoint operations. }

const
  PROCESS_MAX_COUNT = 8;
  PROCESS_INDEX_NONE = PROCESS_MAX_COUNT;
  PROCESS_KERNEL_STACK_SIZE = 32768;
  PROCESS_MAX_USER_IMAGE_SIZE = 524288;
  PROCESS_PENDING_CAPS = 8;

type
  PKernelProcess = ^TKernelProcess;
  TProcessState = (psFree, psReady, psRunning, psWaiting, psZombie);

  TProcessCapabilitySpace = packed record
    Entries: array[0..CAPABILITY_MAX_SLOTS - 1] of TCapabilityEntry;
  end;

  TKernelProcess = record
    PID: QWord;
    ParentPID: QWord;
    State: TProcessState;
    AddressSpaceRoot: QWord;
    EntryPoint: QWord;
    UserStackPointer: QWord;
    KernelStackTop: QWord;
    SavedFrame: QWord;
    ExitCode: QWord;
    WaitTargetPID: QWord;
    WaitStatusAddress: QWord;
    ReapRequested: Byte;
    PendingCapabilityCount: Byte;
    PendingCapabilityHandles: array[0..PROCESS_PENDING_CAPS - 1] of LongWord;
    Capabilities: TProcessCapabilitySpace;
    Files: TVFSProcessFiles;
    CommandLine: array[0..VFS_PATH_MAX] of Char;
  end;

var
  ProcessTable: array[0..PROCESS_MAX_COUNT - 1] of TKernelProcess;
  ProcessKernelStacks: array[0..PROCESS_MAX_COUNT - 1,
    0..PROCESS_KERNEL_STACK_SIZE - 1] of Byte;
  ProcessUserImageScratch: array[0..PROCESS_MAX_USER_IMAGE_SIZE - 1] of Byte;
  CurrentProcessIndex: LongWord;
  NextProcessId: QWord;
  ProcessScheduleCount: QWord;

procedure ProcessRestoreFrame(FrameAddress: QWord);
  assembler; nostackframe; public name 'mistix_process_restore_frame';
asm
  mov rsp, rdi
  pop r15
  pop r14
  pop r13
  pop r12
  pop r11
  pop r10
  pop r9
  pop r8
  pop rdi
  pop rsi
  pop rbp
  pop rdx
  pop rcx
  pop rbx
  pop rax
  add rsp, 16
  iretq
end;

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

procedure ProcessCapabilitySpaceCopy(var Destination: TProcessCapabilitySpace;
  const Source: TProcessCapabilitySpace);
var
  I: LongWord;
begin
  for I := 0 to CAPABILITY_MAX_SLOTS - 1 do
  begin
    Destination.Entries[I].Active := Source.Entries[I].Active;
    Destination.Entries[I].ObjectKind := Source.Entries[I].ObjectKind;
    Destination.Entries[I].ObjectIndex := Source.Entries[I].ObjectIndex;
    Destination.Entries[I].Generation := Source.Entries[I].Generation;
    Destination.Entries[I].ObjectGeneration := Source.Entries[I].ObjectGeneration;
    Destination.Entries[I].Rights := Source.Entries[I].Rights;
  end;
end;

procedure ProcessCopyIPCMessage(var Destination: TIPCMessage;
  const Source: TIPCMessage);
var
  I: LongWord;
begin
  Destination.MessageId := Source.MessageId;
  Destination.WordCount := Source.WordCount;
  for I := 0 to 6 do
    Destination.Reserved[I] := Source.Reserved[I];
  for I := 0 to IPC_MAX_WORDS - 1 do
    Destination.Words[I] := Source.Words[I];
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

procedure ProcessInitializeFrame(ProcessIndex: LongWord);
var
  Top: QWord;
  FrameAddress: QWord;
  Frame: PInterruptFrame;
  Tail: PQWord;
begin
  Top := ProcessTable[ProcessIndex].KernelStackTop;
  FrameAddress := Top - SizeOf(TInterruptFrame) - (2 * SizeOf(QWord));
  Frame := PInterruptFrame(PtrUInt(FrameAddress));
  Frame^.SavedR15 := 0;
  Frame^.SavedR14 := 0;
  Frame^.SavedR13 := 0;
  Frame^.SavedR12 := 0;
  Frame^.SavedR11 := 0;
  Frame^.SavedR10 := 0;
  Frame^.SavedR9 := 0;
  Frame^.SavedR8 := 0;
  Frame^.SavedRDI := 0;
  Frame^.SavedRSI := 0;
  Frame^.SavedRBP := 0;
  Frame^.SavedRDX := 0;
  Frame^.SavedRCX := 0;
  Frame^.SavedRBX := 0;
  Frame^.SavedRAX := 0;
  Frame^.Vector := 0;
  Frame^.ErrorCode := 0;
  Frame^.RIP := ProcessTable[ProcessIndex].EntryPoint;
  Frame^.CS := $43;
  Frame^.RFLAGS := $202; { bit 1 and IF; IOPL, DF, TF remain clear }
  Tail := PQWord(PtrUInt(FrameAddress + SizeOf(TInterruptFrame)));
  Tail^ := ProcessTable[ProcessIndex].UserStackPointer;
  Inc(Tail);
  Tail^ := $3B;
  ProcessTable[ProcessIndex].SavedFrame := FrameAddress;
end;

procedure ProcessManagerInitialize;
var
  I: LongWord;
  J: LongWord;
begin
  for I := 0 to PROCESS_MAX_COUNT - 1 do
  begin
    ProcessTable[I].PID := 0;
    ProcessTable[I].ParentPID := 0;
    ProcessTable[I].State := psFree;
    ProcessTable[I].AddressSpaceRoot := 0;
    ProcessTable[I].EntryPoint := 0;
    ProcessTable[I].UserStackPointer := 0;
    ProcessTable[I].KernelStackTop := StackTopAligned(
      PtrUInt(@ProcessKernelStacks[I, 0]), PROCESS_KERNEL_STACK_SIZE);
    ProcessTable[I].SavedFrame := 0;
    ProcessTable[I].ExitCode := 0;
    ProcessTable[I].WaitTargetPID := 0;
    ProcessTable[I].WaitStatusAddress := 0;
    ProcessTable[I].ReapRequested := 0;
    ProcessTable[I].PendingCapabilityCount := 0;
    for J := 0 to PROCESS_PENDING_CAPS - 1 do
      ProcessTable[I].PendingCapabilityHandles[J] := 0;
    ProcessCapabilitySpaceInitialize(ProcessTable[I].Capabilities);
  end;
  CurrentProcessIndex := PROCESS_INDEX_NONE;
  NextProcessId := 1;
  ProcessScheduleCount := 0;
  WriteText('[ OK ] Process manager initialised (' );
  WriteDecimal(PROCESS_MAX_COUNT);
  WriteText(' process slots; dedicated kernel stacks; per-process capability spaces).'#13#10);
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

function ProcessFindFreeSlot(out ProcessIndex: LongWord): Boolean;
var
  I: LongWord;
begin
  Result := False;
  ProcessIndex := PROCESS_INDEX_NONE;
  for I := 0 to PROCESS_MAX_COUNT - 1 do
    if ProcessTable[I].State = psFree then
    begin
      ProcessIndex := I;
      Result := True;
      Exit;
    end;
end;

function ProcessCreateFromELF(Image: Pointer; ImageSize, ParentPID: QWord;
  out PID: QWord): Boolean;
var
  Slot: LongWord;
  RootPhysical: QWord;
  Entry: QWord;
  UserStack: QWord;
begin
  Result := False;
  PID := 0;
  if (Image = nil) or (ImageSize = 0) or
     (ImageSize > PROCESS_MAX_USER_IMAGE_SIZE) then Exit;
  if not ProcessFindFreeSlot(Slot) then Exit;

  if not AddressSpaceCreate(RootPhysical) then Exit;
  if not AddressSpaceLoadELF64(RootPhysical, Image, ImageSize, Entry) then
  begin
    AddressSpaceDestroy(RootPhysical);
    WriteText('[FAIL] ELF64 loader rejected a user process image.'#13#10);
    Exit;
  end;
  if not AddressSpaceCreateUserStack(RootPhysical, UserStack) then
  begin
    AddressSpaceDestroy(RootPhysical);
    WriteText('[FAIL] Could not create a user stack.'#13#10);
    Exit;
  end;

  ProcessTable[Slot].PID := NextProcessId;
  Inc(NextProcessId);
  if NextProcessId = 0 then NextProcessId := 1;
  ProcessTable[Slot].ParentPID := ParentPID;
  ProcessTable[Slot].State := psReady;
  ProcessTable[Slot].AddressSpaceRoot := RootPhysical;
  ProcessTable[Slot].EntryPoint := Entry;
  ProcessTable[Slot].UserStackPointer := UserStack;
  ProcessTable[Slot].ExitCode := 0;
  ProcessTable[Slot].WaitTargetPID := 0;
  ProcessTable[Slot].WaitStatusAddress := 0;
  ProcessTable[Slot].ReapRequested := 0;
  ProcessTable[Slot].PendingCapabilityCount := 0;
  ProcessCapabilitySpaceInitialize(ProcessTable[Slot].Capabilities);
  VFSProcessInitialize(ProcessTable[Slot].Files);
  ProcessTable[Slot].CommandLine[0] := #0;
  ProcessInitializeFrame(Slot);
  PID := ProcessTable[Slot].PID;

  WriteText('[ OK ] Created user process pid=');
  WriteDecimal(PID);
  WriteText(' parent=');
  WriteDecimal(ParentPID);
  WriteText(' entry=');
  WriteHex64(Entry);
  WriteText(' CR3=');
  WriteHex64(RootPhysical);
  WriteText(#13#10);
  Result := True;
end;

procedure ProcessFinalizeSlot(ProcessIndex: LongWord);
var
  RootPhysical: QWord;
begin
  if ProcessIndex >= PROCESS_MAX_COUNT then Exit;
  if ProcessTable[ProcessIndex].State = psFree then Exit;
  if ProcessTable[ProcessIndex].State <> psZombie then Exit;
  RootPhysical := ProcessTable[ProcessIndex].AddressSpaceRoot;
  VFSProcessCloseAll(ProcessTable[ProcessIndex].Files);
  if (RootPhysical <> 0) and not AddressSpaceDestroy(RootPhysical) then
  begin
    WriteText('[FATAL] Could not reclaim zombie process address space.'#13#10);
    HaltForever;
  end;
  ProcessTable[ProcessIndex].PID := 0;
  ProcessTable[ProcessIndex].ParentPID := 0;
  ProcessTable[ProcessIndex].State := psFree;
  ProcessTable[ProcessIndex].AddressSpaceRoot := 0;
  ProcessTable[ProcessIndex].EntryPoint := 0;
  ProcessTable[ProcessIndex].UserStackPointer := 0;
  ProcessTable[ProcessIndex].SavedFrame := 0;
  ProcessTable[ProcessIndex].ExitCode := 0;
  ProcessTable[ProcessIndex].WaitTargetPID := 0;
  ProcessTable[ProcessIndex].WaitStatusAddress := 0;
  ProcessTable[ProcessIndex].ReapRequested := 0;
  ProcessTable[ProcessIndex].PendingCapabilityCount := 0;
  ProcessTable[ProcessIndex].CommandLine[0] := #0;
  ProcessCapabilitySpaceInitialize(ProcessTable[ProcessIndex].Capabilities);
end;

procedure ProcessWakeParentOnExit(ChildIndex: LongWord);
var
  ParentIndex: LongWord;
  ParentFrame: PInterruptFrame;
  ParentPID: QWord;
  ChildPID: QWord;
  StatusAddress: QWord;
begin
  if ChildIndex >= PROCESS_MAX_COUNT then Exit;
  ParentPID := ProcessTable[ChildIndex].ParentPID;
  ChildPID := ProcessTable[ChildIndex].PID;
  if (ParentPID = 0) or not ProcessFindByPID(ParentPID, ParentIndex) then Exit;
  if ProcessTable[ParentIndex].State <> psWaiting then Exit;
  if (ProcessTable[ParentIndex].WaitTargetPID <> 0) and
     (ProcessTable[ParentIndex].WaitTargetPID <> ChildPID) then Exit;
  if ProcessTable[ParentIndex].SavedFrame = 0 then Exit;
  ParentFrame := PInterruptFrame(PtrUInt(ProcessTable[ParentIndex].SavedFrame));
  StatusAddress := ProcessTable[ParentIndex].WaitStatusAddress;
  if (StatusAddress <> 0) and
     not AddressSpaceCopyTo(ProcessTable[ParentIndex].AddressSpaceRoot,
       StatusAddress, @ProcessTable[ChildIndex].ExitCode, SizeOf(QWord)) then
  begin
    ParentFrame^.SavedRAX := QWord(-14); { -EFAULT; leave child zombie for retry }
    ProcessTable[ChildIndex].ReapRequested := 0;
  end
  else
  begin
    ParentFrame^.SavedRAX := ChildPID;
    ProcessTable[ChildIndex].ReapRequested := 1;
  end;
  ProcessTable[ParentIndex].WaitTargetPID := 0;
  ProcessTable[ParentIndex].WaitStatusAddress := 0;
  ProcessTable[ParentIndex].State := psReady;
end;

function ProcessScheduleFromFrame(Frame: PInterruptFrame): QWord;
var
  OldIndex: LongWord;
  NextIndex: LongWord;
  Step: LongWord;
  Candidate: LongWord;
  Found: Boolean;
  OldWasZombie: Boolean;
begin
  Result := 0;
  OldIndex := CurrentProcessIndex;
  if OldIndex < PROCESS_MAX_COUNT then
  begin
    ProcessTable[OldIndex].SavedFrame := PtrUInt(Frame);
    if ProcessTable[OldIndex].State = psRunning then
      ProcessTable[OldIndex].State := psReady;
  end;

  Found := False;
  NextIndex := PROCESS_INDEX_NONE;
  for Step := 1 to PROCESS_MAX_COUNT do
  begin
    if OldIndex < PROCESS_MAX_COUNT then
      Candidate := (OldIndex + Step) mod PROCESS_MAX_COUNT
    else
      Candidate := Step - 1;
    if ProcessTable[Candidate].State = psReady then
    begin
      NextIndex := Candidate;
      Found := True;
      Break;
    end;
  end;
  if not Found then
  begin
    if (OldIndex < PROCESS_MAX_COUNT) and
       (ProcessTable[OldIndex].State = psReady) then
    begin
      NextIndex := OldIndex;
      Found := True;
    end;
  end;
  if not Found then
  begin
    WriteText('[FATAL] Process scheduler has no runnable process.'#13#10);
    HaltForever;
  end;

  OldWasZombie := (OldIndex < PROCESS_MAX_COUNT) and
                  (ProcessTable[OldIndex].State = psZombie) and
                  (ProcessTable[OldIndex].ReapRequested <> 0);
  ProcessTable[NextIndex].State := psRunning;
  CurrentProcessIndex := NextIndex;
  TaskState.RSP0 := ProcessTable[NextIndex].KernelStackTop;
  if not AddressSpaceActivate(ProcessTable[NextIndex].AddressSpaceRoot) then
  begin
    WriteText('[FATAL] Scheduler could not activate a process address space.'#13#10);
    HaltForever;
  end;
  Inc(ProcessScheduleCount);
  Result := ProcessTable[NextIndex].SavedFrame;

  { Reclaim the old process only after CR3 no longer names its address space. }
  if OldWasZombie and (OldIndex <> NextIndex) then
    ProcessFinalizeSlot(OldIndex);
end;

function ProcessWaitCurrent(Frame: PInterruptFrame; TargetPID,
  StatusAddress: QWord; var ReturnValue: QWord): QWord;
var
  I: LongWord;
  FoundChild: Boolean;
begin
  ReturnValue := QWord(-10); { -ECHILD }
  Result := PtrUInt(Frame);
  if CurrentProcessIndex >= PROCESS_MAX_COUNT then Exit;
  if (StatusAddress <> 0) and not AddressSpaceValidateUserRange(
       ProcessTable[CurrentProcessIndex].AddressSpaceRoot,
       StatusAddress, SizeOf(QWord)) then
  begin
    ReturnValue := QWord(-14); { -EFAULT }
    Exit;
  end;

  FoundChild := False;
  for I := 0 to PROCESS_MAX_COUNT - 1 do
  begin
    if (ProcessTable[I].State = psFree) or
       (ProcessTable[I].ParentPID <> ProcessTable[CurrentProcessIndex].PID) then
      Continue;
    if (TargetPID <> 0) and (ProcessTable[I].PID <> TargetPID) then Continue;
    FoundChild := True;
    if ProcessTable[I].State = psZombie then
    begin
      if (StatusAddress <> 0) and not AddressSpaceCopyTo(
           ProcessTable[CurrentProcessIndex].AddressSpaceRoot,
           StatusAddress, @ProcessTable[I].ExitCode, SizeOf(QWord)) then
      begin
        ReturnValue := QWord(-14);
        Exit;
      end;
      ReturnValue := ProcessTable[I].PID;
      ProcessFinalizeSlot(I);
      Exit;
    end;
  end;

  if not FoundChild then Exit;
  ProcessTable[CurrentProcessIndex].SavedFrame := PtrUInt(Frame);
  ProcessTable[CurrentProcessIndex].WaitTargetPID := TargetPID;
  ProcessTable[CurrentProcessIndex].WaitStatusAddress := StatusAddress;
  ProcessTable[CurrentProcessIndex].State := psWaiting;
  Result := ProcessScheduleFromFrame(Frame);
end;

function ProcessForkCurrent(Frame: PInterruptFrame;
  out ChildPID: QWord): Boolean;
var
  ParentIndex: LongWord;
  ChildIndex: LongWord;
  ChildRoot: QWord;
  FrameBytes: QWord;
  SourceBytes: PByte;
  DestinationBytes: PByte;
  I: QWord;
  ChildFrame: PInterruptFrame;
begin
  Result := False;
  ChildPID := 0;
  ParentIndex := CurrentProcessIndex;
  if ParentIndex >= PROCESS_MAX_COUNT then Exit;
  if not ProcessFindFreeSlot(ChildIndex) then Exit;
  if not COWForkAddressSpace(ProcessTable[ParentIndex].AddressSpaceRoot,
                             ChildRoot) then Exit;

  ProcessTable[ChildIndex].PID := NextProcessId;
  Inc(NextProcessId);
  if NextProcessId = 0 then NextProcessId := 1;
  ProcessTable[ChildIndex].ParentPID := ProcessTable[ParentIndex].PID;
  ProcessTable[ChildIndex].State := psReady;
  ProcessTable[ChildIndex].AddressSpaceRoot := ChildRoot;
  ProcessTable[ChildIndex].EntryPoint := ProcessTable[ParentIndex].EntryPoint;
  ProcessTable[ChildIndex].UserStackPointer := ProcessTable[ParentIndex].UserStackPointer;
  ProcessTable[ChildIndex].ExitCode := 0;
  ProcessTable[ChildIndex].WaitTargetPID := 0;
  ProcessTable[ChildIndex].WaitStatusAddress := 0;
  ProcessTable[ChildIndex].ReapRequested := 0;
  ProcessTable[ChildIndex].PendingCapabilityCount := 0;
  ProcessCapabilitySpaceCopy(ProcessTable[ChildIndex].Capabilities,
    ProcessTable[ParentIndex].Capabilities);
  if not VFSProcessClone(ProcessTable[ChildIndex].Files,
       ProcessTable[ParentIndex].Files) then
  begin
    ProcessTable[ChildIndex].State := psZombie;
    ProcessTable[ChildIndex].ReapRequested := 1;
    ProcessFinalizeSlot(ChildIndex);
    Exit;
  end;
  VFSSetCommandLine(ProcessTable[ChildIndex].CommandLine,
    ProcessTable[ParentIndex].CommandLine);

  FrameBytes := SizeOf(TInterruptFrame) + 2 * SizeOf(QWord);
  DestinationBytes := PByte(PtrUInt(ProcessTable[ChildIndex].KernelStackTop - FrameBytes));
  SourceBytes := PByte(Frame);
  for I := 0 to FrameBytes - 1 do
    DestinationBytes[I] := SourceBytes[I];
  ChildFrame := PInterruptFrame(PtrUInt(DestinationBytes));
  ChildFrame^.SavedRAX := 0; { fork() returns zero in the child. }
  ProcessTable[ChildIndex].SavedFrame := PtrUInt(ChildFrame);
  ChildPID := ProcessTable[ChildIndex].PID;
  WriteText('[PROCESS] fork created child pid=');
  WriteDecimal(ChildPID);
  WriteText(' with copy-on-write mappings.'#13#10);
  Result := True;
end;

function ProcessSpawnFromUser(ImageAddress, ImageSize: QWord;
  out ChildPID: QWord): Boolean;
var
  ChildIndex: LongWord;
begin
  Result := False;
  ChildPID := 0;
  if CurrentProcessIndex >= PROCESS_MAX_COUNT then Exit;
  if (ImageSize = 0) or (ImageSize > PROCESS_MAX_USER_IMAGE_SIZE) then Exit;
  if not AddressSpaceCopyFrom(ProcessTable[CurrentProcessIndex].AddressSpaceRoot,
       ImageAddress, @ProcessUserImageScratch[0], ImageSize) then Exit;
  Result := ProcessCreateFromELF(@ProcessUserImageScratch[0], ImageSize,
    ProcessTable[CurrentProcessIndex].PID, ChildPID);
  if Result then
  begin
    if ProcessFindByPID(ChildPID, ChildIndex) then
    begin
      if not VFSProcessClone(ProcessTable[ChildIndex].Files,
           ProcessTable[CurrentProcessIndex].Files) then
      begin
        ProcessTable[ChildIndex].State := psZombie;
        ProcessTable[ChildIndex].ReapRequested := 1;
        ProcessFinalizeSlot(ChildIndex);
        Result := False;
      end;
    end;
  end;
end;

function ProcessSpawnFromPath(PathAddress, ArgsAddress, InputFD,
  OutputFD: QWord; out ChildPID: QWord): Boolean;
var
  Path: array[0..VFS_PATH_MAX] of Char;
  Args: array[0..VFS_PATH_MAX] of Char;
  Canonical: array[0..VFS_PATH_MAX] of Char;
  Image: Pointer;
  ImageSize: QWord;
  ChildIndex: LongWord;
begin
  Result := False;
  ChildPID := 0;
  if CurrentProcessIndex >= PROCESS_MAX_COUNT then Exit;
  if not VFSCopyUserString(ProcessTable[CurrentProcessIndex].AddressSpaceRoot,
       PathAddress, Path) then Exit;
  if ArgsAddress = 0 then Args[0] := #0
  else if not VFSCopyUserString(ProcessTable[CurrentProcessIndex].AddressSpaceRoot,
       ArgsAddress, Args) then Exit;
  if not VFSNormalizePath(ProcessTable[CurrentProcessIndex].Files.CurrentDirectory,
       Path, Canonical) then Exit;
  if not VFSValidateSpawnFDs(ProcessTable[CurrentProcessIndex].Files,
       InputFD, OutputFD) then Exit;
  if not VFSLookupExecutable(Canonical, Image, ImageSize) then Exit;
  if not ProcessCreateFromELF(Image, ImageSize,
       ProcessTable[CurrentProcessIndex].PID, ChildPID) then Exit;
  if not ProcessFindByPID(ChildPID, ChildIndex) then Exit;
  if not VFSPrepareSpawnFromParent(ProcessTable[ChildIndex].Files,
       ProcessTable[CurrentProcessIndex].Files, InputFD, OutputFD) then
  begin
    ProcessTable[ChildIndex].State := psZombie;
    ProcessTable[ChildIndex].ReapRequested := 1;
    ProcessFinalizeSlot(ChildIndex);
    ChildPID := 0;
    Exit;
  end;
  VFSSetCommandLine(ProcessTable[ChildIndex].CommandLine, Args);
  VFSSetCommandLine(ProcessTable[ChildIndex].Files.CurrentDirectory,
       ProcessTable[CurrentProcessIndex].Files.CurrentDirectory);
  Result := True;
end;

function ProcessCopyCommandLine(ProcessIndex: LongWord; RootPhysical,
  UserBuffer, Capacity: QWord): QWord;
var
  LengthValue: LongWord;
begin
  Result := QWord(-14);
  if ProcessIndex >= PROCESS_MAX_COUNT then Exit;
  LengthValue := VFSStringLength(ProcessTable[ProcessIndex].CommandLine) + 1;
  if Capacity < LengthValue then
  begin
    Result := QWord(-36);
    Exit;
  end;
  if not AddressSpaceCopyTo(RootPhysical, UserBuffer,
       @ProcessTable[ProcessIndex].CommandLine[0], LengthValue) then Exit;
  Result := LengthValue - 1;
end;

function ProcessEndpointCreate(var Process: TKernelProcess;
  out Handle: LongWord): QWord;
var
  I: LongWord;
  NewGeneration: Word;
  Status: TKernelResult;
begin
  Handle := 0;
  Result := QWord(-12); { -ENOMEM }
  for I := 0 to ENDPOINT_MAX_COUNT - 1 do
  begin
    if (EndpointTable[I].Active <> 0) or
       (EndpointTable[I].Generation = High(Word)) then Continue;
    NewGeneration := EndpointTable[I].Generation + 1;
    EndpointTable[I].Generation := NewGeneration;
    EndpointTable[I].Active := 1;
    EndpointTable[I].Head := 0;
    EndpointTable[I].Tail := 0;
    EndpointTable[I].Count := 0;
    Status := ProcessCapabilityInstall(Process.Capabilities, okEndpoint,
      Word(I), NewGeneration,
      CAP_RIGHT_SEND or CAP_RIGHT_RECEIVE or CAP_RIGHT_MANAGE or CAP_RIGHT_TRANSFER,
      Handle);
    if Status <> krSuccess then
    begin
      EndpointTable[I].Active := 0;
      Result := QWord(-24); { -EMFILE }
      Exit;
    end;
    Result := Handle;
    Exit;
  end;
end;

function ProcessIPCSend(var Process: TKernelProcess; Handle,
  MessageAddress: QWord): QWord;
var
  ObjectIndex: Word;
  ObjectGeneration: Word;
  Status: TKernelResult;
  Message: TIPCMessage;
  Endpoint: ^TEndpoint;
begin
  Result := QWord(-9); { -EBADF }
  if Handle > High(LongWord) then Exit;
  Status := ProcessCapabilityLookup(Process.Capabilities, LongWord(Handle),
    CAP_RIGHT_SEND, okEndpoint, ObjectIndex, ObjectGeneration);
  if Status <> krSuccess then
  begin
    if Status = krPermissionDenied then Result := QWord(-1);
    Exit;
  end;
  if ObjectIndex >= ENDPOINT_MAX_COUNT then Exit;
  if (EndpointTable[ObjectIndex].Active = 0) or
     (EndpointTable[ObjectIndex].Generation <> ObjectGeneration) then Exit;
  if not AddressSpaceCopyFrom(Process.AddressSpaceRoot, MessageAddress,
       @Message, SizeOf(TIPCMessage)) then
  begin
    Result := QWord(-14);
    Exit;
  end;
  if Message.WordCount > IPC_MAX_WORDS then
  begin
    Result := QWord(-22);
    Exit;
  end;
  Endpoint := @EndpointTable[ObjectIndex];
  if Endpoint^.Count >= IPC_QUEUE_DEPTH then
  begin
    Result := QWord(-11); { -EAGAIN }
    Exit;
  end;
  ProcessCopyIPCMessage(Endpoint^.Queue[Endpoint^.Tail], Message);
  Endpoint^.Tail := (Endpoint^.Tail + 1) mod IPC_QUEUE_DEPTH;
  Inc(Endpoint^.Count);
  ThreadWakeWaiters(EndpointWaitKey(ObjectIndex, ObjectGeneration,
    WAIT_KEY_ENDPOINT_RECEIVE), True);
  Result := 0;
end;

function ProcessIPCReceive(var Process: TKernelProcess; Handle,
  MessageAddress: QWord): QWord;
var
  ObjectIndex: Word;
  ObjectGeneration: Word;
  Status: TKernelResult;
  Message: TIPCMessage;
  Endpoint: ^TEndpoint;
begin
  Result := QWord(-9); { -EBADF }
  if Handle > High(LongWord) then Exit;
  Status := ProcessCapabilityLookup(Process.Capabilities, LongWord(Handle),
    CAP_RIGHT_RECEIVE, okEndpoint, ObjectIndex, ObjectGeneration);
  if Status <> krSuccess then
  begin
    if Status = krPermissionDenied then Result := QWord(-1);
    Exit;
  end;
  if ObjectIndex >= ENDPOINT_MAX_COUNT then Exit;
  if (EndpointTable[ObjectIndex].Active = 0) or
     (EndpointTable[ObjectIndex].Generation <> ObjectGeneration) then Exit;
  if not AddressSpaceValidateUserRange(Process.AddressSpaceRoot,
       MessageAddress, SizeOf(TIPCMessage)) then
  begin
    Result := QWord(-14);
    Exit;
  end;
  Endpoint := @EndpointTable[ObjectIndex];
  if Endpoint^.Count = 0 then
  begin
    Result := QWord(-11); { -EAGAIN; user process may yield and retry }
    Exit;
  end;
  ProcessCopyIPCMessage(Message, Endpoint^.Queue[Endpoint^.Head]);
  if not AddressSpaceCopyTo(Process.AddressSpaceRoot, MessageAddress,
       @Message, SizeOf(TIPCMessage)) then
  begin
    Result := QWord(-14);
    Exit;
  end;
  Endpoint^.Head := (Endpoint^.Head + 1) mod IPC_QUEUE_DEPTH;
  Dec(Endpoint^.Count);
  Result := 0;
end;

function ProcessCapabilityTransfer(SourceIndex: LongWord; TargetPID: QWord;
  SourceHandle, RequestedRights: LongWord): QWord;
var
  TargetIndex: LongWord;
  ObjectIndex: Word;
  ObjectGeneration: Word;
  NewHandle: LongWord;
  Status: TKernelResult;
  PendingIndex: LongWord;
  SlotEncoded: LongWord;
  Slot: LongWord;
  SourceKind: TObjectKind;
  AllowedRights: LongWord;
begin
  Result := QWord(-1); { -EPERM }
  if SourceIndex >= PROCESS_MAX_COUNT then Exit;
  if not ProcessFindByPID(TargetPID, TargetIndex) then
  begin
    Result := QWord(-3); { -ESRCH }
    Exit;
  end;
  if (ProcessTable[TargetIndex].ParentPID <> ProcessTable[SourceIndex].PID) or
     (ProcessTable[TargetIndex].State = psZombie) then Exit;

  SlotEncoded := SourceHandle and $FFFF;
  if (SlotEncoded = 0) or (SlotEncoded > CAPABILITY_MAX_SLOTS) then
  begin
    Result := QWord(-9);
    Exit;
  end;
  Slot := SlotEncoded - 1;
  if (ProcessTable[SourceIndex].Capabilities.Entries[Slot].Active = 0) or
     (ProcessTable[SourceIndex].Capabilities.Entries[Slot].Generation <>
       Word(SourceHandle shr 16)) then
  begin
    Result := QWord(-9);
    Exit;
  end;
  if ProcessTable[SourceIndex].Capabilities.Entries[Slot].ObjectKind >
     Ord(okMemoryObject) then
  begin
    Result := QWord(-9);
    Exit;
  end;
  SourceKind := TObjectKind(
    ProcessTable[SourceIndex].Capabilities.Entries[Slot].ObjectKind);
  case SourceKind of
    okEndpoint:
      AllowedRights := CAP_RIGHT_SEND or CAP_RIGHT_RECEIVE;
    okMemoryObject:
      AllowedRights := CAP_RIGHT_MAP or CAP_RIGHT_READ or CAP_RIGHT_WRITE or
                       CAP_RIGHT_USER_MAP;
  else
    Result := QWord(-22);
    Exit;
  end;

  Status := ProcessCapabilityLookup(ProcessTable[SourceIndex].Capabilities,
    SourceHandle, CAP_RIGHT_TRANSFER, SourceKind, ObjectIndex, ObjectGeneration);
  if Status <> krSuccess then Exit;
  if (RequestedRights = 0) or ((RequestedRights and not AllowedRights) <> 0) then
  begin
    Result := QWord(-22);
    Exit;
  end;
  if (ProcessTable[SourceIndex].Capabilities.Entries[Slot].Rights and
      RequestedRights) <> RequestedRights then
  begin
    Result := QWord(-1);
    Exit;
  end;
  if ProcessTable[TargetIndex].PendingCapabilityCount >= PROCESS_PENDING_CAPS then
  begin
    Result := QWord(-24);
    Exit;
  end;
  Status := ProcessCapabilityInstall(ProcessTable[TargetIndex].Capabilities,
    SourceKind, ObjectIndex, ObjectGeneration, RequestedRights, NewHandle);
  if Status <> krSuccess then
  begin
    Result := QWord(-24);
    Exit;
  end;
  PendingIndex := ProcessTable[TargetIndex].PendingCapabilityCount;
  ProcessTable[TargetIndex].PendingCapabilityHandles[PendingIndex] := NewHandle;
  Inc(ProcessTable[TargetIndex].PendingCapabilityCount);
  Result := NewHandle;
end;

function ProcessCapabilityReceive(ProcessIndex: LongWord): QWord;
var
  I: LongWord;
begin
  Result := QWord(-11); { -EAGAIN }
  if ProcessIndex >= PROCESS_MAX_COUNT then Exit;
  if ProcessTable[ProcessIndex].PendingCapabilityCount = 0 then Exit;
  Result := ProcessTable[ProcessIndex].PendingCapabilityHandles[0];
  for I := 1 to ProcessTable[ProcessIndex].PendingCapabilityCount - 1 do
    ProcessTable[ProcessIndex].PendingCapabilityHandles[I - 1] :=
      ProcessTable[ProcessIndex].PendingCapabilityHandles[I];
  Dec(ProcessTable[ProcessIndex].PendingCapabilityCount);
  ProcessTable[ProcessIndex].PendingCapabilityHandles[
    ProcessTable[ProcessIndex].PendingCapabilityCount] := 0;
end;

procedure P7ShmReleaseProcess(ProcessIndex: LongWord); forward;

procedure ProcessTerminateIndex(ProcessIndex: LongWord; ExitCode: QWord);
begin
  if ProcessIndex >= PROCESS_MAX_COUNT then Exit;
  if (ProcessTable[ProcessIndex].State = psFree) or
     (ProcessTable[ProcessIndex].State = psZombie) then Exit;
  ProcessTable[ProcessIndex].ExitCode := ExitCode;
  P7ShmReleaseProcess(ProcessIndex);
  VFSProcessCloseAll(ProcessTable[ProcessIndex].Files);
  ProcessTable[ProcessIndex].State := psZombie;
  ProcessWakeParentOnExit(ProcessIndex);
end;

procedure ProcessTerminateCurrent(Frame: PInterruptFrame; ExitCode: QWord);
var
  PID: QWord;
begin
  if CurrentProcessIndex >= PROCESS_MAX_COUNT then HaltForever;
  PID := ProcessTable[CurrentProcessIndex].PID;
  ProcessTable[CurrentProcessIndex].SavedFrame := PtrUInt(Frame);
  ProcessTerminateIndex(CurrentProcessIndex, ExitCode);
  WriteText('[PROCESS] pid=');
  WriteDecimal(PID);
  WriteText(' terminated with status ');
  WriteDecimal(ExitCode);
  WriteText('.'#13#10);
end;

function ProcessKill(SourceIndex: LongWord; TargetPID: QWord;
  SignalNumber: QWord): QWord;
var
  TargetIndex: LongWord;
begin
  Result := QWord(-3); { -ESRCH }
  if not ProcessFindByPID(TargetPID, TargetIndex) then Exit;
  if (TargetIndex <> SourceIndex) and
     (ProcessTable[TargetIndex].ParentPID <> ProcessTable[SourceIndex].PID) then
  begin
    Result := QWord(-1);
    Exit;
  end;
  if (SignalNumber <> 2) and (SignalNumber <> 9) and (SignalNumber <> 15) then
  begin
    Result := QWord(-22);
    Exit;
  end;
  ProcessTerminateIndex(TargetIndex, 128 + SignalNumber);
  Result := 0;
end;

procedure ProcessLaunch(PID: QWord);
var
  Index: LongWord;
begin
  if not ProcessFindByPID(PID, Index) then HaltForever;
  if ProcessTable[Index].State <> psReady then HaltForever;
  CurrentProcessIndex := Index;
  ProcessTable[Index].State := psRunning;
  TaskState.RSP0 := ProcessTable[Index].KernelStackTop;
  if not AddressSpaceActivate(ProcessTable[Index].AddressSpaceRoot) then
  begin
    WriteText('[FAIL] Could not activate initial process address space.'#13#10);
    HaltForever;
  end;
  WriteText('[ OK ] Starting Ring-3 process pid=');
  WriteDecimal(PID);
  WriteText(' on dedicated kernel stack.'#13#10);
  ProcessRestoreFrame(ProcessTable[Index].SavedFrame);
  HaltForever;
end;
