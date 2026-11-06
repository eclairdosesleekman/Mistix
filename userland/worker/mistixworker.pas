unit MistixWorker;

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
  TUserIPCMessage = packed record
    MessageId: QWord;
    WordCount: Byte;
    Reserved: array[0..6] of Byte;
    Words: array[0..5] of QWord;
  end;

const
  WorkerText: array[0..24] of Byte = (
    87, 111, 114, 107, 101, 114, 32, 112, 114, 111, 99, 101,
    115, 115, 32, 115, 116, 97, 114, 116, 101, 100, 33, 13, 10
  );
  IPC_REPLY_ID = $4D4953544958;
  EAGAIN_NEGATIVE = QWord($FFFFFFFFFFFFFFF5);

function MistixWrite(FileDescriptor, BufferAddress,
  ByteCount: QWord): QWord; assembler; nostackframe;
asm
  mov eax, 1
  int $80
end;

function MistixCapabilityReceive: QWord; assembler; nostackframe;
asm
  mov eax, 12
  int $80
end;

function MistixIPCSend(Handle, MessageAddress: QWord): QWord; assembler; nostackframe;
asm
  mov eax, 9
  int $80
end;

procedure MistixYield; assembler; nostackframe;
asm
  mov eax, 4
  int $80
end;

procedure MistixExit(ExitCode: QWord); assembler; nostackframe;
asm
  mov eax, 2
  int $80
  ud2
end;

procedure MistixWorkerMain; cdecl; public name 'mistix_worker_main';
var
  Handle: QWord;
  Message: TUserIPCMessage;
  SendResult: QWord;
begin
  MistixWrite(1, PtrUInt(@WorkerText[0]), SizeOf(WorkerText));
  repeat
    Handle := MistixCapabilityReceive;
    if Handle = EAGAIN_NEGATIVE then
      MistixYield;
  until Handle <> EAGAIN_NEGATIVE;
  if (Handle = 0) or (Handle >= QWord($FFFFFFFFFFFFF000)) then
    MistixExit(70);
  Message.MessageId := IPC_REPLY_ID;
  Message.WordCount := 1;
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
  SendResult := MistixIPCSend(Handle, PtrUInt(@Message));
  if SendResult <> 0 then MistixExit(71);
  MistixExit(23);
end;

procedure MistixWorkerEntry; assembler; nostackframe; public name '_start';
asm
  and rsp, -16
  call mistix_worker_main
  mov edi, 72
  mov eax, 2
  int $80
  ud2
end;

end.
