#pragma once
// 临时的、按环境变量开关的打点工具。
//
// 用法：
//   WEASEL_PAINT_LOG=<路径>  绘制路径每帧追加一行分段耗时（PerfLog）
//   WEASEL_POS_LOG=<路径>    候选窗定位路径每次上报/摆位追加一行（PosLog）
// 变量未设置时所有开销只有一次静态的 getenv 结果判断，不写文件、不取时间戳，
// 可以安全地留在发布构建里。
//
// 之所以做成显式开关而不是无条件打点：上一轮的教训是「带打点的库拿去跑微基准，
// 把日志开销算进了被测项」，所以这里默认全关，且日志只落在被测路径外层。
//
// 实现约定：
// - 每个宿主进程一份文件（文件名后缀 pid），避免多个宿主互相覆盖；
// - PosLog 每行 fflush，进程被杀/崩溃后日志仍然完整；PerfLog 不 flush，
//   保持原有绘制打点的测量口径不变（每帧一次 flush 会污染帧时）。

#include <windows.h>

#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace weasel {
namespace perf {

// 按环境变量开关的追加式日志底座。
class EnvLog {
 public:
  bool enabled() const { return enabled_; }

  void Write(const char* line) {
    if (!enabled_ || file_ == nullptr)
      return;
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    std::lock_guard<std::mutex> lock(mutex_);
    fprintf(file_, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);
    fputs(line, file_);
    fputc('\n', file_);
    // PerfLog 不 flush：每帧 flush 会改变被测帧的成本。
    if (flush_)
      fflush(file_);
  }

  // printf 风格，方便打多字段的定位日志。
  void Writef(const char* fmt, ...) {
    if (!enabled_ || file_ == nullptr)
      return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    Write(buf);
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

 protected:
  explicit EnvLog(const char* env_name, bool flush) : flush_(flush) {
    char buf[MAX_PATH] = {0};
    DWORD n = ::GetEnvironmentVariableA(env_name, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      // 每个进程一份，避免多个宿主互相覆盖
      char path[MAX_PATH + 32];
      sprintf_s(path, "%s.%lu", buf, (unsigned long)::GetCurrentProcessId());
      // 用 _fsopen(..., _SH_DENYNO)：fopen 的默认共享模式会让正在运行的主机
      // 的日志读不出来（EACCES），而「边复现边 tail」正是这个工具的用法。
      file_ = _fsopen(path, "a", _SH_DENYNO);
      enabled_ = (file_ != nullptr);
    }
  }
  ~EnvLog() {
    if (file_)
      fclose(file_);
  }
  EnvLog(const EnvLog&) = delete;
  EnvLog& operator=(const EnvLog&) = delete;

  bool enabled_ = false;
  bool flush_ = false;
  FILE* file_ = nullptr;
  std::mutex mutex_;
};

// 绘制帧打点（WEASEL_PAINT_LOG）。
class PerfLog : public EnvLog {
 public:
  static PerfLog& Instance() {
    static PerfLog inst;
    return inst;
  }

 private:
  PerfLog() : EnvLog("WEASEL_PAINT_LOG", /*flush=*/false) {}
  PerfLog(const PerfLog&) = delete;
  PerfLog& operator=(const PerfLog&) = delete;
};

// 候选窗定位打点（WEASEL_POS_LOG）。
//
// 打点位置（前缀 [tsf] = 客户端 TSF 侧，[ui] = 面板侧，两侧可能在不同进程，
// 看文件名后缀的 pid）：
//   [tsf] ext    每次向 TSF 取合成区矩形（未经 _SetCompositionPosition 去重）
//   [tsf] set    每次上报位置；dup=1 表示被 B4 去重缓存吞掉（不发 IPC、不动窗）
//   [tsf] start/abort/end  合成开始/中止/结束
//   [ui]  move   MoveTo() 收到位置后走了哪个分支
//   [ui]  repo   _RepositionWindow() 最终算出的屏幕坐标与工作区
//   [ui]  refresh / create / destroy  面板刷新与窗口生命周期
//   [ui]  ip-drop  UI::UpdateInputPosition() 因面板窗口不存在而丢弃
//   [cand] startui / endui / destroy / destroyall  CCandidateList 的 UI 会话
//         生命周期；startui 带 uiStarted 与窗口是否存在，出现
//         "recover: uiStarted=1 but no panel window" 就说明那条
//         "Destroy 销毁窗口但标志不复位"的隐患真的被触发了。
//   [sess] add / remove / client-gone  服务端会话表（total= 当前会话数）。
//         "client-gone" 是管道断开时的回收：如果 total 只涨不落，就是泄漏。
class PosLog : public EnvLog {
 public:
  static PosLog& Instance() {
    static PosLog inst;
    return inst;
  }

 private:
  PosLog() : EnvLog("WEASEL_POS_LOG", /*flush=*/true) {}
  PosLog(const PosLog&) = delete;
  PosLog& operator=(const PosLog&) = delete;
};

}  // namespace perf
}  // namespace weasel
