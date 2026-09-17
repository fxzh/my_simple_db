# test 目录说明

测试目录，顶层 `ENABLE_TESTING=ON`（默认 OFF）时构建，依赖系统 googletest。

## 分层

- L1 工具链 e2e：`e2e/initdb`、`e2e/serverctl`，起子进程断言退出码/目录状态/端口
- L2 SQL e2e：`e2e/sql`，golden file 全文 diff

## 目录

- `common/`：e2e 基建静态库 `test_common`——temp_dir（唯一临时目录，失败保留现场）、
  process（spawn/超时 kill/stdout+stderr 捕获）、net_probe（端口探测、就绪轮询）；
  `common_selftest.cpp` 为基建自测
- `e2e/test_config.hpp`：测试专用端口 `kTestPort = 18432`，被占用直接判负，禁止换端口重试
- `e2e/sql/<tier>/cases/xx.sql` + `expected/xx.out`：输入与期望成对出现

## 门控链（fixture）

`DB_READY(initdb_ok) → SRV_UP(server_start) → SQL_DDL_CREATE → SQL_DDL_DROP → SQL_DML_INSERT → sql_dml_delete`

- 上游失败下游整体 Not Run；`server_stop` 只挂 `FIXTURES_CLEANUP`，必须幂等（容忍服务未启动）
- SQL 链上每个用例除前驱 fixture 外必须同时挂 `SRV_UP`，保证 `server_stop` 在整条链之后收尾
- 异常路径用例（重复 start、未运行时 stop 等）不挂 fixture，独立执行
- 同一次 ctest 内 SQL 状态累积：按 fixture 链顺序串行，禁用 `--gtest_shuffle`

## SQL 用例约定

- `test_sql <case.sql> <expected.out>` 驱动 `client -c`，stdout 全文一致为唯一通过判据
- 一个 .sql 文件对应一个 add_test；新增用例在 `CMakeLists.txt` 追加 add_test 并挂 fixture
- 后续 tier 依赖前序 tier 的库表状态（如 02_dml 依赖 01_ddl 建的表）
