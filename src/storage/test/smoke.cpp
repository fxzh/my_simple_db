// smoke.cpp: 存储引擎 M1 冒烟测试(建表/插入/扫描/持久化/删表)
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "codec.h"
#include "storage.h"

using namespace st;

static int g_failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("PASS: %s\n", what);
    }
}

int main() {
    const std::string dir = "smoke_data";
    std::filesystem::remove_all(dir);

    try {
        // 类型名解析
        {
            ColType t;
            uint16_t n = 0;
            check(parse_column_type("int", &t, &n) && t == ColType::Int, "parse int");
            check(parse_column_type("bigint", &t, &n) && t == ColType::BigInt, "parse bigint");
            check(parse_column_type("double", &t, &n) && t == ColType::Double, "parse double");
            check(parse_column_type("float", &t, &n) && t == ColType::Float, "parse float");
            check(parse_column_type("char", &t, &n) && t == ColType::Char && n == 1, "parse char default 1");
            check(parse_column_type("char(8)", &t, &n) && t == ColType::Char && n == 8, "parse char(8)");
            check(parse_column_type("varchar", &t, &n) && t == ColType::VarChar && n == 0, "parse varchar dynamic");
            check(parse_column_type("varchar(20)", &t, &n) && t == ColType::VarChar && n == 20, "parse varchar(20)");
            check(!parse_column_type("char(0)", &t, &n), "reject char(0)");
            check(!parse_column_type("char(abc)", &t, &n), "reject non-digit length");
            check(!parse_column_type("char(3", &t, &n), "reject missing paren");
            check(!parse_column_type("int(5)", &t, &n), "reject length on fixed type");
            check(!parse_column_type("blob", &t, &n), "reject unknown type");
        }

        {
            Database db(dir);
            db.open();

            std::vector<ColumnSpec> cols = {
                    {"id", ColType::Int, 0},
                    {"name", ColType::VarChar, 16},
            };
            db.create_table("t", cols);

            // 重复建表报错
            bool dup = false;
            try {
                db.create_table("t", cols);
            } catch (const std::runtime_error&) {
                dup = true;
            }
            check(dup, "duplicate create_table throws");

            // 值与类型不匹配报错
            bool mismatch = false;
            try {
                db.insert("t", {Value{int64_t{1}}, Value{int64_t{2}}});
            } catch (const std::runtime_error&) {
                mismatch = true;
            }
            check(mismatch, "type mismatch insert throws");

            db.insert("t", {Value{int64_t{1}}, Value{std::string{"alice"}}});
            db.insert("t", {Value{int64_t{2}}, Value{std::string{"bob"}}});
            // 跨页: 一条记录塞满首个数据页后触发扩展
            for (int i = 0; i < 400; ++i) {
                db.insert("t", {Value{int64_t{3}}, Value{std::string{"spam"}}});
            }
            check(db.row_count("t") == 402, "insert 402 rows");

            // float/char 列: 往返 + 越界拒绝
            db.create_table("t2", {
                    {"score", ColType::Float, 0},
                    {"grade", ColType::Char, 3},
            });
            db.insert("t2", {Value{0.5}, Value{std::string{"A"}}});
            db.insert("t2", {Value{-1.25}, Value{std::string{"XYZ"}}});
            bool float_overflow = false;
            try {
                db.insert("t2", {Value{3.5e38}, Value{std::string{"A"}}});
            } catch (const std::runtime_error&) {
                float_overflow = true;
            }
            bool char_overlong = false;
            try {
                db.insert("t2", {Value{1.0}, Value{std::string{"long"}}});
            } catch (const std::runtime_error&) {
                char_overlong = true;
            }
            check(float_overflow, "float overflow insert throws");
            check(char_overlong, "char overlong insert throws");
            db.close();
        }

        {
            Database db(dir);
            db.open();
            check(db.row_count("t") == 402, "persist after reopen");

            size_t n = 0;
            bool order_ok = true;
            {
                auto s = db.scan("t");
                Row r;
                while (s->next(&r)) {
                    ++n;
                    if (n == 1) {
                        order_ok = std::get<int64_t>(r.values[0]) == 1 &&
                                              std::get<std::string>(r.values[1]) == "alice";
                    }
                    if (n == 2) {
                        order_ok = order_ok && std::get<int64_t>(r.values[0]) == 2 &&
                                              std::get<std::string>(r.values[1]) == "bob";
                    }
                }
            }
            check(n == 402, "scan returns 402 rows");
            check(order_ok, "scan row order and values");

            size_t n2 = 0;
            bool t2_ok = true;
            {
                auto s = db.scan("t2");
                Row r;
                while (s->next(&r)) {
                    ++n2;
                    if (n2 == 1) {
                        t2_ok = std::get<double>(r.values[0]) == 0.5 &&
                                        std::get<std::string>(r.values[1]) == "A";
                    }
                    if (n2 == 2) {
                        t2_ok = t2_ok && std::get<double>(r.values[0]) == -1.25 &&
                                 std::get<std::string>(r.values[1]) == "XYZ";
                    }
                }
            }
            check(n2 == 2, "scan t2 returns 2 rows");
            check(t2_ok, "scan t2 float/char values");

            // drop 后再访问报错, 且重启后仍不存在
            db.drop_table("t");
            bool gone = false;
            try {
                db.row_count("t");
            } catch (const std::runtime_error&) {
                gone = true;
            }
            check(gone, "drop_table removes table");
            check(!std::filesystem::exists(dir + "/t_1.dat"), "drop_table removes data file");
            check(std::filesystem::exists(dir + "/t_2.dat"), "drop_table keeps other tables' data file");
            db.close();
        }

        {
            Database db(dir);
            db.open();
            bool gone2 = false;
            try {
                db.row_count("t");
            } catch (const std::runtime_error&) {
                gone2 = true;
            }
            check(gone2, "drop persists after reopen");
            db.close();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "EXCEPTION: %s\n", e.what());
        ++g_failures;
    }

    std::filesystem::remove_all(dir);
    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}