<p align="center">
  <a href="./README.md">English</a> | 日本語
</p>

# TriggerOn Server

マルチプレイヤー FPS「TriggerOn」のサーバー権威型ゲームサーバー。Linux 上で動作し、Docker によるデプロイに対応。

## 主な機能

- **サーバー権威型** — 物理演算、移動処理、当たり判定をすべてサーバー側で実行
- **固定ティックレート 32 Hz** — アキュムレータ方式による安定した更新間隔
- **ENet による UDP 通信**
- **チーム戦** — RED vs BLUE、チームごとに異なる武器性能
- **最大 4 人** 同時接続
- **Docker 対応** — マルチステージビルドによる軽量イメージ

## 動作環境

**ネイティブビルド:**
- Linux（Debian / Ubuntu 推奨）
- C++17 対応の g++
- make

**Docker ビルド:**
- Docker

## クイックスタート

```bash
make
./game_server --port=7777
```

## Docker

```bash
# ビルド
docker build -t triggeron-server .

# 実行
docker run -p 7777:7777/udp triggeron-server
```

### Docker Compose

Docker Hub から公開イメージを取得して実行する場合:

```yaml
# docker-compose.yml
services:
  game-server:
    image: pisto3/triggeron_game_server:latest
    ports:
      - "7777:7777/udp"
    restart: unless-stopped
```

```bash
docker compose up -d
```

## コマンドラインオプション

| オプション | デフォルト値 | 説明 |
|-----------|------------|------|
| `--port=XXXX` | 7777 | 待ち受け UDP ポート |

## サーバーパラメータ

### 全般

| パラメータ | 値 |
|-----------|-----|
| ティックレート | 32 Hz（31.25 ms / tick） |
| 最大プレイヤー数 | 4 |

## アーキテクチャ

本サーバーは **サーバー権威型モデル** を採用しています。

- **クライアント → サーバー**: `InputCmd`（24 バイト） — 移動入力、視点角度、ボタン操作のみ送信
- **サーバー → クライアント**: `Snapshot` — 位置、速度、HP、状態フラグなどのワールドステートを配信

クライアントからの座標は一切信頼せず、すべての移動・衝突判定・戦闘処理をサーバー側で算出します。

主要コンポーネント:
- `ENetServerNetwork` — UDP ピア管理、パケットルーティング
- `GameServer` — プレイヤーごとの状態管理（`unordered_map<uint8_t, PlayerData>`）、物理ティック、戦闘処理
- `server_collision.h` — ヘッダーオンリーの純粋数学ベース当たり判定ライブラリ（DirectXMath 非依存）
- `server_raycast.h` — 射撃ヒット判定用レイキャスト

## ディレクトリ構成

```
main.cpp                        エントリーポイント、シグナルハンドリング、メインループ
Network/
├── game_server.h/cpp           ゲームロジック、物理演算、戦闘処理
├── enet_server_network.h/cpp   ENet UDP サーバー実装
├── net_common.h                プロトコル構造体（クライアントと共有）
├── net_packet.h                パケットタイプ列挙型（クライアントと共有）
├── i_network.h                 ネットワーク抽象インターフェース（クライアントと共有）
├── map_colliders.h             マップジオメトリ（クライアントと共有）
├── server_collision.h          当たり判定（サーバー専用）
└── server_raycast.h            レイキャストヒット判定（サーバー専用）
ThirdParty/
└── enet/                       ENet ネットワークライブラリ（ソースからビルド）
```
