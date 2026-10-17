;
; midi_delay.asm - calibrated busy-wait for the z80neo MIDI player
;
;   void midi_delay_ms(uint16_t ms);
;
; Waits approximately `ms` milliseconds.  The firmware now clocks the Z80 at
; 576 kHz (CPU_SPEED), and every memory access still pays the Pico's bus
; service (~9.6 us: the 3/3/3 us settles plus the PIO round trip) which does
; NOT scale with the clock and now dominates the loop.  One outer iteration,
; with N inner trips, is:
;
;     push bc            11 T   (1 access)
;     ld de,N            10 T   (2 accesses)
;   inner: dec de                6 T   (1)
;          ld a,d               4 T   (1)
;          or e                 4 T   (1)
;          jr nz,inner         12 T taken / 7 T last   (2)
;     pop bc             10 T   (1)
;     dec bc              6 T   (1)
;     ld a,b               4 T   (1)
;     or c                 4 T   (1)
;     jr nz,outer         12 T   (2 accesses)
;
;   T-states = 26*N - 5 + 57 = 26*N + 52
;   accesses = 5*N + 10
;   t(N)     = (26*N + 52)/576000 + (5*N + 10)*9.6 us
;
;   N=9  -> 1.025 ms per outer iteration at 576 kHz.
;   (At the old 128 kHz, N=2 gave ~1.005 ms.)
;
; Measured on the board at 128 kHz: midi_delay_ms(2000) took 2.51 s with the
; old de=3.  Retune when CPU_SPEED or the bus settle times in z80bus_pio.c
; change; the bus service term dominates above a few hundred kHz.
;

    SECTION code_user

    PUBLIC  _midi_delay_ms
    PUBLIC  _midi_play
    PUBLIC  _midi_stopped

; Z80-native dashboard (midi_ui.c): the record buffer, its length, the delay
; and the per-record hook the replay loop feeds.  Only linked when the caller
; assembles with -DMIDI_UI (the CP/M player; the native player omits it).
  IFDEF   MIDI_UI
    EXTERN  _midi_ui_buf
    EXTERN  _midi_ui_n
    EXTERN  _midi_ui_ms
    EXTERN  _midi_ui_record
  ENDIF

; ---------------------------------------------------------------------------
; void midi_play(void);
;
; Streams grouped records from the firmware MIDI API (port 0xA2) and plays
; them.  Each record is [delay lo, delay hi, byte count] followed by
; `byte count` raw MIDI bytes (0 = delay only, 0xFF = end of song).  Waits the
; delay once, then blits the whole tick's bytes from a tight copy loop, so the
; per-record delay/console overhead is paid once per tick instead of once per
; event.  Returns on the 0xFF end sentinel or when 'q'/ESC is typed.
;
; With -DMIDI_UI the record's raw bytes are also copied into midi_ui_buf[] and
; the delay into midi_ui_ms, then midi_ui_record() is called once per record:
; the Z80-native dashboard (midi_ui.c, used by MIDI.COM) parses them and
; redraws.  The hook is once per record, not per byte, so it stays a small,
; bounded part of the wait.  Without the define the hook is left out of the
; loop entirely (the native player draws nothing).
; ---------------------------------------------------------------------------
_midi_play:
    xor     a
    ld      (_midi_stopped), a      ; clear: 0 = played to the end
mlp_loop:
    in      a, ($A2)        ; delay lo
    ld      c, a
    in      a, ($A2)        ; delay hi
    ld      b, a
    in      a, ($A2)        ; byte count (0xFF = end of song)
    ld      e, a
    cp      $FF
    ret     z               ; end of song

  IFDEF   MIDI_UI
    ld      (_midi_ui_ms), bc   ; dashboard clock: delay about to be waited
    ld      a, e
    ld      (_midi_ui_n), a     ; dashboard: raw byte count for this record
  ENDIF

    ld      a, b
    or      c
    jr      z, mlp_send     ; no wait
    push    de
    push    bc              ; wait ms (also the call argument)
    call    _midi_delay_ms
    pop     bc
    pop     de

mlp_send:
    ld      a, e
    or      a
    jr      z, mlp_check    ; delay-only record: nothing to emit
  IFDEF   MIDI_UI
    ld      hl, _midi_ui_buf    ; collect the record's bytes for the UI
  ENDIF

mlp_tx:
    in      a, ($91)        ; one TX-ready check per record
    and     $02
    jr      z, mlp_tx

mlp_byte:
    in      a, ($A2)        ; raw MIDI byte
    out     ($90), a
  IFDEF   MIDI_UI
    ld      (hl), a
    inc     hl
  ENDIF
    dec     e
    jr      nz, mlp_byte

mlp_check:
    in      a, ($81)
    and     $01
    jr      z, mlp_ui
    in      a, ($80)
    cp      'q'
    jr      z, mlp_stop
    cp      'Q'
    jr      z, mlp_stop
    cp      27
    jr      z, mlp_stop
    cp      'n'
    jr      z, mlp_next
    cp      'N'
    jr      z, mlp_next

mlp_ui:
  IFDEF   MIDI_UI
    call    _midi_ui_record
  ENDIF
    jr      mlp_loop

; ---------------------------------------------------------------------------
; Manual stop: send a MIDI panic so the synth does not leave notes hanging.
; All Sound Off (CC120), All Notes Off (CC123) and Reset All Controllers
; (CC121), value 0, on every one of the 16 channels.
; ---------------------------------------------------------------------------
mlp_stop:
    call    mlp_panic
    ld      a, 1
    ld      (_midi_stopped), a      ; 1 = stopped by the user
    ret

; Skip to the next file: same panic, but flag 2 so a play-all run continues.
mlp_next:
    call    mlp_panic
    ld      a, 2
    ld      (_midi_stopped), a      ; 2 = next track
    ret

mlp_panic:
    ld      c, $B0                  ; control change, channel 1
mlp_pan_next:
    ld      b, 3                    ; three controllers
    ld      hl, mlp_pan_ctrl
mlp_pan_cc:
    ld      a, c
    call    mlp_tx_byte
    ld      a, (hl)
    call    mlp_tx_byte
    xor     a
    call    mlp_tx_byte
    inc     hl
    djnz    mlp_pan_cc
    inc     c
    ld      a, c
    cp      $C0                     ; past channel 16?
    jr      nz, mlp_pan_next
    ret

mlp_pan_ctrl:
    db      120, 123, 121

; Send one byte, waiting for UART1 TX-ready first (per-byte flow control).
; Preserves A (the data byte), B, C and HL.
mlp_tx_byte:
    push    af
mlp_tx_wait:
    in      a, ($91)
    and     $02
    jr      z, mlp_tx_wait
    pop     af
    out     ($90), a
    ret

_midi_delay_ms:
    ld      hl, 2
    add     hl, sp
    ld      c, (hl)
    inc     hl
    ld      b, (hl)         ; bc = ms
    ld      a, b
    or      c
    ret     z

outer:
    push    bc
    ld      de, 9
inner:
    dec     de
    ld      a, d
    or      e
    jr      nz, inner
    pop     bc
    dec     bc
    ld      a, b
    or      c
    jr      nz, outer
    ret

    SECTION bss_user
; 0 = midi_play reached the end-of-song sentinel, 1 = stopped by 'q'/ESC,
; 2 = skipped with 'n'/N.
_midi_stopped:
    defs    1
