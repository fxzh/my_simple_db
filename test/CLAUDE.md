# test 目录说明

测试目录，顶层 `ENABLE_TESTING=ON`（默认 OFF）时构建，依赖系统 googletest。

## 分层

- L0 模块冒烟：`storage/test_storage.cpp`，gtest 直测 storage 静态库（页删除整理/增删扫/重开持久化）
- L1 工具链 e2e：`e2e/initdb`、`e2e/serverctl`，起子进程断言退出码/目录状态/端口
- L2 SQL e2e：`e2e/sql`，全文 diff

## 目录

- `common/`：测试基建静态库 `test_common`——temp_dir（唯一临时目录，失败保留现场）、
  process（spawn/超时 kill/stdout+stderr 捕获）、net_probe（端口探测、就绪轮询）；
  `common_selftest.cpp` 为基建自测
- `storage/`：存储引擎模块冒烟 `test_storage.cpp`，只含 SQL e2e 覆盖不到的验证
  （页级不变量、重开持久化、单行删除语义），独立临时目录，不挂 fixture
- `e2e/test_config.hpp`：测试专用端口 `kTestPort = 18432`，被占用直接判负，禁止换端口重试
- `e2e/sql/<功能>/cases/xx.sql` + `expected/xx.out`：输入与期望成对出现，功能目录按功能名命名，不编码依赖顺序

## 门控链（fixture）

- 上游失败下游整体 Not Run；`server_stop` 只挂 `FIXTURES_CLEANUP`，必须幂等（容忍服务未启动）
- SQL 用例只挂 `SRV_UP`，相互独立；所有挂 `SRV_UP` 的用例结束后 `server_stop` 收尾
- 异常路径用例（重复 start、未运行时 stop 等）不挂 fixture，独立执行
- SQL 用例共享同一 server 实例，按注册顺序串行

## SQL 用例约定

- `test_sql <case.sql> <expected.out>` 驱动 `client -a -c`，stdout 全文一致为唯一通过判据（`-a` 逐行回显原始 SQL）
- 一个 .sql 文件对应一个 add_test；新增用例在 `CMakeLists.txt` 追加 add_test 并挂 fixture
- 每个 .sql 自管库表生命周期：开头建表、结尾删表，只依赖 SRV_UP，不依赖其他用例的库表状态
- 项目正在快速开发中，预期输出修改是常有的事
