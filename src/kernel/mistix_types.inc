type
  TUInt64 = QWord;
  PQWord = ^QWord;

  PLimineMemmapResponse = ^TLimineMemmapResponse;
  PLimineMemmapEntry = ^TLimineMemmapEntry;
  PLimineMemmapEntryArray = ^TLimineMemmapEntryArray;

  TLimineMemmapEntry = record
    Base: TUInt64;
    Length: TUInt64;
    EntryType: TUInt64;
  end;

  TLimineMemmapEntryArray = array[0..0] of PLimineMemmapEntry;

  TLimineMemmapResponse = record
    Revision: TUInt64;
    EntryCount: TUInt64;
    Entries: PLimineMemmapEntryArray;
  end;

  PLimineHHDMResponse = ^TLimineHHDMResponse;
  TLimineHHDMResponse = record
    Revision: TUInt64;
    Offset: TUInt64;
  end;

  PLimineStackSizeResponse = ^TLimineStackSizeResponse;
  TLimineStackSizeResponse = record
    Revision: TUInt64;
  end;

  PLiminePagingModeResponse = ^TLiminePagingModeResponse;
  TLiminePagingModeResponse = record
    Revision: TUInt64;
    Mode: TUInt64;
  end;

  TLimineMemmapRequest = record
    ID: array[0..3] of TUInt64;
    Revision: TUInt64;
    Response: PLimineMemmapResponse;
  end;

  TLimineHHDMRequest = record
    ID: array[0..3] of TUInt64;
    Revision: TUInt64;
    Response: PLimineHHDMResponse;
  end;

  TLimineStackSizeRequest = record
    ID: array[0..3] of TUInt64;
    Revision: TUInt64;
    Response: PLimineStackSizeResponse;
    StackSize: TUInt64;
  end;

  TLiminePagingModeRequest = record
    ID: array[0..3] of TUInt64;
    Revision: TUInt64;
    Response: PLiminePagingModeResponse;
    Mode: TUInt64;
    MaxMode: TUInt64;
    MinMode: TUInt64;
  end;

  TLimineRequestBlock = record
    StartMarker: array[0..3] of TUInt64;
    BaseRevision: array[0..2] of TUInt64;
    MemmapRequest: TLimineMemmapRequest;
    HHDMRequest: TLimineHHDMRequest;
    StackSizeRequest: TLimineStackSizeRequest;
    PagingModeRequest: TLiminePagingModeRequest;
    EndMarker: array[0..1] of TUInt64;
  end;

const
  COM1_BASE = $3F8;
  COM1_DATA = COM1_BASE + 0;
  COM1_IER  = COM1_BASE + 1;
  COM1_FCR  = COM1_BASE + 2;
  COM1_LCR  = COM1_BASE + 3;
  COM1_MCR  = COM1_BASE + 4;
  COM1_LSR  = COM1_BASE + 5;

  LIMINE_MEMMAP_USABLE = 0;
  LIMINE_BASE_REVISION = 6;
  LIMINE_PAGING_MODE_X86_64_4LVL = 0;
  LIMINE_MAX_MEMMAP_ENTRIES = 4096;

  PAGE_SIZE = 4096;
  PAGE_MASK = $FFFFFFFFFFFFF000;
  PTE_ADDRESS_MASK = $000FFFFFFFFFF000;
  PTE_PRESENT = $001;
  PTE_WRITABLE = $002;
  PTE_USER = $004;
  PTE_WRITE_THROUGH = $008;
  PTE_CACHE_DISABLE = $010;
  PTE_HUGE = $080;
  PTE_GLOBAL = $100;
  PTE_NO_EXECUTE = $8000000000000000;
  PTE_ALLOWED_FLAGS = PTE_WRITABLE or PTE_USER or PTE_WRITE_THROUGH or
                      PTE_CACHE_DISABLE or PTE_GLOBAL or PTE_NO_EXECUTE;

  { Fixed-size early allocator: supports physical addresses below 64 GiB. }
  PMM_PAGE_COUNT = 16777216;
  PMM_BITMAP_BYTES = PMM_PAGE_COUNT div 8;
  PMM_MAX_MEMMAP_ENTRIES = 4096;

  VMM_TEST_VIRTUAL_ADDRESS = $FFFFC00000000000;
  THREAD_MAX_COUNT = 4;
  THREAD_STACK_SIZE = 16384;

  HexDigits: array[0..15] of Char = (
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
  );

var
  LimineRequests: TLimineRequestBlock = (
    StartMarker: (
      $F6B8F4B39DE7D1AE,
      $FAB91A6940FCB9CF,
      $785C6ED015D3E316,
      $181E920A7852B9D9
    );
    BaseRevision: (
      $F9562B2D5C95A6C8,
      $6A7B384944536BDC,
      LIMINE_BASE_REVISION
    );
    MemmapRequest: (
      ID: (
        $C7B1DD30DF4C8B88,
        $0A82E883A194F07B,
        $67CF3D9D378A806F,
        $E304ACDFC50C3C62
      );
      Revision: 0;
      Response: nil
    );
    HHDMRequest: (
      ID: (
        $C7B1DD30DF4C8B88,
        $0A82E883A194F07B,
        $48DCF1CB8AD2B852,
        $63984E959A98244B
      );
      Revision: 0;
      Response: nil
    );
    StackSizeRequest: (
      ID: (
        $C7B1DD30DF4C8B88,
        $0A82E883A194F07B,
        $224EF0460A8E8926,
        $E1CB0FC25F46EA3D
      );
      Revision: 0;
      Response: nil;
      StackSize: 262144
    );
    PagingModeRequest: (
      ID: (
        $C7B1DD30DF4C8B88,
        $0A82E883A194F07B,
        $95C1A0EDAB0944CB,
        $A4E5CB3842F7488A
      );
      Revision: 1;
      Response: nil;
      Mode: LIMINE_PAGING_MODE_X86_64_4LVL;
      MaxMode: LIMINE_PAGING_MODE_X86_64_4LVL;
      MinMode: LIMINE_PAGING_MODE_X86_64_4LVL
    );
    EndMarker: (
      $ADC0E0531BB10D03,
      $9572709F31764C62
    )
  ); public name 'limine_request_block';
