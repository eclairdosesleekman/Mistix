{ Phase 19 declarations shared by the physical allocator and kernel heap. }
const
  PMM_MAX_ORDER = 40;
  PMM_MAX_REGION_COUNT = LIMINE_MAX_MEMMAP_ENTRIES * 2;
  PMM_DMA32_LIMIT_PFN = QWord(1) shl 20;
  PMM_FRAME_FLAG_PERMANENT = $00000001;
  PMM_FRAME_FLAG_HEAP_BIG = $00000002;
  PMM_FRAME_FLAG_HEAP_PAGES = $00000004;

  KHEAP_CACHE_COUNT = 8;
  KHEAP_SLAB_MAGIC = QWord($4D495354534C4142);
  KHEAP_BIG_MAGIC = QWord($4D49535442494741);
  KHEAP_SLAB_BITMAP_BYTES = 32;
  KHEAP_MAX_SLAB_SLOTS = KHEAP_SLAB_BITMAP_BYTES * 8;

  KHeapCacheSizes: array[0..KHEAP_CACHE_COUNT - 1] of Word =
    (16, 32, 64, 128, 256, 512, 1024, 2048);


type
  TPageTable = array[0..511] of QWord;
  PPageTable = ^TPageTable;

  TMemoryZoneKind = (mzDMA32, mzNormal);
  TPageState = (pgReserved, pgFree, pgAllocated, pgTable,
                pgSlab, pgPinned, pgDevice, pgTail);
  TAllocFlag = (afZero, afDMA32, afPageTable, afPinned,
                afKernelHeapBig, afKernelHeapPages);
  TAllocFlags = set of TAllocFlag;

  TPageFrame = record
    RefCount: LongWord;
    Flags: LongWord;
    NextPFN: QWord;
    PrevPFN: QWord;
    PinCount: LongWord;
    OwnerPFN: QWord;
    Order: ShortInt;
    Zone: Byte;
    State: TPageState;
    Reserved: Byte;
  end;
  PPageFrame = ^TPageFrame;

  TMemoryRegion = record
    BasePFN: QWord;
    PageCount: QWord;
    MetadataPhysical: QWord;
    Frames: PPageFrame;
  end;

  TMemoryRange = record
    StartPFN: QWord;
    EndPFN: QWord; { exclusive }
  end;
  TPMMRangeArray = array[0..PMM_MAX_REGION_COUNT - 1] of TMemoryRange;

  TSlabHeader = packed record
    Magic: QWord;
    NextPhysical: QWord;
    PrevPhysical: QWord;
    CacheIndex: Byte;
    SlotSize: Word;
    SlotCount: Word;
    FreeCount: Word;
    Reserved: Byte;
    Bitmap: array[0..KHEAP_SLAB_BITMAP_BYTES - 1] of Byte;
  end;
  PSlabHeader = ^TSlabHeader;

  TBigAllocationHeader = packed record
    Magic: QWord;
    BasePhysical: QWord;
    RequestedSize: QWord;
    UserPointer: QWord;
    Order: Byte;
    Reserved: array[0..6] of Byte;
  end;
  PBigAllocationHeader = ^TBigAllocationHeader;


var
  PMMFreePageCount: QWord;
  MemoryHHDMOffset: QWord;
  VMMRootPhysical: QWord;
  PMMUsableRanges: TPMMRangeArray;
  PMMReservedRanges: TPMMRangeArray;
  PMMNormalizedRanges: TPMMRangeArray;
  PMMMemoryRegions: array[0..PMM_MAX_REGION_COUNT - 1] of TMemoryRegion;
  PMMRegionCount: LongWord;
  PMMNormalizedRangeCount: LongWord;
  PMMManagedPageCount: QWord;
  BuddyFreeLists: array[0..1, 0..PMM_MAX_ORDER] of QWord;

  KHeapSlabHeads: array[0..KHEAP_CACHE_COUNT - 1] of QWord;
  KHeapInitialized: Boolean;

