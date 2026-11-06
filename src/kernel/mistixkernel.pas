unit MistixKernel;

{$mode objfpc}
{$H-}
{$R-}
{$Q-}
{$S-}
{$I-}
{$ALIGN 8}
{$ASMMODE intel}

interface

implementation

type
  TUInt64 = QWord;
  PLimineMemmapResponse = ^TLimineMemmapResponse;
  PLimineMemmapEntry = ^TLimineMemmapEntry;
  PLimineMemmapEntryArray = ^TLimineMemmapEntryArray;

  TLimineMemmapEntry = packed record
    Base: TUInt64;
    Length: TUInt64;
    EntryType: TUInt64;
  end;

  TLimineMemmapEntryArray = array[0..0] of PLimineMemmapEntry;

  TLimineMemmapResponse = packed record
    Revision: TUInt64;
    EntryCount: TUInt64;
    Entries: PLimineMemmapEntryArray;
  end;

  PLimineHHDMResponse = ^TLimineHHDMResponse;
  TLimineHHDMResponse = packed record
    Revision: TUInt64;
    Offset: TUInt64;
  end;

  PLimineStackSizeResponse = ^TLimineStackSizeResponse;
  TLimineStackSizeResponse = packed record
    Revision: TUInt64;
  end;

  TLimineMemmapRequest = packed record
    ID: array[0..3] of TUInt64;
    Revision: TUInt64;
    Response: PLimineMemmapResponse;
  end;

  TLimineHHDMRequest = packed record
    ID: array[0..3] of TUInt64;
    Revision: TUInt64;
    Response: PLimineHHDMResponse;
  end;

  TLimineStackSizeRequest = packed record
    ID: array[0..3] of TUInt64;
    Revision: TUInt64;
    Response: PLimineStackSizeResponse;
    StackSize: TUInt64;
  end;

  TLimineRequestBlock = packed record
    StartMarker: array[0..3] of TUInt64;
    BaseRevision: array[0..2] of TUInt64;
    MemmapRequest: TLimineMemmapRequest;
    HHDMRequest: TLimineHHDMRequest;
    StackSizeRequest: TLimineStackSizeRequest;
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
  MAX_MEMMAP_ENTRIES_TO_PRINT = 8;
  MAX_MEMMAP_ENTRIES_ACCEPTED = 4096;

  HexDigits: array[0..15] of Char = (
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
  );

var
  { Limine requires these objects to be present in the loaded kernel image.
    Keeping the complete request block in one 8-byte-aligned variable makes
    the start/end delimiters and request records contiguous. }
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
      6
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
    EndMarker: (
      $ADC0E0531BB10D03,
      $9572709F31764C62
    )
  ); public name 'limine_request_block';

procedure Out8(Port: Word; Value: Word); cdecl; assembler; nostackframe;
asm
  mov dx, di
  mov ax, si
  out dx, al
end;

function In8(Port: Word): Word; cdecl; assembler; nostackframe;
asm
  mov dx, di
  xor eax, eax
  in al, dx
end;

procedure SerialInit;
begin
  { 16550-compatible UART, COM1, 115200 baud, 8 data bits, no parity,
    one stop bit. }
  Out8(COM1_IER, 0);
  Out8(COM1_LCR, $80);
  Out8(COM1_DATA, 1);
  Out8(COM1_IER, 0);
  Out8(COM1_LCR, 3);
  Out8(COM1_FCR, $C7);
  Out8(COM1_MCR, $0B);
end;

procedure SerialPutChar(C: Char);
var
  WaitCount: LongWord;
begin
  WaitCount := 0;
  while (In8(COM1_LSR) and $20) = 0 do
  begin
    Inc(WaitCount);
    if WaitCount >= 1000000 then
      Exit;
  end;
  Out8(COM1_DATA, Ord(C));
end;

procedure WriteText(const S: ShortString);
var
  I: LongInt;
begin
  for I := 1 to Length(S) do
    SerialPutChar(S[I]);
end;

procedure WriteHex64(Value: TUInt64);
var
  Nibble: LongInt;
  Digit: Byte;
begin
  WriteText('0x');
  for Nibble := 15 downto 0 do
  begin
    Digit := Byte((Value shr (Nibble * 4)) and $F);
    SerialPutChar(HexDigits[Digit]);
  end;
end;

function WriteMemmap: Boolean;
var
  Response: PLimineMemmapResponse;
  Entry: PLimineMemmapEntry;
  Index: TUInt64;
  Limit: TUInt64;
begin
  Response := LimineRequests.MemmapRequest.Response;
  Result := False;
  if Response = nil then
  begin
    WriteText('[FAIL] Limine did not return a memory map.'#13#10);
    Exit;
  end;

  if Response^.EntryCount > MAX_MEMMAP_ENTRIES_ACCEPTED then
  begin
    WriteText('[FAIL] Memory map entry count exceeds the Phase 0 safety limit.'#13#10);
    Exit;
  end;

  if (Response^.EntryCount <> 0) and (Response^.Entries = nil) then
  begin
    WriteText('[FAIL] Memory map entry array is nil.'#13#10);
    Exit;
  end;

  WriteText('Memory map entries: ');
  WriteHex64(Response^.EntryCount);
  WriteText(#13#10);

  Limit := Response^.EntryCount;
  if Limit > MAX_MEMMAP_ENTRIES_TO_PRINT then
    Limit := MAX_MEMMAP_ENTRIES_TO_PRINT;

  if Limit <> 0 then
  begin
    for Index := 0 to Limit - 1 do
    begin
      Entry := Response^.Entries^[Index];
      if Entry = nil then
      begin
        WriteText('  [FAIL] Null memory map entry pointer.'#13#10);
        Exit;
      end;

      WriteText('  [');
      WriteHex64(Index);
      WriteText('] base=');
      WriteHex64(Entry^.Base);
      WriteText(' length=');
      WriteHex64(Entry^.Length);
      WriteText(' type=');
      WriteHex64(Entry^.EntryType);
      if Entry^.EntryType = LIMINE_MEMMAP_USABLE then
        WriteText(' usable');
      WriteText(#13#10);
    end;
  end;
  Result := True;
end;

function WriteHHDM: Boolean;
var
  Response: PLimineHHDMResponse;
begin
  Response := LimineRequests.HHDMRequest.Response;
  Result := False;
  if Response = nil then
  begin
    WriteText('[FAIL] Limine did not return the HHDM offset.'#13#10);
    Exit;
  end;

  WriteText('HHDM offset: ');
  WriteHex64(Response^.Offset);
  WriteText(#13#10);
  Result := True;
end;

procedure KernelMain; cdecl; public name 'mistix_kernel_main';
var
  BootInfoOK: Boolean;
begin
  SerialInit;
  WriteText(#13#10);
  WriteText('========================================'#13#10);
  WriteText(' Mistix - Phase 0 bootstrap             '#13#10);
  WriteText(' Pascal freestanding kernel / x86-64    '#13#10);
  WriteText('========================================'#13#10);

  if LimineRequests.BaseRevision[2] <> 0 then
  begin
    WriteText('[FAIL] Limine base revision 6 was not accepted.'#13#10);
    WriteText('       Check that the Limine binary is recent enough.'#13#10);
    Exit;
  end;

  WriteText('[ OK ] Limine base revision 6 accepted.'#13#10);

  if LimineRequests.StackSizeRequest.Response <> nil then
    WriteText('[ OK ] Limine stack-size request acknowledged.'#13#10)
  else
    WriteText('[WARN] No stack-size response; using protocol default.'#13#10);

  BootInfoOK := True;
  if not WriteHHDM then
    BootInfoOK := False;
  if not WriteMemmap then
    BootInfoOK := False;

  if BootInfoOK then
  begin
    WriteText('[PASS] Phase 0 entry, Pascal code, COM1 and Limine boot info are alive.'#13#10);
    WriteText('       CPU remains halted with interrupts disabled.'#13#10);
  end
  else
    WriteText('[FAIL] Phase 0 boot-information validation failed.'#13#10);
end;

procedure MistixEntry; assembler; nostackframe; public name '_start';
asm
  cli
  cld
  xor rbp, rbp
  and rsp, -16
  call KernelMain
  cli
@halt_loop:
  hlt
  jmp @halt_loop
end;

end.
