function KHeapEnter: QWord; inline;
begin
  Result := PMMSaveAndDisableInterrupts;
end;

procedure KHeapLeave(SavedFlags: QWord); inline;
begin
  PMMRestoreInterrupts(SavedFlags);
end;

function KHeapAlignUp(Value, Alignment: QWord;
  out Aligned: QWord): Boolean;
begin
  Result := False;
  Aligned := 0;
  if (Alignment = 0) or ((Alignment and (Alignment - 1)) <> 0) then Exit;
  if Value > High(QWord) - (Alignment - 1) then Exit;
  Aligned := (Value + Alignment - 1) and not (Alignment - 1);
  Result := True;
end;

function KHeapSlabDataOffset: QWord;
begin
  Result := (SizeOf(TSlabHeader) + 15) and not QWord(15);
end;

function KHeapPhysicalToVirtual(PhysicalAddress: QWord;
  out VirtualAddress: QWord): Boolean;
begin
  Result := PMMPhysicalToHHDM(PhysicalAddress, VirtualAddress);
end;

function KHeapSlabHeader(PhysicalAddress: QWord;
  out Header: PSlabHeader): Boolean;
var
  VirtualAddress: QWord;
begin
  Result := False;
  Header := nil;
  if not KHeapPhysicalToVirtual(PhysicalAddress, VirtualAddress) then Exit;
  Header := PSlabHeader(PtrUInt(VirtualAddress));
  Result := Header <> nil;
end;

function KHeapCreateSlab(CacheIndex: LongWord;
  out PhysicalAddress: QWord): Boolean;
var
  Header: PSlabHeader;
  OldHead: PSlabHeader;
  OldVirtual: QWord;
  SlotCount: QWord;
  I: LongWord;
begin
  Result := False;
  PhysicalAddress := 0;
  if CacheIndex >= KHEAP_CACHE_COUNT then Exit;
  if not PMMAllocPages(0, [afZero], PhysicalAddress) then Exit;
  if not PMMSetPageState(PhysicalAddress, pgSlab) then
  begin
    PagePut(PhysicalAddress);
    PhysicalAddress := 0;
    Exit;
  end;
  if not KHeapSlabHeader(PhysicalAddress, Header) then
  begin
    PagePut(PhysicalAddress);
    PhysicalAddress := 0;
    Exit;
  end;

  SlotCount := (PAGE_SIZE - KHeapSlabDataOffset) div
               KHeapCacheSizes[CacheIndex];
  if (SlotCount = 0) or (SlotCount > KHEAP_MAX_SLAB_SLOTS) then
  begin
    PagePut(PhysicalAddress);
    PhysicalAddress := 0;
    Exit;
  end;

  Header^.Magic := KHEAP_SLAB_MAGIC;
  Header^.NextPhysical := KHeapSlabHeads[CacheIndex];
  Header^.PrevPhysical := 0;
  Header^.CacheIndex := CacheIndex;
  Header^.SlotSize := KHeapCacheSizes[CacheIndex];
  Header^.SlotCount := Word(SlotCount);
  Header^.FreeCount := Word(SlotCount);
  Header^.Reserved := 0;
  for I := 0 to KHEAP_SLAB_BITMAP_BYTES - 1 do
    Header^.Bitmap[I] := 0;

  if Header^.NextPhysical <> 0 then
  begin
    if not KHeapSlabHeader(Header^.NextPhysical, OldHead) then
    begin
      Header^.Magic := 0;
      PagePut(PhysicalAddress);
      PhysicalAddress := 0;
      Exit;
    end;
    OldHead^.PrevPhysical := PhysicalAddress;
  end;
  KHeapSlabHeads[CacheIndex] := PhysicalAddress;
  Result := True;
end;

function KHeapSlabAllocate(CacheIndex: LongWord): Pointer;
var
  PhysicalAddress: QWord;
  Header: PSlabHeader;
  I, ByteIndex: LongWord;
  BitMask: Byte;
  VirtualAddress, SlotAddress: QWord;
begin
  Result := nil;
  if CacheIndex >= KHEAP_CACHE_COUNT then Exit;

  PhysicalAddress := KHeapSlabHeads[CacheIndex];
  while PhysicalAddress <> 0 do
  begin
    if not KHeapSlabHeader(PhysicalAddress, Header) then Exit;
    if (Header^.Magic <> KHEAP_SLAB_MAGIC) or
       (Header^.CacheIndex <> CacheIndex) or
       (Header^.FreeCount > Header^.SlotCount) then Exit;
    if Header^.FreeCount <> 0 then
    begin
      for I := 0 to Header^.SlotCount - 1 do
      begin
        ByteIndex := I shr 3;
        BitMask := Byte(1 shl (I and 7));
        if (Header^.Bitmap[ByteIndex] and BitMask) <> 0 then Continue;
        Header^.Bitmap[ByteIndex] :=
          Header^.Bitmap[ByteIndex] or BitMask;
        Dec(Header^.FreeCount);
        if not KHeapPhysicalToVirtual(PhysicalAddress, VirtualAddress) then
          Exit;
        SlotAddress := VirtualAddress + KHeapSlabDataOffset +
                       QWord(I) * Header^.SlotSize;
        Result := Pointer(PtrUInt(SlotAddress));
        Exit;
      end;
      Exit;
    end;
    PhysicalAddress := Header^.NextPhysical;
  end;

  if not KHeapCreateSlab(CacheIndex, PhysicalAddress) then Exit;
  if not KHeapSlabHeader(PhysicalAddress, Header) then Exit;
  if Header^.SlotCount = 0 then Exit;
  Header^.Bitmap[0] := Header^.Bitmap[0] or 1;
  Dec(Header^.FreeCount);
  if not KHeapPhysicalToVirtual(PhysicalAddress, VirtualAddress) then Exit;
  SlotAddress := VirtualAddress + KHeapSlabDataOffset;
  Result := Pointer(PtrUInt(SlotAddress));
end;

function KHeapSlabUnlink(PhysicalAddress: QWord;
  Header: PSlabHeader): Boolean;
var
  PreviousHeader, NextHeader: PSlabHeader;
begin
  Result := False;
  if (Header = nil) or (Header^.CacheIndex >= KHEAP_CACHE_COUNT) then Exit;
  if Header^.PrevPhysical = 0 then
  begin
    if KHeapSlabHeads[Header^.CacheIndex] <> PhysicalAddress then Exit;
    KHeapSlabHeads[Header^.CacheIndex] := Header^.NextPhysical;
  end
  else
  begin
    if not KHeapSlabHeader(Header^.PrevPhysical, PreviousHeader) then Exit;
    PreviousHeader^.NextPhysical := Header^.NextPhysical;
  end;
  if Header^.NextPhysical <> 0 then
  begin
    if not KHeapSlabHeader(Header^.NextPhysical, NextHeader) then Exit;
    NextHeader^.PrevPhysical := Header^.PrevPhysical;
  end;
  Header^.NextPhysical := 0;
  Header^.PrevPhysical := 0;
  Result := True;
end;

function KHeapSlabFree(P: Pointer; PhysicalAddress: QWord): Boolean;
var
  Header: PSlabHeader;
  VirtualAddress, DataStart, Offset, SlotIndex: QWord;
  ByteIndex: QWord;
  BitMask: Byte;
begin
  Result := False;
  if not KHeapSlabHeader(PhysicalAddress, Header) then Exit;
  if (Header^.Magic <> KHEAP_SLAB_MAGIC) or
     (Header^.CacheIndex >= KHEAP_CACHE_COUNT) or
     (Header^.SlotSize <> KHeapCacheSizes[Header^.CacheIndex]) or
     (Header^.SlotCount = 0) or
     (Header^.SlotCount > KHEAP_MAX_SLAB_SLOTS) or
     (Header^.FreeCount >= Header^.SlotCount) then Exit;
  if not KHeapPhysicalToVirtual(PhysicalAddress, VirtualAddress) then Exit;

  DataStart := VirtualAddress + KHeapSlabDataOffset;
  if PtrUInt(P) < DataStart then Exit;
  Offset := PtrUInt(P) - DataStart;
  if (Offset mod Header^.SlotSize) <> 0 then Exit;
  SlotIndex := Offset div Header^.SlotSize;
  if SlotIndex >= Header^.SlotCount then Exit;

  ByteIndex := SlotIndex shr 3;
  BitMask := Byte(1 shl (SlotIndex and 7));
  if (Header^.Bitmap[ByteIndex] and BitMask) = 0 then Exit;
  Header^.Bitmap[ByteIndex] := Header^.Bitmap[ByteIndex] and not BitMask;
  Inc(Header^.FreeCount);
  Result := True;

  if Header^.FreeCount = Header^.SlotCount then
  begin
    if not KHeapSlabUnlink(PhysicalAddress, Header) then
    begin
      WriteText('[FATAL] Kernel heap could not unlink an empty slab.'#13#10);
      HaltForever;
    end;
    Header^.Magic := 0;
    if not PagePut(PhysicalAddress) then
    begin
      WriteText('[FATAL] Kernel heap could not release an empty slab.'#13#10);
      HaltForever;
    end;
  end;
end;

function KHeapBigAllocate(Size, Alignment: QWord): Pointer;
var
  HeaderBytes, RequiredBytes, RequiredPages, BlockPages: QWord;
  BasePhysical, BaseVirtual, UserStart, UserAddress: QWord;
  Order: LongInt;
  Header: PBigAllocationHeader;
begin
  Result := nil;
  if Size = 0 then Exit;
  HeaderBytes := SizeOf(TBigAllocationHeader);
  if Alignment - 1 > High(QWord) - HeaderBytes then Exit;
  RequiredBytes := HeaderBytes + Alignment - 1;
  if Size > High(QWord) - RequiredBytes then Exit;
  RequiredBytes := RequiredBytes + Size;
  RequiredPages := RequiredBytes div PAGE_SIZE;
  if (RequiredBytes mod PAGE_SIZE) <> 0 then Inc(RequiredPages);
  if (RequiredPages = 0) or
     (RequiredPages > (QWord(1) shl PMM_MAX_ORDER)) then Exit;

  Order := 0;
  BlockPages := 1;
  while BlockPages < RequiredPages do
  begin
    if Order >= PMM_MAX_ORDER then Exit;
    BlockPages := BlockPages shl 1;
    Inc(Order);
  end;
  if not PMMAllocPages(Byte(Order), [afZero, afKernelHeapBig],
                       BasePhysical) then Exit;
  if not KHeapPhysicalToVirtual(BasePhysical, BaseVirtual) then
  begin
    if not PMMFreePages(BasePhysical, Byte(Order)) then
    begin
      WriteText('[FATAL] Kernel heap big-allocation rollback failed.'#13#10);
      HaltForever;
    end;
    Exit;
  end;

  Header := PBigAllocationHeader(PtrUInt(BaseVirtual));
  UserStart := BaseVirtual + SizeOf(TBigAllocationHeader);
  if not KHeapAlignUp(UserStart, Alignment, UserAddress) then
  begin
    PMMFreePages(BasePhysical, Byte(Order));
    Exit;
  end;
  Header^.Magic := KHEAP_BIG_MAGIC;
  Header^.BasePhysical := BasePhysical;
  Header^.RequestedSize := Size;
  Header^.UserPointer := UserAddress;
  Header^.Order := Byte(Order);
  Result := Pointer(PtrUInt(UserAddress));
end;

procedure KernelHeapInitialize;
var
  I: LongWord;
begin
  KHeapInitialized := False;
  for I := 0 to KHEAP_CACHE_COUNT - 1 do
    KHeapSlabHeads[I] := 0;
  KHeapInitialized := True;
  WriteText('[ OK ] Kernel heap initialised (8 slab size classes + buddy-backed large allocations).'#13#10);
end;

function KAlloc(Size: QWord; Alignment: QWord): Pointer;
var
  SavedFlags: QWord;
  CacheIndex: LongWord;
begin
  Result := nil;
  if not KHeapInitialized or (Size = 0) or
     (Alignment = 0) or
     ((Alignment and (Alignment - 1)) <> 0) then Exit;
  SavedFlags := KHeapEnter;
  if (Size <= KHeapCacheSizes[KHEAP_CACHE_COUNT - 1]) and
     (Alignment <= 16) then
  begin
    CacheIndex := 0;
    while (CacheIndex < KHEAP_CACHE_COUNT - 1) and
          (KHeapCacheSizes[CacheIndex] < Size) do
      Inc(CacheIndex);
    Result := KHeapSlabAllocate(CacheIndex);
  end
  else
    Result := KHeapBigAllocate(Size, Alignment);
  KHeapLeave(SavedFlags);
end;

function KAllocZero(Size: QWord): Pointer;
var
  I: QWord;
  Bytes: PByte;
begin
  Result := KAlloc(Size, 16);
  if Result = nil then Exit;
  Bytes := PByte(Result);
  for I := 0 to Size - 1 do Bytes[I] := 0;
end;

function KAllocPages(Count: QWord): Pointer;
var
  SavedFlags: QWord;
  Order: LongInt;
  Pages: QWord;
  PhysicalAddress, VirtualAddress: QWord;
begin
  Result := nil;
  if not KHeapInitialized or (Count = 0) then Exit;
  { The paired KFreePages API takes the exact buddy block size. }
  if (Count and (Count - 1)) <> 0 then Exit;
  Order := 0;
  Pages := 1;
  while Pages < Count do
  begin
    Pages := Pages shl 1;
    Inc(Order);
  end;
  PhysicalAddress := 0;
  if Order > PMM_MAX_ORDER then Exit;
  SavedFlags := KHeapEnter;
  if PMMAllocPages(Byte(Order), [afKernelHeapPages], PhysicalAddress) and
     KHeapPhysicalToVirtual(PhysicalAddress, VirtualAddress) then
    Result := Pointer(PtrUInt(VirtualAddress))
  else if PhysicalAddress <> 0 then
    PMMFreePages(PhysicalAddress, Byte(Order));
  KHeapLeave(SavedFlags);
end;

function KFreePages(P: Pointer; Count: QWord): Boolean;
var
  SavedFlags: QWord;
  PhysicalAddress, AllocationBasePhysical: QWord;
  Order: LongInt;
  Pages: QWord;
  State: TPageState;
  RefCount, Flags: LongWord;
  ActualOrder: ShortInt;
begin
  Result := False;
  if (P = nil) or (Count = 0) or
     ((Count and (Count - 1)) <> 0) then Exit;
  if PtrUInt(P) < MemoryHHDMOffset then Exit;
  PhysicalAddress := QWord(PtrUInt(P)) - MemoryHHDMOffset;
  if (PhysicalAddress and (PAGE_SIZE - 1)) <> 0 then Exit;
  Order := 0;
  Pages := 1;
  while Pages < Count do
  begin
    Pages := Pages shl 1;
    Inc(Order);
  end;
  if Order > PMM_MAX_ORDER then Exit;
  SavedFlags := KHeapEnter;
  if not PMMGetPageInfo(PhysicalAddress, State, RefCount, ActualOrder,
                        Flags, AllocationBasePhysical) then
  begin
    KHeapLeave(SavedFlags);
    Exit;
  end;
  if (State <> pgAllocated) or
     ((Flags and PMM_FRAME_FLAG_HEAP_PAGES) = 0) or
     (AllocationBasePhysical <> PhysicalAddress) or
     (ActualOrder <> Order) then
  begin
    KHeapLeave(SavedFlags);
    Exit;
  end;
  Result := PMMFreePages(PhysicalAddress, Byte(Order));
  KHeapLeave(SavedFlags);
end;

function KFree(P: Pointer): Boolean;
var
  SavedFlags: QWord;
  UserAddress, PhysicalAddress, PagePhysical: QWord;
  BaseVirtual, BlockBytes: QWord;
  State: TPageState;
  RefCount, Flags: LongWord;
  Order: ShortInt;
  AllocationBasePhysical: QWord;
  Header: PSlabHeader;
  BigHeader: PBigAllocationHeader;
begin
  Result := False;
  if P = nil then Exit;
  UserAddress := PtrUInt(P);
  if UserAddress < MemoryHHDMOffset then Exit;
  PhysicalAddress := UserAddress - MemoryHHDMOffset;
  PagePhysical := PhysicalAddress and PAGE_MASK;

  SavedFlags := KHeapEnter;
  if not PMMGetPageInfo(PagePhysical, State, RefCount, Order,
                        Flags, AllocationBasePhysical) then
  begin
    KHeapLeave(SavedFlags);
    Exit;
  end;
  if State = pgSlab then
    Result := KHeapSlabFree(P, PagePhysical)
  else
  begin
    { KFree only accepts allocations made by the small/large KAlloc path.
      KAllocPages has a distinct tag and must be paired with KFreePages. }
    if ((Flags and PMM_FRAME_FLAG_HEAP_BIG) = 0) or
       (AllocationBasePhysical = 0) or (Order < 0) then
    begin
      KHeapLeave(SavedFlags);
      Exit;
    end;
    if not KHeapPhysicalToVirtual(AllocationBasePhysical, BaseVirtual) then
    begin
      KHeapLeave(SavedFlags);
      Exit;
    end;
    BlockBytes := QWord(PAGE_SIZE) shl Order;
    if (UserAddress < BaseVirtual) or
       ((UserAddress - BaseVirtual) >= BlockBytes) then
    begin
      KHeapLeave(SavedFlags);
      Exit;
    end;
    BigHeader := PBigAllocationHeader(PtrUInt(BaseVirtual));
    if (BigHeader^.Magic <> KHEAP_BIG_MAGIC) or
       (BigHeader^.BasePhysical <> AllocationBasePhysical) or
       (BigHeader^.UserPointer <> UserAddress) or
       (BigHeader^.Order <> Byte(Order)) then
    begin
      KHeapLeave(SavedFlags);
      Exit;
    end;
    BigHeader^.Magic := 0;
    Result := PMMFreePages(AllocationBasePhysical, BigHeader^.Order);
  end;
  KHeapLeave(SavedFlags);
end;

function KernelHeapSelfTest: Boolean;
var
  BeforeCount: QWord;
  A, B, C, Large: Pointer;
  Bytes: PByte;
begin
  Result := False;
  BeforeCount := PMMGetFreePages;
  A := KAlloc(24, 16);
  B := KAlloc(32, 16);
  C := KAllocZero(2048);
  Large := KAlloc(6000, 4096);
  if (A = nil) or (B = nil) or (C = nil) or (Large = nil) then
  begin
    if A <> nil then KFree(A);
    if B <> nil then KFree(B);
    if C <> nil then KFree(C);
    if Large <> nil then KFree(Large);
    Exit;
  end;

  Bytes := PByte(C);
  if (Bytes[0] <> 0) or (Bytes[2047] <> 0) then
  begin
    KFree(A); KFree(B); KFree(C); KFree(Large);
    Exit;
  end;
  if not KFree(A) then Exit;
  if KFree(A) then Exit; { released slab slots must reject a double free }
  if not KFree(B) or not KFree(C) or not KFree(Large) then Exit;
  if PMMGetFreePages <> BeforeCount then Exit;
  Result := PMMBuddyValidate;
end;
