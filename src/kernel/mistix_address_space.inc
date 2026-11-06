function COWReleasePage(PhysicalAddress: QWord): Boolean; forward;
function COWEnsureWritable(RootPhysical, VirtualAddress: QWord): Boolean; forward;

{ Mistix Phase 3: per-process x86-64 address spaces and ELF64 loading.
  The upper PML4 half is shared with the kernel; the lower half is private. }

const
  USER_ADDRESS_LIMIT = $0000800000000000;
  USER_IMAGE_MIN = $0000000000400000;
  USER_STACK_TOP = $00007FFFFFF000;
  USER_STACK_PAGE_COUNT = 16;
  USER_STACK_SIZE = USER_STACK_PAGE_COUNT * PAGE_SIZE;
  USER_STACK_BOTTOM = USER_STACK_TOP - USER_STACK_SIZE;
  USER_STACK_GUARD = USER_STACK_BOTTOM - PAGE_SIZE;
  ELF_MAX_PROGRAM_HEADERS = 32;
  ELF_MAX_IMAGE_PAGES = 256;

  ELF_MAGIC_0 = $7F;
  ELF_MAGIC_1 = $45;
  ELF_MAGIC_2 = $4C;
  ELF_MAGIC_3 = $46;
  ELF_CLASS_64 = 2;
  ELF_DATA_LITTLE_ENDIAN = 1;
  ELF_VERSION_CURRENT = 1;
  ELF_TYPE_EXECUTABLE = 2;
  ELF_MACHINE_X86_64 = 62;
  ELF_PT_LOAD = 1;
  ELF_PT_DYNAMIC = 2;
  ELF_PT_INTERP = 3;
  ELF_PT_TLS = 7;
  ELF_PF_EXECUTE = 1;
  ELF_PF_WRITE = 2;
  ELF_PF_READ = 4;

type
  TElf64Header = packed record
    Ident: array[0..15] of Byte;
    FileType: Word;
    Machine: Word;
    Version: LongWord;
    EntryPoint: QWord;
    ProgramHeaderOffset: QWord;
    SectionHeaderOffset: QWord;
    Flags: LongWord;
    HeaderSize: Word;
    ProgramHeaderEntrySize: Word;
    ProgramHeaderCount: Word;
    SectionHeaderEntrySize: Word;
    SectionHeaderCount: Word;
    SectionNameIndex: Word;
  end;
  PElf64Header = ^TElf64Header;

  TElf64ProgramHeader = packed record
    SegmentType: LongWord;
    Flags: LongWord;
    FileOffset: QWord;
    VirtualAddress: QWord;
    PhysicalAddress: QWord;
    FileSize: QWord;
    MemorySize: QWord;
    Alignment: QWord;
  end;
  PElf64ProgramHeader = ^TElf64ProgramHeader;

procedure WriteCR3(Value: QWord); assembler; nostackframe;
asm
  mov cr3, rdi
end;

function AddressSpaceCreate(out RootPhysical: QWord): Boolean;
var
  NewRoot: QWord;
  NewTable: PPageTable;
  KernelTable: PPageTable;
  I: LongWord;
begin
  RootPhysical := 0;
  if VMMRootPhysical = 0 then
  begin
    Result := False;
    Exit;
  end;
  if not PMMAllocatePage(NewRoot) then
  begin
    Result := False;
    Exit;
  end;
  ZeroPhysicalPage(NewRoot);
  NewTable := PhysicalTablePointer(NewRoot);
  KernelTable := PhysicalTablePointer(VMMRootPhysical);

  { PML4[256..511] are the supervisor-only kernel/HHDM half. }
  for I := 256 to 511 do
    NewTable^[I] := KernelTable^[I];

  RootPhysical := NewRoot;
  Result := True;
end;

function AddressSpaceEnsureChild(Parent: PPageTable; Index: LongWord;
  out Child: PPageTable): Boolean;
var
  EntryValue: QWord;
  ChildPhysical: QWord;
begin
  Child := nil;
  EntryValue := Parent^[Index];
  if (EntryValue and PTE_PRESENT) = 0 then
  begin
    if not AllocatePageTable(ChildPhysical) then
    begin
      Result := False;
      Exit;
    end;
    EntryValue := (ChildPhysical and PTE_ADDRESS_MASK) or
                  PTE_PRESENT or PTE_WRITABLE or PTE_USER;
    Parent^[Index] := EntryValue;
  end
  else
  begin
    if (EntryValue and PTE_HUGE) <> 0 then
    begin
      Result := False;
      Exit;
    end;
    EntryValue := EntryValue or PTE_USER or PTE_WRITABLE;
    Parent^[Index] := EntryValue;
  end;
  Child := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);
  Result := Child <> nil;
end;

function AddressSpaceMapPage(RootPhysical, VirtualAddress,
  PhysicalAddress, Flags: QWord): Boolean;
var
  PML4: PPageTable;
  PDPT: PPageTable;
  PD: PPageTable;
  PT: PPageTable;
  EntryValue: QWord;
  LeafFlags: QWord;
begin
  Result := False;
  if (RootPhysical = 0) or (VirtualAddress >= USER_ADDRESS_LIMIT) then
    Exit;
  if not IsCanonicalAddress(VirtualAddress) then
    Exit;
  if ((VirtualAddress or PhysicalAddress) and (PAGE_SIZE - 1)) <> 0 then
    Exit;
  if (PhysicalAddress and not PTE_ADDRESS_MASK) <> 0 then
    Exit;
  if (Flags and not PTE_ALLOWED_FLAGS) <> 0 then
    Exit;
  if (Flags and PTE_USER) = 0 then
    Exit;
  if ((Flags and PTE_NO_EXECUTE) <> 0) and not CPUHasNXSupport then
    Exit;

  PML4 := PhysicalTablePointer(RootPhysical);
  if not AddressSpaceEnsureChild(PML4,
      LongWord((VirtualAddress shr 39) and $1FF), PDPT) then Exit;
  if not AddressSpaceEnsureChild(PDPT,
      LongWord((VirtualAddress shr 30) and $1FF), PD) then Exit;
  if not AddressSpaceEnsureChild(PD,
      LongWord((VirtualAddress shr 21) and $1FF), PT) then Exit;

  EntryValue := PT^[(VirtualAddress shr 12) and $1FF];
  if (EntryValue and PTE_PRESENT) <> 0 then
    Exit;
  LeafFlags := Flags and PTE_ALLOWED_FLAGS;
  PT^[(VirtualAddress shr 12) and $1FF] :=
    (PhysicalAddress and PTE_ADDRESS_MASK) or PTE_PRESENT or LeafFlags;
  Result := True;
end;

function AddressSpaceTranslate(RootPhysical, VirtualAddress: QWord;
  var PhysicalAddress, EffectiveFlags: QWord): Boolean;
var
  PML4: PPageTable;
  PDPT: PPageTable;
  PD: PPageTable;
  PT: PPageTable;
  EntryValue: QWord;
  AccumulatedFlags: QWord;
  NoExecute: Boolean;
begin
  Result := False;
  PhysicalAddress := 0;
  EffectiveFlags := 0;
  if (RootPhysical = 0) or not IsCanonicalAddress(VirtualAddress) then
    Exit;

  PML4 := PhysicalTablePointer(RootPhysical);
  EntryValue := PML4^[(VirtualAddress shr 39) and $1FF];
  if (EntryValue and PTE_PRESENT) = 0 then Exit;
  AccumulatedFlags := PTE_PRESENT or PTE_USER or PTE_WRITABLE;
  NoExecute := False;
  if (EntryValue and PTE_USER) = 0 then
    AccumulatedFlags := AccumulatedFlags and not PTE_USER;
  if (EntryValue and PTE_WRITABLE) = 0 then
    AccumulatedFlags := AccumulatedFlags and not PTE_WRITABLE;
  if (EntryValue and PTE_NO_EXECUTE) <> 0 then NoExecute := True;
  PDPT := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);

  EntryValue := PDPT^[(VirtualAddress shr 30) and $1FF];
  if (EntryValue and PTE_PRESENT) = 0 then Exit;
  if (EntryValue and PTE_USER) = 0 then
    AccumulatedFlags := AccumulatedFlags and not PTE_USER;
  if (EntryValue and PTE_WRITABLE) = 0 then
    AccumulatedFlags := AccumulatedFlags and not PTE_WRITABLE;
  if (EntryValue and PTE_NO_EXECUTE) <> 0 then NoExecute := True;
  if (EntryValue and PTE_HUGE) <> 0 then
  begin
    PhysicalAddress := (EntryValue and $000FFFFFC0000000) or
                       (VirtualAddress and $3FFFFFFF);
    EffectiveFlags := AccumulatedFlags;
    if NoExecute then EffectiveFlags := EffectiveFlags or PTE_NO_EXECUTE;
    Result := True;
    Exit;
  end;
  PD := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);

  EntryValue := PD^[(VirtualAddress shr 21) and $1FF];
  if (EntryValue and PTE_PRESENT) = 0 then Exit;
  if (EntryValue and PTE_USER) = 0 then
    AccumulatedFlags := AccumulatedFlags and not PTE_USER;
  if (EntryValue and PTE_WRITABLE) = 0 then
    AccumulatedFlags := AccumulatedFlags and not PTE_WRITABLE;
  if (EntryValue and PTE_NO_EXECUTE) <> 0 then NoExecute := True;
  if (EntryValue and PTE_HUGE) <> 0 then
  begin
    PhysicalAddress := (EntryValue and $000FFFFFFFE00000) or
                       (VirtualAddress and $1FFFFF);
    EffectiveFlags := AccumulatedFlags;
    if NoExecute then EffectiveFlags := EffectiveFlags or PTE_NO_EXECUTE;
    Result := True;
    Exit;
  end;
  PT := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);

  EntryValue := PT^[(VirtualAddress shr 12) and $1FF];
  if (EntryValue and PTE_PRESENT) = 0 then Exit;
  if (EntryValue and PTE_USER) = 0 then
    AccumulatedFlags := AccumulatedFlags and not PTE_USER;
  if (EntryValue and PTE_WRITABLE) = 0 then
    AccumulatedFlags := AccumulatedFlags and not PTE_WRITABLE;
  if (EntryValue and PTE_NO_EXECUTE) <> 0 then NoExecute := True;
  PhysicalAddress := (EntryValue and PTE_ADDRESS_MASK) or
                     (VirtualAddress and (PAGE_SIZE - 1));
  EffectiveFlags := AccumulatedFlags;
  if NoExecute then EffectiveFlags := EffectiveFlags or PTE_NO_EXECUTE;
  Result := True;
end;

function AddressSpaceValidateUserRange(RootPhysical, StartAddress,
  ByteCount: QWord): Boolean;
var
  CurrentAddress: QWord;
  Remaining: QWord;
  Chunk: QWord;
  PhysicalAddress: QWord;
  Flags: QWord;
begin
  Result := False;
  if ByteCount = 0 then
  begin
    Result := True;
    Exit;
  end;
  if (StartAddress >= USER_ADDRESS_LIMIT) or
     (ByteCount > USER_ADDRESS_LIMIT - StartAddress) then
    Exit;

  CurrentAddress := StartAddress;
  Remaining := ByteCount;
  while Remaining <> 0 do
  begin
    if not AddressSpaceTranslate(RootPhysical, CurrentAddress,
                                 PhysicalAddress, Flags) then
      Exit;
    if ((Flags and (PTE_PRESENT or PTE_USER)) <>
        (PTE_PRESENT or PTE_USER)) then
      Exit;
    Chunk := PAGE_SIZE - (CurrentAddress and (PAGE_SIZE - 1));
    if Chunk > Remaining then
      Chunk := Remaining;
    Inc(CurrentAddress, Chunk);
    Dec(Remaining, Chunk);
  end;
  Result := True;
end;

function AddressSpaceUnmapPage(RootPhysical, VirtualAddress: QWord;
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
  if (RootPhysical = 0) or (VirtualAddress >= USER_ADDRESS_LIMIT) or
     ((VirtualAddress and (PAGE_SIZE - 1)) <> 0) then Exit;

  PML4 := PhysicalTablePointer(RootPhysical);
  EntryValue := PML4^[(VirtualAddress shr 39) and $1FF];
  if (EntryValue and PTE_PRESENT) = 0 then Exit;
  PDPT := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);
  EntryValue := PDPT^[(VirtualAddress shr 30) and $1FF];
  if ((EntryValue and PTE_PRESENT) = 0) or
     ((EntryValue and PTE_HUGE) <> 0) then Exit;
  PD := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);
  EntryValue := PD^[(VirtualAddress shr 21) and $1FF];
  if ((EntryValue and PTE_PRESENT) = 0) or
     ((EntryValue and PTE_HUGE) <> 0) then Exit;
  PT := PhysicalTablePointer(EntryValue and PTE_ADDRESS_MASK);
  EntryValue := PT^[(VirtualAddress shr 12) and $1FF];
  if (EntryValue and PTE_PRESENT) = 0 then Exit;
  PhysicalAddress := EntryValue and PTE_ADDRESS_MASK;
  PT^[(VirtualAddress shr 12) and $1FF] := 0;
  Result := True;
end;

function AddressSpaceFreePT(PTPhysical: QWord): Boolean;
var
  PT: PPageTable;
  I: LongWord;
  EntryValue: QWord;
begin
  Result := False;
  PT := PhysicalTablePointer(PTPhysical);
  for I := 0 to 511 do
  begin
    EntryValue := PT^[I];
    if (EntryValue and PTE_PRESENT) = 0 then Continue;
    if not COWReleasePage(EntryValue and PTE_ADDRESS_MASK) then Exit;
    PT^[I] := 0;
  end;
  Result := PMMFreePage(PTPhysical);
end;

function AddressSpaceFreePD(PDPhysical: QWord): Boolean;
var
  PD: PPageTable;
  I: LongWord;
  EntryValue: QWord;
begin
  Result := False;
  PD := PhysicalTablePointer(PDPhysical);
  for I := 0 to 511 do
  begin
    EntryValue := PD^[I];
    if (EntryValue and PTE_PRESENT) = 0 then Continue;
    if (EntryValue and PTE_HUGE) <> 0 then Exit;
    if not AddressSpaceFreePT(EntryValue and PTE_ADDRESS_MASK) then Exit;
    PD^[I] := 0;
  end;
  Result := PMMFreePage(PDPhysical);
end;

function AddressSpaceFreePDPT(PDPTPhysical: QWord): Boolean;
var
  PDPT: PPageTable;
  I: LongWord;
  EntryValue: QWord;
begin
  Result := False;
  PDPT := PhysicalTablePointer(PDPTPhysical);
  for I := 0 to 511 do
  begin
    EntryValue := PDPT^[I];
    if (EntryValue and PTE_PRESENT) = 0 then Continue;
    if (EntryValue and PTE_HUGE) <> 0 then Exit;
    if not AddressSpaceFreePD(EntryValue and PTE_ADDRESS_MASK) then Exit;
    PDPT^[I] := 0;
  end;
  Result := PMMFreePage(PDPTPhysical);
end;

function AddressSpaceDestroy(RootPhysical: QWord): Boolean;
var
  Root: PPageTable;
  I: LongWord;
  EntryValue: QWord;
begin
  Result := False;
  if (RootPhysical = 0) or (RootPhysical = VMMRootPhysical) or
     ((ReadCR3 and PTE_ADDRESS_MASK) = RootPhysical) then Exit;
  Root := PhysicalTablePointer(RootPhysical);
  for I := 0 to 255 do
  begin
    EntryValue := Root^[I];
    if (EntryValue and PTE_PRESENT) = 0 then Continue;
    if (EntryValue and PTE_HUGE) <> 0 then Exit;
    if not AddressSpaceFreePDPT(EntryValue and PTE_ADDRESS_MASK) then Exit;
    Root^[I] := 0;
  end;
  { The upper half is shared with the kernel and must never be freed here. }
  Result := PMMFreePage(RootPhysical);
end;

function AddressSpaceMapZeroPage(RootPhysical, VirtualAddress,
  Flags: QWord): Boolean;
var
  PhysicalAddress: QWord;
begin
  Result := False;
  if not PMMAllocatePage(PhysicalAddress) then Exit;
  ZeroPhysicalPage(PhysicalAddress);
  if not AddressSpaceMapPage(RootPhysical, VirtualAddress,
                             PhysicalAddress, Flags) then
  begin
    PMMFreePage(PhysicalAddress);
    Exit;
  end;
  Result := True;
end;

function AddressSpaceCopyTo(RootPhysical, DestinationAddress: QWord;
  Source: Pointer; ByteCount: QWord): Boolean;
var
  SourceBytes: PByte;
  DestinationBytes: PByte;
  CurrentAddress: QWord;
  Remaining: QWord;
  PhysicalAddress: QWord;
  Flags: QWord;
  Chunk: QWord;
  I: QWord;
begin
  Result := False;
  if (ByteCount <> 0) and (Source = nil) then Exit;
  if not AddressSpaceValidateUserRange(RootPhysical, DestinationAddress,
                                       ByteCount) then Exit;
  SourceBytes := PByte(Source);
  CurrentAddress := DestinationAddress;
  Remaining := ByteCount;
  while Remaining <> 0 do
  begin
    if not AddressSpaceTranslate(RootPhysical, CurrentAddress,
                                 PhysicalAddress, Flags) then Exit;
    if (Flags and PTE_USER) = 0 then Exit;
    if (Flags and PTE_WRITABLE) = 0 then
    begin
      if not COWEnsureWritable(RootPhysical, CurrentAddress) then Exit;
      if not AddressSpaceTranslate(RootPhysical, CurrentAddress,
                                   PhysicalAddress, Flags) then Exit;
      if (Flags and PTE_WRITABLE) = 0 then Exit;
    end;
    Chunk := PAGE_SIZE - (CurrentAddress and (PAGE_SIZE - 1));
    if Chunk > Remaining then Chunk := Remaining;
    DestinationBytes := PByte(PtrUInt(MemoryHHDMOffset + PhysicalAddress));
    for I := 0 to Chunk - 1 do
      DestinationBytes[I] := SourceBytes[I];
    Inc(SourceBytes, Chunk);
    Inc(CurrentAddress, Chunk);
    Dec(Remaining, Chunk);
  end;
  Result := True;
end;

function AddressSpaceCopyFrom(RootPhysical, SourceAddress: QWord;
  Destination: Pointer; ByteCount: QWord): Boolean;
var
  SourceBytes: PByte;
  DestinationBytes: PByte;
  CurrentAddress: QWord;
  Remaining: QWord;
  PhysicalAddress: QWord;
  Flags: QWord;
  Chunk: QWord;
  I: QWord;
begin
  Result := False;
  if (ByteCount <> 0) and (Destination = nil) then Exit;
  if not AddressSpaceValidateUserRange(RootPhysical, SourceAddress,
                                       ByteCount) then Exit;
  DestinationBytes := PByte(Destination);
  CurrentAddress := SourceAddress;
  Remaining := ByteCount;
  while Remaining <> 0 do
  begin
    if not AddressSpaceTranslate(RootPhysical, CurrentAddress,
                                 PhysicalAddress, Flags) then Exit;
    if (Flags and PTE_USER) = 0 then Exit;
    Chunk := PAGE_SIZE - (CurrentAddress and (PAGE_SIZE - 1));
    if Chunk > Remaining then Chunk := Remaining;
    SourceBytes := PByte(PtrUInt(MemoryHHDMOffset + PhysicalAddress));
    for I := 0 to Chunk - 1 do
      DestinationBytes[I] := SourceBytes[I];
    Inc(DestinationBytes, Chunk);
    Inc(CurrentAddress, Chunk);
    Dec(Remaining, Chunk);
  end;
  Result := True;
end;

function AddressSpaceLoadELF64(RootPhysical: QWord; Image: Pointer;
  ImageSize: QWord; out EntryPoint: QWord): Boolean;
var
  Header: PElf64Header;
  ProgramHeader: PElf64ProgramHeader;
  I, J: LongWord;
  SegmentEnd: QWord;
  FirstPage: QWord;
  EndPage: QWord;
  PageAddress: QWord;
  PageFlags: QWord;
  ImagePageCount: QWord;
  FileSource: Pointer;
  PhysicalAddress: QWord;
  EffectiveFlags: QWord;
  LoadableSegmentCount: LongWord;
  EntryPointExecutable: Boolean;
begin
  Result := False;
  EntryPoint := 0;
  EntryPointExecutable := False;
  if (Image = nil) or (ImageSize < SizeOf(TElf64Header)) then Exit;
  { Ring-3 data/code separation is mandatory for this phase. }
  if not CPUHasNXSupport then Exit;
  Header := PElf64Header(Image);
  if (Header^.Ident[0] <> ELF_MAGIC_0) or
     (Header^.Ident[1] <> ELF_MAGIC_1) or
     (Header^.Ident[2] <> ELF_MAGIC_2) or
     (Header^.Ident[3] <> ELF_MAGIC_3) or
     (Header^.Ident[4] <> ELF_CLASS_64) or
     (Header^.Ident[5] <> ELF_DATA_LITTLE_ENDIAN) or
     (Header^.Ident[6] <> ELF_VERSION_CURRENT) then Exit;
  if (Header^.FileType <> ELF_TYPE_EXECUTABLE) or
     (Header^.Machine <> ELF_MACHINE_X86_64) or
     (Header^.Version <> ELF_VERSION_CURRENT) or
     (Header^.HeaderSize < SizeOf(TElf64Header)) then Exit;
  if (Header^.ProgramHeaderCount = 0) or
     (Header^.ProgramHeaderCount > ELF_MAX_PROGRAM_HEADERS) or
     (Header^.ProgramHeaderEntrySize <> SizeOf(TElf64ProgramHeader)) then Exit;
  if Header^.ProgramHeaderOffset < Header^.HeaderSize then Exit;
  if Header^.ProgramHeaderOffset > ImageSize then Exit;
  if QWord(Header^.ProgramHeaderCount) >
     (ImageSize - Header^.ProgramHeaderOffset) div SizeOf(TElf64ProgramHeader) then Exit;

  ImagePageCount := 0;
  LoadableSegmentCount := 0;
  for I := 0 to Header^.ProgramHeaderCount - 1 do
  begin
    ProgramHeader := PElf64ProgramHeader(PtrUInt(Image) +
      Header^.ProgramHeaderOffset + QWord(I) * SizeOf(TElf64ProgramHeader));
    if (ProgramHeader^.SegmentType = ELF_PT_DYNAMIC) or
       (ProgramHeader^.SegmentType = ELF_PT_INTERP) or
       (ProgramHeader^.SegmentType = ELF_PT_TLS) then Exit;
    if ProgramHeader^.SegmentType <> ELF_PT_LOAD then Continue;
    if ProgramHeader^.MemorySize = 0 then Continue;
    if ProgramHeader^.MemorySize < ProgramHeader^.FileSize then Exit;
    if ProgramHeader^.FileOffset > ImageSize then Exit;
    if ProgramHeader^.FileSize > ImageSize - ProgramHeader^.FileOffset then Exit;
    if ProgramHeader^.VirtualAddress > High(QWord) - ProgramHeader^.MemorySize then Exit;
    SegmentEnd := ProgramHeader^.VirtualAddress + ProgramHeader^.MemorySize;
    if (ProgramHeader^.VirtualAddress < USER_IMAGE_MIN) or
       (SegmentEnd >= USER_STACK_GUARD) or
       (SegmentEnd >= USER_ADDRESS_LIMIT) then Exit;
    if (ProgramHeader^.Flags and (ELF_PF_WRITE or ELF_PF_EXECUTE)) =
       (ELF_PF_WRITE or ELF_PF_EXECUTE) then Exit; { no W+X pages }
    if ((ProgramHeader^.Flags and ELF_PF_EXECUTE) <> 0) and
       (Header^.EntryPoint >= ProgramHeader^.VirtualAddress) and
       (Header^.EntryPoint < SegmentEnd) then
      EntryPointExecutable := True;
    if ((ProgramHeader^.VirtualAddress xor ProgramHeader^.FileOffset) and
        (PAGE_SIZE - 1)) <> 0 then Exit;
    if (ProgramHeader^.Alignment > 1) then
    begin
      if (ProgramHeader^.Alignment and (ProgramHeader^.Alignment - 1)) <> 0 then Exit;
      if ((ProgramHeader^.VirtualAddress - ProgramHeader^.FileOffset) and
          (ProgramHeader^.Alignment - 1)) <> 0 then Exit;
    end;

    FirstPage := ProgramHeader^.VirtualAddress and PAGE_MASK;
    if SegmentEnd > High(QWord) - (PAGE_SIZE - 1) then Exit;
    EndPage := (SegmentEnd + PAGE_SIZE - 1) and PAGE_MASK;
    if EndPage <= FirstPage then Exit;
    ImagePageCount := ImagePageCount + ((EndPage - FirstPage) div PAGE_SIZE);
    if ImagePageCount > ELF_MAX_IMAGE_PAGES then Exit;

    PageFlags := PTE_USER;
    if (ProgramHeader^.Flags and ELF_PF_WRITE) <> 0 then
      PageFlags := PageFlags or PTE_WRITABLE;
    if CPUHasNXSupport and ((ProgramHeader^.Flags and ELF_PF_EXECUTE) = 0) then
      PageFlags := PageFlags or PTE_NO_EXECUTE;

    PageAddress := FirstPage;
    while PageAddress < EndPage do
    begin
      if not AddressSpaceMapZeroPage(RootPhysical, PageAddress, PageFlags) then Exit;
      Inc(PageAddress, PAGE_SIZE);
    end;

    if ProgramHeader^.FileSize <> 0 then
    begin
      FileSource := Pointer(PtrUInt(Image) + ProgramHeader^.FileOffset);
      if not AddressSpaceCopyTo(RootPhysical, ProgramHeader^.VirtualAddress,
                                FileSource, ProgramHeader^.FileSize) then Exit;
    end;
    Inc(LoadableSegmentCount);
  end;

  if (LoadableSegmentCount = 0) or not EntryPointExecutable then Exit;
  if not AddressSpaceTranslate(RootPhysical, Header^.EntryPoint,
                               PhysicalAddress, EffectiveFlags) then Exit;
  if ((EffectiveFlags and (PTE_PRESENT or PTE_USER)) <>
      (PTE_PRESENT or PTE_USER)) then Exit;
  if CPUHasNXSupport and ((EffectiveFlags and PTE_NO_EXECUTE) <> 0) then Exit;
  EntryPoint := Header^.EntryPoint;
  Result := True;
end;

function AddressSpaceCreateUserStack(RootPhysical: QWord;
  out StackPointer: QWord): Boolean;
var
  PageIndex: LongWord;
  PageAddress: QWord;
  PhysicalAddress: QWord;
  Flags: QWord;
begin
  Result := False;
  StackPointer := 0;
  Flags := PTE_USER or PTE_WRITABLE;
  if CPUHasNXSupport then
    Flags := Flags or PTE_NO_EXECUTE;
  for PageIndex := 0 to USER_STACK_PAGE_COUNT - 1 do
  begin
    PageAddress := USER_STACK_BOTTOM + QWord(PageIndex) * PAGE_SIZE;
    if not AddressSpaceMapZeroPage(RootPhysical, PageAddress, Flags) then Exit;
  end;
  { Leave a guard page below the stack. A zero fake return address makes the
    initial stack safe if a minimal _start accidentally executes RET. }
  StackPointer := USER_STACK_TOP - SizeOf(QWord);
  if not AddressSpaceTranslate(RootPhysical, StackPointer,
                               PhysicalAddress, Flags) then Exit;
  PQWord(PtrUInt(MemoryHHDMOffset + PhysicalAddress))^ := 0;
  Result := True;
end;
