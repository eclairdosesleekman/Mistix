type
  TPageTable = array[0..511] of QWord;
  PPageTable = ^TPageTable;

var
  PageBitmap: array[0..PMM_BITMAP_BYTES - 1] of Byte;
  { Tracks pages allocated by this PMM instance; reserved pages cannot be freed. }
  AllocatedBitmap: array[0..PMM_BITMAP_BYTES - 1] of Byte;
  PMMFreePageCount: QWord;
  PMMAllocationHint: QWord;
  MemoryHHDMOffset: QWord;
  VMMRootPhysical: QWord;

function PMMPageIsUsed(PageIndex: QWord): Boolean;
var
  ByteIndex: QWord;
  Mask: Byte;
begin
  ByteIndex := PageIndex shr 3;
  Mask := Byte(1 shl (PageIndex and 7));
  Result := (PageBitmap[ByteIndex] and Mask) <> 0;
end;

function PMMPageWasAllocated(PageIndex: QWord): Boolean;
var
  ByteIndex: QWord;
  Mask: Byte;
begin
  ByteIndex := PageIndex shr 3;
  Mask := Byte(1 shl (PageIndex and 7));
  Result := (AllocatedBitmap[ByteIndex] and Mask) <> 0;
end;

procedure PMMSetAllocated(PageIndex: QWord);
var
  ByteIndex: QWord;
  Mask: Byte;
begin
  ByteIndex := PageIndex shr 3;
  Mask := Byte(1 shl (PageIndex and 7));
  AllocatedBitmap[ByteIndex] := AllocatedBitmap[ByteIndex] or Mask;
end;

procedure PMMClearAllocated(PageIndex: QWord);
var
  ByteIndex: QWord;
  Mask: Byte;
begin
  ByteIndex := PageIndex shr 3;
  Mask := Byte(1 shl (PageIndex and 7));
  AllocatedBitmap[ByteIndex] := AllocatedBitmap[ByteIndex] and not Mask;
end;

procedure PMMMarkUsed(PageIndex: QWord);
var
  ByteIndex: QWord;
  Mask: Byte;
begin
  ByteIndex := PageIndex shr 3;
  Mask := Byte(1 shl (PageIndex and 7));
  if (PageBitmap[ByteIndex] and Mask) = 0 then
    Dec(PMMFreePageCount);
  PageBitmap[ByteIndex] := PageBitmap[ByteIndex] or Mask;
end;

procedure PMMMarkFree(PageIndex: QWord);
var
  ByteIndex: QWord;
  Mask: Byte;
begin
  if (PageIndex = 0) or (PageIndex >= PMM_PAGE_COUNT) then
    Exit;
  ByteIndex := PageIndex shr 3;
  Mask := Byte(1 shl (PageIndex and 7));
  if (PageBitmap[ByteIndex] and Mask) <> 0 then
  begin
    PageBitmap[ByteIndex] := PageBitmap[ByteIndex] and not Mask;
    Inc(PMMFreePageCount);
  end;
end;

function PMMInitialize(Memmap: PLimineMemmapResponse): Boolean;
var
  I: QWord;
  PageIndex: QWord;
  StartAddress: QWord;
  EndAddress: QWord;
  Entry: PLimineMemmapEntry;
  ByteIndex: QWord;
begin
  Result := False;
  if Memmap = nil then
  begin
    WriteText('[FAIL] PMM received a nil Limine memory map.'#13#10);
    Exit;
  end;
  if Memmap^.EntryCount > LIMINE_MAX_MEMMAP_ENTRIES then
  begin
    WriteText('[FAIL] PMM memory map entry count exceeds safety limit.'#13#10);
    Exit;
  end;
  if (Memmap^.EntryCount <> 0) and (Memmap^.Entries = nil) then
  begin
    WriteText('[FAIL] PMM memory map entry array is nil.'#13#10);
    Exit;
  end;

  { Start conservatively: every physical page is reserved. Only Limine
    USABLE ranges are released to the allocator. Bootloader-reclaimable,
    kernel/module, ACPI, framebuffer and reserved pages remain unavailable. }
  for ByteIndex := 0 to PMM_BITMAP_BYTES - 1 do
  begin
    PageBitmap[ByteIndex] := $FF;
    AllocatedBitmap[ByteIndex] := 0;
  end;
  PMMFreePageCount := 0;
  PMMAllocationHint := 1;

  if Memmap^.EntryCount <> 0 then
  begin
    for I := 0 to Memmap^.EntryCount - 1 do
    begin
      Entry := Memmap^.Entries^[I];
      if Entry = nil then
      begin
        WriteText('[FAIL] Null memory-map entry while initialising PMM.'#13#10);
        Exit;
      end;
      if Entry^.EntryType <> LIMINE_MEMMAP_USABLE then
        Continue;
      if Entry^.Length = 0 then
        Continue;
      if Entry^.Length > High(QWord) - Entry^.Base then
        Continue;

      StartAddress := Entry^.Base;
      if (StartAddress and (PAGE_SIZE - 1)) <> 0 then
      begin
        if StartAddress > High(QWord) - (PAGE_SIZE - 1) then
          Continue;
        StartAddress := (StartAddress + PAGE_SIZE - 1) and PAGE_MASK;
      end;
      EndAddress := (Entry^.Base + Entry^.Length) and PAGE_MASK;
      if EndAddress <= StartAddress then
        Continue;

      PageIndex := StartAddress shr 12;
      while (PageIndex < PMM_PAGE_COUNT) and ((PageIndex shl 12) < EndAddress) do
      begin
        PMMMarkFree(PageIndex);
        Inc(PageIndex);
      end;
    end;
  end;

  { Physical page zero is never handed out, even if firmware labels it usable. }
  if not PMMPageIsUsed(0) then
    PMMMarkUsed(0);

  Result := True;
  WriteText('[ OK ] PMM bitmap initialised; free 4 KiB pages: ');
  WriteDecimal(PMMFreePageCount);
  WriteText(#13#10);
end;

function PMMAllocatePage(var PhysicalAddress: QWord): Boolean;
var
  Offset: QWord;
  PageIndex: QWord;
begin
  Result := False;
  PhysicalAddress := 0;
  if PMMFreePageCount = 0 then
    Exit;

  for Offset := 0 to PMM_PAGE_COUNT - 1 do
  begin
    PageIndex := PMMAllocationHint + Offset;
    if PageIndex >= PMM_PAGE_COUNT then
      PageIndex := PageIndex - PMM_PAGE_COUNT;
    if PageIndex = 0 then
      Continue;
    if not PMMPageIsUsed(PageIndex) then
    begin
      PMMMarkUsed(PageIndex);
      PMMSetAllocated(PageIndex);
      PMMAllocationHint := PageIndex + 1;
      if PMMAllocationHint >= PMM_PAGE_COUNT then
        PMMAllocationHint := 1;
      PhysicalAddress := PageIndex shl 12;
      Result := True;
      Exit;
    end;
  end;
end;

function PMMFreePage(PhysicalAddress: QWord): Boolean;
var
  PageIndex: QWord;
begin
  Result := False;
  if (PhysicalAddress and (PAGE_SIZE - 1)) <> 0 then
    Exit;
  PageIndex := PhysicalAddress shr 12;
  if (PageIndex = 0) or (PageIndex >= PMM_PAGE_COUNT) then
    Exit;
  if not PMMPageWasAllocated(PageIndex) then
    Exit; { reject reserved pages and double frees }
  PMMClearAllocated(PageIndex);
  PMMMarkFree(PageIndex);
  if PageIndex < PMMAllocationHint then
    PMMAllocationHint := PageIndex;
  Result := True;
end;

function PMMGetFreePages: QWord;
begin
  Result := PMMFreePageCount;
end;

function IsCanonicalAddress(VirtualAddress: QWord): Boolean;
var
  TopBits: QWord;
begin
  TopBits := VirtualAddress shr 48;
  if (VirtualAddress and $0000800000000000) = 0 then
    Result := TopBits = 0
  else
    Result := TopBits = $FFFF;
end;

function PhysicalTablePointer(PhysicalAddress: QWord): PPageTable;
begin
  Result := PPageTable(PtrUInt(MemoryHHDMOffset +
                                (PhysicalAddress and PTE_ADDRESS_MASK)));
end;

procedure ZeroPhysicalPage(PhysicalAddress: QWord);
var
  Words: PPageTable;
  I: LongWord;
begin
  Words := PQWord(PtrUInt(MemoryHHDMOffset + PhysicalAddress));
  for I := 0 to 511 do
    Words^[I] := 0;
end;

function AllocatePageTable(out PhysicalAddress: QWord): Boolean;
begin
  Result := PMMAllocatePage(PhysicalAddress);
  if not Result then
    Exit;
  ZeroPhysicalPage(PhysicalAddress);
end;

function GetOrCreateChildTable(Parent: PPageTable; Index: QWord;
  UserMapping, WritableMapping: Boolean; out Child: PPageTable): Boolean;
var
  EntryValue: QWord;
  NewPhysical: QWord;
begin
  Result := False;
  Child := nil;
  EntryValue := Parent^[Index];

  if (EntryValue and PTE_PRESENT) = 0 then
  begin
    if not AllocatePageTable(NewPhysical) then
      Exit;
    EntryValue := (NewPhysical and PTE_ADDRESS_MASK) or
                  PTE_PRESENT or PTE_WRITABLE;
    if UserMapping then
      EntryValue := EntryValue or PTE_USER;
    Parent^[Index] := EntryValue;
  end
  else
  begin
    if (EntryValue and PTE_HUGE) <> 0 then
      Exit; { a 1 GiB / 2 MiB page occupies this branch }
    if WritableMapping and ((EntryValue and PTE_WRITABLE) = 0) then
      EntryValue := EntryValue or PTE_WRITABLE;
    if UserMapping and ((EntryValue and PTE_USER) = 0) then
      EntryValue := EntryValue or PTE_USER;
    Parent^[Index] := EntryValue;
  end;

  Child := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);
  Result := Child <> nil;
end;

function VMMMapPage(VirtualAddress, PhysicalAddress, Flags: QWord): Boolean;
var
  PML4: PPageTable;
  PDPT: PPageTable;
  PD: PPageTable;
  PT: PPageTable;
  EntryValue: QWord;
  UserMapping: Boolean;
  WritableMapping: Boolean;
  LeafFlags: QWord;
begin
  Result := False;
  if not IsCanonicalAddress(VirtualAddress) then
    Exit;
  if ((VirtualAddress or PhysicalAddress) and (PAGE_SIZE - 1)) <> 0 then
    Exit;
  if (PhysicalAddress and not PTE_ADDRESS_MASK) <> 0 then
    Exit;
  if ((Flags and PTE_NO_EXECUTE) <> 0) and not CPUHasNXSupport then
    Exit;

  UserMapping := (Flags and PTE_USER) <> 0;
  WritableMapping := (Flags and PTE_WRITABLE) <> 0;
  PML4 := PhysicalTablePointer(VMMRootPhysical);
  if not GetOrCreateChildTable(PML4,
      (VirtualAddress shr 39) and $1FF, UserMapping, WritableMapping, PDPT) then
    Exit;
  if not GetOrCreateChildTable(PDPT,
      (VirtualAddress shr 30) and $1FF, UserMapping, WritableMapping, PD) then
    Exit;
  if not GetOrCreateChildTable(PD,
      (VirtualAddress shr 21) and $1FF, UserMapping, WritableMapping, PT) then
    Exit;

  EntryValue := PT^[(VirtualAddress shr 12) and $1FF];
  if (EntryValue and PTE_PRESENT) <> 0 then
    Exit;

  LeafFlags := Flags and PTE_ALLOWED_FLAGS;
  PT^[(VirtualAddress shr 12) and $1FF] :=
    (PhysicalAddress and PTE_ADDRESS_MASK) or PTE_PRESENT or LeafFlags;
  InvalidatePage(VirtualAddress);
  Result := True;
end;

function VMMUnmapPage(VirtualAddress: QWord;
  var PhysicalAddress: QWord): Boolean;
var
  PML4: PPageTable;
  PDPT: PPageTable;
  PD: PPageTable;
  PT: PPageTable;
  EntryValue: QWord;
  Index: QWord;
begin
  Result := False;
  PhysicalAddress := 0;
  if not IsCanonicalAddress(VirtualAddress) then
    Exit;
  if (VirtualAddress and (PAGE_SIZE - 1)) <> 0 then
    Exit;

  PML4 := PhysicalTablePointer(VMMRootPhysical);
  EntryValue := PML4^[(VirtualAddress shr 39) and $1FF];
  if (EntryValue and PTE_PRESENT) = 0 then Exit;
  PDPT := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);

  EntryValue := PDPT^[(VirtualAddress shr 30) and $1FF];
  if ((EntryValue and PTE_PRESENT) = 0) or ((EntryValue and PTE_HUGE) <> 0) then Exit;
  PD := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);

  EntryValue := PD^[(VirtualAddress shr 21) and $1FF];
  if ((EntryValue and PTE_PRESENT) = 0) or ((EntryValue and PTE_HUGE) <> 0) then Exit;
  PT := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);

  Index := (VirtualAddress shr 12) and $1FF;
  EntryValue := PT^[Index];
  if (EntryValue and PTE_PRESENT) = 0 then Exit;
  PhysicalAddress := EntryValue and PTE_ADDRESS_MASK;
  PT^[Index] := 0;
  InvalidatePage(VirtualAddress);
  Result := True;
end;

function VMMTranslate(VirtualAddress: QWord;
  var PhysicalAddress: QWord): Boolean;
var
  PML4: PPageTable;
  PDPT: PPageTable;
  PD: PPageTable;
  PT: PPageTable;
  EntryValue: QWord;
begin
  Result := False;
  PhysicalAddress := 0;
  if not IsCanonicalAddress(VirtualAddress) then
    Exit;

  PML4 := PhysicalTablePointer(VMMRootPhysical);
  EntryValue := PML4^[(VirtualAddress shr 39) and $1FF];
  if (EntryValue and PTE_PRESENT) = 0 then Exit;
  PDPT := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);

  EntryValue := PDPT^[(VirtualAddress shr 30) and $1FF];
  if (EntryValue and PTE_PRESENT) = 0 then Exit;
  if (EntryValue and PTE_HUGE) <> 0 then
  begin
    PhysicalAddress := (EntryValue and $000FFFFFC0000000) or
                       (VirtualAddress and $3FFFFFFF);
    Result := True;
    Exit;
  end;
  PD := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);

  EntryValue := PD^[(VirtualAddress shr 21) and $1FF];
  if (EntryValue and PTE_PRESENT) = 0 then Exit;
  if (EntryValue and PTE_HUGE) <> 0 then
  begin
    PhysicalAddress := (EntryValue and $000FFFFFFFE00000) or
                       (VirtualAddress and $1FFFFF);
    Result := True;
    Exit;
  end;
  PT := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);

  EntryValue := PT^[(VirtualAddress shr 12) and $1FF];
  if (EntryValue and PTE_PRESENT) = 0 then Exit;
  PhysicalAddress := (EntryValue and PTE_ADDRESS_MASK) or
                     (VirtualAddress and (PAGE_SIZE - 1));
  Result := True;
end;

function FindFreeTestVirtualAddress(var VirtualAddress: QWord): Boolean;
var
  Attempt: LongWord;
  Candidate: QWord;
  ExistingPhysical: QWord;
begin
  Result := False;
  Candidate := VMM_TEST_VIRTUAL_ADDRESS;
  for Attempt := 0 to 4095 do
  begin
    if not VMMTranslate(Candidate, ExistingPhysical) then
    begin
      VirtualAddress := Candidate;
      Result := True;
      Exit;
    end;
    Inc(Candidate, $40000000); { skip 1 GiB in case this is a huge mapping }
  end;
end;

function VMMInitialize: Boolean;
begin
  VMMRootPhysical := ReadCR3 and PTE_ADDRESS_MASK;
  Result := VMMRootPhysical <> 0;
  if Result then
  begin
    WriteText('[ OK ] VMM attached to Limine 4-level page tables; CR3 root=');
    WriteHex64(VMMRootPhysical);
    WriteText(#13#10);
  end
  else
    WriteText('[FAIL] VMM could not obtain a valid CR3 root.'#13#10);
end;

function MemorySelfTest: Boolean;
var
  TestPhysical: QWord;
  Translated: QWord;
  UnmappedPhysical: QWord;
  TestFlags: QWord;
  TestVirtual: QWord;
  TestByte: PByte;
begin
  Result := False;
  if not FindFreeTestVirtualAddress(TestVirtual) then
  begin
    WriteText('[FAIL] VMM self-test could not find an unused canonical virtual page.'#13#10);
    Exit;
  end;
  if not PMMAllocatePage(TestPhysical) then
  begin
    WriteText('[FAIL] PMM self-test could not allocate a page.'#13#10);
    Exit;
  end;

  TestFlags := PTE_WRITABLE;
  if CPUHasNXSupport then
    TestFlags := TestFlags or PTE_NO_EXECUTE;

  if not VMMMapPage(TestVirtual, TestPhysical, TestFlags) then
  begin
    PMMFreePage(TestPhysical);
    WriteText('[FAIL] VMM self-test could not map its test page.'#13#10);
    Exit;
  end;

  TestByte := PByte(PtrUInt(TestVirtual));
  TestByte^ := $5A;
  if TestByte^ <> $5A then
  begin
    if VMMUnmapPage(TestVirtual, UnmappedPhysical) and
       (UnmappedPhysical = TestPhysical) then
      PMMFreePage(TestPhysical);
    WriteText('[FAIL] VMM mapped-page write/read test failed.'#13#10);
    Exit;
  end;

  if not VMMTranslate(TestVirtual, Translated) or
     (Translated <> TestPhysical) then
  begin
    { Never release a frame while it may still be mapped. }
    if VMMUnmapPage(TestVirtual, UnmappedPhysical) and
       (UnmappedPhysical = TestPhysical) then
      PMMFreePage(TestPhysical);
    WriteText('[FAIL] VMM translation did not match the mapped page.'#13#10);
    Exit;
  end;

  if not VMMUnmapPage(TestVirtual, UnmappedPhysical) then
  begin
    { The frame may still be reachable through the failed mapping. Keep it allocated. }
    WriteText('[FAIL] VMM self-test could not unmap its test page.'#13#10);
    Exit;
  end;
  if UnmappedPhysical <> TestPhysical then
  begin
    WriteText('[FAIL] VMM unmap returned an unexpected physical page.'#13#10);
    Exit;
  end;
  if not PMMFreePage(TestPhysical) then
  begin
    WriteText('[FAIL] PMM self-test could not return the test page.'#13#10);
    Exit;
  end;

  WriteText('[ OK ] PMM allocate/free and VMM map/translate/unmap self-test passed.'#13#10);
  Result := True;
end;

function MemoryInitialize(Memmap: PLimineMemmapResponse;
  HHDMOffset: QWord): Boolean;
begin
  Result := False;
  if HHDMOffset = 0 then
  begin
    WriteText('[FAIL] HHDM offset is zero.'#13#10);
    Exit;
  end;
  MemoryHHDMOffset := HHDMOffset;
  if not PMMInitialize(Memmap) then
    Exit;
  if not VMMInitialize then
    Exit;
  if not MemorySelfTest then
    Exit;
  Result := True;
end;
