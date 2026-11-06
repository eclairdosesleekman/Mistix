{ Mistix Phase 2: coalescing asynchronous notification objects. }

const
  NOTIFICATION_MAX_COUNT = 16;
  WAIT_KEY_NOTIFICATION = $21;

type
  TNotification = record
    Active: Byte;
    Generation: Word;
    PendingBits: QWord;
  end;

var
  NotificationTable: array[0..NOTIFICATION_MAX_COUNT - 1] of TNotification;

function NotificationWaitKey(ObjectIndex, ObjectGeneration: Word): QWord;
begin
  Result := (QWord(2) shl 60) or (QWord(WAIT_KEY_NOTIFICATION) shl 52) or
            (QWord(ObjectGeneration) shl 16) or QWord(ObjectIndex + 1);
end;

procedure NotificationInitialize;
var
  I: LongWord;
begin
  for I := 0 to NOTIFICATION_MAX_COUNT - 1 do
  begin
    NotificationTable[I].Active := 0;
    NotificationTable[I].Generation := 0;
    NotificationTable[I].PendingBits := 0;
  end;
  WriteText('[ OK ] Notification registry initialised (' );
  WriteDecimal(NOTIFICATION_MAX_COUNT);
  WriteText(' objects).'#13#10);
end;

function NotificationResolve(Handle: LongWord; RequiredRights: LongWord;
  out ObjectIndex: Word; out ObjectGeneration: Word): TKernelResult;
begin
  Result := CapabilityLookup(Handle, RequiredRights, okNotification,
                             ObjectIndex, ObjectGeneration);
  if Result <> krSuccess then
    Exit;
  if ObjectIndex >= NOTIFICATION_MAX_COUNT then
  begin
    Result := krInvalidHandle;
    Exit;
  end;
  if (NotificationTable[ObjectIndex].Active = 0) or
     (NotificationTable[ObjectIndex].Generation <> ObjectGeneration) then
    Result := krObjectClosed;
end;

function NotificationCreate(out Handle: LongWord): TKernelResult;
var
  I: LongWord;
  NewGeneration: Word;
begin
  Handle := 0;
  for I := 0 to NOTIFICATION_MAX_COUNT - 1 do
  begin
    if (NotificationTable[I].Active <> 0) or
       (NotificationTable[I].Generation = High(Word)) then
      Continue;
    NewGeneration := NotificationTable[I].Generation + 1;
    NotificationTable[I].Generation := NewGeneration;
    NotificationTable[I].PendingBits := 0;
    NotificationTable[I].Active := 1;
    Result := CapabilityInstall(okNotification, Word(I), NewGeneration,
      CAP_RIGHT_SIGNAL or CAP_RIGHT_WAIT or CAP_RIGHT_MANAGE, Handle);
    if Result <> krSuccess then
      NotificationTable[I].Active := 0;
    Exit;
  end;
  Result := krNoResources;
end;

function NotificationSignal(Handle: LongWord; Bits: QWord): TKernelResult;
var
  ObjectIndex: Word;
  ObjectGeneration: Word;
begin
  if Bits = 0 then
  begin
    Result := krInvalidArgument;
    Exit;
  end;
  Result := NotificationResolve(Handle, CAP_RIGHT_SIGNAL,
                                ObjectIndex, ObjectGeneration);
  if Result <> krSuccess then
    Exit;

  NotificationTable[ObjectIndex].PendingBits :=
    NotificationTable[ObjectIndex].PendingBits or Bits;
  ThreadWakeWaiters(NotificationWaitKey(ObjectIndex, ObjectGeneration), True);
  Result := krSuccess;
end;

function NotificationWait(Handle: LongWord; Mask: QWord;
  out ObservedBits: QWord): TKernelResult;
var
  ObjectIndex: Word;
  ObjectGeneration: Word;
  Pending: QWord;
begin
  ObservedBits := 0;
  if Mask = 0 then
  begin
    Result := krInvalidArgument;
    Exit;
  end;

  while True do
  begin
    Result := NotificationResolve(Handle, CAP_RIGHT_WAIT,
                                  ObjectIndex, ObjectGeneration);
    if Result <> krSuccess then
      Exit;
    Pending := NotificationTable[ObjectIndex].PendingBits and Mask;
    if Pending <> 0 then
      Break;
    ThreadBlockCurrent(NotificationWaitKey(ObjectIndex, ObjectGeneration));
  end;

  ObservedBits := Pending;
  NotificationTable[ObjectIndex].PendingBits :=
    NotificationTable[ObjectIndex].PendingBits and not Pending;
  Result := krSuccess;
end;

function NotificationDestroy(Handle: LongWord): TKernelResult;
var
  ObjectIndex: Word;
  ObjectGeneration: Word;
begin
  Result := NotificationResolve(Handle, CAP_RIGHT_MANAGE,
                                ObjectIndex, ObjectGeneration);
  if Result <> krSuccess then
    Exit;
  NotificationTable[ObjectIndex].Active := 0;
  NotificationTable[ObjectIndex].PendingBits := 0;
  ThreadWakeWaiters(NotificationWaitKey(ObjectIndex, ObjectGeneration), True);
  CapabilityRevokeObject(okNotification, ObjectIndex, ObjectGeneration);
  Result := krSuccess;
end;

{ Integration test: waiter blocks until signaler posts the requested bit. }
var
  NotificationTestHandle: LongWord;
  NotificationTestObserved: QWord;
  NotificationTestStatus: TKernelResult;

procedure NotificationTestWaiter;
begin
  NotificationTestStatus := NotificationWait(NotificationTestHandle,
                                              $0000000000000004,
                                              NotificationTestObserved);
  if NotificationTestStatus = krSuccess then
    WriteText('[NOTIFY] waiter resumed after signal.'#13#10)
  else
    WriteText('[NOTIFY] waiter returned an error.'#13#10);
end;

procedure NotificationTestSignaler;
var
  Status: TKernelResult;
begin
  Status := NotificationSignal(NotificationTestHandle,
                               $0000000000000004);
  if Status = krSuccess then
    WriteText('[NOTIFY] signal bit posted.'#13#10)
  else
    WriteText('[NOTIFY] signal failed.'#13#10);
end;

function NotificationSelfTest: Boolean;
var
  WaiterId: LongWord;
  SignalerId: LongWord;
begin
  Result := False;
  NotificationTestObserved := 0;
  NotificationTestStatus := krInvalidArgument;
  Result := NotificationCreate(NotificationTestHandle) = krSuccess;
  if not Result then
  begin
    WriteText('[FAIL] Could not create notification test object.'#13#10);
    Exit;
  end;

  if not ThreadCreate(@NotificationTestWaiter, WaiterId) then
  begin
    NotificationDestroy(NotificationTestHandle);
    WriteText('[FAIL] Could not create notification waiter.'#13#10);
    Result := False;
    Exit;
  end;
  if not ThreadCreate(@NotificationTestSignaler, SignalerId) then
  begin
    NotificationDestroy(NotificationTestHandle);
    WriteText('[FAIL] Could not create notification signaler.'#13#10);
    Result := False;
    Exit;
  end;

  ThreadsRunUntilIdle;
  Result := (NotificationTestStatus = krSuccess) and
            (NotificationTestObserved = $0000000000000004);
  if Result then
    WriteText('[ OK ] Notification wait/signal test passed.'#13#10)
  else
    WriteText('[FAIL] Notification result mismatch.'#13#10);
  NotificationDestroy(NotificationTestHandle);
end;
