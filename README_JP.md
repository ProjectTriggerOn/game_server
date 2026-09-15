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
- **最大 10 人** 同時接続（5v5: RED 5 + BLUE 5）
- **ラグ補償** — プレイヤーごとに 64 ティック（2 秒）分の位置履歴を保持し、射線判定は射撃側が実際に見ていた時点のワールド（`InputCmd.viewTick` + サブティック補間）に巻き戻して行います
- **COD 型リコイル** — 見た目だけ跳ね上がるパンチ、実際に狙点がずれるキック、ブルーム（拡散の増大）をサーバー側で進行させ、スナップショットに載せて配信します。パターンは `fireCounter` のみから決まり（乱数なし）、クライアントとサーバーが独立に同じ弾道を算出します。`recoil_math.h` はクライアント側と同一内容をミラーしており、常に同期を保つ必要があります。
- **マッチ進行** — 参加者が 2 人揃うまで待機し、5 秒のカウントダウンを経て本戦へ。チームキルによるスコア（先取 10 キル）、制限時間 60 秒、全スナップショットに載る 8 件のキルフィードリング、勝利チームを含む終了ステート
- **マップ読み込み** — クライアントと共有する `.map` バイナリ形式を起動時に読み込み（`--map=`、失敗時はコンパイル済みジオメトリにフォールバック）、接続時に `MAP_INFO` を送信して当たり判定チェックサムで別マップのクライアントを検出します
- **受信フラッド対策** — ピアごとのトークンバケット、1 回のポーリングで処理するイベント数の上限、ホスト側パケットサイズ上限に加え、毎秒の受信数 / キュー / ティック時間をレポート（閾値は `Network/net_limits.h`）
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
./game_server --port=7777 --map=shipment.map
# または小さなエンジン検証用ボックス:
./game_server --port=7777 --map=default.map
```

## Docker

```bash
# ビルド
docker build -t triggeron-server .

# 実行
docker run -p 7777:7777/udp triggeron-server
```

イメージには `default.map` と `shipment.map` が `/app/` に同梱され、作業ディレクトリが `/app` のため既定の `--map=shipment.map` がそのまま解決されます。小さなエンジン検証用ボックスで動かす場合は `--map=default.map` を渡してください。別のマップで動かす場合は同梱名のいずれかにマウントしてください。

```bash
docker run -p 7777:7777/udp -v ./my.map:/app/shipment.map triggeron-server
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
| `--map=PATH` | `shipment.map` | シミュレートする `.map` ファイル（作業ディレクトリからの相対パス）。読み込めない場合はコンパイル済みマップ（`map_colliders.h`）にフォールバックしますが、そのチェックサムはどのクライアントとも一致しないため警告が出ます。 |

## サーバーパラメータ

### 全般

| パラメータ | 値 |
|-----------|-----|
| ティックレート | 32 Hz（31.25 ms / tick） |
| 最大プレイヤー数 | 10（RED 5 + BLUE 5） |
| 位置履歴（ラグ補償） | 64 ティック（2 秒） |

### マッチ

| パラメータ | 値 |
|-----------|-----|
| 開始に必要な人数 | 2（`MatchConfig::MIN_PLAYERS`） — 接続ピア数ではなく参加済みプレイヤー数 |
| 開始前カウントダウン | 5 秒 |
| スコア上限 | 10 キル |
| 制限時間 | 60 秒 |
| キルフィードリング | スナップショットあたり 8 件 |

フェーズ: `WAITING`（ウォームアップ — 移動・射撃可、スコアは動かない）→ `COUNTDOWN`
（スポーン地点で停止）→ `PLAYING` → `ENDED`。人数が下限を割ると、どのフェーズからでも
待機状態に戻ります。

### 受信制限（`Network/net_limits.h`）

| パラメータ | 値 |
|-----------|-----|
| ピアごとの入力レート | 1 ティックあたり 32 個補充（1024 個 / 秒）、バケット深さ 64 |
| `PollEvents` 1 回あたりのイベント数 | 4096 |
| パケットサイズ上限（ホスト設定） | 1024 バイト — MTU（1392）を下回るため、通常のパケットが断片化することはありません |

## アーキテクチャ

本サーバーは **サーバー権威型モデル** を採用しています。

- **クライアント → サーバー**: `InputCmd`（32 バイト） — 移動入力、視点角度、ボタン操作、観測中ティック（ラグ補償用）のみ送信
- **クライアント → サーバー**: `JOIN_REQUEST`（1 バイト） — マッチへの参加要求。ペイロードなし
- **サーバー → クライアント**: `Snapshot`（10 人時 784 バイト） — 位置、速度、HP、状態フラグ、リコイル、残弾、スコア、キルフィードを配信
- **サーバー → クライアント**: `MapInfo`（68 バイト） — マップ名と当たり判定チェックサム。接続時に 1 回送信

**接続しただけでは参加したことになりません。** クライアントはプロセス起動時に ENet ピアを
開き、プロセス終了まで保持し続けますが、その時間の大半、プレイヤーはメニュー画面にいます。
そのため接続直後のピアは観戦者として扱い、同じマップを読み込んでいるか照合するための
`MapInfo` だけを送ります。`JOIN_REQUEST` を受け取るまではスポーンさせず、スナップショット
も送らず、`MatchConfig::MIN_PLAYERS` にも数えません。この切り分けがないと、タイトル画面に
放置されたクライアントだけで開始人数の条件が満たされ、誰もいないマップで 60 秒のマッチが
最後まで進行してしまいます。

クライアントからの座標は一切信頼せず、移動・衝突判定・戦闘処理はすべてサーバー側で実行します。

主要コンポーネント:
- `ENetServerNetwork` — UDP ピア管理、パケットルーティング、ピアごとのレート制限と受信統計
- `GameServer` — プレイヤーごとの状態管理（`unordered_map<uint8_t, PlayerData>`）、物理ティック、戦闘処理、ラグ補償、マッチ / スコア管理
- `server_collision.h` — ヘッダーオンリーの純粋数学ベース当たり判定ライブラリ（DirectXMath 非依存）
- `server_raycast.h` — 射撃ヒット判定用レイキャスト
- `recoil_math.h` — リコイル計算（クライアント側と同一内容をミラー）

## テスト

`tools/run_sessiontest.sh` は、サーバーの起動から参加までの挙動をまとめて検証するスイート
です。ポートを確保できなかったサーバーが「ソケットを持たないまま黙って動き続ける」のでは
なく終了コード 0 以外で終了すること、および接続と参加の切り分けが保たれていること
（`JOIN_REQUEST` を送っていないピアはスポーンもスナップショットもなし、送ったピアは
プレイヤーになる、接続しただけのピアでは `MIN_PLAYERS` に到達しない、実際に 2 人が参加
すればマッチは開始する）を確認します。

```bash
make
g++ -std=c++17 -O2 -INetwork -IThirdParty/enet/include tools/session_test.cpp \
    -o session_test -LThirdParty/enet/lib -lenet -lpthread
tools/run_sessiontest.sh          # 失敗があれば非ゼロ終了
```

## 負荷テスト

`tools/flood_client.cpp` は単一クライアントによるフラッドを計測するためのヘッドレス ENet クライアントです。`--rate N` は通常レートの「被害者」として動作しつつ観測したスナップショット間隔を出力し、`--flood` は入力パケットを可能な限り高速に送信、`--junk` は不正長パケットを送信します。製品コードではなくテスト用ツールです。

```bash
g++ -std=c++17 -O2 -INetwork -IThirdParty/enet/include tools/flood_client.cpp \
    -o floodtest -LThirdParty/enet/lib -lenet -lpthread
./floodtest --rate 60 --secs 30 --label victim
./floodtest --flood --secs 30 --label attacker
```

## ディレクトリ構成

```
main.cpp                        エントリーポイント、シグナルハンドリング、メインループ、ステータス出力
server_log.h                    タイムスタンプ付き標準出力ロガー
default.map / shipment.map      実行ファイルと同じ階層に置くマップ（Docker では /app/）
Network/
├── game_server.h/cpp           ゲームロジック、物理演算、戦闘処理、ラグ補償、マッチ管理
├── enet_server_network.h/cpp   ENet UDP サーバー実装、受信レート制限
├── net_common.h                プロトコル構造体（クライアントと共有）
├── net_packet.h                パケットタイプ列挙型 + MapInfo（クライアントと共有）
├── net_limits.h                フラッド対策の閾値（サーバー専用）
├── i_network.h                 ネットワーク抽象インターフェース（クライアントと共有）
├── map_io.h                    .map バイナリ形式（クライアントと共有）
├── map_colliders.h             コンパイル済みフォールバックマップ（クライアントと共有）
├── recoil_math.h               リコイル計算（クライアント側とミラー）
├── server_collision.h          当たり判定（サーバー専用）
└── server_raycast.h            レイキャストヒット判定（サーバー専用）
tools/
├── flood_client.cpp            ヘッドレス負荷 / フラッドテストクライアント（非同梱）
├── session_test.cpp            接続 / 参加を検証するヘッドレスクライアント（非同梱）
├── run_sessiontest.sh          起動 / 参加まわりの検証スイート
└── run_floodtest.sh            フラッド前後比較シナリオ
ThirdParty/
└── enet/                       ENet ネットワークライブラリ（ソースからビルド）
.github/workflows/              Docker イメージのビルド・公開（main / dev）
```
