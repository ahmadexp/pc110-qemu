; PC110 Easy-Setup floppy loader with "exit -> boot hard disk (Personaware)"
; Boots from floppy (drive 0). Loads Easy-Setup image (LBA 1..) to 0x50000 via
; CHS, sets mode 12h, installs an exit stub at 0000:0600, and enters Easy-Setup
; with that stub as the return address. When Easy-Setup exits (retf), the stub
; chain-boots the hard disk (0x80) = Personaware, i.e. normal mode.
BITS 16
ORG 0x7C00

SECTORS   equ 454
LOAD_SEG  equ 0x5000
STUB_OFF  equ 0x0600        ; exit stub at 0000:0600

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    ; --- copy exit stub to 0000:0600 ---
    mov si, exit_stub
    mov di, STUB_OFF
    mov cx, exit_stub_end - exit_stub
    cld
    rep movsb

    ; --- point INT 19h (bootstrap) at the exit stub, so if Easy-Setup reboots
    ;     via INT 19h it chain-boots the hard disk instead of the floppy ---
    xor ax, ax
    mov es, ax
    mov word [es:0x19*4], STUB_OFF
    mov word [es:0x19*4+2], 0x0000
    push ds
    pop es                         ; restore es=ds=0 (unchanged, both 0)

    ; --- load Easy-Setup image, one sector at a time, via CHS ---
    mov word [cur_seg], LOAD_SEG
    mov word [lba], 1
    mov cx, SECTORS
.rd:
    push cx
    ; CHS from LBA (floppy 1.44M: 18 spt, 2 heads)
    mov ax, [lba]
    xor dx, dx
    mov bx, 36                 ; 18*2 sectors per cylinder
    div bx                     ; ax=cyl, dx=rem
    mov [chs_cyl], al
    mov ax, dx
    xor dx, dx
    mov bx, 18
    div bx                     ; ax=head, dx=sector-1
    mov [chs_head], al
    inc dx
    mov [chs_sec], dl
    ; read 1 sector to cur_seg:0000
    mov ax, [cur_seg]
    mov es, ax
    xor bx, bx
    mov ah, 0x02
    mov al, 0x01
    mov ch, [chs_cyl]
    mov cl, [chs_sec]
    mov dh, [chs_head]
    xor dl, dl                 ; floppy A
    int 0x13
    jc .err
    mov ax, [cur_seg]
    add ax, 0x20               ; +512 bytes
    mov [cur_seg], ax
    inc word [lba]
    pop cx
    loop .rd

    ; --- VGA mode 12h ---
    mov ax, 0x0012
    int 0x10

    ; --- enter Easy-Setup: DS=ES=0, SS:SP stack, return = exit stub ---
    cli
    mov ax, 0x9000
    mov ss, ax
    mov sp, 0xFFFE
    xor ax, ax
    mov ds, ax
    mov es, ax
    push word 0x0000           ; return CS
    push word STUB_OFF         ; return IP -> 0000:0600
    sti
    jmp LOAD_SEG:0x0000

.err:
    mov si, errmsg
.pr:
    lodsb
    or al, al
    jz .h
    mov ah, 0x0E
    int 0x10
    jmp .pr
.h: hlt
    jmp .h

; ---- exit stub: runs at 0000:0600 when Easy-Setup returns ----
exit_stub:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    ; read hard-disk MBR (0x80, CHS 0,0,1) to 0000:7C00
    mov ah, 0x02
    mov al, 0x01
    mov ch, 0x00
    mov cl, 0x01
    mov dh, 0x00
    mov dl, 0x80
    mov bx, 0x7C00
    int 0x13
    jc .sh
    mov dl, 0x80
    jmp 0x0000:0x7C00
.sh:
    hlt
    jmp .sh
exit_stub_end:

errmsg db "FLOPPY ERR", 0
chs_cyl  db 0
chs_head db 0
chs_sec  db 0
cur_seg  dw 0
lba      dw 0

times 510-($-$$) db 0
dw 0xAA55
