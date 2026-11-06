# Mistix Phase 7 — Shared-memory services, writable VFS overlay, device registry and loopback networking

Mistix Phase 7 は Phase 6 の x86-64 / Limine / Pascal カーネルを引き継ぎ、プロセス間共有メモリとユーザーランドサービスのデータ転送を強化する開発スナップショットです。

## Phase 7 の主な追加機能

### 1. プロセスごとの Shared Memory

`src/kernel/mistix_shm.inc` と syscall ABI 25–27 を追加しました。

- `shm_create(page_count)` — PMM から連続でなくてもよい物理ページを確保し、プロセス専用 Capability を発行
- `shm_map(handle, user_address, writable)` — 同じ物理ページを呼び出し元プロセスの PML4 にユーザー領域としてマップ
- `shm_unmap(handle, user_address)` — 対応するプロセスマッピングを解除
- Capability Transfer は Endpoint に加えて Memory Object をサポート
- プロセス終了時に、そのプロセスの Shared Memory mapping を解除
- 共有マッピングを保持するプロセスの `fork()` は `EBUSY` で拒否（COW 参照管理への統合前の安全策）
- W^X を維持するため共有領域は NX とし、実行可能マッピングは許可しない

上限は 16 オブジェクト、1 オブジェクト当たり最大16ページ（64 KiB）、全体で64マッピングです。現在は破棄・参照カウント API が未実装で、オブジェクトページはカーネル稼働中に保持されます。これは固定サイズの Phase 7 プロトタイプ上の制限です。

### 2. ユーザーランド VFS サーバー: MFS1 + writable `/tmp`

`vfsd` は独立した Ring 3 Pascal プロセスです。リクエスト／レスポンスは IPC、データ本体は共有メモリ窓を使用します。

RPC:

- `PING`
- `OPEN`
- `READ`
- `CLOSE`
- `STAT`
- `CREATE`
- `WRITE`
- `UNLINK`

MFS1 アーカイブは read-only の lower layer、`/tmp` は最大8ファイル・各4 KiBの volatile upper layer です。書き込み可能領域を `/tmp/` に限定することで、組み込み実行ファイルや `/etc` を上書きしないようにしています。

**重要:** 既存の `open/read/write/getdents` syscall はまだカーネル内 Phase 5 VFS を使用します。Phase 7 では `vfsd` の RPC と共有メモリ転送を実装しましたが、全アプリケーションの標準 FD 操作を自動的に `vfsd` へ転送する syscall broker はまだ実装していません。完全な VFS 移行済みとは扱いません。

### 3. Shared-memory block service

`blockd` は64 KiBの揮発性 RAM disk をユーザーランドで管理します。READ/WRITE のデータ本体は共有メモリ経由で転送し、IPC メッセージにはオフセットと長さだけを格納します。

このサービスは Phase 6 のテスト用 RAM disk を拡張したもので、物理 ATA PIO ドライバや Ext2 カーネルドライバにまだ接続されていません。DMA を実装したものではありません。

### 4. Device Manager (`devd`)

独立したユーザーランドデバイス登録サービスを追加しました。

- `REGISTER`
- `LIST`
- `INFO`
- `REMOVE`

最大16デバイスを保持し、世代番号付き ID を使用します。現段階はメタデータの登録簿であり、PCI 列挙、ドライバ起動、デバイス Capability の発行は次段階です。

### 5. Network service (`netd`)

`netd` は共有メモリを用いた RPC インターフェースを持ち、以下を実装します。

- loopback interface 情報 (`127.0.0.1`)
- loopback の ARP cache lookup
- Ethernet + IPv4 + UDP フレーム生成
- IPv4 checksum と UDP pseudo-header checksum 検証
- ソフトウェア UDP echo

これはソフトウェア・ループバック NIC です。物理 NIC、PCI bus mastering、DMA descriptor ring、実際の Ethernet 送受信、TCP、routing は未実装です。

## ソースツリー

```text
mistix-phase7/
├── Makefile
├── linker.ld
├── boot/limine.conf
├── src/kernel/
│   ├── mistixkernel.pas
│   ├── mistix_shm.inc
│   ├── mistix_process.inc
│   ├── mistix_syscall.inc
│   └── ... Phase 0–6 kernel sources
├── userland/
│   ├── common/mistixlib.inc
│   ├── init/mistixinit.pas
│   ├── vfs/mistixvfsd.pas
│   ├── block/mistixblockd.pas
│   ├── device/mistixdevd.pas
│   ├── network/mistixnetd.pas
│   ├── sh/mistixsh.pas
│   └── bin/{mistixls,mistixcat,mistixecho}.pas
├── tools/
│   ├── check_source.py
│   └── mkinitfs.py
└── docs/
    └── PHASE7-DESIGN.md
```

## Build

開発ホストは Linux x86-64 を想定します。

必要ツール:

- Free Pascal Compiler (FPC)
- GNU binutils (`ld`, `nm`, `readelf`)
- GNU Make
- Python 3
- xorriso
- QEMU (`qemu-system-x86_64`)
- ビルド済み Limine v12 tree

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

IDE primary-master の ATA PIO を試す場合:

```sh
make run LIMINE_DIR=../limine DISK_IMAGE=../mistix-disk.img
```

## Phase 7 起動時の自己テスト

`init` は `vfsd`, `blockd`, `devd`, `netd` を spawn し、Endpoint と必要な Shared Memory Capability を個別に転送します。

- VFS RPC v2 handshake
- `/etc/motd` shared-memory read
- `/tmp/phase7.t` create/write/read round-trip
- blockd shared-memory bulk READ/WRITE round-trip
- devd device register/list
- netd loopback IFINFO / ARP lookup / UDP echo

期待ログ例は `docs/PHASE7-DESIGN.md` に記載します。実機で取得したログではありません。

## 検証状況

この生成環境には FPC と QEMU がないため、Pascal の実コンパイルと QEMU 起動試験は未実施です。`make check-source` はソース構造・ABI の静的チェックであり、Pascal の型検査、ページテーブル実行時安全性、割り込み／スケジューラとの競合を保証しません。開発ホストで `make user-verify`、`make verify`、QEMU の serial log を必ず検証してください。

## 未実装・既知の制約

- 全標準 VFS syscall の userland VFS RPC broker 化
- Shared Memory object destruction / reference counting
- Shared Memory ページテーブル空階層の回収
- Shared Memory マッピングを継承する `fork()` / COW 参照管理との統合（現段階は該当 `fork()` を EBUSY で拒否）
- blockd と ATA PIO / Ext2 の接続
- VFS server 内の Ext2 filesystem driver と block RPC mount
- 書き込み可能な永続ディスク FS、journal、fsync
- directory enumeration、symlink、hard link、permissions の完全実装
- PCI/virtio/e1000 NIC driver、物理 ARP、DHCP、routing、TCP sockets
- PCI device enumeration と device driver manager
- blocking pipe、TTY canonical mode、signals、job control
- SMP と完全な POSIX ABI

Phase 7 は Pascal によるユーザーランドサービス境界と共有メモリ ABI を検証する段階です。ストレージ・ネットワークの実ハードウェア対応や完全な POSIX 互換を達成した段階ではありません。
