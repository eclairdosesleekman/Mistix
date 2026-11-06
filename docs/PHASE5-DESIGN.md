# Mistix Phase 5 設計メモ

## 目的

Phase 5 は、Phase 4 のプロセス／COW／IPC カーネルの上に、UNIX ライクなファイル操作層と最低限の対話型ユーザーランドを追加する。

## 内部データモデル

### VFS node

ノードはディレクトリ、通常ファイル、console、null、serial のいずれか。初期段階では固定テーブルを使い、ノード名と親ノード index で木構造を表現する。

### Open-file description

FD は open-file description の参照であり、FD の複製や fork/spawn は参照数を増やす。同じ description を共有する FD はファイル位置を共有する。Pipe reader/writer の寿命も description の最後の参照が閉じられた時点で更新する。

### Process FD table

各プロセスの `TVFSProcessFiles` が FD 0..15 を保持し、初期状態では 0/1/2 を console に関連付ける。fork はすべての FD を共有 description 参照として複製する。`spawn_path` は指定された入力 FD と出力 FD を子の 0 と 1 に複製し、それ以外の FD 3 以上を閉じる。

### Pipe

Pipe は単一 CPU 初期実装の固定長 FIFO。読み書きは非ブロッキングであり、`-EAGAIN` を返す。writer description がすべて閉じられてバッファが空なら read は EOF を返す。reader description がなくなった write は `-EPIPE` を返す。

## MFS1 archive

Header:

- 4 bytes: ASCII `MFS1`
- 4 bytes: version (`1`, little endian)
- 4 bytes: entry count (`little endian`)

Each entry:

- 2 bytes: path length
- 2 bytes: reserved
- 8 bytes: data length
- path bytes (ASCII, absolute canonical path)
- file data bytes

The parser checks count, path length, image bounds and canonical path components before adding nodes.

## マイクロカーネル移行計画

現段階では VFS syscall の実装をカーネル内に置く。ただし、VFS node／open-file description／pipe の操作 API はプロセス管理コードと分離している。次の移行では syscall 層を IPC client に置き換え、VFS server をユーザーランドで動かす。ブロックデバイスと永続 FS は VFS server の下流サービスとして追加する。
