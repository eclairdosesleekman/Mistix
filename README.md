# Mistix Phase 5 — UNIX 基盤

Mistix Phase 5 は Phase 4 の x86-64 Pascal カーネルを引き継ぎ、UNIX ライクなファイル操作と最小ユーザーランドを追加する開発段階です。

## 実装範囲

- Pascal 製 VFS ブートストラップ
- 読み取り専用 MFS1 InitFS
- DevFS 相当の `/dev/console`、`/dev/null`、`/dev/serial`
- プロセスごとのファイルディスクリプタ表
- `fork()`／`spawn_path()` 後の open-file description 共有
- `open`、`read`、`write`、`close`、`dup2`
- `pipe`、`getdents`、`chdir`、`getcwd`
- Pascal 製 `init`、`sh`、`ls`、`cat`、`echo`
- 1 段のパイプライン（例: `cat /etc/motd | cat`）

## アーキテクチャー

```text
Pascal applications
  init / sh / ls / cat / echo
          |
          | int 0x80 syscall ABI
          v
Process manager + VFS bootstrap core
  per-process FD table / open-file descriptions / pipes
          |
          +---- InitFS (read-only MFS1 archive)
          |
          +---- DevFS-like device nodes
          |
          v
Mistix microkernel
  address spaces / scheduler / IPC / capabilities / COW
```

### マイクロカーネル境界について

この段階では、VFS の要求処理を既存のシステムコール入口に接続するため、VFS コアをカーネル内の Pascal モジュールとして実装しています。これは起動可能な UNIX 基盤を先に作るための移行用構成であり、最終的なマイクロカーネル境界ではありません。

VFS のノード管理、open-file description、ファイル操作 API を独立したユニットとして隔離しています。後続段階では、これらの操作を IPC と Capability 経由でユーザーランド VFS サーバーへ移します。

## ファイルシステム

### InitFS

MFS1 という読み取り専用アーカイブをビルド時に生成し、カーネル ELF に埋め込みます。現在の初期イメージには次のファイルを含めます。

- `/bin/sh`
- `/bin/ls`
- `/bin/cat`
- `/bin/echo`
- `/etc/motd`

MFS1 のヘッダーは `MFS1` magic、バージョン、エントリー数です。各エントリーはパス長、予約フィールド、データ長、ASCII パス、ファイルデータで構成されます。マウント時に範囲とパス形式を検査します。

### DevFS 相当

初期デバイスノードは次のとおりです。

- `/dev/console`: COM1 を使う標準コンソール
- `/dev/serial`: COM1 の生入出力
- `/dev/null`: 書き込みを捨て、読み込み時に EOF を返す

TTY は現段階では raw serial です。カーネルの標準入力は非ブロッキングで `-EAGAIN` を返し、シェルが文字単位で読み取り・エコー・簡易編集を行います。

## ファイルディスクリプタと Pipe

FD はプロセスごとの `TVFSProcessFiles` に保持します。複製された FD は共有 open-file description を参照するため、ファイル位置や Pipe endpoint の寿命が `dup2`／`fork`／`spawn_path` をまたいで整合します。

Pipe は固定長 4096 byte の FIFO です。読み込み側はデータがなく writer が残っている場合 `-EAGAIN`、writer がすべて閉じられた場合 EOF (`0`) を返します。書き込み側は reader がいなければ `-EPIPE`、バッファが満杯なら `-EAGAIN` を返します。ユーザーランドの `SysWriteAll` は部分書き込みと `-EAGAIN` を再試行します。

## システムコール ABI

既存の Phase 0〜4 の番号は維持し、以下を追加しています。x86-64 の `int 0x80` を使い、戻り値は `RAX` です。

| 番号 | 名前 | 引数 |
|---:|---|---|
| 15 | `open` | `RDI=path`, `RSI=flags` |
| 16 | `read` | `RDI=fd`, `RSI=buffer`, `RDX=count` |
| 17 | `close` | `RDI=fd` |
| 18 | `dup2` | `RDI=oldfd`, `RSI=newfd` |
| 19 | `pipe` | `RDI=QWord[2]` のユーザーポインタ |
| 20 | `getdents` | `RDI=fd`, `RSI=entries`, `RDX=max_entries` |
| 21 | `chdir` | `RDI=path` |
| 22 | `getcwd` | `RDI=buffer`, `RSI=capacity` |
| 23 | `spawn_path` | `RDI=path`, `RSI=args`, `RDX=stdin_fd`, `RCX=stdout_fd` |
| 24 | `getcmdline` | `RDI=buffer`, `RSI=capacity` |

既存の syscall 1 `write` は FD 経由の実装に接続しています。

## ユーザーランド

`init` は `/etc/motd` を表示し、`/bin/sh` を起動して終了後に再起動します。`sh` は次の組み込みコマンドを持ちます。

```text
help
pwd
cd /etc
exit
```

外部コマンド:

```text
ls /
cat /etc/motd
echo Hello, Mistix!
cat /etc/motd | cat
```

シェルは単純な空白区切りと 1 段の `|` のみに対応します。クォート、環境変数展開、リダイレクト、複数段パイプ、ジョブ制御は未実装です。

## ビルド

必要なツール:

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

## 制約と未実装項目

- この環境では FPC と QEMU が利用できなかったため、実コンパイルと起動テストは未実施です。構造チェックと Makefile のドライランを行う段階です。
- VFS はまだカーネル内のブートストラップコアであり、独立したユーザーランド VFS サーバーではありません。
- InitFS は read-only。書き込み可能な FS、mount/unmount、symlink、permission、rename、unlink は未実装です。
- Pipe は固定長かつ非ブロッキングで、カーネルスレッドをブロックする完全な POSIX pipe ではありません。
- TTY は raw COM1 のみ。canonical line discipline、PTY、job control、foreground process group は未実装です。
- `spawn_path` は簡易 command-line 文字列 ABI で、完全な `argv`／`envp` ABI ではありません。
- POSIX 完全準拠、動的リンク、SMP、複数 CPU は未対応です。
- カーネルとユーザーランドは Pascal。CPU entry、割り込み、syscall、コンテキストスイッチの低レベル境界には Pascal ソース内の inline assembler を使用します。

## ディレクトリ

- `src/kernel/`: Pascal カーネル
- `userland/init/`: 初期プロセス
- `userland/sh/`: シェル
- `userland/bin/`: 基本コマンド
- `userland/common/`: Pascal syscall ラッパー
- `tools/`: ホスト側 InitFS ビルダーとソース構造チェッカー
- `docs/`: 設計資料
