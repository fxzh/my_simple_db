// test_sql.cpp, 用法: test_sql <case.sql> <expected.out>
// 以 client -a -c 的 stdout 与期望文件全文一致为唯一通过判据;
// 失败时输出行级 diff、client stderr 与服务端日志摘录;
// 每次运行将实际输出落盘, 不一致时另存 diff(仅 diff 内容本身):
//   <kOutDir>/<功能目录>_<stem>.actual / .diff
// 退出码: 0 一致 / 1 不一致或超时 / 2 用法与文件读取错误
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "common/process.hpp"
#include "common/temp_dir.hpp"
#include "e2e/test_config.hpp"

// 编译期注入: 被测二进制目录与跨用例状态目录、测试产物目录
constexpr const char* kBinDir = MSDB_BIN_DIR;
constexpr const char* kStateDir = MSDB_STATE_DIR;
constexpr const char* kOutDir = MSDB_OUT_DIR;

// 读文件全文(二进制), 失败返回 false
static bool read_file(const std::string& path, std::string& content)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// 写文件全文(二进制), 失败返回 false
static bool write_file(const std::string& path, const std::string& content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return !out.fail();
}

// 按 '\n' 切行并丢弃行尾换行, 供 diff 展示
static std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string line;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(std::move(line));
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    if (!line.empty()) {
        lines.push_back(std::move(line));
    }
    return lines;
}

// 行级 diff: LCS 对齐, '-' 期望独有 / '+' 实际独有 / ' ' 一致
static std::string render_diff(const std::vector<std::string>& exp, const std::vector<std::string>& act)
{
    const std::size_t n = exp.size();
    const std::size_t m = act.size();
    std::vector<std::vector<int>> lcs(n + 1, std::vector<int>(m + 1, 0));
    for (std::size_t i = n; i-- > 0;) {
        for (std::size_t j = m; j-- > 0;) {
            if (exp[i] == act[j]) {
                lcs[i][j] = lcs[i + 1][j + 1] + 1;
            } else {
                lcs[i][j] = std::max(lcs[i + 1][j], lcs[i][j + 1]);
            }
        }
    }
    std::string diff;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < n && j < m) {
        if (exp[i] == act[j]) {
            diff += "  " + exp[i] + "\n";
            ++i;
            ++j;
        } else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
            diff += "- " + exp[i] + "\n";
            ++i;
        } else {
            diff += "+ " + act[j] + "\n";
            ++j;
        }
    }
    for (; i < n; ++i) {
        diff += "- " + exp[i] + "\n";
    }
    for (; j < m; ++j) {
        diff += "+ " + act[j] + "\n";
    }
    return diff;
}

// 产物名: <功能目录>_<stem>, 如 .../dml/cases/insert.sql -> dml_insert, 防跨目录重名
static std::string artifact_stem(const std::string& sql_path)
{
    const std::filesystem::path p(sql_path);
    return p.parent_path().parent_path().filename().string() + "_" + p.stem().string();
}

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::cerr << "用法: " << argv[0] << " <case.sql> <expected.out>" << std::endl;
        return 2;
    }
    std::string sql_text;
    std::string expected;
    if (!read_file(argv[1], sql_text)) {
        std::cerr << "读取用例失败: " << argv[1] << std::endl;
        return 2;
    }
    if (!read_file(argv[2], expected)) {
        std::cerr << "读取期望失败: " << argv[2] << std::endl;
        return 2;
    }

    const std::string client = (std::filesystem::path(kBinDir) / "client").string();
    const std::vector<std::string> argv_list = {client, "-a", "-h", "127.0.0.1", "-p",
                                                std::to_string(kTestPort), "-c", sql_text};
    tcommon::ProcessResult r;
    std::string error;
    if (!tcommon::run_process(argv_list, 15000, r, error)) {
        std::cerr << "run_process 基建错误: " << error << std::endl;
        return 2;
    }

    // 产物落盘: 实际输出始终保存(超时为部分输出也照存), 不一致时另存 diff,
    // 一致时清掉上轮失败残留的旧 diff; 落盘失败仅告警, 不影响判定
    std::error_code ec;
    std::filesystem::create_directories(kOutDir, ec);
    const std::string stem = artifact_stem(argv[1]);
    const std::string actual_path = (std::filesystem::path(kOutDir) / (stem + ".actual")).string();
    const std::string diff_path = (std::filesystem::path(kOutDir) / (stem + ".diff")).string();
    if (!write_file(actual_path, r.out)) {
        std::cerr << "警告: 实际输出落盘失败: " << actual_path << std::endl;
    }

    if (!r.timed_out && r.out == expected) {
        std::error_code rm_ec;
        std::filesystem::remove(diff_path, rm_ec);  // 旧 diff 不存在时无副作用
        return 0;
    }

    const std::string diff = render_diff(split_lines(expected), split_lines(r.out));
    if (!write_file(diff_path, diff)) {
        std::cerr << "警告: diff 落盘失败: " << diff_path << std::endl;
    }
    std::cerr << "diff 不一致: " << argv[1] << std::endl;
    if (r.timed_out) {
        std::cerr << "client 超时被杀" << std::endl;
    }
    if (!r.err.empty()) {
        std::cerr << "-- client stderr --" << std::endl << r.err;
    }
    std::cerr << "-- 期望(-) / 实际(+) --" << std::endl << diff;
    const std::string log = (std::filesystem::path(kStateDir) / "data" / "simple.log").string();
    std::string tail;
    if (tcommon::read_file_tail(log, 4096, tail, error)) {
        std::cerr << "-- 服务端日志尾 --" << std::endl << tail;
    }
    return 1;
}
