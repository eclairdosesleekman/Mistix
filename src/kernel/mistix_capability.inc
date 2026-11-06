{ Mistix Phase 2: kernel capability namespace.
  Phase 2 has kernel threads but no user processes yet, so this is one
  bootstrap capability space. Phase 3 will attach a table to each process. }

const
  CAPABILITY_MAX_SLOTS = 64;

  CAP_RIGHT_SEND       = $00000001;
  CAP_RIGHT_RECEIVE    = $00000002;
  CAP_RIGHT_SIGNAL     = $00000004;
  CAP_RIGHT_WAIT       = $00000008;
  CAP_RIGHT_MAP        = $00000010;
  CAP_RIGHT_READ       = $00000020;
  CAP_RIGHT_WRITE      = $00000040;
  CAP_RIGHT_MANAGE     = $00000080;
  CAP_RIGHT_USER_MAP   = $00000100;
  CAP_RIGHT_EXECUTE    = $00000200;

type
  TObjectKind = (okNone, okEndpoint, okNotification, okMemoryObject);

  TKernelResult = (
    krSuccess,
    krInvalidHandle,
    krPermissionDenied,
    krNoResources,
    krInvalidArgument,
    krObjectClosed,
    krWouldBlock,
    krBusy
  );

  TCapabilityEntry = packed record
    Active: Byte;
    ObjectKind: Byte;
    ObjectIndex: Word;
    Generation: Word;
    ObjectGeneration: Word;
    Rights: LongWord;
  end;

var
  CapabilityTable: array[0..CAPABILITY_MAX_SLOTS - 1] of TCapabilityEntry;

procedure CapabilityInitialize;
var
  I: LongWord;
begin
  for I := 0 to CAPABILITY_MAX_SLOTS - 1 do
  begin
    CapabilityTable[I].Active := 0;
    CapabilityTable[I].ObjectKind := Ord(okNone);
    CapabilityTable[I].ObjectIndex := 0;
    CapabilityTable[I].Generation := 0;
    CapabilityTable[I].ObjectGeneration := 0;
    CapabilityTable[I].Rights := 0;
  end;
  WriteText('[ OK ] Bootstrap capability table initialised (' );
  WriteDecimal(CAPABILITY_MAX_SLOTS);
  WriteText(' slots).'#13#10);
end;

function CapabilityInstall(Kind: TObjectKind; ObjectIndex,
  ObjectGeneration: Word; Rights: LongWord; out Handle: LongWord): TKernelResult;
var
  I: LongWord;
  NewGeneration: Word;
begin
  Handle := 0;
  if (Kind = okNone) or (Rights = 0) then
  begin
    Result := krInvalidArgument;
    Exit;
  end;

  for I := 0 to CAPABILITY_MAX_SLOTS - 1 do
  begin
    if CapabilityTable[I].Active <> 0 then
      Continue;

    { Retire a slot rather than wrapping its generation and reviving a stale
      handle (ABA protection). }
    if CapabilityTable[I].Generation = High(Word) then
      Continue;
    NewGeneration := CapabilityTable[I].Generation + 1;

    CapabilityTable[I].Active := 1;
    CapabilityTable[I].ObjectKind := Ord(Kind);
    CapabilityTable[I].ObjectIndex := ObjectIndex;
    CapabilityTable[I].Generation := NewGeneration;
    CapabilityTable[I].ObjectGeneration := ObjectGeneration;
    CapabilityTable[I].Rights := Rights;
    Handle := (LongWord(NewGeneration) shl 16) or (I + 1);
    Result := krSuccess;
    Exit;
  end;

  Result := krNoResources;
end;

function CapabilityLookup(Handle: LongWord; RequiredRights: LongWord;
  ExpectedKind: TObjectKind; out ObjectIndex,
  ObjectGeneration: Word): TKernelResult;
var
  EncodedSlot: LongWord;
  Slot: LongWord;
  Generation: Word;
  Entry: ^TCapabilityEntry;
begin
  ObjectIndex := 0;
  ObjectGeneration := 0;
  EncodedSlot := Handle and $FFFF;
  Generation := Word(Handle shr 16);

  if (EncodedSlot = 0) or (EncodedSlot > CAPABILITY_MAX_SLOTS) or
     (Generation = 0) then
  begin
    Result := krInvalidHandle;
    Exit;
  end;

  Slot := EncodedSlot - 1;
  Entry := @CapabilityTable[Slot];
  if (Entry^.Active = 0) or (Entry^.Generation <> Generation) then
  begin
    Result := krInvalidHandle;
    Exit;
  end;
  if Entry^.ObjectKind <> Ord(ExpectedKind) then
  begin
    Result := krInvalidHandle;
    Exit;
  end;
  if (Entry^.Rights and RequiredRights) <> RequiredRights then
  begin
    Result := krPermissionDenied;
    Exit;
  end;

  ObjectIndex := Entry^.ObjectIndex;
  ObjectGeneration := Entry^.ObjectGeneration;
  Result := krSuccess;
end;

function CapabilityRestrict(Handle: LongWord; NewRights: LongWord;
  out RestrictedHandle: LongWord): TKernelResult;
var
  EncodedSlot: LongWord;
  Slot: LongWord;
  Generation: Word;
  Entry: ^TCapabilityEntry;
begin
  RestrictedHandle := 0;
  EncodedSlot := Handle and $FFFF;
  Generation := Word(Handle shr 16);
  if (EncodedSlot = 0) or (EncodedSlot > CAPABILITY_MAX_SLOTS) or
     (Generation = 0) then
  begin
    Result := krInvalidHandle;
    Exit;
  end;

  Slot := EncodedSlot - 1;
  Entry := @CapabilityTable[Slot];
  if (Entry^.Active = 0) or (Entry^.Generation <> Generation) then
  begin
    Result := krInvalidHandle;
    Exit;
  end;
  if (NewRights = 0) or ((NewRights and not Entry^.Rights) <> 0) then
  begin
    Result := krPermissionDenied;
    Exit;
  end;

  Result := CapabilityInstall(TObjectKind(Entry^.ObjectKind),
    Entry^.ObjectIndex, Entry^.ObjectGeneration, NewRights, RestrictedHandle);
end;

function CapabilityClose(Handle: LongWord): TKernelResult;
var
  EncodedSlot: LongWord;
  Slot: LongWord;
  Generation: Word;
begin
  EncodedSlot := Handle and $FFFF;
  Generation := Word(Handle shr 16);
  if (EncodedSlot = 0) or (EncodedSlot > CAPABILITY_MAX_SLOTS) or
     (Generation = 0) then
  begin
    Result := krInvalidHandle;
    Exit;
  end;

  Slot := EncodedSlot - 1;
  if (CapabilityTable[Slot].Active = 0) or
     (CapabilityTable[Slot].Generation <> Generation) then
  begin
    Result := krInvalidHandle;
    Exit;
  end;

  CapabilityTable[Slot].Active := 0;
  CapabilityTable[Slot].Rights := 0;
  Result := krSuccess;
end;

procedure CapabilityRevokeObject(Kind: TObjectKind; ObjectIndex,
  ObjectGeneration: Word);
var
  I: LongWord;
begin
  for I := 0 to CAPABILITY_MAX_SLOTS - 1 do
    if (CapabilityTable[I].Active <> 0) and
       (CapabilityTable[I].ObjectKind = Ord(Kind)) and
       (CapabilityTable[I].ObjectIndex = ObjectIndex) and
       (CapabilityTable[I].ObjectGeneration = ObjectGeneration) then
    begin
      CapabilityTable[I].Active := 0;
      CapabilityTable[I].Rights := 0;
    end;
end;
