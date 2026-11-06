# Mistix Phase 6 — Storage services and filesystem groundwork

Mistix Phase 6 は Phase 5 の Pascal カーネル／UNIX ユーザーランドを引き継ぎ、ストレージとサービス分離の次の段階を追加する開発スナップショットです。

## Phase 6 の実装範囲

### 実際に接続したユーザーランドサービス

- `vfsd`: 独立した Ring 3 プロセスとして起動する Pascal 製 VFS RPC サーバー
- `blockd`: 独立した Ring 3 プロセスとして起動する Pascal 製 RAM ブロックサーバー
- Capability Transfer によるサーバー Endpoint の受け渡し
- リクエスト Endpoint と返信 Endpoint の分離
- `vfsd` の MFS1 アーカイブ検証、`OPEN`、`READ`、`CLOSE`、`PING`
- `blockd` の `INFO`、`READ`、`WRITE`（64 KiB の揮発性 RAM disk）
- `init` から両サービスを spawn し、IPC RPC のラウンドトリップを検査

`vfsd` はビルド時に `/bin` と `/etc/motd` を含む MFS1 イメージを自身の ELF に埋め込みます。起動後、サービスはそのイメージをユーザーモードで読み取り、IPC 要求に応答します。IPC メッセージのペイロードは 6 個の `QWord` なので、パスは最大 39 文字、1 回の READ は最大 40 byte に制限しています。

### カーネル内ストレージ基盤

- ATA PIO primary-master の IDENTIFY
- LBA28 のポーリング READ SECTORS / WRITE SECTORS
- MBR primary partition の検出
- 読み取り専用 Ext2 の superblock / group descriptor / inode / directory lookup
- Ext2 direct block による小さな通常ファイルの読み込み
- mount namespace の最小テーブル
- IPv4 header checksum と Ethernet / ARP / IPv4 / UDP frame parser
- 合成 Ethernet/IPv4/UDP パケットを使ったパーサー自己テスト

## 重要なアーキテクチャー上の境界

この Phase 6 は、VFS の完全なユーザーランド移行を完了したものではありません。

- `vfsd` は独立した IPC サーバーとして実際に起動し、MFS1 のファイルを応答できます。
- しかし、既存の `open/read/write/getdents` syscall は互換性維持のため、まだ Phase 5 のカーネル内 VFS を呼び出します。
- `blockd` はサービス境界の IPC 試験用 RAM disk であり、物理 ATA デバイスをユーザーランドへ公開するドライバではありません。
- ATA PIO ドライバと Ext2 読み取りコードはカーネル内にあります。VFS サーバーからブロックサーバーへ Ext2 I/O を IPC で委譲する接続は未完了です。
- mount table は Ext2 mount metadata を保持しますが、Phase 5 VFS の pathname resolver とはまだ統合していません。
- ネットワーク部はパケット解析コアのみです。NIC ドライバ、ARP cache、routing、TCP/UDP socket API、ユーザーランド network server は未実装です。

この構成を明確に分けたのは、動作していないマイクロカーネル移行を「完了」と誤認しないためです。次段階では、共有メモリを使う RPC、カーネル VFS syscall のサービス転送、ブロックサービスへの Ext2 I/O 委譲を行います。

## Phase 6 のファイル

```text
mistix-phase6/
├── boot/limine.conf
├── linker.ld
├── Makefile
├── src/kernel/
│   ├── mistixkernel.pas
│   ├── mistix_block.inc
│   ├── mistix_ext2.inc
│   ├── mistix_mount.inc
│   ├── mistix_net.inc
│   └── ... Phase 0–5 のカーネル基盤
├── userland/
│   ├── common/mistixlib.inc
│   ├── init/mistixinit.pas
│   ├── vfs/mistixvfsd.pas
│   ├── block/mistixblockd.pas
│   ├── sh/mistixsh.pas
│   └── bin/{mistixls,mistixcat,mistixecho}.pas
├── tools/
│   ├── mkinitfs.py
│   └── check_source.py
└── docs/
    └── PHASE6-DESIGN.md
```

## ビルド要件

- x86-64 Linux 開発ホスト
- Free Pascal Compiler (FPC)
- GNU binutils (`ld`, `nm`, `readelf`)
- Python 3
- xorriso
- QEMU (`qemu-system-x86_64`)
- ビルド済み Limine v12 ツリー

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

物理 ATA PIO デバイスを試す場合は、ディスクイメージを指定します。

```sh
make run LIMINE_DIR=../limine DISK_IMAGE=../mistix-disk.img
```

ディスクイメージを指定した `run` は、primary IDE PIO を提供する QEMU `pc` machine type で起動します。Q35 上で AHCI disk を使う場合は AHCI driver が必要であり、この Phase 6 の ATA PIO driver では検出できません。

## 検証状況

この生成環境には FPC と QEMU がないため、FPC 実コンパイルおよび QEMU 起動試験は未実施です。`make check-source` はソース構成・ABI 名・依存順序を調べる静的チェッカーであり、Pascal の型検査や実行時安全性を保証しません。GNU ld を使う ELF 埋め込み・Limine ISO の最終リンクも、開発ホストで再検証してください。

## 制約

- ATA: primary master、PIO polling、LBA28 のみ。AHCI/NVMe、DMA、IRQ、secondary channel、slave、LBA48 は未対応。
- MBR: primary partition 4 個まで。GPT/拡張パーティションは未対応。
- Ext2: read-only。indirect block、HTree directory index、Extents、journal replay、symlink は未対応。direct block のみなので 12 * block size を超えるファイルは読めません。
- `blockd`: 64 KiB volatile RAM disk。電源断・再起動で内容は消えます。ATA driver とは未接続。
- `vfsd`: read-only MFS1 service。長い path、write/create/unlink/rename、directory enumeration、mount RPC は未対応。
- VFS syscalls は現時点でカーネル内互換実装を利用します。
- Network: parser/self-test のみ。ハードウェア NIC や送受信 path はありません。
- single CPU 前提。SMP、完全な POSIX、動的リンク、TCP socket API は未対応。
- カーネルとユーザーランドは Pascal。CPU 命令境界には Pascal ソース内の inline assembler を使用します。
