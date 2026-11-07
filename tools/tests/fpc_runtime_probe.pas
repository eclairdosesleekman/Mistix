unit FPC_Runtime_Probe;

{$mode objfpc}
{$H-}
{$ifdef MISTIX_DEBUG_CHECKS}
{$R+}
{$Q+}
{$else}
{$R-}
{$Q-}
{$endif}
{$S-}
{$I-}

interface

procedure ProbeRange(Index: LongInt);
procedure ProbeOverflow(Left, Right: Int64);
procedure ProbeDivideByZero(Numerator, Denominator: LongInt);

implementation

type
  TProbeIndex = 0..3;

var
  ProbeTable: array[TProbeIndex] of Byte;
  ProbeArithmeticResult: Int64;
  ProbeDivisionResult: LongInt;

procedure ProbeRange(Index: LongInt);
begin
  ProbeTable[Index] := Byte(Index);
end;

procedure ProbeOverflow(Left, Right: Int64);
begin
  ProbeArithmeticResult := Left + Right;
end;

procedure ProbeDivideByZero(Numerator, Denominator: LongInt);
begin
  ProbeDivisionResult := Numerator div Denominator;
end;

end.
