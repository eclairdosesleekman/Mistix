{
  Mistix-owned FPC runtime-error entry points. This code does not call the
  hosted RTL: it writes directly to COM1 and stops the processor.
}
procedure MistixRuntimePanic(Code: LongWord); cdecl; assembler; nostackframe;
asm
  cli
  cmp edi, 200
  je @division_by_zero
  cmp edi, 201
  je @range_error

  mov al, 'F'
  call @putc
  mov al, 'P'
  call @putc
  mov al, 'C'
  call @putc
  mov al, ' '
  call @putc
  mov al, 'O'
  call @putc
  mov al, 'V'
  call @putc
  mov al, 'E'
  call @putc
  mov al, 'R'
  call @putc
  mov al, 'F'
  call @putc
  mov al, 'L'
  call @putc
  mov al, 'O'
  call @putc
  mov al, 'W'
  call @putc
  jmp @line_end

@range_error:
  mov al, 'F'
  call @putc
  mov al, 'P'
  call @putc
  mov al, 'C'
  call @putc
  mov al, ' '
  call @putc
  mov al, 'R'
  call @putc
  mov al, 'A'
  call @putc
  mov al, 'N'
  call @putc
  mov al, 'G'
  call @putc
  mov al, 'E'
  call @putc
  mov al, ' '
  call @putc
  mov al, 'C'
  call @putc
  mov al, 'H'
  call @putc
  mov al, 'E'
  call @putc
  mov al, 'C'
  call @putc
  mov al, 'K'
  call @putc
  jmp @line_end

@division_by_zero:
  mov al, 'F'
  call @putc
  mov al, 'P'
  call @putc
  mov al, 'C'
  call @putc
  mov al, ' '
  call @putc
  mov al, 'D'
  call @putc
  mov al, 'I'
  call @putc
  mov al, 'V'
  call @putc
  mov al, 'I'
  call @putc
  mov al, 'D'
  call @putc
  mov al, 'E'
  call @putc
  mov al, ' '
  call @putc
  mov al, 'B'
  call @putc
  mov al, 'Y'
  call @putc
  mov al, ' '
  call @putc
  mov al, 'Z'
  call @putc
  mov al, 'E'
  call @putc
  mov al, 'R'
  call @putc
  mov al, 'O'
  call @putc

@line_end:
  mov al, 13
  call @putc
  mov al, 10
  call @putc
  jmp @halt

@putc:
  mov ah, al
  mov dx, $3FD
  mov ecx, 100000
@wait_for_uart:
  in al, dx
  test al, $20
  jnz @write_uart
  loop @wait_for_uart
  ret
@write_uart:
  mov dx, $3F8
  mov al, ah
  out dx, al
  ret

@halt:
  cli
@halt_forever:
  hlt
  jmp @halt_forever
end;

procedure MistixFPCRangeError; cdecl; public name 'fpc_rangeerror';
begin
  MistixRuntimePanic(201);
end;

{ Keep the documented uppercase spelling as a compatibility alias. }
procedure MistixFPCRangeErrorCompat; cdecl; public name 'FPC_RANGEERROR';
begin
  MistixRuntimePanic(201);
end;

procedure MistixFPCOverflow; cdecl; public name 'fpc_overflow';
begin
  MistixRuntimePanic(215);
end;

{ Preserve the uppercase runtime ABI spelling for compatibility. }
procedure MistixFPCOverflowCompat; cdecl; public name 'FPC_OVERFLOW';
begin
  MistixRuntimePanic(215);
end;

procedure MistixFPCDivideByZero; cdecl; public name 'FPC_DIVBYZERO';
begin
  MistixRuntimePanic(200);
end;
