# Mistix — Phase 2（x86-64 / Pascal マイクロカーネル基盤）

Phase 2 は Phase 1 の CPU・メモリ管理基盤に、Capability、Endpoint IPC、Notification、ページバックされた Memory Object を追加します。

カーネルは引き続き **freestanding Free Pascal の単一 unit** として構築します。機能別 `.inc` は同一の Pascal unit に展開されます。C ソース、C ランタイム、ホスト OS のランタイムはリンクしません。

## 実装内容

### Capability

- 世代番号付き 32-bit ハンドル（上位16-bit generation、下位16-bit slot+1）。
- 固定サイズの capability table。
- オブジェクト種別、オブジェクト index/generation、権限ビットを保持。
- 権限を増やせない `CapabilityRestrict`。
- stale handle、無効 slot、権限不足の検出。
- オブジェクト破棄時の関連 capability 一括 revoke。

Phase 2 はまだユーザープロセスを持たないため、capability table は**カーネル全体で共有するブートストラップ空間**です。ユーザーモードの隔離境界として完成したものではありません。Phase 3 でプロセスごとの capability space に移します。

### Endpoint / IPC

- 最大16 Endpoint。
- Endpoint ごとに固定長8メッセージの FIFO キュー。
- 1メッセージ最大6個の 64-bit word。
- `IPCSend` はキュー満杯時に送信スレッドをブロック。
- `IPCReceive` はキュー空時に受信スレッドをブロック。
- enqueue/dequeue で待機スレッドを起床。
- close 時に待機者を起床し、オブジェクト generation を無効化。

IPC は現段階では単一CPU・協調スケジューラ上で動きます。ロックフリー／SMP セーフなキューではありません。割り込みハンドラから直接呼び出すことも想定していません。

### Notification

- 最大16個の Notification object。
- 64-bit pending bitset。
- signal はビットを OR し、待機スレッドを起床。
- wait は指定 mask に一致するビットを待ち、観測したビットだけを clear。
- notification はイベントの coalescing を行います。同じビットの複数回 signal は pending 状態では一つにまとまります。

### Memory Object

- 最大8オブジェクト、1オブジェクトあたり最大16物理ページ。
- PMM から物理ページを確保し、HHDM 経由でゼロ初期化。
- 同じオブジェクトページを複数の仮想アドレスにマップ。
- Capability により map/read/write/user-map/execute の要求を検証。
- mapping が残っているオブジェクトの destroy は `krBusy` で拒否。
- unmap は期待する物理ページと実際の PTE が一致することを確認。

Phase 2 の VMM は依然として Limine が作成した**単一の kernel address space**を利用します。したがって、同一 Memory Object の alias は実証しますが、異なるプロセス間の共有マッピングはまだ実装していません。独立 CR3 を持つプロセスができた段階で同じ物理ページを別々の root に map します。

## スケジューラの拡張

- `tsBlocked` 状態を追加。
- wait key を使ったブロック／起床 API。
- IPC と Notification が wait key を共有。
- 最大12スレッドに拡張。

まだ協調型スケジューラです。割り込みは無効（IF=0）のままで、タイマーによるプリエンプションや SMP 同期はありません。ブロックした全スレッドに runnable な相手がいない場合は、現段階では進行不能として halt します。Phase 3 以降で idle thread と割り込み駆動の wakeup を導入します。

## 起動時自己テスト

カーネルは以下を実行します。

1. Phase 1 の Limine / GDT / IDT / PMM / VMM 初期化。
2. Capability table とオブジェクト registry の初期化。
3. Endpoint を作成し、send-only に縮小した capability で receive が拒否されることを確認。
4. 受信スレッドを先に実行してブロックし、送信スレッドが IPC メッセージを送って受信側が再開することを確認。
5. Notification waiter をブロックさせ、signaler がビットを立てて waiter が再開することを確認。
6. Memory Object の1物理ページを2仮想アドレスへ map し、一方への書き込みが他方から読めることを確認。
7. alias を unmap し、Memory Object の物理ページを PMM に返す。

シリアルログで `[PASS]` に到達すれば、上記の基本的な kernel self-test が成功したことを示します。

## ビルド環境

x86-64 Linux 開発ホストに以下を用意してください。

- Free Pascal Compiler（FPC）3.2.x 以降を出発点として検証
- GNU binutils（`ld`、`nm`、`readelf`）、GNU Make
- Limine v12 のビルド済みソースツリー
- `xorriso`、`qemu-system-x86_64`

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

QEMU は single CPU、256 MiB RAM で起動し、COM1 を標準入出力に接続します。

## 意図的に未実装の機能

1. Capability table はプロセス単位ではなく kernel-global。
2. Capability transfer / IPC message による capability の move / copy は未実装。
3. ユーザーモードから安全に呼び出す syscall ABI は未実装。
4. プロセスごとの仮想アドレス空間、CR3 切替は未実装。
5. Memory Object の異なるプロセス間共有は未実装（同一 root 内 alias のみ）。
6. IPC キューと capability table は固定容量で、動的拡張しない。
7. IPC timeout、優先度継承、キャンセル、複数 CPU 同期は未実装。
8. Notification は bitset 型であり、イベント回数や順序を保持しない。
9. Endpoint の queue full / empty 時はブロッキングのみ。nonblocking API は未実装。
10. すべてのロック／待機操作は現段階の単一CPU・割り込み無効の実行文脈を前提とする。

## Pascal とアセンブリの境界

OS のロジック、型、キュー、Capability 検査、Memory Object 管理は Pascal です。CPU 固有命令、I/O port access、IDT/GDT 操作、CR レジスタ操作、TLB invalidation、コンテキストスイッチには Pascal ソース内のインラインアセンブリを使用します。C と C ランタイムは使用しません。

## 検証状況

この作業環境には FPC と QEMU がないため、Phase 2 ソースの実コンパイル／起動試験は**未実施**です。Makefile のビルド定義、ソース構造、依存関係は整えていますが、`make verify` と QEMU シリアルログの `[PASS]` を確認するまでは実機・仮想マシンで検証済みとは扱わないでください。
