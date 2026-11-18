function PMMSaveAndDisableInterrupts: QWord; assembler; nostackframe;
asm
  pushfq
  pop rax
  cli
end;

procedure PMMRestoreInterrupts(Flags: QWord); assembler; nostackframe;
asm
  test rdi, $200
  jz @restore_done
  sti
@restore_done:
end;

function PMMIsCanonicalAddress(Address: QWord): Boolean;
var
  TopBits: QWord;
begin
  TopBits := Address shr 48;
  if (Address and $0000800000000000) = 0 then
    Result := TopBits = 0
  else
    Result := TopBits = $FFFF;
end;

function PMMPhysicalToHHDM(PhysicalAddress: QWord;
  out VirtualAddress: QWord): Boolean;
begin
  Result := False;
  VirtualAddress := 0;
  if PhysicalAddress > High(QWord) - MemoryHHDMOffset then Exit;
  VirtualAddress := MemoryHHDMOffset + PhysicalAddress;
  Result := PMMIsCanonicalAddress(VirtualAddress);
end;

function PMMAppendRange(var Ranges: TPMMRangeArray;
  var Count: LongWord; StartPFN, EndPFN: QWord): Boolean;
begin
  Result := False;
  if EndPFN <= StartPFN then Exit;
  if Count >= PMM_MAX_REGION_COUNT then Exit;
  Ranges[Count].StartPFN := StartPFN;
  Ranges[Count].EndPFN := EndPFN;
  Inc(Count);
  Result := True;
end;

procedure PMMSortAndMergeRanges(var Ranges: TPMMRangeArray;
  var Count: LongWord);
var
  I, J, WriteIndex: LongWord;
  Item: TMemoryRange;
begin
  if Count < 2 then Exit;
  for I := 1 to Count - 1 do
  begin
    Item := Ranges[I];
    J := I;
    while (J > 0) and
      (Ranges[J - 1].StartPFN > Item.StartPFN) do
    begin
      Ranges[J] := Ranges[J - 1];
      Dec(J);
    end;
    Ranges[J] := Item;
  end;

  WriteIndex := 0;
  for I := 1 to Count - 1 do
  begin
    if Ranges[I].StartPFN <= Ranges[WriteIndex].EndPFN then
    begin
      if Ranges[I].EndPFN > Ranges[WriteIndex].EndPFN then
        Ranges[WriteIndex].EndPFN := Ranges[I].EndPFN;
    end
    else
    begin
      Inc(WriteIndex);
      Ranges[WriteIndex] := Ranges[I];
    end;
  end;
  Count := WriteIndex + 1;
end;

function PMMNormalizeMemoryMap(Memmap: PLimineMemmapResponse;
  out NormalizedCount: LongWord): Boolean;
var
  I, J: QWord;
  UsableCount, ReservedCount: LongWord;
  StartPFN, EndPFN, CursorPFN, SegmentEnd: QWord;
  Entry: PLimineMemmapEntry;
begin
  Result := False;
  NormalizedCount := 0;
  UsableCount := 0;
  ReservedCount := 0;
  if Memmap = nil then Exit;
  if Memmap^.EntryCount > LIMINE_MAX_MEMMAP_ENTRIES then Exit;
  if (Memmap^.EntryCount <> 0) and (Memmap^.Entries = nil) then Exit;

  if Memmap^.EntryCount <> 0 then
    for I := 0 to Memmap^.EntryCount - 1 do
    begin
      Entry := Memmap^.Entries^[I];
    if Entry = nil then Exit;
    if Entry^.Length = 0 then Continue;
    if Entry^.Length > High(QWord) - Entry^.Base then Exit;

    { All non-USABLE types win over overlapping USABLE ranges. Reserved
      ranges round outwards; USABLE ranges round inwards to full pages. }
    if Entry^.EntryType = LIMINE_MEMMAP_USABLE then
    begin
      StartPFN := Entry^.Base shr 12;
      if (Entry^.Base and (PAGE_SIZE - 1)) <> 0 then Inc(StartPFN);
      EndPFN := (Entry^.Base + Entry^.Length) shr 12;
      if StartPFN = 0 then StartPFN := 1;
      if EndPFN <= StartPFN then Continue;
      if UsableCount >= LIMINE_MAX_MEMMAP_ENTRIES then Exit;
      if not PMMAppendRange(PMMUsableRanges, UsableCount,
                            StartPFN, EndPFN) then Exit;
    end
    else
    begin
      StartPFN := Entry^.Base shr 12;
      EndPFN := (Entry^.Base + Entry^.Length) shr 12;
      if ((Entry^.Base + Entry^.Length) and (PAGE_SIZE - 1)) <> 0 then
        Inc(EndPFN);
      if EndPFN <= StartPFN then Continue;
      if ReservedCount >= LIMINE_MAX_MEMMAP_ENTRIES then Exit;
      if not PMMAppendRange(PMMReservedRanges, ReservedCount,
                            StartPFN, EndPFN) then Exit;
    end;
  end;

  PMMSortAndMergeRanges(PMMUsableRanges, UsableCount);
  PMMSortAndMergeRanges(PMMReservedRanges, ReservedCount);

  { Subtract the merged reserved intervals from the merged usable intervals.
    The result is sorted, disjoint and safe to hand to the page allocator. }
  if UsableCount <> 0 then
    for I := 0 to UsableCount - 1 do
    begin
      CursorPFN := PMMUsableRanges[I].StartPFN;
      EndPFN := PMMUsableRanges[I].EndPFN;
      if ReservedCount <> 0 then
        for J := 0 to ReservedCount - 1 do
        begin
          if PMMReservedRanges[J].EndPFN <= CursorPFN then Continue;
          if PMMReservedRanges[J].StartPFN >= EndPFN then Break;
          if PMMReservedRanges[J].StartPFN > CursorPFN then
          begin
            SegmentEnd := PMMReservedRanges[J].StartPFN;
            if SegmentEnd > EndPFN then SegmentEnd := EndPFN;
            if not PMMAppendRange(PMMNormalizedRanges, NormalizedCount,
                                  CursorPFN, SegmentEnd) then Exit;
          end;
          if PMMReservedRanges[J].EndPFN > CursorPFN then
            CursorPFN := PMMReservedRanges[J].EndPFN;
          if CursorPFN >= EndPFN then Break;
        end;
      if CursorPFN < EndPFN then
        if not PMMAppendRange(PMMNormalizedRanges, NormalizedCount,
                              CursorPFN, EndPFN) then Exit;
    end;

  { PTE address bits are the final physical-address limit. }
  if NormalizedCount <> 0 then
    for I := 0 to NormalizedCount - 1 do
    begin
      if PMMNormalizedRanges[I].StartPFN >
         (PTE_ADDRESS_MASK shr 12) then
        PMMNormalizedRanges[I].EndPFN := PMMNormalizedRanges[I].StartPFN
      else if PMMNormalizedRanges[I].EndPFN >
         (PTE_ADDRESS_MASK shr 12) + 1 then
        PMMNormalizedRanges[I].EndPFN := (PTE_ADDRESS_MASK shr 12) + 1;
    end;

  { Drop ranges clipped to zero length. }
  J := 0;
  if NormalizedCount <> 0 then
    for I := 0 to NormalizedCount - 1 do
      if PMMNormalizedRanges[I].EndPFN >
         PMMNormalizedRanges[I].StartPFN then
      begin
        PMMNormalizedRanges[J] := PMMNormalizedRanges[I];
        Inc(J);
      end;
  NormalizedCount := LongWord(J);
  Result := True;
end;

function PMMFrameForPFN(PFN: QWord; out RegionIndex: LongInt): PPageFrame;
var
  LowIndex, HighIndex, Middle: LongInt;
  Region: TMemoryRegion;
  Offset, FrameAddress: QWord;
begin
  Result := nil;
  RegionIndex := -1;
  LowIndex := 0;
  HighIndex := LongInt(PMMRegionCount) - 1;
  while LowIndex <= HighIndex do
  begin
    Middle := LowIndex + ((HighIndex - LowIndex) div 2);
    Region := PMMMemoryRegions[Middle];
    if PFN < Region.BasePFN then
      HighIndex := Middle - 1
    else if (PFN - Region.BasePFN) >= Region.PageCount then
      LowIndex := Middle + 1
    else
    begin
      Offset := PFN - Region.BasePFN;
      FrameAddress := QWord(PtrUInt(Region.Frames)) +
                      Offset * SizeOf(TPageFrame);
      Result := PPageFrame(PtrUInt(FrameAddress));
      RegionIndex := Middle;
      Exit;
    end;
  end;
end;

function PMMPhysicalRangeToPointer(PhysicalAddress, ByteCount: QWord;
  out Buffer: PByte): Boolean;
var
  VirtualAddress: QWord;
begin
  Result := False;
  Buffer := nil;
  if not PMMPhysicalToHHDM(PhysicalAddress, VirtualAddress) then Exit;
  if (ByteCount <> 0) and
     (VirtualAddress > High(QWord) - (ByteCount - 1)) then Exit;
  Buffer := PByte(PtrUInt(VirtualAddress));
  Result := True;
end;

procedure PMMZeroPhysical(PhysicalAddress, ByteCount: QWord);
var
  Buffer: PByte;
  I: QWord;
begin
  if ByteCount = 0 then Exit;
  if not PMMPhysicalRangeToPointer(PhysicalAddress, ByteCount, Buffer) then Exit;
  for I := 0 to ByteCount - 1 do Buffer[I] := 0;
end;

function PMMZoneForPFN(PFN: QWord): TMemoryZoneKind;
begin
  if PFN < PMM_DMA32_LIMIT_PFN then
    Result := mzDMA32
  else
    Result := mzNormal;
end;

function PMMListInsert(PFN: QWord; Order: LongInt;
  Zone: TMemoryZoneKind): Boolean;
var
  Frame, OldHead: PPageFrame;
  HeadPFN: QWord;
  RegionIndex: LongInt;
begin
  Result := False;
  if (Order < 0) or (Order > PMM_MAX_ORDER) then Exit;
  Frame := PMMFrameForPFN(PFN, RegionIndex);
  if Frame = nil then Exit;
  HeadPFN := BuddyFreeLists[Ord(Zone), Order];
  Frame^.RefCount := 0;
  Frame^.PinCount := 0;
  Frame^.OwnerPFN := 0;
  Frame^.Flags := 0;
  Frame^.Order := ShortInt(Order);
  Frame^.Zone := Ord(Zone);
  Frame^.State := pgFree;
  Frame^.PrevPFN := 0;
  Frame^.NextPFN := HeadPFN;
  if HeadPFN <> 0 then
  begin
    OldHead := PMMFrameForPFN(HeadPFN, RegionIndex);
    if OldHead = nil then Exit;
    OldHead^.PrevPFN := PFN;
  end;
  BuddyFreeLists[Ord(Zone), Order] := PFN;
  Result := True;
end;

function PMMListRemove(PFN: QWord; Order: LongInt;
  Zone: TMemoryZoneKind): Boolean;
var
  Frame, Previous, Next: PPageFrame;
  RegionIndex: LongInt;
begin
  Result := False;
  Frame := PMMFrameForPFN(PFN, RegionIndex);
  if (Frame = nil) or (Frame^.State <> pgFree) or
     (Frame^.Order <> Order) or (Frame^.Zone <> Ord(Zone)) then Exit;

  if Frame^.PrevPFN = 0 then
  begin
    if BuddyFreeLists[Ord(Zone), Order] <> PFN then Exit;
    BuddyFreeLists[Ord(Zone), Order] := Frame^.NextPFN;
  end
  else
  begin
    Previous := PMMFrameForPFN(Frame^.PrevPFN, RegionIndex);
    if Previous = nil then Exit;
    Previous^.NextPFN := Frame^.NextPFN;
  end;

  if Frame^.NextPFN <> 0 then
  begin
    Next := PMMFrameForPFN(Frame^.NextPFN, RegionIndex);
    if Next = nil then Exit;
    Next^.PrevPFN := Frame^.PrevPFN;
  end;
  Frame^.NextPFN := 0;
  Frame^.PrevPFN := 0;
  Frame^.OwnerPFN := 0;
  Frame^.RefCount := 0;
  Frame^.Order := -1;
  Frame^.State := pgTail;
  Result := True;
end;

function PMMAddFreeRange(StartPFN, EndPFN: QWord;
  Zone: TMemoryZoneKind): Boolean;
var
  Remaining, BlockPages: QWord;
  Order: LongInt;
begin
  Result := False;
  while StartPFN < EndPFN do
  begin
    Remaining := EndPFN - StartPFN;
    Order := PMM_MAX_ORDER;
    while Order > 0 do
    begin
      BlockPages := QWord(1) shl Order;
      if (BlockPages <= Remaining) and
         ((StartPFN and (BlockPages - 1)) = 0) then Break;
      Dec(Order);
    end;
    BlockPages := QWord(1) shl Order;
    if not PMMListInsert(StartPFN, Order, Zone) then Exit;
    Inc(PMMFreePageCount, BlockPages);
    Inc(StartPFN, BlockPages);
  end;
  Result := True;
end;

function PMMBuildRegions: Boolean;
var
  I: LongWord;
  J, StartPFN, EndPFN, PageCount, FrameIndex: QWord;
  MetadataBytes, MetadataPages, MetadataPhysical: QWord;
  MetadataVirtual: QWord;
  FreeStart, DMAEnd, NormalStart: QWord;
  Frame: PPageFrame;
  FrameAddress: QWord;
begin
  Result := False;
  PMMRegionCount := 0;
  PMMManagedPageCount := 0;

  for I := 0 to PMMNormalizedRangeCount - 1 do
  begin
    StartPFN := PMMNormalizedRanges[I].StartPFN;
    EndPFN := PMMNormalizedRanges[I].EndPFN;
    if StartPFN = 0 then StartPFN := 1;
    if EndPFN <= StartPFN then Continue;
    PageCount := EndPFN - StartPFN;
    if PageCount > High(QWord) div SizeOf(TPageFrame) then Exit;
    MetadataBytes := PageCount * SizeOf(TPageFrame);
    MetadataPages := MetadataBytes div PAGE_SIZE;
    if (MetadataBytes mod PAGE_SIZE) <> 0 then Inc(MetadataPages);
    { Very small regions are not worth managing if their own frame metadata
      would consume the complete region. }
    if MetadataPages >= PageCount then Continue;
    if PMMRegionCount >= PMM_MAX_REGION_COUNT then Exit;

    MetadataPhysical := StartPFN shl 12;
    if not PMMPhysicalToHHDM(MetadataPhysical, MetadataVirtual) then Continue;
    if MetadataBytes > High(PtrUInt) then Exit;

    PMMMemoryRegions[PMMRegionCount].BasePFN := StartPFN;
    PMMMemoryRegions[PMMRegionCount].PageCount := PageCount;
    PMMMemoryRegions[PMMRegionCount].MetadataPhysical := MetadataPhysical;
    PMMMemoryRegions[PMMRegionCount].Frames :=
      PPageFrame(PtrUInt(MetadataVirtual));
    Inc(PMMRegionCount); { publish this region before free-list insertion }

    for FrameIndex := 0 to PageCount - 1 do
    begin
      FrameAddress := MetadataVirtual + FrameIndex * SizeOf(TPageFrame);
      Frame := PPageFrame(PtrUInt(FrameAddress));
      Frame^.RefCount := 0;
      Frame^.Flags := 0;
      Frame^.NextPFN := 0;
      Frame^.PrevPFN := 0;
      Frame^.PinCount := 0;
      Frame^.OwnerPFN := 0;
      Frame^.Order := -1;
      Frame^.Zone := Ord(PMMZoneForPFN(StartPFN + FrameIndex));
      Frame^.State := pgTail;
      Frame^.Reserved := 0;
    end;

    for J := 0 to MetadataPages - 1 do
    begin
      Frame := PPageFrame(PtrUInt(MetadataVirtual +
        J * SizeOf(TPageFrame)));
      Frame^.State := pgPinned;
      Frame^.Flags := PMM_FRAME_FLAG_PERMANENT;
    end;

    FreeStart := StartPFN + MetadataPages;
    if FreeStart < EndPFN then
    begin
      DMAEnd := EndPFN;
      if DMAEnd > PMM_DMA32_LIMIT_PFN then
        DMAEnd := PMM_DMA32_LIMIT_PFN;
      if FreeStart < DMAEnd then
        if not PMMAddFreeRange(FreeStart, DMAEnd, mzDMA32) then Exit;
      NormalStart := FreeStart;
      if NormalStart < PMM_DMA32_LIMIT_PFN then
        NormalStart := PMM_DMA32_LIMIT_PFN;
      if NormalStart < EndPFN then
        if not PMMAddFreeRange(NormalStart, EndPFN, mzNormal) then Exit;
    end;

    Inc(PMMManagedPageCount, PageCount - MetadataPages);
  end;
  Result := PMMRegionCount <> 0;
end;

function PMMInitialize(Memmap: PLimineMemmapResponse): Boolean;
var
  I, J: LongWord;
begin
  Result := False;
  PMMFreePageCount := 0;
  PMMManagedPageCount := 0;
  PMMRegionCount := 0;
  PMMNormalizedRangeCount := 0;
  for I := 0 to 1 do
    for J := 0 to PMM_MAX_ORDER do
      BuddyFreeLists[I, J] := 0;

{$ifdef MISTIX_DEBUG_CHECKS}
  WriteText('[DEBUG] PMMInitialize: normalizing firmware memory map.'#13#10);
{$endif}
  if not PMMNormalizeMemoryMap(Memmap, PMMNormalizedRangeCount) then
  begin
    WriteText('[FAIL] PMM memory-map normalization rejected invalid ranges.'#13#10);
    Exit;
  end;
  if PMMNormalizedRangeCount = 0 then
  begin
    WriteText('[FAIL] PMM memory map contains no complete usable pages.'#13#10);
    Exit;
  end;
  if not PMMBuildRegions then
  begin
    WriteText('[FAIL] PMM could not build frame metadata and buddy zones.'#13#10);
    Exit;
  end;
  if PMMFreePageCount = 0 then
  begin
    WriteText('[FAIL] PMM has no allocatable pages after metadata reservation.'#13#10);
    Exit;
  end;

  Result := True;
  WriteText('[ OK ] Buddy PMM initialised; managed pages: ');
  WriteDecimal(PMMManagedPageCount);
  WriteText(', free 4 KiB pages: ');
  WriteDecimal(PMMFreePageCount);
  WriteText(', regions: ');
  WriteDecimal(PMMRegionCount);
  WriteText(#13#10);
end;

function PMMFreePagesUnlocked(BasePhysical: QWord;
  Order: Byte): Boolean;
var
  BasePFN, BuddyPFN, BlockPages: QWord;
  CurrentOrder, RegionIndex, BuddyRegionIndex: LongInt;
  Frame, BuddyFrame: PPageFrame;
  OriginalPFN, TailIndex: QWord;
  Zone: TMemoryZoneKind;
begin
  Result := False;
  if Order > PMM_MAX_ORDER then Exit;
  BlockPages := QWord(1) shl Order;
  if (BasePhysical and ((BlockPages shl 12) - 1)) <> 0 then Exit;
  BasePFN := BasePhysical shr 12;
  OriginalPFN := BasePFN;
  Frame := PMMFrameForPFN(BasePFN, RegionIndex);
  if Frame = nil then Exit;
  if (Frame^.Order <> ShortInt(Order)) or (Frame^.RefCount <> 1) or
     (Frame^.PinCount <> 0) or
     not (Frame^.State in [pgAllocated, pgTable, pgSlab, pgPinned]) or
     ((Frame^.Flags and PMM_FRAME_FLAG_PERMANENT) <> 0) then Exit;
  if (Frame^.Flags and (PMM_FRAME_FLAG_HEAP_BIG or
       PMM_FRAME_FLAG_HEAP_PAGES)) <> 0 then
  begin
    { Invalidate owner metadata on every non-head page before the block can
      be reused for an unrelated allocation. }
    if Order <> 0 then
      for TailIndex := 1 to (QWord(1) shl Order) - 1 do
      begin
        BuddyFrame := PMMFrameForPFN((BasePhysical shr 12) + TailIndex,
                                     BuddyRegionIndex);
        if BuddyFrame = nil then Exit;
        BuddyFrame^.OwnerPFN := 0;
        BuddyFrame^.Flags := 0;
        BuddyFrame^.RefCount := 0;
        BuddyFrame^.Order := -1;
        BuddyFrame^.State := pgTail;
      end;
  end;
  Zone := TMemoryZoneKind(Frame^.Zone);
  CurrentOrder := Order;
  while CurrentOrder < PMM_MAX_ORDER do
  begin
    BuddyPFN := BasePFN xor (QWord(1) shl CurrentOrder);
    BuddyFrame := PMMFrameForPFN(BuddyPFN, BuddyRegionIndex);
    if (BuddyFrame = nil) or (BuddyRegionIndex <> RegionIndex) or
       (BuddyFrame^.State <> pgFree) or
       (BuddyFrame^.Order <> CurrentOrder) or
       (BuddyFrame^.Zone <> Ord(Zone)) then Break;
    if not PMMListRemove(BuddyPFN, CurrentOrder, Zone) then Exit;
    if BuddyPFN < BasePFN then BasePFN := BuddyPFN;
    Inc(CurrentOrder);
  end;

  if BasePFN <> OriginalPFN then
  begin
    Frame^.RefCount := 0;
    Frame^.OwnerPFN := 0;
    Frame^.Flags := 0;
    Frame^.Order := -1;
    Frame^.State := pgTail;
  end;
  if not PMMListInsert(BasePFN, CurrentOrder, Zone) then Exit;
  Inc(PMMFreePageCount, BlockPages);
  Result := True;
end;

function PMMAllocPages(Order: Byte; Flags: TAllocFlags;
  out BasePhysical: QWord): Boolean;
var
  SavedFlags: QWord;
  SearchOrder, FoundOrder, SearchZone: LongInt;
  SelectedPFN, BuddyPFN, Pages: QWord;
  Zone: TMemoryZoneKind;
  Frame, BuddyFrame: PPageFrame;
  RegionIndex: LongInt;
  VirtualAddress: QWord;
begin
  Result := False;
  BasePhysical := 0;
  if Order > PMM_MAX_ORDER then Exit;
  if (afKernelHeapBig in Flags) and (afKernelHeapPages in Flags) then Exit;
  SavedFlags := PMMSaveAndDisableInterrupts;

  FoundOrder := -1;
  SearchZone := -1;
  if afDMA32 in Flags then
    SearchZone := Ord(mzDMA32)
  else
    SearchZone := Ord(mzNormal);

  for SearchOrder := Order to PMM_MAX_ORDER do
    if BuddyFreeLists[SearchZone, SearchOrder] <> 0 then
    begin
      FoundOrder := SearchOrder;
      Break;
    end;

  if (FoundOrder < 0) and not (afDMA32 in Flags) then
  begin
    SearchZone := Ord(mzDMA32);
    for SearchOrder := Order to PMM_MAX_ORDER do
      if BuddyFreeLists[SearchZone, SearchOrder] <> 0 then
      begin
        FoundOrder := SearchOrder;
        Break;
      end;
  end;

  if FoundOrder < 0 then
  begin
    PMMRestoreInterrupts(SavedFlags);
    Exit;
  end;

  Zone := TMemoryZoneKind(SearchZone);
  SelectedPFN := BuddyFreeLists[SearchZone, FoundOrder];
  if not PMMListRemove(SelectedPFN, FoundOrder, Zone) then
  begin
    PMMRestoreInterrupts(SavedFlags);
    Exit;
  end;

  while FoundOrder > Order do
  begin
    Dec(FoundOrder);
    BuddyPFN := SelectedPFN + (QWord(1) shl FoundOrder);
    if not PMMListInsert(BuddyPFN, FoundOrder, Zone) then
    begin
      PMMRestoreInterrupts(SavedFlags);
      Exit;
    end;
  end;

  Frame := PMMFrameForPFN(SelectedPFN, RegionIndex);
  if Frame = nil then
  begin
    PMMRestoreInterrupts(SavedFlags);
    Exit;
  end;
  if afZero in Flags then
  begin
    if not PMMPhysicalToHHDM(SelectedPFN shl 12, VirtualAddress) then
    begin
      { Return the selected block to its free list; split buddies are already
        independently linked and remain valid. }
      if not PMMListInsert(SelectedPFN, Order, Zone) then
      begin
        WriteText('[FATAL] PMM zero-allocation rollback failed.'#13#10);
        HaltForever;
      end;
      PMMRestoreInterrupts(SavedFlags);
      Exit;
    end;
  end;
  Frame^.RefCount := 1;
  Frame^.PinCount := 0;
  Frame^.OwnerPFN := SelectedPFN;
  Frame^.Flags := 0;
  if afKernelHeapBig in Flags then
    Frame^.Flags := Frame^.Flags or PMM_FRAME_FLAG_HEAP_BIG;
  if afKernelHeapPages in Flags then
    Frame^.Flags := Frame^.Flags or PMM_FRAME_FLAG_HEAP_PAGES;
  Frame^.Order := ShortInt(Order);
  Frame^.Zone := Ord(Zone);
  if afPageTable in Flags then
    Frame^.State := pgTable
  else if afPinned in Flags then
  begin
    Frame^.State := pgPinned;
    Frame^.PinCount := 1;
  end
  else
    Frame^.State := pgAllocated;
  Frame^.NextPFN := 0;
  Frame^.PrevPFN := 0;

  Pages := QWord(1) shl Order;
  if (Frame^.Flags and (PMM_FRAME_FLAG_HEAP_BIG or
       PMM_FRAME_FLAG_HEAP_PAGES)) <> 0 then
  begin
    if Pages > 1 then
      for BuddyPFN := 1 to Pages - 1 do
      begin
        BuddyFrame := PMMFrameForPFN(SelectedPFN + BuddyPFN,
                                     RegionIndex);
        if BuddyFrame = nil then
        begin
          WriteText('[FATAL] PMM could not tag a heap allocation tail frame.'#13#10);
          HaltForever;
        end;
        BuddyFrame^.OwnerPFN := SelectedPFN;
        BuddyFrame^.Flags := Frame^.Flags;
        BuddyFrame^.RefCount := 0;
        BuddyFrame^.PinCount := 0;
        BuddyFrame^.Order := -1;
        BuddyFrame^.State := pgTail;
      end;
  end;
  if PMMFreePageCount < Pages then
  begin
    PMMRestoreInterrupts(SavedFlags);
    Exit;
  end;
  Dec(PMMFreePageCount, Pages);
  BasePhysical := SelectedPFN shl 12;

  if afZero in Flags then
    PMMZeroPhysical(BasePhysical, Pages shl 12);

  PMMRestoreInterrupts(SavedFlags);
  Result := True;
end;

function PMMFreePages(BasePhysical: QWord; Order: Byte): Boolean;
var
  SavedFlags: QWord;
begin
  SavedFlags := PMMSaveAndDisableInterrupts;
  Result := PMMFreePagesUnlocked(BasePhysical, Order);
  PMMRestoreInterrupts(SavedFlags);
end;

function PageGet(PhysicalAddress: QWord): Boolean;
var
  SavedFlags: QWord;
  Frame: PPageFrame;
  RegionIndex: LongInt;
begin
  Result := False;
  if ((PhysicalAddress and (PAGE_SIZE - 1)) <> 0) or
     (PhysicalAddress = 0) then Exit;
  SavedFlags := PMMSaveAndDisableInterrupts;
  Frame := PMMFrameForPFN(PhysicalAddress shr 12, RegionIndex);
  if (Frame <> nil) and (Frame^.Order = 0) and
     (Frame^.RefCount <> 0) and
     (Frame^.RefCount <> High(LongWord)) and
     ((Frame^.Flags and PMM_FRAME_FLAG_PERMANENT) = 0) and
     (Frame^.State in [pgAllocated, pgTable, pgSlab, pgPinned]) then
  begin
    Inc(Frame^.RefCount);
    Result := True;
  end;
  PMMRestoreInterrupts(SavedFlags);
end;

function PagePut(PhysicalAddress: QWord): Boolean;
var
  SavedFlags: QWord;
  Frame: PPageFrame;
  RegionIndex: LongInt;
begin
  Result := False;
  if ((PhysicalAddress and (PAGE_SIZE - 1)) <> 0) or
     (PhysicalAddress = 0) then Exit;
  SavedFlags := PMMSaveAndDisableInterrupts;
  Frame := PMMFrameForPFN(PhysicalAddress shr 12, RegionIndex);
  if (Frame = nil) or (Frame^.Order <> 0) or
     (Frame^.RefCount = 0) or
     (Frame^.RefCount <= Frame^.PinCount) or
     ((Frame^.Flags and PMM_FRAME_FLAG_PERMANENT) <> 0) or
     not (Frame^.State in [pgAllocated, pgTable, pgSlab, pgPinned]) then
  begin
    PMMRestoreInterrupts(SavedFlags);
    Exit;
  end;
  if Frame^.RefCount > 1 then
  begin
    Dec(Frame^.RefCount);
    Result := True;
  end
  else
    Result := PMMFreePagesUnlocked(PhysicalAddress, 0);
  PMMRestoreInterrupts(SavedFlags);
end;

function PageReferenceCount(PhysicalAddress: QWord): LongWord;
var
  SavedFlags: QWord;
  Frame: PPageFrame;
  RegionIndex: LongInt;
begin
  Result := 0;
  if ((PhysicalAddress and (PAGE_SIZE - 1)) <> 0) then Exit;
  SavedFlags := PMMSaveAndDisableInterrupts;
  Frame := PMMFrameForPFN(PhysicalAddress shr 12, RegionIndex);
  if (Frame <> nil) and
     ((Frame^.Flags and PMM_FRAME_FLAG_PERMANENT) = 0) then
    Result := Frame^.RefCount;
  PMMRestoreInterrupts(SavedFlags);
end;

function PagePin(BasePhysical: QWord; Count: QWord): Boolean;
var
  SavedFlags: QWord;
  I: QWord;
  Frame: PPageFrame;
  RegionIndex: LongInt;
begin
  Result := False;
  if (Count = 0) or ((BasePhysical and (PAGE_SIZE - 1)) <> 0) or
     (Count > (High(QWord) - BasePhysical) div PAGE_SIZE) then Exit;
  SavedFlags := PMMSaveAndDisableInterrupts;
  for I := 0 to Count - 1 do
  begin
    Frame := PMMFrameForPFN((BasePhysical shr 12) + I, RegionIndex);
    if (Frame = nil) or (Frame^.Order <> 0) or
       (Frame^.State <> pgAllocated) or
       (Frame^.RefCount = 0) or
       (Frame^.RefCount = High(LongWord)) or
       (Frame^.PinCount = High(LongWord)) then
    begin
      PMMRestoreInterrupts(SavedFlags);
      Exit;
    end;
  end;
  for I := 0 to Count - 1 do
  begin
    Frame := PMMFrameForPFN((BasePhysical shr 12) + I, RegionIndex);
    Inc(Frame^.RefCount);
    Inc(Frame^.PinCount);
  end;
  Result := True;
  PMMRestoreInterrupts(SavedFlags);
end;

function PageUnpin(BasePhysical: QWord; Count: QWord): Boolean;
var
  SavedFlags: QWord;
  I: QWord;
  Frame: PPageFrame;
  RegionIndex: LongInt;
  Physical: QWord;
begin
  Result := False;
  if (Count = 0) or ((BasePhysical and (PAGE_SIZE - 1)) <> 0) or
     (Count > (High(QWord) - BasePhysical) div PAGE_SIZE) then Exit;
  SavedFlags := PMMSaveAndDisableInterrupts;
  for I := 0 to Count - 1 do
  begin
    Frame := PMMFrameForPFN((BasePhysical shr 12) + I, RegionIndex);
    if (Frame = nil) or (Frame^.PinCount = 0) or
       (Frame^.RefCount = 0) or
       ((Frame^.Flags and PMM_FRAME_FLAG_PERMANENT) <> 0) then
    begin
      PMMRestoreInterrupts(SavedFlags);
      Exit;
    end;
  end;
  for I := 0 to Count - 1 do
  begin
    Physical := BasePhysical + I * PAGE_SIZE;
    Frame := PMMFrameForPFN(Physical shr 12, RegionIndex);
    Dec(Frame^.PinCount);
    if Frame^.RefCount = 1 then
    begin
      if not PMMFreePagesUnlocked(Physical, 0) then
      begin
        PMMRestoreInterrupts(SavedFlags);
        Exit;
      end;
    end
    else
      Dec(Frame^.RefCount);
  end;
  Result := True;
  PMMRestoreInterrupts(SavedFlags);
end;

function PMMAllocatePage(var PhysicalAddress: QWord): Boolean;
begin
  Result := PMMAllocPages(0, [], PhysicalAddress);
end;

function PMMFreePage(PhysicalAddress: QWord): Boolean;
begin
  Result := PagePut(PhysicalAddress);
end;

function PMMGetFreePages: QWord;
begin
  Result := PMMFreePageCount;
end;

function PMMGetManagedPages: QWord;
begin
  Result := PMMManagedPageCount;
end;

function PMMGetPageInfo(PhysicalAddress: QWord; out State: TPageState;
  out RefCount: LongWord; out Order: ShortInt; out Flags: LongWord;
  out AllocationBasePhysical: QWord): Boolean;
var
  SavedFlags: QWord;
  Frame, BuddyFrame: PPageFrame;
  RegionIndex, BuddyRegionIndex: LongInt;
begin
  Result := False;
  State := pgReserved;
  RefCount := 0;
  Order := -1;
  Flags := 0;
  AllocationBasePhysical := 0;
  if (PhysicalAddress and (PAGE_SIZE - 1)) <> 0 then Exit;
  SavedFlags := PMMSaveAndDisableInterrupts;
  Frame := PMMFrameForPFN(PhysicalAddress shr 12, RegionIndex);
  if Frame <> nil then
  begin
    State := Frame^.State;
    RefCount := Frame^.RefCount;
    Flags := Frame^.Flags;
    if Frame^.OwnerPFN <> 0 then
    begin
      AllocationBasePhysical := Frame^.OwnerPFN shl 12;
      BuddyFrame := PMMFrameForPFN(Frame^.OwnerPFN, BuddyRegionIndex);
      if BuddyFrame <> nil then
        Order := BuddyFrame^.Order;
    end
    else
    begin
      AllocationBasePhysical := PhysicalAddress;
      Order := Frame^.Order;
    end;
    Result := True;
  end;
  PMMRestoreInterrupts(SavedFlags);
end;

function PMMSetPageState(PhysicalAddress: QWord;
  NewState: TPageState): Boolean;
var
  SavedFlags: QWord;
  Frame: PPageFrame;
  RegionIndex: LongInt;
begin
  Result := False;
  if (PhysicalAddress and (PAGE_SIZE - 1)) <> 0 then Exit;
  SavedFlags := PMMSaveAndDisableInterrupts;
  Frame := PMMFrameForPFN(PhysicalAddress shr 12, RegionIndex);
  if (Frame <> nil) and (Frame^.Order = 0) and
     (Frame^.RefCount = 1) and (Frame^.PinCount = 0) and
     (Frame^.State = pgAllocated) and
     (NewState in [pgAllocated, pgSlab]) then
  begin
    Frame^.State := NewState;
    Frame^.OwnerPFN := PhysicalAddress shr 12;
    Result := True;
  end;
  PMMRestoreInterrupts(SavedFlags);
end;

function PMMBuddyValidate: Boolean;
var
  ZoneIndex, Order: LongInt;
  PFN, PreviousPFN, Visited: QWord;
  Frame: PPageFrame;
  RegionIndex: LongInt;
  TotalFree: QWord;
begin
  Result := False;
  TotalFree := 0;
  for ZoneIndex := 0 to 1 do
    for Order := 0 to PMM_MAX_ORDER do
    begin
      PFN := BuddyFreeLists[ZoneIndex, Order];
      PreviousPFN := 0;
      Visited := 0;
      while PFN <> 0 do
      begin
        Inc(Visited);
        if Visited > PMMFreePageCount then Exit;
        Frame := PMMFrameForPFN(PFN, RegionIndex);
        if (Frame = nil) or (Frame^.State <> pgFree) or
           (Frame^.Order <> Order) or (Frame^.Zone <> ZoneIndex) or
           (Frame^.PrevPFN <> PreviousPFN) or
           (Frame^.RefCount <> 0) then Exit;
        Inc(TotalFree, QWord(1) shl Order);
        PreviousPFN := PFN;
        PFN := Frame^.NextPFN;
      end;
    end;
  Result := TotalFree = PMMFreePageCount;
end;

function PMMBuddySelfTest: Boolean;
var
  BeforeCount, Physical, RefPhysical: QWord;
begin
  Result := False;
  BeforeCount := PMMGetFreePages;
  if not PMMAllocPages(1, [afDMA32, afZero], Physical) then
    if not PMMAllocPages(1, [afZero], Physical) then Exit;
  if (Physical and ((PAGE_SIZE shl 1) - 1)) <> 0 then Exit;
  if not PMMFreePages(Physical, 1) then Exit;
  if PMMGetFreePages <> BeforeCount then Exit;

  if not PMMAllocPages(0, [], RefPhysical) then Exit;
  if PageReferenceCount(RefPhysical) <> 1 then Exit;
  if not PageGet(RefPhysical) then Exit;
  if PageReferenceCount(RefPhysical) <> 2 then Exit;
  if not PagePut(RefPhysical) then Exit;
  if PageReferenceCount(RefPhysical) <> 1 then Exit;
  if not PagePut(RefPhysical) then Exit;
  if PagePut(RefPhysical) then Exit; { double put must be rejected }
  if PMMGetFreePages <> BeforeCount then Exit;
  Result := PMMBuddyValidate;
end;
