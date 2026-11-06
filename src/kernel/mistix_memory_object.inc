{ Mistix Phase 2: page-backed memory objects for shared mappings.
  The current VMM root is still the single kernel address space. Mapping the
  same object into two roots will become possible after per-process roots exist. }

const
  MEMORY_OBJECT_MAX_COUNT = 8;
  MEMORY_OBJECT_MAX_PAGES = 16;
  MEMORY_OBJECT_MAX_MAPPINGS = 32;
  MEMORY_OBJECT_TEST_BASE = $FFFFC10000000000;

type
  TMemoryObjectMapping = record
    Active: Byte;
    ObjectPageIndex: Word;
    VirtualAddress: QWord;
  end;

  TMemoryObject = record
    Active: Byte;
    Generation: Word;
    PageCount: Word;
    Pages: array[0..MEMORY_OBJECT_MAX_PAGES - 1] of QWord;
    Mappings: array[0..MEMORY_OBJECT_MAX_MAPPINGS - 1] of TMemoryObjectMapping;
  end;

var
  MemoryObjectTable: array[0..MEMORY_OBJECT_MAX_COUNT - 1] of TMemoryObject;

procedure MemoryObjectInitialize;
var
  I, J: LongWord;
begin
  for I := 0 to MEMORY_OBJECT_MAX_COUNT - 1 do
  begin
    MemoryObjectTable[I].Active := 0;
    MemoryObjectTable[I].Generation := 0;
    MemoryObjectTable[I].PageCount := 0;
    for J := 0 to MEMORY_OBJECT_MAX_PAGES - 1 do
      MemoryObjectTable[I].Pages[J] := 0;
    for J := 0 to MEMORY_OBJECT_MAX_MAPPINGS - 1 do
    begin
      MemoryObjectTable[I].Mappings[J].Active := 0;
      MemoryObjectTable[I].Mappings[J].ObjectPageIndex := 0;
      MemoryObjectTable[I].Mappings[J].VirtualAddress := 0;
    end;
  end;
  WriteText('[ OK ] Memory-object registry initialised (' );
  WriteDecimal(MEMORY_OBJECT_MAX_COUNT);
  WriteText(' objects, up to ');
  WriteDecimal(MEMORY_OBJECT_MAX_PAGES);
  WriteText(' pages each).'#13#10);
end;

function MemoryObjectResolve(Handle: LongWord; RequiredRights: LongWord;
  out ObjectIndex: Word; out ObjectGeneration: Word): TKernelResult;
begin
  Result := CapabilityLookup(Handle, RequiredRights, okMemoryObject,
                             ObjectIndex, ObjectGeneration);
  if Result <> krSuccess then
    Exit;
  if ObjectIndex >= MEMORY_OBJECT_MAX_COUNT then
  begin
    Result := krInvalidHandle;
    Exit;
  end;
  if (MemoryObjectTable[ObjectIndex].Active = 0) or
     (MemoryObjectTable[ObjectIndex].Generation <> ObjectGeneration) then
    Result := krObjectClosed;
end;

procedure ZeroPhysicalPageForObject(PhysicalAddress: QWord);
var
  Words: PQWord;
  I: LongWord;
begin
  Words := PQWord(PtrUInt(MemoryHHDMOffset + PhysicalAddress));
  for I := 0 to 511 do
    Words^[I] := 0;
end;

function MemoryObjectCreate(PageCount: Word;
  out Handle: LongWord): TKernelResult;
var
  I, J: LongWord;
  ObjectIndex: LongWord;
  NewGeneration: Word;
  TempPages: array[0..MEMORY_OBJECT_MAX_PAGES - 1] of QWord;
begin
  Handle := 0;
  if (PageCount = 0) or (PageCount > MEMORY_OBJECT_MAX_PAGES) then
  begin
    Result := krInvalidArgument;
    Exit;
  end;

  ObjectIndex := MEMORY_OBJECT_MAX_COUNT;
  for I := 0 to MEMORY_OBJECT_MAX_COUNT - 1 do
    if (MemoryObjectTable[I].Active = 0) and
       (MemoryObjectTable[I].Generation <> High(Word)) then
    begin
      ObjectIndex := I;
      Break;
    end;
  if ObjectIndex = MEMORY_OBJECT_MAX_COUNT then
  begin
    Result := krNoResources;
    Exit;
  end;

  for I := 0 to MEMORY_OBJECT_MAX_PAGES - 1 do
    TempPages[I] := 0;
  for I := 0 to PageCount - 1 do
  begin
    if not PMMAllocatePage(TempPages[I]) then
    begin
      if I <> 0 then
        for J := 0 to I - 1 do
          if TempPages[J] <> 0 then
            PMMFreePage(TempPages[J]);
      Result := krNoResources;
      Exit;
    end;
    ZeroPhysicalPageForObject(TempPages[I]);
  end;

  NewGeneration := MemoryObjectTable[ObjectIndex].Generation + 1;
  MemoryObjectTable[ObjectIndex].Generation := NewGeneration;
  MemoryObjectTable[ObjectIndex].PageCount := PageCount;
  for I := 0 to MEMORY_OBJECT_MAX_PAGES - 1 do
    MemoryObjectTable[ObjectIndex].Pages[I] := TempPages[I];
  for I := 0 to MEMORY_OBJECT_MAX_MAPPINGS - 1 do
  begin
    MemoryObjectTable[ObjectIndex].Mappings[I].Active := 0;
    MemoryObjectTable[ObjectIndex].Mappings[I].ObjectPageIndex := 0;
    MemoryObjectTable[ObjectIndex].Mappings[I].VirtualAddress := 0;
  end;
  MemoryObjectTable[ObjectIndex].Active := 1;

  Result := CapabilityInstall(okMemoryObject, Word(ObjectIndex), NewGeneration,
    CAP_RIGHT_MAP or CAP_RIGHT_READ or CAP_RIGHT_WRITE or CAP_RIGHT_USER_MAP or
    CAP_RIGHT_MANAGE, Handle);
  if Result <> krSuccess then
  begin
    MemoryObjectTable[ObjectIndex].Active := 0;
    for I := 0 to PageCount - 1 do
      PMMFreePage(TempPages[I]);
    Exit;
  end;

  Result := krSuccess;
end;

function MemoryObjectMapPage(Handle: LongWord; ObjectPageIndex: Word;
  VirtualAddress, Flags: QWord): TKernelResult;
var
  ObjectIndex: Word;
  ObjectGeneration: Word;
  I: LongWord;
  FreeMapping: LongWord;
  RequiredRights: LongWord;
  PhysicalAddress: QWord;
begin
  RequiredRights := CAP_RIGHT_MAP or CAP_RIGHT_READ;
  if (Flags and PTE_WRITABLE) <> 0 then
    RequiredRights := RequiredRights or CAP_RIGHT_WRITE;
  if (Flags and PTE_USER) <> 0 then
    RequiredRights := RequiredRights or CAP_RIGHT_USER_MAP;
  if CPUHasNXSupport and ((Flags and PTE_NO_EXECUTE) = 0) then
    RequiredRights := RequiredRights or CAP_RIGHT_EXECUTE;

  Result := MemoryObjectResolve(Handle, RequiredRights,
                                ObjectIndex, ObjectGeneration);
  if Result <> krSuccess then
    Exit;
  if ObjectPageIndex >= MemoryObjectTable[ObjectIndex].PageCount then
  begin
    Result := krInvalidArgument;
    Exit;
  end;
  if (VirtualAddress and (PAGE_SIZE - 1)) <> 0 then
  begin
    Result := krInvalidArgument;
    Exit;
  end;
  if ((Flags and PTE_USER) <> 0) and
     (VirtualAddress >= $0000800000000000) then
  begin
    Result := krInvalidArgument;
    Exit;
  end;
  if (Flags and not PTE_ALLOWED_FLAGS) <> 0 then
  begin
    Result := krInvalidArgument;
    Exit;
  end;
  if ((Flags and PTE_NO_EXECUTE) <> 0) and not CPUHasNXSupport then
  begin
    Result := krInvalidArgument;
    Exit;
  end;

  FreeMapping := MEMORY_OBJECT_MAX_MAPPINGS;
  for I := 0 to MEMORY_OBJECT_MAX_MAPPINGS - 1 do
  begin
    if (MemoryObjectTable[ObjectIndex].Mappings[I].Active <> 0) and
       (MemoryObjectTable[ObjectIndex].Mappings[I].VirtualAddress = VirtualAddress) then
    begin
      Result := krInvalidArgument;
      Exit;
    end;
    if (FreeMapping = MEMORY_OBJECT_MAX_MAPPINGS) and
       (MemoryObjectTable[ObjectIndex].Mappings[I].Active = 0) then
      FreeMapping := I;
  end;
  if FreeMapping = MEMORY_OBJECT_MAX_MAPPINGS then
  begin
    Result := krNoResources;
    Exit;
  end;

  PhysicalAddress := MemoryObjectTable[ObjectIndex].Pages[ObjectPageIndex];
  if not VMMMapPage(VirtualAddress, PhysicalAddress, Flags) then
  begin
    Result := krBusy;
    Exit;
  end;

  MemoryObjectTable[ObjectIndex].Mappings[FreeMapping].Active := 1;
  MemoryObjectTable[ObjectIndex].Mappings[FreeMapping].ObjectPageIndex := ObjectPageIndex;
  MemoryObjectTable[ObjectIndex].Mappings[FreeMapping].VirtualAddress := VirtualAddress;
  Result := krSuccess;
end;

function MemoryObjectUnmapPage(Handle: LongWord;
  VirtualAddress: QWord): TKernelResult;
var
  ObjectIndex: Word;
  ObjectGeneration: Word;
  I: LongWord;
  PhysicalAddress: QWord;
  ExpectedPhysical: QWord;
  PageIndex: Word;
begin
  Result := MemoryObjectResolve(Handle, CAP_RIGHT_MAP,
                                ObjectIndex, ObjectGeneration);
  if Result <> krSuccess then
    Exit;

  for I := 0 to MEMORY_OBJECT_MAX_MAPPINGS - 1 do
    if (MemoryObjectTable[ObjectIndex].Mappings[I].Active <> 0) and
       (MemoryObjectTable[ObjectIndex].Mappings[I].VirtualAddress = VirtualAddress) then
    begin
      PageIndex := MemoryObjectTable[ObjectIndex].Mappings[I].ObjectPageIndex;
      ExpectedPhysical := MemoryObjectTable[ObjectIndex].Pages[PageIndex];
      if not VMMUnmapPage(VirtualAddress, PhysicalAddress) then
      begin
        Result := krInvalidArgument;
        Exit;
      end;
      if PhysicalAddress <> ExpectedPhysical then
      begin
        WriteText('[FATAL] Memory-object mapping physical page mismatch.'#13#10);
        HaltForever;
      end;
      MemoryObjectTable[ObjectIndex].Mappings[I].Active := 0;
      MemoryObjectTable[ObjectIndex].Mappings[I].VirtualAddress := 0;
      MemoryObjectTable[ObjectIndex].Mappings[I].ObjectPageIndex := 0;
      Result := krSuccess;
      Exit;
    end;
  Result := krInvalidArgument;
end;

function MemoryObjectDestroy(Handle: LongWord): TKernelResult;
var
  ObjectIndex: Word;
  ObjectGeneration: Word;
  I: LongWord;
begin
  Result := MemoryObjectResolve(Handle, CAP_RIGHT_MANAGE,
                                ObjectIndex, ObjectGeneration);
  if Result <> krSuccess then
    Exit;

  for I := 0 to MEMORY_OBJECT_MAX_MAPPINGS - 1 do
    if MemoryObjectTable[ObjectIndex].Mappings[I].Active <> 0 then
    begin
      Result := krBusy;
      Exit;
    end;

  for I := 0 to MemoryObjectTable[ObjectIndex].PageCount - 1 do
    if not PMMFreePage(MemoryObjectTable[ObjectIndex].Pages[I]) then
    begin
      WriteText('[FATAL] Memory-object page could not be returned to PMM.'#13#10);
      HaltForever;
    end;

  MemoryObjectTable[ObjectIndex].Active := 0;
  MemoryObjectTable[ObjectIndex].PageCount := 0;
  CapabilityRevokeObject(okMemoryObject, ObjectIndex, ObjectGeneration);
  Result := krSuccess;
end;

function FindMemoryObjectTestAddress(var BaseAddress: QWord): Boolean;
var
  Attempt: LongWord;
  Candidate: QWord;
  PhysicalAddress: QWord;
begin
  Result := False;
  Candidate := MEMORY_OBJECT_TEST_BASE;
  for Attempt := 0 to 4095 do
  begin
    if not VMMTranslate(Candidate, PhysicalAddress) and
       not VMMTranslate(Candidate + PAGE_SIZE, PhysicalAddress) then
    begin
      BaseAddress := Candidate;
      Result := True;
      Exit;
    end;
    Inc(Candidate, $40000000);
  end;
end;

function MemoryObjectAliasSelfTest: Boolean;
var
  Handle: LongWord;
  BaseAddress: QWord;
  Flags: QWord;
  ByteA: PByte;
  ByteB: PByte;
  StatusA, StatusB: TKernelResult;
begin
  Result := False;
  if not FindMemoryObjectTestAddress(BaseAddress) then
  begin
    WriteText('[FAIL] Could not find two unused adjacent virtual pages.'#13#10);
    Exit;
  end;
  if MemoryObjectCreate(1, Handle) <> krSuccess then
  begin
    WriteText('[FAIL] Could not create memory object.'#13#10);
    Exit;
  end;

  Flags := PTE_WRITABLE;
  if CPUHasNXSupport then
    Flags := Flags or PTE_NO_EXECUTE;

  StatusA := MemoryObjectMapPage(Handle, 0, BaseAddress, Flags);
  StatusB := MemoryObjectMapPage(Handle, 0, BaseAddress + PAGE_SIZE, Flags);
  if (StatusA <> krSuccess) or (StatusB <> krSuccess) then
  begin
    if StatusA = krSuccess then
      MemoryObjectUnmapPage(Handle, BaseAddress);
    if StatusB = krSuccess then
      MemoryObjectUnmapPage(Handle, BaseAddress + PAGE_SIZE);
    MemoryObjectDestroy(Handle);
    WriteText('[FAIL] Could not map memory object aliases.'#13#10);
    Exit;
  end;

  ByteA := PByte(PtrUInt(BaseAddress));
  ByteB := PByte(PtrUInt(BaseAddress + PAGE_SIZE));
  ByteA^ := $A5;
  Result := ByteB^ = $A5;

  StatusA := MemoryObjectUnmapPage(Handle, BaseAddress);
  StatusB := MemoryObjectUnmapPage(Handle, BaseAddress + PAGE_SIZE);
  if (StatusA <> krSuccess) or (StatusB <> krSuccess) then
    Result := False;
  if MemoryObjectDestroy(Handle) <> krSuccess then
    Result := False
  else if MemoryObjectMapPage(Handle, 0, BaseAddress, Flags) <> krInvalidHandle then
    Result := False;

  if Result then
    WriteText('[ OK ] Shared memory object alias/map/unmap test passed.'#13#10)
  else
    WriteText('[FAIL] Shared memory object alias test failed.'#13#10);
end;
