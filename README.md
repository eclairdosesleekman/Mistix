# Mistix Phase 9 — ユーザーランド VFS syscall broker

Mistix Phase 9 は Phase 8 の x86-64 / Limine / Pascal カーネルを引き継ぎ、通常の `open()` / `read()` / `write()` / `close()` syscall からユーザーランド `vfsd` へ IPC リクエストを転送するブートストラップ VFS broker を追加した開発スナップショットです。

> **検証上の注意:** この実行環境には Free Pascal Compiler (FPC) と QEMU がないため、Pascal 実コンパイルおよび仮想マシン起動は未確認です。`make check-source` は構造・ABI の静的チェックであり、FPC コンパイルや実行時検証の代替ではありません。

## Phase 9 の主な変更

### 1. 通常ファイル syscall の VFS broker 化

カーネルに `vfs_register()` syscall 31 を追加しました。PID 1 が起動した直接の子 `vfsd` と request/reply Endpoint を登録すると、通常ファイルに対する syscall をユーザーランドサーバーへ委譲します。

| syscall | 動作 |
|---|---|
| `open(path, flags)` | `vfsd` の MFS1 / `/tmp` / Ext2 read-only namespace で開く |
| `read(fd, buf, count)` | broker RPC と共有メモリ経由で読み込む |
| `write(fd, buf, count)` | broker RPC と共有メモリ経由で書き込む（現状 `/tmp` のみ） |
| `close(fd)` | リモート open description の最終参照を非同期 CLOSE で解放 |

`/dev/console`、`/dev/serial`、`/dev/null` と `O_DIRECTORY` は既存のカーネル内デバイス／ディレクトリ処理を維持します。ディレクトリ列挙 (`getdents`) と `chdir/getcwd` もまだカーネル内 VFS 経路です。

### 2. Remote file description

カーネルの `TVFSOpenDescription` に `voRemote` と `RemoteHandle` を追加しました。FD はローカルの open-description table を参照し、同じ description を `dup2()` や `fork()` で共有するため、リモートハンドルとファイル位置も共有されます。

最後のローカル参照がなくなると、`VFSReleaseDescription` が CLOSE を小さな固定長キューへ登録します。`close()`、`dup2()` の置換、プロセス終了を同じ経路で処理し、broker の次の機会に非同期 CLOSE を vfsd へ送ります。サーバーが停止した場合は古いハンドルを再起動後のサーバーへ誤送信しないよう、保留 CLOSE を破棄します。

### 3. IPC protocol と共有メモリ

Phase 8 の IPC message ABI を変更せず、既存の6ワードメッセージを利用します。broker は同時に1件の VFS transaction のみを実行し、transaction ID と呼び出し元 PID で応答を対応付けます。`EAGAIN` 再試行時には owner PID、operation、remote handle、user buffer、byte count、flags、OPEN の正規化済みパス全体を照合し、別要求の応答を誤って消費しないようにします。要求内容が変わった再試行は `EBUSY` になります。

- OPEN: 128-byte path buffer と flags
- READ: remote handle と最大4096-byte length。返却データは共有メモリ窓
- WRITE: remote handle と最大4096-byte length。入力データは共有メモリ窓
- CLOSE: remote handle。最終参照解放時は transaction ID 0 の非同期要求

PID 1 と vfsd の共有窓については、単に同じ仮想アドレスがマップされているだけでなく、全16ページの物理ページ番号と user/writable flags が一致することを登録時に検証します。

request/reply Endpoint の capability、vfsd が PID 1 の直接の子であること、プロセス状態、共有ページの対応関係を登録時に検証します。broker の RPC slot は単一 CPU 向けであり、SMP ロックや複数 outstanding request はまだありません。

### 4. vfsd の通常ファイル API

ユーザーランド vfsd は broker 用 OPEN/READ/WRITE/CLOSE RPC を処理します。

| データソース | 読み込み | 書き込み |
|---|---:|---:|
| MFS1 初期アーカイブ | 対応 | 読み取り専用 |
| `/tmp` RAM overlay | 対応 | 対応 |
| `/mnt/disk` Ext2 | 対応 | 読み取り専用 |

`O_CREAT` は `/tmp` overlay に限定しています。`write()` は現時点で最大4096 bytesの部分書き込みです。`SysWriteAll` は部分書き込みを繰り返しますが、エラー時に無限再試行しないようにしています。

### 5. リモート FD の終了処理

open description の最終参照解放時に remote CLOSE をキューへ入れます。これにより通常 `close()` だけでなく、`dup2()` の置換や `VFSProcessCloseAll()` の経路でもサーバー側ハンドルを解放できます。broker は非同期 CLOSE の応答（transaction ID 0）を通常要求の応答として誤認しません。

## 起動時テスト

`init` は Phase 8 と同じ順序で blockd と vfsd を起動し、既存の direct RPC smoke test を済ませた後に `vfs_register()` を実行します。その後は一般 syscall を通じて以下を確認します。

1. `/etc/motd` を `open/read/close` で読み出す。
2. `/tmp/phase9.txt` を `O_RDWR | O_CREAT` で開く。
3. 標準 `write()` でデータを書き込む。
4. close 後に再 open/read し、内容が一致することを検証する。
5. 標準出力の `/dev/console` は従来どおりカーネル内デバイス FD で出力する。

このテストコードは起動時に実行する設計ですが、QEMU で実行した結果ではありません。

## ビルド

Linux x86-64 開発環境で FPC、GNU binutils、GNU Make、Python 3、xorriso、QEMU、Limine v12 を用意します。

```sh
cd mistix-phase9
make clean
make check-source
make check-tools
make user-verify
make kernel
make verify
make iso LIMINE_DIR=../limine
make run LIMINE_DIR=../limine
```

Ext2 raw disk image を IDE primary disk として添付する場合:

```sh
make run LIMINE_DIR=../limine DISK_IMAGE=../mistix-ext2.img
```

## 主なソースツリー

```text
mistix-phase9/
├── src/kernel/
│   ├── mistix_vfs.inc       # voRemote / remote handle / close queue hook
│   └── mistix_syscall.inc   # vfs_register と open/read/write/close broker
├── userland/
│   ├── common/mistixlib.inc # syscall raw stub と EAGAIN retry wrappers
│   ├── init/mistixinit.pas # broker 登録と標準 syscall の read/write test
│   └── vfs/mistixvfsd.pas  # broker OPEN/READ/WRITE/CLOSE RPC
├── docs/PHASE9-DESIGN.md
└── tools/check_source.py
```

## 既知の制約

- FPC 実コンパイル、ISO 作成、QEMU 起動は未確認です。
- broker はグローバルに1つの vfsd を登録するブートストラップ実装で、登録権限は PID 1 に限定しています。
- broker は同時に1件の transaction のみ処理します。複数プロセスは `EAGAIN` を受け、ユーザーランド wrapper が `yield()` して再試行します。同じ slot の再試行では要求パラメータと OPEN パスを完全一致で検証し、別要求へのすり替えを `EBUSY` で拒否します。
- syscall 1回の VFS I/O 上限は4096 bytesです。長い転送は呼び出し元が分割します。
- `getdents()`、`chdir()`、`getcwd()`、`O_DIRECTORY` はまだカーネル内 VFS を利用します。Ext2 ディレクトリを通常 `ls` で列挙するための userland directory-FD proxy は未実装です。
- `/dev/*` はカーネル内デバイス処理を維持します。
- 書き込み可能なのは `/tmp` overlay のみです。MFS1 と Ext2 は読み取り専用で、Ext2 journal、create/unlink/rename、永続 write は未対応です。
- broker が永久に応答しない場合、wrapper は有限回の yield/retry 後に `EAGAIN` を返します。タイムアウトを監視する watchdog や要求キャンセルはありません。
- broker の共有メモリ窓は単一グローバル領域であり、複数 outstanding request や SMP 並列には対応しません。
- vfsd の再起動ポリシー、プロセスごとの broker 再接続、サーバー死亡時に開いていた FD の自動再オープンは未実装です。
- remote close queue は固定長です。ブローカーが長時間停止した状態で大量のファイルを開閉した場合の永続化は行いません。
- `fork()` 後は remote open description を共有し、ファイル位置も共有します。Copy-on-Write と同様の UNIX 共有 open-file-description の意味論を意図しています。

次段階では `getdents/chdir/getcwd` の broker 化、複数 outstanding RPC、キャンセル／タイムアウト、vfsd 再起動後の FD 回復、UID/GID/mode、Ext2 write transaction を進めます。
