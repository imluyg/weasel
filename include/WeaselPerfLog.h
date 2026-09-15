#pragma once
// 临时的、按环境变量开关的性能打点工具。
//
// 用法：设置环境变量 WEASEL_PAINT_LOG=<路径> 后启动宿主进程（例如记事本），
// 绘制路径会往该文件追加每帧一行分段耗时。**未设置该变量时所有开销只有一次
// 静态的 getenv 结果判断**，不写文件、不取时间戳，可以安全地留在发布构建里。
//
// 之所以做成显式开关而不是无条件打点：上一轮的教训是「带打点的库拿去跑微基准，
// 把日志开销算进了被测项」，所以这里默认全关，且日志只落在绘制路径外层。

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace weasel {
namespace perf {

class PerfLog {
 public:
  static PerfLog& Instance() {
    static PerfLog inst;
    return inst;
  }

  bool enabled() const { return enabled_; }

  // 每个宿主进程第一次调用时打开文件，追加写入。
  void Write(const char* line) {
    if (!enabled_ || file_ == nullptr)
      return;
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    fprintf(file_, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);
    fputs(line, file_);
    fputc('\n', file_);
  }

  static double Ms(ULONGLONG t0, ULONGLONG t1) {
    static const double freq = [] {
      LARGE_INTEGER f;
      ::QueryPerformanceFrequency(&f);
      return (double)f.QuadPart;
    }();
    return (double)(t1 - t0) * 1000.0 / freq;
  }

  static ULONGLONG Now() {
    LARGE_INTEGER c;
    ::QueryPerformanceCounter(&c);
    return (ULONGLONG)c.QuadPart;
  }

 private:
  PerfLog() {
    char buf[MAX_PATH] = {0};
    DWORD n = ::GetEnvironmentVariableA("WEASEL_PAINT_LOG", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      // 每个进程一份，避免多个宿主互相覆盖
      char path[MAX_PATH + 32];
      sprintf_s(path, "%s.%lu", buf, (unsigned long)::GetCurrentProcessId());
      fopen_s(&file_, path, "a");
      enabled_ = (file_ != nullptr);
    }
  }
  ~PerfLog() {
    if (file_)
      fclose(file_);
  }
  PerfLog(const PerfLog&) = delete;
  PerfLog& operator=(const PerfLog&) = delete;

  bool enabled_ = false;
  FILE* file_ = nullptr;
};

}  // namespace perf
}  // namespace weasel
