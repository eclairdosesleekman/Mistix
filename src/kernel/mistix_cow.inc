{ Phase 19: COW uses the PMM frame reference count as its sole owner. }

procedure COWInitialize;
begin
  WriteText('[ OK ] COW page ownership is backed by PMM frame refcounts.'#13#10);
end;

function COWAcquireSharedPage(PhysicalAddress: QWord): Boolean;
begin
  Result := PageGet(PhysicalAddress);
end;

function COWReleasePage(PhysicalAddress: QWord): Boolean;
begin
  Result := PagePut(PhysicalAddress);
end;

function COWGetLeaf(RootPhysical, VirtualAddress: QWord;
  out Leaf: PQWord): Boolean;
var
  PML4: PPageTable;
  PDPT: PPageTable;
  PD: PPageTable;
  PT: PPageTable;
  EntryValue: QWord;
begin
  Result := False;
  Leaf := nil;
  if (RootPhysical = 0) or (VirtualAddress >= USER_ADDRESS_LIMIT) then Exit;
  PML4 := PhysicalTablePointer(RootPhysical);
  EntryValue := PML4^[(VirtualAddress shr 39) and $1FF];
  if (EntryValue and PTE_PRESENT) = 0 then Exit;
  PDPT := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);
  EntryValue := PDPT^[(VirtualAddress shr 30) and $1FF];
  if ((EntryValue and PTE_PRESENT) = 0) or ((EntryValue and PTE_HUGE) <> 0) then Exit;
  PD := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);
  EntryValue := PD^[(VirtualAddress shr 21) and $1FF];
  if ((EntryValue and PTE_PRESENT) = 0) or ((EntryValue and PTE_HUGE) <> 0) then Exit;
  PT := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);
  Leaf := @PT^[(VirtualAddress shr 12) and $1FF];
  Result := (Leaf^ and PTE_PRESENT) <> 0;
end;

function COWMapClonePage(SourceRoot, DestinationRoot,
  VirtualAddress: QWord): Boolean;
var
  Leaf: PQWord;
  EntryValue: QWord;
  PhysicalAddress: QWord;
  Flags: QWord;
begin
  Result := False;
  if not COWGetLeaf(SourceRoot, VirtualAddress, Leaf) then Exit;
  EntryValue := Leaf^;
  PhysicalAddress := EntryValue and PTE_ADDRESS_MASK;
  Flags := EntryValue and PTE_ALLOWED_FLAGS;
  if (Flags and PTE_WRITABLE) <> 0 then
  begin
    Flags := Flags and not PTE_WRITABLE;
    Flags := Flags or PTE_COW;
  end;
  if not COWAcquireSharedPage(PhysicalAddress) then Exit;
  if not AddressSpaceMapPage(DestinationRoot, VirtualAddress,
                             PhysicalAddress, Flags) then
  begin
    if not COWReleasePage(PhysicalAddress) then
    begin
      WriteText('[FATAL] COW clone rollback failed.'#13#10);
      HaltForever;
    end;
    Exit;
  end;
  Result := True;
end;

function COWCloneUserMappings(SourceRoot, DestinationRoot: QWord): Boolean;
var
  PML4: PPageTable;
  PDPT: PPageTable;
  PD: PPageTable;
  PT: PPageTable;
  E4, E3, E2, E1: QWord;
  I4, I3, I2, I1: LongWord;
  VirtualAddress: QWord;
  EntryValue: QWord;
begin
  Result := False;
  PML4 := PhysicalTablePointer(SourceRoot);
  for I4 := 0 to 255 do
  begin
    E4 := PML4^[I4];
    if (E4 and PTE_PRESENT) = 0 then Continue;
    PDPT := PhysicalTablePointer(E4 and PTE_ADDRESS_MASK);
    for I3 := 0 to 511 do
    begin
      E3 := PDPT^[I3];
      if (E3 and PTE_PRESENT) = 0 then Continue;
      if (E3 and PTE_HUGE) <> 0 then Exit;
      PD := PhysicalTablePointer(E3 and PTE_ADDRESS_MASK);
      for I2 := 0 to 511 do
      begin
        E2 := PD^[I2];
        if (E2 and PTE_PRESENT) = 0 then Continue;
        if (E2 and PTE_HUGE) <> 0 then Exit;
        PT := PhysicalTablePointer(E2 and PTE_ADDRESS_MASK);
        for I1 := 0 to 511 do
        begin
          E1 := PT^[I1];
          if (E1 and PTE_PRESENT) = 0 then Continue;
          if (E1 and PTE_USER) = 0 then Exit;
          VirtualAddress := (QWord(I4) shl 39) or (QWord(I3) shl 30) or
                            (QWord(I2) shl 21) or (QWord(I1) shl 12);
          if not COWMapClonePage(SourceRoot, DestinationRoot,
                                 VirtualAddress) then Exit;
        end;
      end;
    end;
  end;
  Result := True;
end;

procedure COWMarkSourceMappings(SourceRoot: QWord);
var
  PML4: PPageTable;
  PDPT: PPageTable;
  PD: PPageTable;
  PT: PPageTable;
  E4, E3, E2, E1: QWord;
  I4, I3, I2, I1: LongWord;
  VirtualAddress: QWord;
begin
  PML4 := PhysicalTablePointer(SourceRoot);
  for I4 := 0 to 255 do
  begin
    E4 := PML4^[I4];
    if (E4 and PTE_PRESENT) = 0 then Continue;
    PDPT := PhysicalTablePointer(E4 and PTE_ADDRESS_MASK);
    for I3 := 0 to 511 do
    begin
      E3 := PDPT^[I3];
      if ((E3 and PTE_PRESENT) = 0) or ((E3 and PTE_HUGE) <> 0) then Continue;
      PD := PhysicalTablePointer(E3 and PTE_ADDRESS_MASK);
      for I2 := 0 to 511 do
      begin
        E2 := PD^[I2];
        if ((E2 and PTE_PRESENT) = 0) or ((E2 and PTE_HUGE) <> 0) then Continue;
        PT := PhysicalTablePointer(E2 and PTE_ADDRESS_MASK);
        for I1 := 0 to 511 do
        begin
          E1 := PT^[I1];
          if ((E1 and (PTE_PRESENT or PTE_USER or PTE_WRITABLE)) <>
              (PTE_PRESENT or PTE_USER or PTE_WRITABLE)) then Continue;
          PT^[I1] := (E1 and not PTE_WRITABLE) or PTE_COW;
          VirtualAddress := (QWord(I4) shl 39) or (QWord(I3) shl 30) or
                            (QWord(I2) shl 21) or (QWord(I1) shl 12);
          if (ReadCR3 and PTE_ADDRESS_MASK) = SourceRoot then
            InvalidatePage(VirtualAddress);
        end;
      end;
    end;
  end;
end;

function COWForkAddressSpace(SourceRoot: QWord;
  out DestinationRoot: QWord): Boolean;
begin
  Result := False;
  DestinationRoot := 0;
  if not AddressSpaceCreate(DestinationRoot) then Exit;
  if not COWCloneUserMappings(SourceRoot, DestinationRoot) then
  begin
    if not AddressSpaceDestroy(DestinationRoot) then
    begin
      WriteText('[FATAL] Could not destroy a partially cloned address space.'#13#10);
      HaltForever;
    end;
    DestinationRoot := 0;
    Exit;
  end;
  COWMarkSourceMappings(SourceRoot);
  Result := True;
end;

function COWEnsureWritable(RootPhysical, VirtualAddress: QWord): Boolean;
var
  Leaf: PQWord;
  EntryValue: QWord;
  PhysicalAddress: QWord;
  NewPhysicalAddress: QWord;
  ReferenceCount: LongWord;
  SourceBytes: PByte;
  DestinationBytes: PByte;
  I: LongWord;
begin
  Result := False;
  if not COWGetLeaf(RootPhysical, VirtualAddress, Leaf) then Exit;
  EntryValue := Leaf^;
  if (EntryValue and PTE_USER) = 0 then Exit;
  if (EntryValue and PTE_WRITABLE) <> 0 then
  begin
    Result := True;
    Exit;
  end;
  if (EntryValue and PTE_COW) = 0 then Exit;
  PhysicalAddress := EntryValue and PTE_ADDRESS_MASK;
  ReferenceCount := PageReferenceCount(PhysicalAddress);
  if ReferenceCount = 0 then Exit;

  if ReferenceCount = 1 then
  begin
    Leaf^ := (EntryValue or PTE_WRITABLE) and not PTE_COW;
    if (ReadCR3 and PTE_ADDRESS_MASK) = RootPhysical then
      InvalidatePage(VirtualAddress and PAGE_MASK);
    Result := True;
    Exit;
  end;

  if not PMMAllocatePage(NewPhysicalAddress) then Exit;
  SourceBytes := PByte(PtrUInt(MemoryHHDMOffset + PhysicalAddress));
  DestinationBytes := PByte(PtrUInt(MemoryHHDMOffset + NewPhysicalAddress));
  for I := 0 to PAGE_SIZE - 1 do
    DestinationBytes[I] := SourceBytes[I];
  Leaf^ := (NewPhysicalAddress and PTE_ADDRESS_MASK) or PTE_PRESENT or
           (((EntryValue and PTE_ALLOWED_FLAGS) or PTE_WRITABLE) and not PTE_COW);
  if not PagePut(PhysicalAddress) then
  begin
    WriteText('[FATAL] COW could not drop the replaced frame reference.'#13#10);
    HaltForever;
  end;
  if (ReadCR3 and PTE_ADDRESS_MASK) = RootPhysical then
    InvalidatePage(VirtualAddress and PAGE_MASK);
  Result := True;
end;

function COWHandlePageFault(RootPhysical, FaultAddress,
  ErrorCode: QWord): Boolean;
begin
  Result := False;
  { Present + write + user fault, and the leaf must be marked COW. }
  if (ErrorCode and $7) <> $7 then Exit;
  Result := COWEnsureWritable(RootPhysical, FaultAddress);
end;
