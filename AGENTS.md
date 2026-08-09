## プロジェクト概要

- Ubuntu24.04, WRX80やDell T7610の環境がローカルに存在し、この環境下でintel Omni-Path 100 seriesが最大限のパフォーマンス(≈最大限の帯域幅)を発揮できるようにする。
- 最終的にはGPLライセンスでGitHubに挙げることを想定。
- 細かいgit管理やCI/CDは考えず、雑に進める。

## 役割と正本

- 初回の行動開始時に`docs/plans/main-plan.md`を必ず読む。
- `docs/plans/main-plan.md`には重要なproduct・architecture・compatibility上の決定、開発計画と順序、進捗、未解決事項だけを記録し、恒久的な実行手順を重複させない。
- 調査と実装はsubagentが担当する。main agentは計画の作成・編集、subagentの指示・監視、全体調整、Git操作、特権操作、その他の雑務を担当する。
- 重いshell commandは開始から15分を最初の状況確認時点とし、その後も少なくとも15分ごとにprocessの生存、出力の更新、resource使用、停止兆候を確認する。正常に進行していれば終了させず継続し、確認結果をユーザーへ報告する。15分での一律timeoutは禁止し、明示された時間制約またはtest contract固有のtimeoutだけを使用する。



## Repositoryと保護対象

- `docs/plans/active`には未完了のplan、`docs/plans/archive`には完了または放棄したplan、`docs/history`には詳細な変更履歴をMarkdownで置く。各directoryは`YYYY/MM/1-10`、`11-20`、`21-`の区分を使う。
- `docs/plans/main-plan.md`以外のplanは対応するhistoryを、historyは対応するplanを、それぞれ末尾からlinkする。
- `.gitignore`への新規行の追記は、事前許可なく行える。
  - 既存行の変更・削除・移動は、変更内容についてユーザーから事前に許可を得る。
  - ユーザーが手動で行った変更は、内容をreviewしたうえで、追加の許可なくcommit・pushできる。
- `AGENTS.md`を変更した場合はユーザーへ確認する。

## 特権操作

- 無人での進行を優先する。特権操作はmain agentがtask scope内で対象と効果を限定。
- 専用local hostでは、`homelab1`への`NOPASSWD: ALL`を意図的に許可し、そのriskを受容する。この権限はtask scopeを拡張せず、対象確認や破壊的操作の安全確認も省略しない。

## Canonical documents

- 全体の決定・計画・進捗・未解決事項: `docs/plans/main-plan.md`
