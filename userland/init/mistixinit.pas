unit MistixInit;

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

const
  EAGAIN_NEGATIVE = QWord($FFFFFFFFFFFFFFF5);
  CAP_RIGHT_SEND = $00000001;
  CAP_RIGHT_RECEIVE = $00000002;

  ForkChildText: array[0..36] of Byte = (
    70, 111, 114, 107, 32, 99, 104, 105, 108, 100, 32,
    114, 117, 110, 110, 105, 110, 103, 32, 119, 105, 116,
    104, 32, 67, 79, 87, 32, 109, 101, 109, 111, 114, 121, 33, 13, 10
  );
  ForkParentText: array[0..37] of Byte = (
    70, 111, 114, 107, 32, 112, 97, 114, 101, 110, 116, 32,
    99, 111, 110, 116, 105, 110, 117, 101, 100, 32, 97, 102,
    116, 101, 114, 32, 67, 79, 87, 32, 102, 111, 114, 107, 13, 10
  );
  WorkerReplyText: array[0..33] of Byte = (
    87, 111, 114, 107, 101, 114, 32, 114, 101, 112, 108, 105,
    101, 100, 32, 116, 104, 114, 111, 117, 103, 104, 32, 117,
    115, 101, 114, 32, 73, 80, 67, 33, 13, 10
  );

  USER_CAP_SEND = CAP_RIGHT_SEND;
  USER_CAP_RECEIVE = CAP_RIGHT_RECEIVE;
  IPC_REPLY_ID = $4D4953544958;

type
  TUserIPCMessage = packed record
    MessageId: QWord;
    WordCount: Byte;
    Reserved: array[0..6] of Byte;
    Words: array[0..5] of QWord;
  end;

var
  WorkerImageStart: Byte; external name '_binary_worker_elf_start';
  WorkerImageEnd: Byte; external name '_binary_worker_elf_end';
  { Writable .data value: both fork branches write different values to force COW. }
  ForkMarker: QWord = $1122334455667788;

function MistixWrite(FileDescriptor, BufferAddress,
  ByteCount: QWord): QWord; assembler; nostackframe;
asm
  mov eax, 1
  int $80
end;

procedure MistixExit(ExitCode: QWord); assembler; nostackframe;
asm
  mov eax, 2
  int $80
  ud2
end;

function MistixGetPid: QWord; assembler; nostackframe;
asm
  mov eax, 3
  int $80
end;

procedure MistixYield; assembler; nostackframe;
asm
  mov eax, 4
  int $80
end;

function MistixFork: QWord; assembler; nostackframe;
asm
  mov eax, 5
  int $80
end;

function MistixSpawn(ImageAddress, ImageSize: QWord): QWord; assembler; nostackframe;
asm
  mov eax, 6
  int $80
end;

function MistixWaitPID(PID, StatusAddress: QWord): QWord; assembler; nostackframe;
asm
  mov eax, 7
  int $80
end;

function MistixEndpointCreate: QWord; assembler; nostackframe;
asm
  mov eax, 8
  int $80
end;

function MistixIPCSend(Handle, MessageAddress: QWord): QWord; assembler; nostackframe;
asm
  mov eax, 9
  int $80
end;

function MistixIPCReceive(Handle, MessageAddress: QWord): QWord; assembler; nostackframe;
asm
  mov eax, 10
  int $80
end;

function MistixCapabilityTransfer(TargetPID, Handle, Rights: QWord): QWord;
  assembler; nostackframe;
asm
  mov eax, 11
  int $80
end;

function MistixCapabilityReceive: QWord; assembler; nostackframe;
asm
  mov eax, 12
  int $80
end;

procedure WriteStatus(Status: QWord);
var
  Digits: array[0..19] of Byte;
  Count: LongInt;
  Value: QWord;
begin
  Value := Status;
  Count := 0;
  if Value = 0 then
  begin
    Digits[0] := Ord('0');
    Count := 1;
  end
  else
    while (Value <> 0) and (Count < 20) do
    begin
      Digits[Count] := Ord('0') + Byte(Value mod 10);
      Value := Value div 10;
      Inc(Count);
    end;
  while Count > 0 do
  begin
    Dec(Count);
    MistixWrite(1, PtrUInt(@Digits[Count]), 1);
  end;
end;

procedure MistixUserMain; cdecl; public name 'mistix_user_main';
var
  ForkResult: QWord;
  WaitStatus: QWord;
  EndpointHandle: QWord;
  WorkerPID: QWord;
  WorkerImageSize: QWord;
  TransferredHandle: QWord;
  Message: TUserIPCMessage;
  IPCStatus: QWord;
begin
  ForkResult := MistixFork;
  if ForkResult = 0 then
  begin
    ForkMarker := $AABBCCDDEEFF0011; { must trigger a user-mode COW page fault }
    if ForkMarker <> $AABBCCDDEEFF0011 then
      MistixExit(12);
    MistixWrite(1, PtrUInt(@ForkChildText[0]), SizeOf(ForkChildText));
    MistixExit(11);
  end;

  if ForkResult <> QWord($FFFFFFFFFFFFFFF4) then
    ForkMarker := $5566778899AABBCC; { parent must get an independent page }
  MistixWrite(1, PtrUInt(@ForkParentText[0]), SizeOf(ForkParentText));
  WaitStatus := 0;
  if ForkResult <> QWord($FFFFFFFFFFFFFFF4) then
  begin
    if MistixWaitPID(ForkResult, PtrUInt(@WaitStatus)) <> ForkResult then
      MistixExit(94);
    if (WaitStatus <> 11) or (ForkMarker <> $5566778899AABBCC) then
      MistixExit(95);
    WriteStatus(WaitStatus);
  end;

  EndpointHandle := MistixEndpointCreate;
  WorkerImageSize := PtrUInt(@WorkerImageEnd) - PtrUInt(@WorkerImageStart);
  WorkerPID := MistixSpawn(PtrUInt(@WorkerImageStart), WorkerImageSize);
  if (EndpointHandle = 0) or
     (EndpointHandle >= QWord($FFFFFFFFFFFFF000)) or
     (WorkerPID = 0) or
     (WorkerPID >= QWord($FFFFFFFFFFFFF000)) then
    MistixExit(90);

  TransferredHandle := MistixCapabilityTransfer(WorkerPID, EndpointHandle,
    USER_CAP_SEND or USER_CAP_RECEIVE);
  if TransferredHandle >= QWord($FFFFFFFFFFFFF000) then
    MistixExit(91);

  Message.MessageId := 0;
  Message.WordCount := 0;
  Message.Reserved[0] := 0;
  Message.Reserved[1] := 0;
  Message.Reserved[2] := 0;
  Message.Reserved[3] := 0;
  Message.Reserved[4] := 0;
  Message.Reserved[5] := 0;
  Message.Reserved[6] := 0;
  Message.Words[0] := 0;
  Message.Words[1] := 0;
  Message.Words[2] := 0;
  Message.Words[3] := 0;
  Message.Words[4] := 0;
  Message.Words[5] := 0;
  repeat
    IPCStatus := MistixIPCReceive(EndpointHandle, PtrUInt(@Message));
    if IPCStatus = EAGAIN_NEGATIVE then MistixYield;
  until IPCStatus <> EAGAIN_NEGATIVE;

  if IPCStatus = 0 then
  begin
    if (Message.MessageId <> IPC_REPLY_ID) or (Message.WordCount <> 1) then
      MistixExit(92);
    MistixWrite(1, PtrUInt(@WorkerReplyText[0]), SizeOf(WorkerReplyText));
    if MistixWaitPID(WorkerPID, PtrUInt(@WaitStatus)) <> WorkerPID then
      MistixExit(96);
    if WaitStatus <> 23 then MistixExit(97);
    WriteStatus(WaitStatus);
  end
  else
    MistixExit(93);
  MistixExit(0);
end;

procedure MistixUserEntry; assembler; nostackframe; public name '_start';
asm
  and rsp, -16
  call mistix_user_main
  xor edi, edi
  mov eax, 2
  int $80
  ud2
end;

end.
