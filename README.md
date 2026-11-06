# Mistix — Phase 1（x86-64 / Pascal カーネル）

Phase 1 は Phase 0 の Limine 起動コードを土台に、CPU の記述子テーブル、例外ハンドラ、物理ページ管理、仮想メモリ管理、協調型カーネルスレッドを追加する段階です。

カーネルは引き続き **単一の freestanding Free Pascal unit** としてビルドします。`.inc` ファイルはソースを機能別に整理するためのテキストインクルードであり、別々の Pascal unit やランタイム初期化機構を追加するものではありません。

## 実装内容

- GDT：Limine の初期セレクタ `CS=0x28`、データ `0x30` を維持し、ring 3 用コード／データ記述子を追加。
- 64-bit TSS：カーネル遷移用 `RSP0` とダブルフォルト用 IST1 スタックを設定。
- IDT：例外ベクタ 0〜31 を個別スタブへ接続。ベクタ8は IST1 を使用。
- 例外診断：汎用レジスタ、例外番号、エラーコード、RIP を保存し、ページフォルト時は CR2 を表示。
- Limine Paging Mode Request：x86-64 4-level paging を明示的に要求。
- PMM：Limine メモリマップの `USABLE` 領域だけを解放するビットマップ式物理ページアロケータ。
- VMM：Limine が設定した CR3 の4段ページテーブルを引き継いで拡張。4 KiB ページの map/unmap、4 KiB・2 MiB・1 GiB ページの translate に対応。
- メモリ自己テスト：物理ページを確保し、未使用仮想アドレスにマップ、実際に1 byteを書き戻し、変換・アンマップ・解放。
- カーネルスレッド：静的スタックを用いた協調型 round-robin スケジューラと x86-64 コンテキストスイッチ。
- COM1 シリアルログ、致命的エラー時の停止処理。

## 意図的に未実装としている機能

これは動作確認用の Phase 1 基盤であり、完成した OS カーネルではありません。

1. PMM のビットマップは物理アドレス 64 GiB 未満を対象にします。それ以上のメモリは割り当てません。
2. ブートローダーの応答データや初期ページテーブルを誤って解放しないよう、`USABLE` 以外のメモリタイプはすべて予約したままです。Bootloader Reclaimable の回収は後続フェーズです。
3. VMM は Limine の既存ページテーブルを拡張します。独立した CR3 への切替、プロセスごとのアドレス空間、ページテーブルの空テーブル回収は未実装です。
4. スケジューラは single-CPU の協調型です。割り込みは無効のままで、タイマーによるプリエンプション、SMP ロック、CPU ごとの状態は未実装です。
5. TSS はロードしますが、ring 3 への遷移、ユーザーモードからの復帰、スレッドごとの `RSP0` 更新は未実装です。
6. IDT のベクタ 32〜255 は既定スタブに接続するだけです。PIC/APIC の割り込みルーティングや EOI 処理は行いません。
7. Limine が設定した既存マッピングの権限を一括で引き締める処理は行いません。Mistix が新規作成するページマッピングに指定されたフラグを適用します。

## ビルド環境

x86-64 Linux 開発ホストに以下を用意してください。

- Free Pascal Compiler（FPC）3.2.x 以降を出発点として使用。実際のインラインアセンブラ互換性は対象環境で確認してください。
- GNU binutils（`ld`、`nm`、`readelf`）、GNU Make
- Limine v12 のビルド済みソースツリー
- `xorriso`、`qemu-system-x86_64`

Limine はリポジトリ外に置きます。

```sh
git clone --branch v12.x https://github.com/limine-bootloader/limine.git ../limine
make -C ../limine
```

## ビルドと起動

```sh
make clean
make check-tools
make kernel
make verify
make iso LIMINE_DIR=../limine
make run LIMINE_DIR=../limine
```

QEMU は CPU 1個、メモリ 256 MiB、COM1 を標準入出力へ接続して起動します。ログは概ね次の順序になります。

```text
Mistix - Phase 1: CPU and memory foundations
[ OK ] Limine base revision 6 accepted.
[ OK ] Limine selected x86-64 4-level paging.
[ OK ] GDT loaded; Limine selectors preserved; 64-bit TSS installed.
[ OK ] IDT loaded with exception gates 0..31 and double-fault IST.
[ OK ] PMM bitmap initialised; free 4 KiB pages: ...
[ OK ] VMM attached to Limine 4-level page tables; CR3 root=...
[ OK ] PMM allocate/free and VMM map/translate/unmap self-test passed.
[ OK ] Cooperative kernel-thread scheduler initialised.
[THREAD] alpha: first dispatch.
[THREAD] beta: first dispatch.
[THREAD] alpha: resumed after cooperative yield.
[THREAD] thread ... terminated.
[THREAD] beta: resumed after cooperative yield.
[THREAD] thread ... terminated.
[PASS] Phase 1 CPU tables, exception IDT, PMM, VMM and kernel-thread context switching are alive.
[NOTE] Interrupts remain disabled; scheduling is cooperative, not timer-preemptive.
```

ページ数、物理アドレス、スレッド ID は環境によって変わります。

## 重要な設計判断

### GDT / TSS

Limine の現行 x86-64 プロトコル仕様に従い、初期コードセレクタ `0x28` とデータセレクタ `0x30` のスロットを維持します。その上で GDT、TSS、IDT を Mistix 自身のものに置き換えます。ダブルフォルトゲートには IST1 を割り当てます。

### PMM / HHDM

PMM は `LIMINE_MEMMAP_USABLE`（type 0）の完全な 4 KiB ページだけを解放します。ブートローダー再利用可能領域、カーネル／モジュール、ACPI、フレームバッファ、予約領域、不良メモリは予約したままです。ページテーブルと Limine 応答構造体へのアクセスには、起動時に返された HHDM オフセットを使用し、固定値を仮定しません。

### VMM

Limine によって構築された4段ページテーブルを引き継ぎ、CR3 を切り替えずに必要なテーブルだけ追加します。4段ページングを明示要求するため、5段ページングとページテーブル走査の不整合を避けます。

### Pascal とアセンブリの境界

OS のロジックとデータ構造は Pascal です。インラインアセンブリは、ポート I/O、`lgdt` / `lidt` / `ltr`、CR/MSR/CPUID 操作、`invlpg`、例外入口／復帰、コンテキストスイッチなど、CPU 命令または厳密なレジスタ規約が必要な箇所に限定しています。C ソース、C ランタイム、ホスト OS のランタイムはリンクしません。

割り込み／例外を扱うカーネルでは、FPC が生成した機械語を必ず逆アセンブルして、スタックの red-zone 利用、スタックアラインメント、保存レジスタ、外部 RTL 参照がないことを確認してください。

## 検証状況

この実行環境には FPC と QEMU がないため、Pascal ソースの実コンパイルおよび仮想マシンでの起動試験は **未実施** です。Makefile は、開発ホストでビルドした際に未解決シンボル、ELF64、`_start`、例外ディスパッチャ、コンテキストスイッチシンボルを検査します。`make verify` と QEMU 起動試験が通るまでは、実機検証済みとは扱わないでください。
