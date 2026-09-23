; Signature-agnostic zone hook stub (x64 MASM).
;
; A per-hook runtime thunk loads its ZoneHookContext* into r11 and jumps here.
; The stub calls the original function with the caller's register and stack
; arguments reproduced exactly, and brackets that call with
; SET_ZoneHookEnter/SET_ZoneHookExit. Unlike return-address hijacking, the stub
; is a real frame with unwind data, so C++/SEH unwinding and stack walks
; (including our sampler's) pass through it normally. If an exception unwinds
; through the frame, SET_ZoneHookUnwind closes the zone.
;
; Preserved for the callee: rcx, rdx, r8, r9, xmm0-xmm5 (covers __fastcall and
; __vectorcall), and up to kStackArgSlots stack arguments. Returned to the
; caller: rax, rdx, xmm0-xmm3. Non-volatile registers are never touched.
; Limits: functions with more than 16 stack arguments (20 total), and callees
; that read their own return address, are not supported.
;
; Frame layout (offsets from rsp after the prologue; must match
; GenericZoneHooks.cpp):
;   000h  home space for the calls we make
;   020h  copy of the caller's stack arguments (16 slots)
;   0A0h  rcx / return rax
;   0A8h  rdx / return rdx
;   0B0h  r8
;   0B8h  r9
;   0C0h  ZoneHookContext*            (kContextOffset)
;   0C8h  state: 1 while the zone is open (kStateOffset)
;   0D0h  xmm0 .. 120h xmm5
;   130h  padding (keeps rsp 16-byte aligned)
;   138h  return address; the caller's stack arguments start at 160h

EXTERN SET_ZoneHookEnter:PROC
EXTERN SET_ZoneHookExit:PROC
EXTERN SET_ZoneHookUnwind:PROC

kFrameSize      EQU 138h
kArgCopy        EQU 20h
kStackArgSlots  EQU 16
kSaveRcx        EQU 0A0h
kSaveRdx        EQU 0A8h
kSaveR8         EQU 0B0h
kSaveR9         EQU 0B8h
kContext        EQU 0C0h
kState          EQU 0C8h
kSaveXmm0       EQU 0D0h
kCallerArgs     EQU kFrameSize + 8 + 20h

.code

SET_ZoneHookEntry PROC FRAME:SET_ZoneHookUnwind
	sub rsp, kFrameSize
	.allocstack kFrameSize
	.endprolog

	mov [rsp + kSaveRcx], rcx
	mov [rsp + kSaveRdx], rdx
	mov [rsp + kSaveR8], r8
	mov [rsp + kSaveR9], r9
	mov [rsp + kContext], r11
	mov qword ptr [rsp + kState], 0
	movups [rsp + kSaveXmm0], xmm0
	movups [rsp + kSaveXmm0 + 10h], xmm1
	movups [rsp + kSaveXmm0 + 20h], xmm2
	movups [rsp + kSaveXmm0 + 30h], xmm3
	movups [rsp + kSaveXmm0 + 40h], xmm4
	movups [rsp + kSaveXmm0 + 50h], xmm5

	; Copy min(kStackArgSlots, slots left before the stack base) qwords of the
	; caller's stack arguments, so the callee finds them at the usual offsets.
	lea r10, [rsp + kCallerArgs]
	mov rax, gs:[8]                         ; NT_TIB.StackBase
	sub rax, r10
	shr rax, 3
	mov ecx, kStackArgSlots
	cmp rax, rcx
	cmova rax, rcx
	xor ecx, ecx
copy_args:
	cmp rcx, rax
	jae copy_done
	mov rdx, [r10 + rcx * 8]
	mov [rsp + kArgCopy + rcx * 8], rdx
	inc rcx
	jmp copy_args
copy_done:

	mov rcx, [rsp + kContext]
	call SET_ZoneHookEnter
	mov qword ptr [rsp + kState], 1

	mov rcx, [rsp + kSaveRcx]
	mov rdx, [rsp + kSaveRdx]
	mov r8, [rsp + kSaveR8]
	mov r9, [rsp + kSaveR9]
	movups xmm0, [rsp + kSaveXmm0]
	movups xmm1, [rsp + kSaveXmm0 + 10h]
	movups xmm2, [rsp + kSaveXmm0 + 20h]
	movups xmm3, [rsp + kSaveXmm0 + 30h]
	movups xmm4, [rsp + kSaveXmm0 + 40h]
	movups xmm5, [rsp + kSaveXmm0 + 50h]
	mov rax, [rsp + kContext]
	call qword ptr [rax]                    ; ZoneHookContext::original

	mov [rsp + kSaveRcx], rax
	mov [rsp + kSaveRdx], rdx
	movups [rsp + kSaveXmm0], xmm0
	movups [rsp + kSaveXmm0 + 10h], xmm1
	movups [rsp + kSaveXmm0 + 20h], xmm2
	movups [rsp + kSaveXmm0 + 30h], xmm3

	mov qword ptr [rsp + kState], 0
	mov rcx, [rsp + kContext]
	call SET_ZoneHookExit

	mov rax, [rsp + kSaveRcx]
	mov rdx, [rsp + kSaveRdx]
	movups xmm0, [rsp + kSaveXmm0]
	movups xmm1, [rsp + kSaveXmm0 + 10h]
	movups xmm2, [rsp + kSaveXmm0 + 20h]
	movups xmm3, [rsp + kSaveXmm0 + 30h]

	add rsp, kFrameSize
	ret
SET_ZoneHookEntry ENDP

END
