{ Mistix Phase 2: bounded, blocking, message-based IPC endpoints. }

const
  ENDPOINT_MAX_COUNT = 16;
  IPC_QUEUE_DEPTH = 8;
  IPC_MAX_WORDS = 6;
  WAIT_KEY_ENDPOINT_RECEIVE = $11;
  WAIT_KEY_ENDPOINT_SEND = $12;

type
  TIPCMessage = packed record
    MessageId: QWord;
    WordCount: Byte;
    Reserved: array[0..6] of Byte;
    Words: array[0..IPC_MAX_WORDS - 1] of QWord;
  end;

  TEndpoint = record
    Active: Byte;
    Generation: Word;
    Head: LongWord;
    Tail: LongWord;
    Count: LongWord;
    Queue: array[0..IPC_QUEUE_DEPTH - 1] of TIPCMessage;
  end;

var
  EndpointTable: array[0..ENDPOINT_MAX_COUNT - 1] of TEndpoint;

function EndpointWaitKey(ObjectIndex, ObjectGeneration: Word;
  WaitClass: Byte): QWord;
begin
  Result := (QWord(1) shl 60) or (QWord(WaitClass) shl 52) or
            (QWord(ObjectGeneration) shl 16) or QWord(ObjectIndex + 1);
end;

procedure IPCInitialize;
var
  I: LongWord;
begin
  for I := 0 to ENDPOINT_MAX_COUNT - 1 do
  begin
    EndpointTable[I].Active := 0;
    EndpointTable[I].Generation := 0;
    EndpointTable[I].Head := 0;
    EndpointTable[I].Tail := 0;
    EndpointTable[I].Count := 0;
  end;
  WriteText('[ OK ] IPC endpoint registry initialised (' );
  WriteDecimal(ENDPOINT_MAX_COUNT);
  WriteText(' endpoints, queue depth ');
  WriteDecimal(IPC_QUEUE_DEPTH);
  WriteText(').'#13#10);
end;

function EndpointResolve(Handle: LongWord; RequiredRights: LongWord;
  out ObjectIndex: Word; out ObjectGeneration: Word): TKernelResult;
begin
  Result := CapabilityLookup(Handle, RequiredRights, okEndpoint,
                             ObjectIndex, ObjectGeneration);
  if Result <> krSuccess then
    Exit;
  if ObjectIndex >= ENDPOINT_MAX_COUNT then
  begin
    Result := krInvalidHandle;
    Exit;
  end;
  if (EndpointTable[ObjectIndex].Active = 0) or
     (EndpointTable[ObjectIndex].Generation <> ObjectGeneration) then
    Result := krObjectClosed;
end;

function EndpointCreate(out Handle: LongWord): TKernelResult;
var
  I: LongWord;
  NewGeneration: Word;
begin
  Handle := 0;
  for I := 0 to ENDPOINT_MAX_COUNT - 1 do
  begin
    if (EndpointTable[I].Active <> 0) or
       (EndpointTable[I].Generation = High(Word)) then
      Continue;
    NewGeneration := EndpointTable[I].Generation + 1;
    EndpointTable[I].Generation := NewGeneration;
    EndpointTable[I].Active := 1;
    EndpointTable[I].Head := 0;
    EndpointTable[I].Tail := 0;
    EndpointTable[I].Count := 0;
    Result := CapabilityInstall(okEndpoint, Word(I), NewGeneration,
      CAP_RIGHT_SEND or CAP_RIGHT_RECEIVE or CAP_RIGHT_MANAGE, Handle);
    if Result <> krSuccess then
      EndpointTable[I].Active := 0;
    Exit;
  end;
  Result := krNoResources;
end;

function EndpointClose(Handle: LongWord): TKernelResult;
var
  ObjectIndex: Word;
  ObjectGeneration: Word;
begin
  Result := EndpointResolve(Handle, CAP_RIGHT_MANAGE,
                            ObjectIndex, ObjectGeneration);
  if Result <> krSuccess then
    Exit;

  EndpointTable[ObjectIndex].Active := 0;
  EndpointTable[ObjectIndex].Count := 0;
  ThreadWakeWaiters(EndpointWaitKey(ObjectIndex, ObjectGeneration,
                                    WAIT_KEY_ENDPOINT_RECEIVE), True);
  ThreadWakeWaiters(EndpointWaitKey(ObjectIndex, ObjectGeneration,
                                    WAIT_KEY_ENDPOINT_SEND), True);
  CapabilityRevokeObject(okEndpoint, ObjectIndex, ObjectGeneration);
  Result := krSuccess;
end;

function IPCSend(Handle: LongWord; const Message: TIPCMessage): TKernelResult;
var
  ObjectIndex: Word;
  ObjectGeneration: Word;
  WaitKey: QWord;
  Endpoint: ^TEndpoint;
begin
  Result := krInvalidArgument;
  if Message.WordCount > IPC_MAX_WORDS then
    Exit;

  while True do
  begin
    Result := EndpointResolve(Handle, CAP_RIGHT_SEND,
                              ObjectIndex, ObjectGeneration);
    if Result <> krSuccess then
      Exit;
    Endpoint := @EndpointTable[ObjectIndex];
    if Endpoint^.Count < IPC_QUEUE_DEPTH then
      Break;

    WaitKey := EndpointWaitKey(ObjectIndex, ObjectGeneration,
                               WAIT_KEY_ENDPOINT_SEND);
    ThreadBlockCurrent(WaitKey);
  end;

  Endpoint^.Queue[Endpoint^.Tail] := Message;
  Endpoint^.Tail := (Endpoint^.Tail + 1) mod IPC_QUEUE_DEPTH;
  Inc(Endpoint^.Count);
  ThreadWakeWaiters(EndpointWaitKey(ObjectIndex, ObjectGeneration,
                                    WAIT_KEY_ENDPOINT_RECEIVE), False);
  Result := krSuccess;
end;

function IPCReceive(Handle: LongWord; out Message: TIPCMessage): TKernelResult;
var
  ObjectIndex: Word;
  ObjectGeneration: Word;
  WaitKey: QWord;
  Endpoint: ^TEndpoint;
begin
  while True do
  begin
    Result := EndpointResolve(Handle, CAP_RIGHT_RECEIVE,
                              ObjectIndex, ObjectGeneration);
    if Result <> krSuccess then
      Exit;
    Endpoint := @EndpointTable[ObjectIndex];
    if Endpoint^.Count <> 0 then
      Break;

    WaitKey := EndpointWaitKey(ObjectIndex, ObjectGeneration,
                               WAIT_KEY_ENDPOINT_RECEIVE);
    ThreadBlockCurrent(WaitKey);
  end;

  Message := Endpoint^.Queue[Endpoint^.Head];
  Endpoint^.Head := (Endpoint^.Head + 1) mod IPC_QUEUE_DEPTH;
  Dec(Endpoint^.Count);
  ThreadWakeWaiters(EndpointWaitKey(ObjectIndex, ObjectGeneration,
                                    WAIT_KEY_ENDPOINT_SEND), False);
  Result := krSuccess;
end;

{ Boot-time integration test: receiver blocks, sender enqueues, receiver wakes. }
var
  IPCTestEndpoint: LongWord;
  IPCTestReceiverStatus: TKernelResult;
  IPCTestReceived: TIPCMessage;
  IPCTestExpected: QWord;

procedure IPCTestReceiverWorker;
begin
  IPCTestReceiverStatus := IPCReceive(IPCTestEndpoint, IPCTestReceived);
  if IPCTestReceiverStatus = krSuccess then
    WriteText('[IPC] receiver woke and dequeued a message.'#13#10)
  else
    WriteText('[IPC] receiver returned an error.'#13#10);
end;

procedure IPCTestSenderWorker;
var
  Message: TIPCMessage;
  Status: TKernelResult;
begin
  Message.MessageId := $4D4953544958;
  Message.WordCount := 2;
  Message.Reserved[0] := 0;
  Message.Reserved[1] := 0;
  Message.Reserved[2] := 0;
  Message.Reserved[3] := 0;
  Message.Reserved[4] := 0;
  Message.Reserved[5] := 0;
  Message.Reserved[6] := 0;
  Message.Words[0] := $50415343414C;
  Message.Words[1] := IPCTestExpected;
  Message.Words[2] := 0;
  Message.Words[3] := 0;
  Message.Words[4] := 0;
  Message.Words[5] := 0;
  Status := IPCSend(IPCTestEndpoint, Message);
  if Status = krSuccess then
    WriteText('[IPC] sender queued a message.'#13#10)
  else
    WriteText('[IPC] sender returned an error.'#13#10);
end;

function IPCRendezvousSelfTest: Boolean;
var
  ReceiverThreadId: LongWord;
  SenderThreadId: LongWord;
  RestrictedHandle: LongWord;
  DeniedMessage: TIPCMessage;
  Status: TKernelResult;
begin
  Result := False;
  IPCTestExpected := $1122334455667788;
  IPCTestReceiverStatus := krInvalidArgument;
  IPCTestReceived.MessageId := 0;
  IPCTestReceived.WordCount := 0;

  Status := EndpointCreate(IPCTestEndpoint);
  if Status <> krSuccess then
  begin
    WriteText('[FAIL] Could not create IPC test endpoint.'#13#10);
    Exit;
  end;

  Status := CapabilityRestrict(IPCTestEndpoint, CAP_RIGHT_SEND,
                               RestrictedHandle);
  if Status <> krSuccess then
  begin
    EndpointClose(IPCTestEndpoint);
    WriteText('[FAIL] Could not derive send-only capability.'#13#10);
    Exit;
  end;
  Status := IPCReceive(RestrictedHandle, DeniedMessage);
  CapabilityClose(RestrictedHandle);
  if Status <> krPermissionDenied then
  begin
    EndpointClose(IPCTestEndpoint);
    WriteText('[FAIL] Capability rights check did not deny receive.'#13#10);
    Exit;
  end;
  WriteText('[ OK ] Capability restriction denied receive on send-only handle.'#13#10);

  if not ThreadCreate(@IPCTestReceiverWorker, ReceiverThreadId) then
  begin
    EndpointClose(IPCTestEndpoint);
    WriteText('[FAIL] Could not create IPC receiver thread.'#13#10);
    Exit;
  end;
  if not ThreadCreate(@IPCTestSenderWorker, SenderThreadId) then
  begin
    EndpointClose(IPCTestEndpoint);
    WriteText('[FAIL] Could not create IPC sender thread.'#13#10);
    Exit;
  end;
  WriteText('[ OK ] IPC test threads created: ');
  WriteDecimal(ReceiverThreadId);
  WriteText(' / ');
  WriteDecimal(SenderThreadId);
  WriteText(#13#10);

  ThreadsRunUntilIdle;
  Result := (IPCTestReceiverStatus = krSuccess) and
            (IPCTestReceived.MessageId = $4D4953544958) and
            (IPCTestReceived.WordCount = 2) and
            (IPCTestReceived.Words[0] = $50415343414C) and
            (IPCTestReceived.Words[1] = IPCTestExpected);

  if Result then
    WriteText('[ OK ] Blocking IPC send/receive rendezvous test passed.'#13#10)
  else
    WriteText('[FAIL] IPC rendezvous payload/status mismatch.'#13#10);

  EndpointClose(IPCTestEndpoint);
  Status := IPCReceive(IPCTestEndpoint, DeniedMessage);
  if Status <> krInvalidHandle then
  begin
    Result := False;
    WriteText('[FAIL] Closed endpoint capability was not revoked.'#13#10);
  end;
end;
