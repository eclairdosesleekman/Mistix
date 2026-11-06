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

procedure WriteDecimal(Value: TUInt64);
var
  Digits: array[0..19] of Char;
  Count: LongInt;
begin
  if Value = 0 then
  begin
    SerialPutChar('0');
    Exit;
  end;

  Count := 0;
  while (Value <> 0) and (Count < 20) do
  begin
    Digits[Count] := Char(Ord('0') + (Value mod 10));
    Value := Value div 10;
    Inc(Count);
  end;
  while Count > 0 do
  begin
    Dec(Count);
    SerialPutChar(Digits[Count]);
  end;
end;

function SerialTryGetChar(out C: Char): Boolean;
begin
  Result := False;
  C := #0;
  if (In8(COM1_LSR) and $01) = 0 then Exit;
  C := Char(In8(COM1_DATA) and $FF);
  Result := True;
end;
