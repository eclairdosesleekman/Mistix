unit MistixNetd;

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

{$I mistixlib.inc}

const
  NET_RPC_IFINFO = $4D4E455400000001;
  NET_RPC_ARP_LOOKUP = $4D4E455400000002;
  NET_RPC_UDP_ECHO = $4D4E455400000003;
  NET_SHARED_BASE = MISTIX_SHM_DEFAULT_BASE;
  NET_SHARED_BYTES = 65536;
  NET_FRAME_OFFSET = 2048;
  NET_FRAME_MAX = 1600;
  NET_ETHERTYPE_IPV4 = $0800;
  NET_IPPROTO_UDP = 17;
  NET_ERR_INVAL = QWord(-22);
  NET_ERR_NOENT = QWord(-2);
  NET_LOOPBACK_IP = $7F000001;
  NET_MTU = 1500;

type
  TARPEntry = record
    Active: Byte;
    IPv4: LongWord;
    MAC: array[0..5] of Byte;
  end;

var
  RequestEndpoint: QWord;
  ReplyEndpoint: QWord;
  SharedHandle: QWord;
  SharedBuffer: PByte;
  ARPCache: array[0..7] of TARPEntry;

procedure ClearMessage(var Message: TMistixIPCMessage);
var I: LongWord;
begin
  Message.MessageId := 0;
  Message.WordCount := 0;
  for I := 0 to 6 do Message.Reserved[I] := 0;
  for I := 0 to 5 do Message.Words[I] := 0;
end;

function ReceiveOneCapability(out Handle: QWord): Boolean;
var Retry: LongWord;
begin
  Result := False;
  Handle := 0;
  Retry := 0;
  while Retry < 1000000 do
  begin
    Handle := SysCapabilityReceive;
    if not IsError(Handle) then
    begin
      Result := Handle <> 0;
      Exit;
    end;
    if Handle <> MISTIX_EAGAIN then Exit;
    SysYield;
    Inc(Retry);
  end;
end;

function ReceiveCapabilities: Boolean;
var A, B: QWord;
begin
  Result := False;
  if not ReceiveOneCapability(A) then Exit;
  if not ReceiveOneCapability(B) then Exit;
  if not ReceiveOneCapability(SharedHandle) then Exit;
  RequestEndpoint := A;
  ReplyEndpoint := B;
  if IsError(SysShmMap(SharedHandle, NET_SHARED_BASE, 1)) then Exit;
  SharedBuffer := PByte(PtrUInt(NET_SHARED_BASE));
  Result := True;
end;

procedure PutBE16(Buffer: PByte; Offset: LongWord; Value: Word); inline;
begin
  Buffer[Offset] := Byte(Value shr 8);
  Buffer[Offset + 1] := Byte(Value);
end;

function GetBE16(Buffer: PByte; Offset: LongWord): Word; inline;
begin
  Result := (Word(Buffer[Offset]) shl 8) or Buffer[Offset + 1];
end;

function FoldChecksum(Sum: QWord): Word;
begin
  while (Sum shr 16) <> 0 do Sum := (Sum and $FFFF) + (Sum shr 16);
  Result := Word(not Sum);
end;

function Checksum(Buffer: PByte; Length: LongWord): Word;
var
  Sum: QWord;
  I: LongWord;
begin
  Sum := 0;
  I := 0;
  while I + 1 < Length do
  begin
    Inc(Sum, (Word(Buffer[I]) shl 8) or Buffer[I + 1]);
    Inc(I, 2);
  end;
  if I < Length then Inc(Sum, QWord(Buffer[I]) shl 8);
  Result := FoldChecksum(Sum);
end;

function UDPChecksum(IP: PByte; UDP: PByte; UDPLength: Word): Word;
var
  Sum: QWord;
  I: LongWord;
  Value: Word;
begin
  Sum := 0;
  I := 12;
  while I <= 18 do
  begin
    Value := (Word(IP[I]) shl 8) or IP[I + 1];
    Inc(Sum, Value);
    Inc(I, 2);
  end;
  Inc(Sum, NET_IPPROTO_UDP);
  Inc(Sum, UDPLength);
  I := 0;
  while I + 1 < UDPLength do
  begin
    Inc(Sum, (Word(UDP[I]) shl 8) or UDP[I + 1]);
    Inc(I, 2);
  end;
  if I < UDPLength then Inc(Sum, QWord(UDP[I]) shl 8);
  Result := FoldChecksum(Sum);
end;

function BuildLoopbackFrame(PayloadLength: Word; DestinationPort: Word;
  out FrameLength: LongWord): Boolean;
var
  Frame, IP, UDP: PByte;
  I: LongWord;
  C: Word;
begin
  Result := False;
  FrameLength := 0;
  if PayloadLength > NET_MTU - 28 then Exit;
  Frame := PByte(PtrUInt(NET_SHARED_BASE + NET_FRAME_OFFSET));
  { Ethernet header: loopback pseudo-MAC -> loopback pseudo-MAC. }
  for I := 0 to 5 do
  begin
    Frame[I] := $02;
    Frame[6 + I] := $02;
  end;
  Frame[0] := $02; Frame[1] := 0; Frame[2] := 0;
  Frame[3] := 0; Frame[4] := 0; Frame[5] := 1;
  for I := 0 to 5 do Frame[6 + I] := Frame[I];
  PutBE16(Frame, 12, NET_ETHERTYPE_IPV4);
  IP := PByte(PtrUInt(Frame) + 14);
  for I := 0 to 19 do IP[I] := 0;
  IP[0] := $45;
  PutBE16(IP, 2, Word(28 + LongWord(PayloadLength)));
  PutBE16(IP, 4, $0701);
  IP[8] := 64;
  IP[9] := NET_IPPROTO_UDP;
  IP[12] := 127; IP[13] := 0; IP[14] := 0; IP[15] := 1;
  IP[16] := 127; IP[17] := 0; IP[18] := 0; IP[19] := 1;
  C := Checksum(IP, 20);
  PutBE16(IP, 10, C);
  UDP := PByte(PtrUInt(IP) + 20);
  for I := 0 to 7 do UDP[I] := 0;
  PutBE16(UDP, 0, 40000);
  PutBE16(UDP, 2, DestinationPort);
  PutBE16(UDP, 4, Word(8 + LongWord(PayloadLength)));
  if PayloadLength <> 0 then
    for I := 0 to LongWord(PayloadLength) - 1 do
      UDP[8 + I] := SharedBuffer[I];
  C := UDPChecksum(IP, UDP, 8 + PayloadLength);
  if C = 0 then C := $FFFF;
  PutBE16(UDP, 6, C);
  FrameLength := 14 + 20 + 8 + PayloadLength;
  Result := True;
end;

function ValidateLoopbackFrame(FrameLength: LongWord;
  ExpectedPort: Word; out PayloadLength: Word): Boolean;
var
  Frame, IP, UDP: PByte;
  IPLength, UDPLength: Word;
begin
  Result := False;
  PayloadLength := 0;
  if (FrameLength < 42) or (FrameLength > NET_FRAME_MAX) then Exit;
  Frame := PByte(PtrUInt(NET_SHARED_BASE + NET_FRAME_OFFSET));
  if GetBE16(Frame, 12) <> NET_ETHERTYPE_IPV4 then Exit;
  IP := PByte(PtrUInt(Frame) + 14);
  if (IP[0] <> $45) or (IP[9] <> NET_IPPROTO_UDP) then Exit;
  IPLength := GetBE16(IP, 2);
  if (IPLength < 28) or (IPLength > FrameLength - 14) then Exit;
  if Checksum(IP, 20) <> 0 then Exit;
  if (GetBE16(IP, 6) and $3FFF) <> 0 then Exit;
  UDP := PByte(PtrUInt(IP) + 20);
  UDPLength := GetBE16(UDP, 4);
  if (UDPLength < 8) or (UDPLength <> IPLength - 20) then Exit;
  if GetBE16(UDP, 2) <> ExpectedPort then Exit;
  if UDPChecksum(IP, UDP, UDPLength) <> 0 then Exit;
  PayloadLength := UDPLength - 8;
  Result := True;
end;

procedure InitializeARPCache;
begin
  ARPCache[0].Active := 1;
  ARPCache[0].IPv4 := NET_LOOPBACK_IP;
  ARPCache[0].MAC[0] := $02; ARPCache[0].MAC[1] := 0;
  ARPCache[0].MAC[2] := 0; ARPCache[0].MAC[3] := 0;
  ARPCache[0].MAC[4] := 0; ARPCache[0].MAC[5] := 1;
end;

procedure Dispatch(const Request: TMistixIPCMessage;
  var Response: TMistixIPCMessage);
var
  I: LongWord;
  FrameLength: LongWord;
  PayloadLength: Word;
  RequestedLength: QWord;
  Frame, UDP: PByte;
begin
  ClearMessage(Response);
  Response.MessageId := NET_ERR_INVAL;
  case Request.MessageId of
    NET_RPC_IFINFO:
      begin
        Response.MessageId := 0;
        Response.WordCount := 3;
        Response.Words[0] := NET_LOOPBACK_IP;
        Response.Words[1] := NET_MTU;
        Response.Words[2] := 1; { loopback interface is administratively up }
      end;
    NET_RPC_ARP_LOOKUP:
      begin
        if Request.WordCount < 1 then Exit;
        for I := 0 to High(ARPCache) do
          if (ARPCache[I].Active <> 0) and
             (ARPCache[I].IPv4 = LongWord(Request.Words[0])) then
          begin
            Response.MessageId := 0;
            Response.WordCount := 3;
            Response.Words[0] := 1;
            Response.Words[1] := $02000000;
            Response.Words[2] := $00000001;
            Exit;
          end;
        Response.MessageId := NET_ERR_NOENT;
      end;
    NET_RPC_UDP_ECHO:
      begin
        if Request.WordCount < 2 then Exit;
        RequestedLength := Request.Words[1];
        if (RequestedLength > NET_MTU - 28) or (Request.Words[0] > 65535) then
        begin
          Response.MessageId := NET_ERR_INVAL;
          Exit;
        end;
        if not BuildLoopbackFrame(Word(RequestedLength), Word(Request.Words[0]),
             FrameLength) then Exit;
        if not ValidateLoopbackFrame(FrameLength, Word(Request.Words[0]),
             PayloadLength) then Exit;
        Frame := PByte(PtrUInt(NET_SHARED_BASE + NET_FRAME_OFFSET));
        UDP := PByte(PtrUInt(Frame) + 14 + 20);
        if PayloadLength <> 0 then
          for I := 0 to LongWord(PayloadLength) - 1 do
            SharedBuffer[I] := UDP[8 + I];
        Response.MessageId := 0;
        Response.WordCount := 1;
        Response.Words[0] := PayloadLength;
      end;
  end;
end;

procedure MistixNetdMain; cdecl; public name 'mistix_user_main';
var
  Request, Response: TMistixIPCMessage;
  Status: QWord;
begin
  if not ReceiveCapabilities then SysExit(40);
  InitializeARPCache;
  WriteCString('netd: loopback virtual NIC, ARP cache and UDP test endpoint online'#13#10);
  while True do
  begin
    Status := SysIPCReceive(RequestEndpoint, PtrUInt(@Request));
    if IsError(Status) then
    begin
      if Status <> MISTIX_EAGAIN then SysExit(41);
      SysYield;
      Continue;
    end;
    Dispatch(Request, Response);
    repeat
      Status := SysIPCSend(ReplyEndpoint, PtrUInt(@Response));
      if Status = MISTIX_EAGAIN then SysYield
      else if IsError(Status) then SysExit(42);
    until not IsError(Status);
  end;
end;

procedure MistixNetdEntry; assembler; nostackframe; public name '_start';
asm
  and rsp, -16
  call MistixNetdMain
  xor edi, edi
  mov eax, 2
  int $80
  ud2
end;

end.
