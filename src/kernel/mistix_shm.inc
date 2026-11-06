{ Mistix Phase 7: process-scoped shared memory objects.
  Objects are kernel-owned physical pages; per-process mappings are tracked
  independently and use each process's own page-table root. }

const
  P7_SHM_OBJECT_MAX = 16;
  P7_SHM_PAGES_MAX = 16;
  P7_SHM_MAPPING_MAX = 64;
  P7_SHM_PAGE_SIZE = 4096;
  P7_SHM_USER_LIMIT = $0000800000000000;
  P7_SHM_RIGHTS = CAP_RIGHT_MAP or CAP_RIGHT_READ or CAP_RIGHT_WRITE or
                  CAP_RIGHT_USER_MAP or CAP_RIGHT_TRANSFER;

type
  TP7SharedObject = record
    Active: Byte;
    Generation: Word;
    PageCount: Word;
    Pages: array[0..P7_SHM_PAGES_MAX - 1] of QWord;
  end;

  TP7SharedMapping = record
    Active: Byte;
    ProcessIndex: Byte;
    ObjectIndex: Word;
    ObjectGeneration: Word;
    PageCount: Word;
    BaseAddress: QWord;
  end;

var
  P7SharedObjects: array[0..P7_SHM_OBJECT_MAX - 1] of TP7SharedObject;
  P7SharedMappings: array[0..P7_SHM_MAPPING_MAX - 1] of TP7SharedMapping;


procedure P7ShmInvalidate(VirtualAddress: QWord); assembler; nostackframe;
asm
  invlpg [rdi]
end;

procedure P7ShmInitialize;
var
  I, J: LongWord;
begin
  for I := 0 to P7_SHM_OBJECT_MAX - 1 do
  begin
    P7SharedObjects[I].Active := 0;
    P7SharedObjects[I].Generation := 0;
    P7SharedObjects[I].PageCount := 0;
    for J := 0 to P7_SHM_PAGES_MAX - 1 do
      P7SharedObjects[I].Pages[J] := 0;
  end;
  for I := 0 to P7_SHM_MAPPING_MAX - 1 do
  begin
    P7SharedMappings[I].Active := 0;
    P7SharedMappings[I].ProcessIndex := 0;
    P7SharedMappings[I].ObjectIndex := 0;
    P7SharedMappings[I].ObjectGeneration := 0;
    P7SharedMappings[I].PageCount := 0;
    P7SharedMappings[I].BaseAddress := 0;
  end;
  WriteText('[ OK ] Process-shared memory registry initialised.'#13#10);
end;

function P7ShmObjectValid(ObjectIndex, ObjectGeneration: Word): Boolean;
begin
  Result := (ObjectIndex < P7_SHM_OBJECT_MAX) and
    (P7SharedObjects[ObjectIndex].Active <> 0) and
    (P7SharedObjects[ObjectIndex].Generation = ObjectGeneration);
end;

function P7ShmCreate(ProcessIndex: LongWord; PageCount: QWord): QWord;
var
  I, J: LongWord;
  ObjectIndex: LongWord;
  Physical: array[0..P7_SHM_PAGES_MAX - 1] of QWord;
  Generation: Word;
  Handle: LongWord;
  Status: TKernelResult;
  Words: PQWord;
begin
  Result := QWord(-22);
  if (ProcessIndex >= PROCESS_MAX_COUNT) or
     (ProcessTable[ProcessIndex].State = psFree) or
     (PageCount = 0) or (PageCount > P7_SHM_PAGES_MAX) then Exit;

  ObjectIndex := P7_SHM_OBJECT_MAX;
  for I := 0 to P7_SHM_OBJECT_MAX - 1 do
    if (P7SharedObjects[I].Active = 0) and
       (P7SharedObjects[I].Generation <> High(Word)) then
    begin
      ObjectIndex := I;
      Break;
    end;
  if ObjectIndex = P7_SHM_OBJECT_MAX then
  begin
    Result := QWord(-12);
    Exit;
  end;

  for I := 0 to P7_SHM_PAGES_MAX - 1 do Physical[I] := 0;
  for I := 0 to LongWord(PageCount) - 1 do
  begin
    if not PMMAllocatePage(Physical[I]) then
    begin
      if I <> 0 then
        for J := 0 to I - 1 do
          if Physical[J] <> 0 then PMMFreePage(Physical[J]);
      Result := QWord(-12);
      Exit;
    end;
    Words := PQWord(PtrUInt(MemoryHHDMOffset + Physical[I]));
    for J := 0 to 511 do Words^[J] := 0;
  end;

  Generation := P7SharedObjects[ObjectIndex].Generation + 1;
  P7SharedObjects[ObjectIndex].Generation := Generation;
  P7SharedObjects[ObjectIndex].PageCount := Word(PageCount);
  for I := 0 to P7_SHM_PAGES_MAX - 1 do
    P7SharedObjects[ObjectIndex].Pages[I] := Physical[I];
  P7SharedObjects[ObjectIndex].Active := 1;

  Status := ProcessCapabilityInstall(ProcessTable[ProcessIndex].Capabilities,
    okMemoryObject, Word(ObjectIndex), Generation, P7_SHM_RIGHTS, Handle);
  if Status <> krSuccess then
  begin
    P7SharedObjects[ObjectIndex].Active := 0;
    for I := 0 to LongWord(PageCount) - 1 do PMMFreePage(Physical[I]);
    Result := QWord(-24);
    Exit;
  end;
  Result := Handle;
end;

function P7ShmResolve(ProcessIndex: LongWord; Handle: QWord;
  RequiredRights: LongWord; out ObjectIndex, ObjectGeneration: Word): Boolean;
begin
  Result := False;
  ObjectIndex := 0;
  ObjectGeneration := 0;
  if (ProcessIndex >= PROCESS_MAX_COUNT) or (Handle > High(LongWord)) then Exit;
  if (ProcessTable[ProcessIndex].State = psFree) or
     (ProcessTable[ProcessIndex].State = psZombie) or
     (ProcessTable[ProcessIndex].AddressSpaceRoot = 0) then Exit;
  if ProcessCapabilityLookup(ProcessTable[ProcessIndex].Capabilities,
      LongWord(Handle), RequiredRights, okMemoryObject,
      ObjectIndex, ObjectGeneration) <> krSuccess then Exit;
  Result := P7ShmObjectValid(ObjectIndex, ObjectGeneration);
end;

function P7ShmMap(ProcessIndex: LongWord; Handle, BaseAddress: QWord;
  Writable: Boolean): QWord;
var
  ObjectIndex, ObjectGeneration: Word;
  I, J, FreeSlot: LongWord;
  Flags, Physical, ExistingPhysical, ExistingFlags: QWord;
  Mapped: LongWord;
begin
  Result := QWord(-9);
  if not P7ShmResolve(ProcessIndex, Handle,
       LongWord(CAP_RIGHT_MAP or CAP_RIGHT_READ or CAP_RIGHT_USER_MAP or
       (LongWord(Ord(Writable)) * CAP_RIGHT_WRITE)),
       ObjectIndex, ObjectGeneration) then Exit;
  if ((BaseAddress and (P7_SHM_PAGE_SIZE - 1)) <> 0) or
     (BaseAddress >= P7_SHM_USER_LIMIT) or
     (QWord(P7SharedObjects[ObjectIndex].PageCount) * P7_SHM_PAGE_SIZE >
       P7_SHM_USER_LIMIT - BaseAddress) then
  begin
    Result := QWord(-22);
    Exit;
  end;
  FreeSlot := P7_SHM_MAPPING_MAX;
  for I := 0 to P7_SHM_MAPPING_MAX - 1 do
  begin
    if P7SharedMappings[I].Active = 0 then
    begin
      if FreeSlot = P7_SHM_MAPPING_MAX then FreeSlot := I;
      Continue;
    end;
    if (P7SharedMappings[I].ProcessIndex = ProcessIndex) and
       (BaseAddress < P7SharedMappings[I].BaseAddress +
         QWord(P7SharedMappings[I].PageCount) * P7_SHM_PAGE_SIZE) and
       (P7SharedMappings[I].BaseAddress < BaseAddress +
         QWord(P7SharedObjects[ObjectIndex].PageCount) * P7_SHM_PAGE_SIZE) then
    begin
      Result := QWord(-16);
      Exit;
    end;
  end;
  if FreeSlot = P7_SHM_MAPPING_MAX then
  begin
    Result := QWord(-24);
    Exit;
  end;

  Flags := PTE_USER;
  if Writable then Flags := Flags or PTE_WRITABLE;
  if CPUHasNXSupport then Flags := Flags or PTE_NO_EXECUTE;
  Mapped := 0;
  for I := 0 to P7SharedObjects[ObjectIndex].PageCount - 1 do
  begin
    if AddressSpaceTranslate(ProcessTable[ProcessIndex].AddressSpaceRoot,
        BaseAddress + QWord(I) * P7_SHM_PAGE_SIZE,
        ExistingPhysical, ExistingFlags) then Break;
    Physical := P7SharedObjects[ObjectIndex].Pages[I];
    if not AddressSpaceMapPage(ProcessTable[ProcessIndex].AddressSpaceRoot,
        BaseAddress + QWord(I) * P7_SHM_PAGE_SIZE, Physical, Flags) then Break;
    Inc(Mapped);
  end;
  if Mapped <> P7SharedObjects[ObjectIndex].PageCount then
  begin
    if Mapped <> 0 then
      for J := 0 to Mapped - 1 do
      begin
        if AddressSpaceUnmapPage(ProcessTable[ProcessIndex].AddressSpaceRoot,
            BaseAddress + QWord(J) * P7_SHM_PAGE_SIZE, ExistingPhysical) then ;
      end;
    Result := QWord(-16);
    Exit;
  end;

  P7SharedMappings[FreeSlot].Active := 1;
  P7SharedMappings[FreeSlot].ProcessIndex := ProcessIndex;
  P7SharedMappings[FreeSlot].ObjectIndex := ObjectIndex;
  P7SharedMappings[FreeSlot].ObjectGeneration := ObjectGeneration;
  P7SharedMappings[FreeSlot].PageCount := P7SharedObjects[ObjectIndex].PageCount;
  P7SharedMappings[FreeSlot].BaseAddress := BaseAddress;
  Result := 0;
end;

function P7ShmUnmap(ProcessIndex: LongWord; Handle, BaseAddress: QWord): QWord;
var
  ObjectIndex, ObjectGeneration: Word;
  I, J: LongWord;
  Physical: QWord;
begin
  Result := QWord(-9);
  if not P7ShmResolve(ProcessIndex, Handle, CAP_RIGHT_MAP,
       ObjectIndex, ObjectGeneration) then Exit;
  for I := 0 to P7_SHM_MAPPING_MAX - 1 do
    if (P7SharedMappings[I].Active <> 0) and
       (P7SharedMappings[I].ProcessIndex = ProcessIndex) and
       (P7SharedMappings[I].ObjectIndex = ObjectIndex) and
       (P7SharedMappings[I].ObjectGeneration = ObjectGeneration) and
       (P7SharedMappings[I].BaseAddress = BaseAddress) then
    begin
      for J := 0 to P7SharedMappings[I].PageCount - 1 do
        if not AddressSpaceUnmapPage(ProcessTable[ProcessIndex].AddressSpaceRoot,
            BaseAddress + QWord(J) * P7_SHM_PAGE_SIZE, Physical) then
        begin
          Result := QWord(-14);
          Exit;
        end
        else
        begin
          if Physical <> P7SharedObjects[ObjectIndex].Pages[J] then
          begin
            WriteText('[FATAL] Shared-memory physical-page mismatch.'#13#10);
            HaltForever;
          end;
          if ProcessIndex = CurrentProcessIndex then
            P7ShmInvalidate(BaseAddress + QWord(J) * P7_SHM_PAGE_SIZE);
        end;
      P7SharedMappings[I].Active := 0;
      P7SharedMappings[I].PageCount := 0;
      P7SharedMappings[I].BaseAddress := 0;
      Result := 0;
      Exit;
    end;
end;

function P7ShmProcessHasMappings(ProcessIndex: LongWord): Boolean;
var
  I: LongWord;
begin
  Result := False;
  if ProcessIndex >= PROCESS_MAX_COUNT then Exit;
  for I := 0 to P7_SHM_MAPPING_MAX - 1 do
    if (P7SharedMappings[I].Active <> 0) and
       (P7SharedMappings[I].ProcessIndex = ProcessIndex) then
    begin
      Result := True;
      Exit;
    end;
end;

procedure P7ShmReleaseProcess(ProcessIndex: LongWord);
var
  I, J: LongWord;
  Physical: QWord;
begin
  if ProcessIndex >= PROCESS_MAX_COUNT then Exit;
  for I := 0 to P7_SHM_MAPPING_MAX - 1 do
    if (P7SharedMappings[I].Active <> 0) and
       (P7SharedMappings[I].ProcessIndex = ProcessIndex) then
    begin
      for J := 0 to P7SharedMappings[I].PageCount - 1 do
        if AddressSpaceUnmapPage(ProcessTable[ProcessIndex].AddressSpaceRoot,
          P7SharedMappings[I].BaseAddress + QWord(J) * P7_SHM_PAGE_SIZE,
          Physical) and (ProcessIndex = CurrentProcessIndex) then
          P7ShmInvalidate(P7SharedMappings[I].BaseAddress +
            QWord(J) * P7_SHM_PAGE_SIZE);
      P7SharedMappings[I].Active := 0;
      P7SharedMappings[I].PageCount := 0;
    end;
end;

