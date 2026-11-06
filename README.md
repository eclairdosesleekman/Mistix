# Mistix — Phase 3（Ring 3 / Process / ELF64 / Syscall）

Mistix Phase 3 は Phase 2 の x86-64 Pascal マイクロカーネルを引き継ぎ、**独立したユーザー仮想アドレス空間を持つ最初のユーザープロセスを Ring 3 で実行する**ための基盤を追加します。

カーネルは Free Pascal の freestanding unit として構築します。初期ユーザープログラムも Pascal unit としてコンパイルし、ELF64 実行ファイルにリンクします。C ソースや C ランタイムは使用しません。CPU 特権遷移、CR3 操作、割り込み入口などの一部には Pascal ソース内の x86-64 inline assembly を使用します。

## Phase 3 の実装範囲

### 1. プロセスごとのアドレス空間

- 物理ページを使って独立した PML4 root を作成。
- Limine が構築した PML4 の上位半分（256〜511）を共有し、カーネル／HHDM のマッピングを維持。
- 下位半分はプロセスごとに独立。
- ユーザー領域は PTE_USER を各ページテーブル階層で設定。
- ELF のコードは RX、データは RW/NX、スタックは RW/NX としてマップ。
- CR3 を切り替えてプロセスのアドレス空間を有効化。
- プロセス破棄時に下位半分のユーザーページとページテーブルを回収。

CR3 の上位半分を共有する方式なので、カーネルコード、カーネルデータ、TSS、IDT、HHDM はどのプロセスからもカーネル特権でアクセスされます。上位 PML4 エントリーに USER 権限を与えないことが境界の要点です。

### 2. ELF64 ローダー

現在の初期ローダーは次の形式に限定しています。

- ELF64、little-endian、x86-64。
- ET_EXEC（固定仮想アドレスの実行ファイル）。
- PT_LOAD のみをロード。
- 最大32 program headers、最大256ユーザーページ。
- `p_filesz <= p_memsz`、ファイル境界、仮想アドレス境界、ページ整列を検証。
- W+X セグメントを拒否。
- セグメントの `p_filesz` を物理ページへコピーし、残りの `p_memsz` はゼロ初期化。
- エントリポイントがユーザー実行可能ページにあることを確認。

動的リンク、ET_DYN/PIE、PT_INTERP、PT_DYNAMIC、TLS、リロケーション、共有ライブラリは未対応です。Phase 3 のユーザー実行には NX 対応 CPU を必須とします。

### 3. ユーザースタック

- 16ページ（64 KiB）の固定ユーザースタック。
- スタックの下に未マップの guard page を確保。
- 初期 RSP は SysV AMD64 の関数エントリ整列に合う値。
- スタックページは RW/NX。

### 4. Ring 3 と syscall ABI

GDT の既存ユーザーセレクタを使用します。

| セレクタ | 用途 |
|---|---|
| `0x28` | Ring 0 code（Limine 互換） |
| `0x30` | Ring 0 data |
| `0x3B` | Ring 3 data |
| `0x43` | Ring 3 code |

Limine の初期スタックを離れて専用カーネルスタックへ移行した後、IDT vector `0x80` を DPL 3 の interrupt gate として設定し、TSS.RSP0 のカーネルスタックへ遷移します。syscall ABI は次の通りです。

| レジスタ | 意味 |
|---|---|
| RAX | syscall 番号／戻り値 |
| RDI | 第1引数 |
| RSI | 第2引数 |
| RDX | 第3引数 |

実装 syscall：

| 番号 | 名前 | 動作 |
|---:|---|---|
| 1 | `write(fd, buf, len)` | fd 1/2 のみ。最大4096バイトを COM1 へ出力 |
| 2 | `exit(status)` | 現在の初期プロセスを終了し、現段階ではカーネルを停止 |
| 3 | `getpid()` | 現在の PID |
| 4 | `yield()` | 現段階では no-op |

`write` はユーザーアドレス範囲をページごとに検査してから読み出します。現在は単一CPU・割り込み無効の環境なので、検査後に別スレッドが同じユーザーページを変更する競合はありません。SMP やプリエンプションを導入する前に copy-in バッファ方式へ進化させる必要があります。

### 5. プロセスごとの Capability Space

`TKernelProcess` は独自の `TProcessCapabilitySpace` を持ちます。各空間は Phase 2 の Capability と同じ世代番号付きハンドル方式です。権限の追加は禁止し、restrict は権限を縮小するだけです。

既存の Phase 2 カーネルサービス API はまだ kernel-global Capability Table を使用します。ユーザープロセスへ Endpoint / Memory Object の能力を移譲する IPC capability transfer は未実装であり、ユーザー syscall に公開していません。Phase 3 の自己テストは、ハンドルが別プロセス空間では解決されないことを確認します。

## ソース構成

```text
mistix-phase3/
├── src/kernel/
│   ├── mistixkernel.pas
│   ├── mistix_address_space.inc  # CR3 root、user page tables、ELF64
│   ├── mistix_process.inc        # process table、per-process capability space
│   ├── mistix_syscall.inc        # int 0x80、Ring 3 transition、syscall dispatch
│   └── ...                       # Phase 1 / 2 のカーネルコード
├── userland/
│   ├── init/mistixinit.pas       # 最初の Pascal ユーザープログラム
│   └── user.ld                  # user ELF64 linker script
├── boot/limine.conf
├── linker.ld
├── Makefile
└── tools/check_source.py
```

初期ユーザー ELF はビルド時に生成し、`ld -r -b binary` でカーネルのデータ領域へ埋め込みます。Limine module request に追加依存を作らず、Phase 3 の初回起動経路を単純化しています。

## ビルド環境

Linux x86-64 開発ホストに以下が必要です。

- Free Pascal Compiler（FPC）3.2.x 以降を出発点として検証
- GNU binutils（`ld`、`nm`、`readelf`）、GNU Make
- Limine v12 のビルド済みソースツリー
- `xorriso`、`qemu-system-x86_64`

```sh
git clone --branch v12.x https://github.com/limine-bootloader/limine.git ../limine
make -C ../limine
```

## ビルドと実行

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

`make user-verify` は Pascal ユーザー unit をコンパイルし、未解決ランタイムシンボルがないことを検査して ELF64 にリンクします。カーネルビルドはその ELF を埋め込みます。

## 想定されるシリアル出力

実機での実測ログではなく、自己テスト成功時に期待されるログの例です。

```text
[ OK ] GDT loaded; Limine selectors preserved; 64-bit TSS installed.
[ OK ] CR0.WP enabled; supervisor writes respect read-only PTEs.
[ OK ] PMM bitmap initialised.
[ OK ] VMM attached to Limine 4-level page tables.
[ OK ] Process manager initialised (8 process slots; per-process capability spaces).
[ OK ] Per-process capability isolation self-test passed.
[ OK ] Embedded Pascal user ELF image size=... bytes.
[ OK ] Loaded ELF64 user process pid=1 entry=... CR3=...
[ OK ] DPL-3 int 0x80 syscall gate installed.
[PASS] Phase 3 kernel address spaces, ELF loader, per-process
       capabilities and ring-3 syscall entry are initialised.
[ OK ] Entering ring 3: pid=1 CS=0x43 SS=0x3B.
Hello from Mistix ring 3!
[PROCESS] user process pid=1 exited with code 0.
```

## 既知の制約と未実装

1. シングルCPUのみ。SMP、ロック、TLB shootdown は未対応。
2. IF は無効のまま。タイマー割り込みとプリエンプティブスケジューリングは未実装。
3. 初回実装ではフォアグラウンドのユーザープロセスを1つだけ起動。プロセス間コンテキストスイッチは未実装。
4. `exit` はプロセスを terminated にしてカーネルを halt。親子プロセス、wait、spawn、fork は未実装。
5. `yield` は no-op。
6. syscall は `int 0x80` を使用。`SYSCALL/SYSRET` MSR 経路は未実装。
7. 例外はまだカーネル致命的エラーとして停止。ユーザープロセス単位の fault kill は未実装。
8. ELF は静的 ET_EXEC のみ。PIE、動的 linker、relocation、shared library は未対応。
9. Capability Space はプロセスごとに確保するが、Capability transfer とユーザー向け IPC syscall は未実装。
10. 共有メモリの異なるプロセス間マッピングは未実装。Phase 2 の alias test は従来どおり kernel root 上で行う。
11. syscall `write` は COM1 へ直接出力し、VFS / terminal server はまだ利用しない。
12. syscall 入力ポインタはページ単位で検証する。将来の SMP / preemption 前にカーネルバッファへの copy-in を導入する。

## 検証状況

この生成環境には FPC と QEMU がないため、Pascal ソースの実コンパイルと仮想マシン起動は**未実施**です。ソース構成チェッカー、Makefile のドライラン、および GNU ld による user linker script / ELF program-header / embedded-blob シンボルの独立確認を行いました。`make verify` と QEMU のシリアルログで実測するまでは、実行確認済みとは扱わないでください。
