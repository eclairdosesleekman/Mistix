# Mistix Phase 8 — ATA block broker とユーザーランド Ext2

Mistix Phase 8 は Phase 7 の x86-64 / Limine / Pascal カーネルを引き継ぎ、物理 ATA ディスクの読み取りを制限付き syscall と `blockd` RPC に集約し、ユーザーランド `vfsd` から Ext2 を読み取り専用で利用する開発スナップショットです。

> **検証上の注意:** この成果物はソース・ビルド構成をまとめた開発スナップショットです。この実行環境には Free Pascal Compiler (FPC) と QEMU がないため、FPC 実コンパイルおよび仮想マシン起動は未確認です。`make check-source` はソース構造チェックであり、Pascal コンパイラの代替ではありません。

## Phase 8 の主な変更

### 1. 制限付き ATA ブロック読み取り ABI

`src/kernel/mistix_block.inc` と `src/kernel/mistix_syscall.inc` に block-service 専用 ABI を追加しました。

| syscall | ID | 用途 |
|---|---:|---|
| `block_register()` | 28 | ブート時 block service を登録 |
| `block_info()` | 29 | 物理ディスクの容量・セクタサイズを取得 |
| `block_read(lba, sectors, buffer)` | 30 | 物理ディスクを読み取り |

制限事項:

- 登録は PID 1 の直接の子プロセスに限定し、登録は一度だけです。
- 読み取り API のみを公開します。物理ディスクへの write syscall は追加していません。
- 1 syscall あたり最大128セクタ（64 KiB）です。
- バッファは呼び出し元プロセスのユーザー領域として検証されます。
- これはブートストラップ用の単一 blockd 登録方式です。汎用デバイス Capability モデルへの完全統合ではありません。

### 2. `blockd` の物理 ATA / RAM バックエンド

`userland/block/mistixblockd.pas` は起動時に `block_register()` を呼び、ハードウェアが存在すれば ATA PIO の読み取り専用バックエンドを使います。物理ディスクがない QEMU 環境では従来の64 KiB揮発性 RAM ディスクにフォールバックします。

RPC:

- `INFO`: sector count、sector size、byte capacity、backend flag を返す
- `READ`: バイト範囲を読み、共有メモリ窓へ転送
- `WRITE`: RAM fallback のみ書き込み可能。物理 ATA backend は `EROFS`

ATA の読み取りでは必要なセクタ範囲を計算し、カーネルの block-read syscall でセクタを取得して、要求されたバイト範囲だけ共有メモリへコピーします。ブート時自己テストは物理ディスクに書き込みません。

### 3. ユーザーランド `vfsd` 内の Ext2 読み取り専用クライアント

`userland/vfs/mistix_ext2_client.inc` と `mistix_ext2_types.inc` を追加しました。`vfsd` は IPC で `blockd` に接続し、ディスクデータを共有メモリ RPC で読み込みます。

対応範囲:

- 512-byte sector の blockd backend
- MBR の最初の primary Linux (`0x83`) partition、または partition table のない raw Ext2 image
- Ext2 block size 1 KiB / 2 KiB / 4 KiB
- superblock、group descriptor、inode、directory entry
- direct block および single-indirect block
- 読み取り専用の `OPEN` / `READ` / `STAT` / `READDIR`
- マウントポイント `/mnt/disk`

安全側に倒すため、次の形式・機能はマウント拒否または未対応です:

- Ext3 journal / journal replay
- Ext4 extents や未対応 incompat feature
- GPT、extended/logical partition、複数 partition の探索
- double / triple indirect block
- Ext2 への永続書き込み、create、unlink、rename

`/tmp` の揮発性オーバーレイは従来どおり `vfsd` 内の RAM 上で動作します。

### 4. VFS RPC

追加した RPC:

| RPC | 内容 |
|---|---|
| `MOUNT_EXT2` | blockd backend 上の Ext2 を `/mnt/disk` に read-only mount |
| `READDIR` | `/mnt/disk` 配下のディレクトリを列挙し、dirent 配列を共有メモリに返す |

既存の `OPEN` / `READ` / `STAT` は MFS1、`/tmp`、Ext2 の各データソースを区別します。

**重要:** Phase 8 でも、一般アプリケーションが呼ぶカーネル `open()` / `read()` syscall はまだ Phase 5 のカーネル内 VFS 経路を使用します。ユーザーランド `vfsd` は IPC サービスとして Ext2 を提供しますが、標準ファイル syscall 全体の broker 化は完了していません。次段階で VFS client syscall shim と descriptor proxy を導入する必要があります。

## 起動時テスト

`init` は `blockd` を先に起動し、共有メモリと request/reply capability を渡した後、`vfsd` に blockd RPC capability を追加で渡します。

テスト項目:

1. blockd registration と `INFO`
2. RAM fallback の場合は共有メモリ READ/WRITE round-trip
3. 物理 ATA の場合は LBA 0 の read-only RPC（書き込みなし）
4. vfsd RPC v3 handshake
5. Ext2 mount（ディスクが無ければ正常に unmounted と報告）
6. Ext2 root directory enumeration（mount が成功した場合）
7. MFS1 `/etc/motd` の read
8. `/tmp` overlay create/write/read
9. device registry と loopback network service の既存テスト

## ビルド

Linux x86-64 開発環境で FPC、GNU binutils、GNU Make、Python 3、xorriso、QEMU、Limine v12 を用意します。

```sh
cd mistix-phase8
make clean
make check-source
make check-tools
make user-verify
make kernel
make verify
make iso LIMINE_DIR=../limine
make run LIMINE_DIR=../limine
```

ディスクイメージを QEMU の primary IDE disk として添付する場合:

```sh
make run LIMINE_DIR=../limine DISK_IMAGE=../mistix-ext2.img
```

`mistix-ext2.img` は、先頭に MBR がある場合は最初の primary type `0x83` partition に Ext2 を置くか、partition table のない raw Ext2 image にしてください。Ext2 は読み取り専用で開きます。

raw Ext2 の簡易テストイメージは、ホストに `mke2fs` と `debugfs` がある場合、例えば次のように作成できます。

```sh
truncate -s 64M ../mistix-ext2.img
mke2fs -t ext2 -F -b 4096 -O ^dir_index ../mistix-ext2.img
debugfs -w -R "write docs/motd.txt /hello.txt" ../mistix-ext2.img
make run LIMINE_DIR=../limine DISK_IMAGE=../mistix-ext2.img
```

`dir_index` はこの parser が未対応のため無効にします。イメージの作成はホスト側で行い、Mistix からは read-only で参照します。

## ソースツリーの主な追加・変更

```text
mistix-phase8/
├── src/kernel/
│   ├── mistix_block.inc       # BlockServerPID 登録状態
│   └── mistix_syscall.inc     # block_register/info/read syscall
├── userland/
│   ├── common/mistixlib.inc   # block service syscall wrapper
│   ├── block/mistixblockd.pas # ATA read-only / RAM backend
│   ├── vfs/
│   │   ├── mistixvfsd.pas
│   │   ├── mistix_ext2_types.inc
│   │   └── mistix_ext2_client.inc
│   └── init/mistixinit.pas    # service capability bootstrap/tests
├── docs/PHASE8-DESIGN.md
└── tools/check_source.py
```

## 既知の制約と次の作業

- FPC による実コンパイルと QEMU 起動試験は未実施です。
- ATA ドライバは primary master / LBA28 の既存カーネル実装を利用し、AHCI / NVMe / LBA48 は未対応です。x86 PIO の実際の I/O 命令は特権モードのカーネル側に残り、`blockd` は制限された読み取り syscall だけを呼び出します。
- 既存のカーネル起動処理は互換性のため独自の read-only Ext2 namespace も初期化します。Phase 8 の `vfsd` は別のユーザーランド Ext2 実装で同じ物理ディスクを RPC 経由で読みます。カーネル側 Ext2 / block 経路を削除したわけではありません。
- Ext2 は read-only の限定実装です。
- `vfsd` の RPC は独立していますが、通常の FD syscall はまだ kernel VFS を通ります。
- `READDIR` の固定 `TMistixDirent` ABI は名前を最大63バイトに制限し、1回あたり最大128件です。それを超えるディレクトリはエラーになります。
- block service 登録は PID 1 直下の最初の1サービスに限定した bring-up 用です。
- `blockd` の physical read syscall は単一の 64 KiB kernel scratch buffer を使うため、同時 read の並列化は行いません。
- shared-memory object の完全な参照カウント・破棄やプロセス間の fork 継承は未完了です。

次の段階では、`open/read/getdents` syscall をユーザーランド VFS proxy に切り替え、ファイルディスクリプタと RPC request を非同期に対応付ける必要があります。その後に Ext2 write transaction と block-device write capability を別途設計します。
