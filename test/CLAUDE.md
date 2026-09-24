# test
测试目录：顶层 ENABLE_TESTINGS=ON(默认 OFF)时构建，依赖系统 googletest

- 通过判据：test_sql 以 client -a -c 跑用例，stdout 全文一致
- 失败现场：temp_dir 失败时保留临时目录；实际输出与 diff 落盘到构建树 test/e2e_out/(<功能>_<stem>.actual / .diff，通过时清旧 .diff)
- SQL 用例统一挂 SRV_UP fixture：共享同一 server、按注册顺序串行，故用例必须相互独立；异常路径用例(重复 start、未运行时 stop 等)不挂 fixture 独立执行
- 用例编写要求见 ai_docs/testcase.md
