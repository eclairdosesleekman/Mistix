unit MistixDevd;

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
  DEV_RPC_REGISTER = $4D44455600000001;
  DEV_RPC_LIST = $4D44455600000002;
  DEV_RPC_INFO = $4D44455600000003;
  DEV_RPC_REMOVE = $4D44455600000004;
  DEV_MAX = 16;
  DEV_NAME_BYTES = 32;
  DEV_ERR_NOENT = QWord(-2);
  DEV_ERR_NOSPC = QWord(-28);
  DEV_ERR_INVAL = QWord(-22);

type
  TDeviceRecord = record
    Active: Byte;
    DeviceClass: Byte;
    State: Byte;
    Reserved: Byte;
    Generation: Word;
    Name: array[0..DEV_NAME_BYTES - 1] of Char;
  end;

var
  Devices: array[0..DEV_MAX - 1] of TDeviceRecord;
  RequestEndpoint: QWord;
  ReplyEndpoint: QWord;

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
begin
  Result := ReceiveOneCapability(RequestEndpoint) and
            ReceiveOneCapability(ReplyEndpoint);
end;

function RegisterDevice(const Request: TMistixIPCMessage): QWord;
var
  I, J: LongWord;
  Source: PByte;
begin
  Result := DEV_ERR_NOSPC;
  if Request.WordCount < 6 then
  begin
    Result := DEV_ERR_INVAL;
    Exit;
  end;
  if (Request.Words[0] > 15) or (Request.Words[1] > 2) then
  begin
    Result := DEV_ERR_INVAL;
    Exit;
  end;
  Source := PByte(@Request.Words[2]);
  if Source[0] = 0 then
  begin
    Result := DEV_ERR_INVAL;
    Exit;
  end;
  for I := 0 to DEV_MAX - 1 do
  begin
    if Devices[I].Active <> 0 then Continue;
    if Devices[I].Generation = High(Word) then Continue;
    Inc(Devices[I].Generation);
    Devices[I].Active := 1;
    Devices[I].DeviceClass := Byte(Request.Words[0]);
    Devices[I].State := Byte(Request.Words[1]);
    Devices[I].Reserved := 0;
    for J := 0 to DEV_NAME_BYTES - 1 do
    begin
      Devices[I].Name[J] := Char(Source[J]);
      if Source[J] = 0 then Break;
    end;
    if J = DEV_NAME_BYTES then Devices[I].Name[DEV_NAME_BYTES - 1] := #0;
    Result := (QWord(Devices[I].Generation) shl 16) or (I + 1);
    Exit;
  end;
end;

procedure Dispatch(const Request: TMistixIPCMessage;
  var Response: TMistixIPCMessage);
var
  I: LongWord;
  Handle, Slot, Generation: LongWord;
  Dest: PByte;
begin
  ClearMessage(Response);
  Response.MessageId := DEV_ERR_INVAL;
  case Request.MessageId of
    DEV_RPC_REGISTER:
      begin
        Response.MessageId := RegisterDevice(Request);
        if not IsError(Response.MessageId) then
        begin
          Response.WordCount := 1;
          Response.Words[0] := Response.MessageId;
          Response.MessageId := 0;
        end;
      end;
    DEV_RPC_LIST:
      begin
        Response.MessageId := 0;
        Response.WordCount := 1;
        Response.Words[0] := 0;
        for I := 0 to DEV_MAX - 1 do
          if Devices[I].Active <> 0 then Inc(Response.Words[0]);
      end;
    DEV_RPC_INFO:
      begin
        if Request.WordCount < 1 then Exit;
        Handle := LongWord(Request.Words[0]);
        Slot := (Handle and $FFFF);
        Generation := Handle shr 16;
        if (Slot = 0) or (Slot > DEV_MAX) then
        begin
          Response.MessageId := DEV_ERR_NOENT;
          Exit;
        end;
        Dec(Slot);
        if (Devices[Slot].Active = 0) or
           (Devices[Slot].Generation <> Generation) then
        begin
          Response.MessageId := DEV_ERR_NOENT;
          Exit;
        end;
        Response.MessageId := 0;
        Response.WordCount := 4;
        Response.Words[0] := Devices[Slot].DeviceClass;
        Response.Words[1] := Devices[Slot].State;
        Dest := PByte(@Response.Words[2]);
        for I := 0 to 15 do Dest[I] := Ord(Devices[Slot].Name[I]);
      end;
    DEV_RPC_REMOVE:
      begin
        if Request.WordCount < 1 then Exit;
        Handle := LongWord(Request.Words[0]);
        Slot := Handle and $FFFF;
        Generation := Handle shr 16;
        if (Slot = 0) or (Slot > DEV_MAX) then
        begin
          Response.MessageId := DEV_ERR_NOENT;
          Exit;
        end;
        Dec(Slot);
        if (Devices[Slot].Active = 0) or
           (Devices[Slot].Generation <> Generation) then
        begin
          Response.MessageId := DEV_ERR_NOENT;
          Exit;
        end;
        Devices[Slot].Active := 0;
        Response.MessageId := 0;
      end;
  end;
end;

procedure MistixDevdMain; cdecl; public name 'mistix_user_main';
var
  Request, Response: TMistixIPCMessage;
  Status: QWord;
begin
  if not ReceiveCapabilities then SysExit(30);
  WriteCString('devd: device registry service online'#13#10);
  while True do
  begin
    Status := SysIPCReceive(RequestEndpoint, PtrUInt(@Request));
    if IsError(Status) then
    begin
      if Status <> MISTIX_EAGAIN then SysExit(31);
      SysYield;
      Continue;
    end;
    Dispatch(Request, Response);
    repeat
      Status := SysIPCSend(ReplyEndpoint, PtrUInt(@Response));
      if Status = MISTIX_EAGAIN then SysYield
      else if IsError(Status) then SysExit(32);
    until not IsError(Status);
  end;
end;

procedure MistixDevdEntry; assembler; nostackframe; public name '_start';
asm
  and rsp, -16
  call MistixDevdMain
  xor edi, edi
  mov eax, 2
  int $80
  ud2
end;

end.
