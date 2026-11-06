# Mistix — Phase 4

Mistix Phase 4 は Phase 3 の Pascal 製 x86-64 カーネルを拡張し、複数の Ring 3 プロセスを単一 CPU 上で実行するためのプロセス実行基盤を追加します。今回の焦点は、PIT によるタイマー割り込み、割り込みフレームを使ったプロセス切り替え、`fork` の Copy-on-Write、`spawn` / `waitpid`、プロセスごとの Capability Space、ユーザー空間 Endpoint IPC です。

カーネルとユーザーランドは Free Pascal の freestanding unit として実装します。C ソースや C ランタイムには依存しません。CPU 固有の命令（割り込み入口、CR3、IRETQ、I/O port、レジスタ保存など）は Pascal ソース内の x86-64 inline assembly に限定します。文字どおりアセンブリ命令も禁止する要件ではありません。

## Phase 4 の機能

### 1. プリエンプティブな単一 CPU スケジューラ

- 8259 PIC を vector `0x20` / `0x28` に再配置し、IRQ0 のみを有効化。
- PIT channel 0 を 100 Hz に設定。
- Ring 3 から戻ったタイマー割り込みで、3 tick ごとに Round-Robin プロセス切り替え。
- `int 0x80` の `yield()` でも即時切り替え。
- 各プロセスに 32 KiB の専用カーネルスタック。
- TSS.RSP0 と CR3 をプロセス切り替え時に更新。
- 共通 ISR は Pascal trap dispatcher が返す interrupt-frame pointer を復元するため、別プロセスのカーネルスタックへ切り替えてから `iretq` できる。

### 2. プロセスのライフサイクル

| 操作 | syscall | 内容 |
|---|---:|---|
| `write(fd, buf, len)` | 1 | fd 1/2 を COM1 へ出力。最大4096 byte |
| `exit(status)` | 2 | 現在のプロセスを終了 |
| `getpid()` | 3 | PID を取得 |
| `yield()` | 4 | スケジューラへ明け渡す。戻り値は0 |
| `fork()` | 5 | COW でアドレス空間を複製 |
| `spawn(image, size)` | 6 | ユーザー空間の静的 ELF64 image を新規プロセスにロード |
| `waitpid(pid, status)` | 7 | 子プロセスを待つ。pid=0 は任意の子 |
| `endpoint_create()` | 8 | 現在のプロセスに Endpoint Capability を作成 |
| `ipc_send(cap, msg)` | 9 | Endpoint に固定長メッセージを送信 |
| `ipc_receive(cap, msg)` | 10 | Endpoint から固定長メッセージを受信 |
| `cap_transfer(pid, cap, rights)` | 11 | 親から子へ Endpoint 権限を縮小して移譲 |
| `cap_receive()` | 12 | 移譲済み Capability handle を取得 |
| `cap_close(cap)` | 13 | 現在の Capability Space から handle を閉じる |
| `kill(pid, sig)` | 14 | 自分または直接の子を終了させる |

syscall ABI は RAX=番号／戻り値、RDI/RSI/RDX=引数です。プロセス状態は `Free / Ready / Running / Waiting / Zombie` で管理します。親が `waitpid` している場合、子の終了時に親を Ready に戻し、終了ステータスを書き込んでから子のアドレス空間を回収します。

### 3. Copy-on-Write fork

- 子用 PML4 を作成し、カーネル上位 half を共有。
- ユーザーの下位 half の PTE を走査して子へ複製。
- writable page は親子とも読み取り専用 + `PTE_COW` に変更。
- 書き込みページフォルトで新しい物理ページを確保し、ページ内容をコピー。
- 参照数が1になった COW page はコピーせず writable に戻す。
- アドレス空間破棄時に共有参照数を減らし、最後の参照で物理ページを解放。

COW 参照テーブルは固定8192スロットです。対応ページ数を超えた fork は失敗します。現状は4 KiB user leaf mapping のみを COW 複製対象とし、ユーザー空間に huge page を作りません。

### 4. プロセス Capability Space と IPC

- 最大8プロセス、各プロセスに64スロットの Capability Table。
- 世代番号付き handle。無効化された古い handle は再利用後も失効。
- fork は親の Capability Table を複製。
- spawn は空の Capability Space から開始。
- Endpoint は最大16個、FIFO キュー深さ8、メッセージは64 byte。
- Capability Transfer は親から直接の子に限り、SEND / RECEIVE の縮小した権限だけを移譲。
- 子は `cap_receive()` で新しい handle を取り出す。
- IPC send/receive は非ブロッキングで、キューが空または満杯の場合は `-EAGAIN`。ユーザー側で `yield()` して再試行。

### 5. 同梱の Pascal ユーザープログラム

`userland/init/mistixinit.pas` は次の確認を行います。

1. `fork()` し、親子が同じ writable global `ForkMarker` に異なる値を書き込み、実際の COW page fault とメモリ分離を検査。子側は exit status 11 で終了。
2. 親側は marker が親の値を保持していることを確認し、`waitpid()` で子を回収。
3. Endpoint を作成し、`spawn()` で同梱 Worker ELF を起動。
4. 親から子へ SEND/RECEIVE Capability を移譲。
5. Worker は Capability が届くまで EAGAIN + yield で待ち、メッセージを親へ返す。
6. 親は IPC 応答を検証し、`waitpid()` で Worker の exit status 23 を回収。

Worker ELF はビルド時に生成され、`ld -r -b binary` によって Init ELF に埋め込まれます。Init ELF はさらにカーネルの data segment に埋め込まれます。実行時にディスクドライバや VFS は必要ありません。

## ディレクトリ構成

```text
mistix-phase4/
├── src/kernel/
│   ├── mistixkernel.pas
│   ├── mistix_process.inc       # process table, fork/spawn/wait/kill, user IPC
│   ├── mistix_cow.inc           # COW page references and write-fault handling
│   ├── mistix_address_space.inc # PML4, user mappings, ELF64, copyin/copyout
│   ├── mistix_syscall.inc       # int 0x80 syscall ABI
│   ├── mistix_timer.inc         # PIC remap and PIT IRQ0
│   ├── mistix_trap.inc          # unified IRQ/exception/syscall dispatcher
│   ├── mistix_cpu.inc           # GDT, IDT, TSS, common ISR frame
│   └── ...                     # Phase 0–2 base units
├── userland/
│   ├── init/mistixinit.pas      # Pascal parent process
│   ├── worker/mistixworker.pas  # Pascal child service
│   └── user.ld                 # ELF64 user linker script
├── tools/check_source.py
├── boot/limine.conf
├── linker.ld
├── Makefile
└── README.md
```

## ビルド環境

Linux x86-64 上で以下を用意してください。

- Free Pascal Compiler（FPC。対象バージョンの inline assembler と ELF64 object 出力を要検証）
- GNU binutils (`ld`, `nm`, `readelf`)、GNU Make
- Limine v12 のビルド済みツリー
- `xorriso`、`qemu-system-x86_64`
- Python 3（ソース構成チェッカー）

例：

```sh
git clone --branch v12.x https://github.com/limine-bootloader/limine.git ../limine
make -C ../limine
```

## ビルドと起動

```sh
make clean
make check-source
make check-tools
make user-verify
make kernel
make verify
make iso LIMINE_DIR=../limine
make run LIMINE_DIR=../limine
```

QEMU は現在 `-smp 1` で起動します。デバッグ用に GDB stub を有効にする場合：

```sh
make run-debug LIMINE_DIR=../limine
```

## 期待されるシリアル出力例

以下は実測ログではなく、想定される出力の例です。スケジューリング順序や PID は実行タイミングにより変わります。

```text
Mistix - Phase 4: preemptive multi-process microkernel
[ OK ] PIT configured at 100 Hz; PIC IRQ0 mapped to vector 32.
[ OK ] Process manager initialised (8 process slots; dedicated kernel stacks; per-process capability spaces).
[ OK ] Created user process pid=1 ...
[PASS] Phase 4 process scheduler, PIT preemption, fork/COW,
       spawn/wait, per-process capabilities and user IPC ready.
[ OK ] Starting Ring-3 process pid=1 on dedicated kernel stack.
Fork parent continued after COW fork
Fork child running with COW memory!
[PROCESS] pid=2 terminated with status 11.
11Worker process started!
Worker replied through user IPC!
[PROCESS] pid=3 terminated with status 23.
23[PROCESS] pid=1 terminated with status 0.
```

## 既知の制約

この段階は単一 CPU 上の学習・研究用カーネルです。完全な UNIX/POSIX OS ではありません。

1. SMP / APIC / TLB shootdown / スケジューラロックは未実装。PIC/PIT のシングルCPUのみ。
2. PIT タイマーは100 Hz。ユーザーモード時に3 tickごとに切り替えます。カーネル内プリエンプションは行いません。
3. 最大8プロセス、16 Endpoint、64 Capability / process、Endpoint queue depth 8、最大512 KiB ELF image。
4. IPC は非ブロッキングで `-EAGAIN` を返します。ユーザー側の再試行を使用します。
5. `fork` は通常の4 KiB user mapping を COW 共有します。共有メモリの複雑な alias や huge page、DMA page は対象外。
6. Capability Transfer は親→直接の子に限定し、Endpoint の SEND/RECEIVE 権限だけを転送します。IPC message に Capability を添付する方式は未実装。
7. Endpoint object の参照数管理・自動破棄は未実装。Capability close はプロセスの表から handle を外すだけです。
8. 親終了時の orphan adoption、プロセスグループ、完全な wait semantics、ユーザー定義 signal handler は未実装。`kill` は直接の親子間の SIGINT(2)/SIGKILL(9)/SIGTERM(15) を即時終了として扱います。
9. ELF は静的 ET_EXEC のみ。PIE、動的リンカー、共有ライブラリ、TLS、relocation は未対応。
10. VFS、端末サービス、ディスク・ネットワーク・デバイスドライバは未実装。`write` は COM1 へ直接出力します。
11. カーネルスタックはプロセスあたり32 KiB。スタックオーバーフロー検出用 guard page は未実装。
12. FPU/SSE/AVX レジスタ状態、FS/GS base、debug register のプロセス切り替え保存は未実装です。現行の同梱 Pascal テストは整数演算のみを使います。
13. カーネルの起動時 self-test は Phase 2 の Endpoint / Notification / Memory Object 基礎テストと per-process capability isolation を実施します。COW fork・ユーザー IPC は同梱ユーザープログラムで実行する想定です。

## 検証状況

この生成環境には FPC と QEMU がないため、Pascal ソースの実コンパイルと QEMU 起動試験は未実施です。Python のソース構成検査、Makefile の `make -n all`、GNU `ld` を使った user/kernel linker script と ELF 埋め込みシンボルの独立スモークテストを行いました。**実機でコンパイル・起動確認した成果物ではありません。** `make user-verify`, `make verify`, `make run` による実測を次の検証工程としてください。
