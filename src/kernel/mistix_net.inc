{ Mistix Phase 6 packet parser and protocol checks. This is deliberately a
  pure-Pascal protocol core; a NIC driver and userland network service are
  not yet connected to it. }

const
  NET_ETHERTYPE_IPV4 = $0800;
  NET_ETHERTYPE_ARP = $0806;
  NET_IPPROTO_ICMP = 1;
  NET_IPPROTO_TCP = 6;
  NET_IPPROTO_UDP = 17;

type
  TNetFrameKind = (nfUnknown, nfARP, nfIPv4);
  TNetFrameInfo = record
    Kind: TNetFrameKind;
    EtherType: Word;
    IPProtocol: Byte;
    HeaderLength: Byte;
    TotalLength: Word;
    SourceIPv4: LongWord;
    DestIPv4: LongWord;
    SourcePort: Word;
    DestPort: Word;
    PayloadOffset: LongWord;
    PayloadLength: LongWord;
  end;

function NetReadBE16(Buffer: Pointer; Offset: LongWord): Word; inline;
begin
  Result := (Word(PByte(PtrUInt(Buffer) + Offset)^) shl 8) or
    Word(PByte(PtrUInt(Buffer) + Offset + 1)^);
end;

function NetReadBE32(Buffer: Pointer; Offset: LongWord): LongWord; inline;
begin
  Result := (LongWord(NetReadBE16(Buffer, Offset)) shl 16) or
    NetReadBE16(Buffer, Offset + 2);
end;

function NetIPv4Checksum(Buffer: Pointer; ByteCount: LongWord): Word;
var
  Sum: QWord;
  I: LongWord;
  Value: Word;
begin
  Sum := 0;
  I := 0;
  while I + 1 < ByteCount do
  begin
    Value := NetReadBE16(Buffer, I);
    Inc(Sum, Value);
    Inc(I, 2);
  end;
  if I < ByteCount then Inc(Sum, QWord(PByte(PtrUInt(Buffer) + I)^) shl 8);
  while (Sum shr 16) <> 0 do
    Sum := (Sum and $FFFF) + (Sum shr 16);
  Result := Word(not Sum);
end;

function NetParseEthernetFrame(Buffer: Pointer; FrameLength: LongWord;
  out Info: TNetFrameInfo): Boolean;
var
  IP: Pointer;
  UDP: Pointer;
  IHL, IPLength, UDPSize: LongWord;
  EtherType: Word;
begin
  Result := False;
  Info.Kind := nfUnknown;
  Info.EtherType := 0;
  Info.IPProtocol := 0;
  Info.HeaderLength := 0;
  Info.TotalLength := 0;
  Info.SourceIPv4 := 0;
  Info.DestIPv4 := 0;
  Info.SourcePort := 0;
  Info.DestPort := 0;
  Info.PayloadOffset := 0;
  Info.PayloadLength := 0;
  if (Buffer = nil) or (FrameLength < 14) then Exit;
  EtherType := NetReadBE16(Buffer, 12);
  Info.EtherType := EtherType;
  if EtherType = NET_ETHERTYPE_ARP then
  begin
    if FrameLength < 42 then Exit;
    Info.Kind := nfARP;
    Info.PayloadOffset := 14;
    Info.PayloadLength := FrameLength - 14;
    Result := True;
    Exit;
  end;
  if EtherType <> NET_ETHERTYPE_IPV4 then Exit;
  if FrameLength < 34 then Exit;
  IP := Pointer(PtrUInt(Buffer) + 14);
  if (PByte(IP)^ shr 4) <> 4 then Exit;
  IHL := (PByte(IP)^ and $0F) * 4;
  if (IHL < 20) or (IHL > 60) or (FrameLength < 14 + IHL) then Exit;
  IPLength := NetReadBE16(IP, 2);
  if (IPLength < IHL) or (IPLength > FrameLength - 14) then Exit;
  { Fragments need reassembly and are not passed to the transport parser yet. }
  if (NetReadBE16(IP, 6) and $3FFF) <> 0 then Exit;
  if NetIPv4Checksum(IP, IHL) <> 0 then Exit;
  Info.Kind := nfIPv4;
  Info.IPProtocol := PByte(PtrUInt(IP) + 9)^;
  Info.HeaderLength := IHL;
  Info.TotalLength := IPLength;
  Info.SourceIPv4 := NetReadBE32(IP, 12);
  Info.DestIPv4 := NetReadBE32(IP, 16);
  Info.PayloadOffset := 14 + IHL;
  Info.PayloadLength := IPLength - IHL;
  if Info.IPProtocol = NET_IPPROTO_UDP then
  begin
    if Info.PayloadLength < 8 then Exit;
    UDP := Pointer(PtrUInt(IP) + IHL);
    UDPSize := NetReadBE16(UDP, 4);
    if (UDPSize < 8) or (UDPSize > Info.PayloadLength) then Exit;
    Info.SourcePort := NetReadBE16(UDP, 0);
    Info.DestPort := NetReadBE16(UDP, 2);
    Info.PayloadOffset := 14 + IHL + 8;
    Info.PayloadLength := UDPSize - 8;
  end;
  Result := True;
end;

function NetParserSelfTest: Boolean;
var
  Frame: array[0..41] of Byte;
  Checksum: Word;
  Info: TNetFrameInfo;
  I: LongWord;
begin
  Result := False;
  for I := 0 to High(Frame) do Frame[I] := 0;
  Frame[12] := $08; Frame[13] := $00; { Ethernet IPv4 }
  Frame[14] := $45;                  { IPv4, 20-byte header }
  Frame[16] := 0; Frame[17] := 28;   { IP total length }
  Frame[22] := 64; Frame[23] := NET_IPPROTO_UDP;
  Frame[26] := 10; Frame[27] := 0; Frame[28] := 0; Frame[29] := 1;
  Frame[30] := 10; Frame[31] := 0; Frame[32] := 0; Frame[33] := 2;
  Checksum := NetIPv4Checksum(@Frame[14], 20);
  Frame[24] := Byte(Checksum shr 8);
  Frame[25] := Byte(Checksum);
  Frame[34] := $12; Frame[35] := $34;
  Frame[36] := $56; Frame[37] := $78;
  Frame[38] := 0; Frame[39] := 8;
  if not NetParseEthernetFrame(@Frame[0], SizeOf(Frame), Info) then Exit;
  Result := (Info.Kind = nfIPv4) and
            (Info.IPProtocol = NET_IPPROTO_UDP) and
            (Info.SourcePort = $1234) and
            (Info.DestPort = $5678) and
            (Info.PayloadLength = 0);
end;

procedure NetInitialize;
begin
  if NetParserSelfTest then
    WriteText('[ OK ] Ethernet / IPv4 / UDP parser self-test passed.'#13#10)
  else
    WriteText('[FAIL] Ethernet / IPv4 / UDP parser self-test failed.'#13#10);
  WriteText('[NOTE] No NIC driver is attached; packet I/O is not available yet.'#13#10);
end;
