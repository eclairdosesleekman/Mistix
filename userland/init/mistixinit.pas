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
  { "Hello from Mistix ring 3!" followed by CR/LF. }
  HelloMessage: array[0..26] of Byte = (
    72, 101, 108, 108, 111, 32, 102, 114, 111, 109, 32,
    77, 105, 115, 116, 105, 120, 32, 114, 105, 110, 103,
    32, 51, 33, 13, 10
  );

function MistixWrite(FileDescriptor, BufferAddress,
  ByteCount: QWord): QWord; assembler; nostackframe;
asm
  mov eax, 1
  int $80
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

procedure MistixExit(ExitCode: QWord); assembler; nostackframe;
asm
  mov eax, 2
  int $80
  ud2
end;

procedure MistixUserMain; cdecl; public name 'mistix_user_main';
begin
  if MistixGetPid() = 1 then
    MistixWrite(1, PtrUInt(@HelloMessage[0]), SizeOf(HelloMessage));
  MistixYield();
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
