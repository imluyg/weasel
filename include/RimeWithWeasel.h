#pragma once
#include <WeaselIPC.h>
#include <WeaselUI.h>
#include <map>
#include <string>
#include <mutex>

#include <rime_api.h>

struct CaseInsensitiveCompare {
  bool operator()(const std::string& str1, const std::string& str2) const {
    std::string str1Lower, str2Lower;
    std::transform(str1.begin(), str1.end(), std::back_inserter(str1Lower),
                   [](char c) { return std::tolower(c); });
    std::transform(str2.begin(), str2.end(), std::back_inserter(str2Lower),
                   [](char c) { return std::tolower(c); });
    return str1Lower < str2Lower;
  }
};

typedef std::map<std::string, bool> AppOptions;
typedef std::map<std::string, AppOptions, CaseInsensitiveCompare>
    AppOptionsByAppName;

struct SessionStatus {
  SessionStatus() : style(weasel::UIStyle()), __synced(false), session_id(0) {
    // 这里原来写的是 RIME_STRUCT(RimeStatus, status)：该宏在**函数体里声明一个同名
    // 局部变量**，所以成员 status 从来没被初始化过 —— 读它的 is_ascii_mode 之类的
    // 字段拿到的是未初始化内存（真机上 [sess] 语义就可能随机漂）。显式清零并设好
    // data_size，让它有确定的默认值。
    status = RimeStatus();
    RIME_STRUCT_INIT(RimeStatus, status);
  }
  weasel::UIStyle style;
  RimeStatus status;
  bool __synced;
  RimeSessionId session_id;
};
typedef std::map<DWORD, SessionStatus> SessionStatusMap;
typedef DWORD WeaselSessionId;
class RimeWithWeaselHandler : public weasel::RequestHandler {
 public:
  RimeWithWeaselHandler(weasel::UI* ui);
  virtual ~RimeWithWeaselHandler();
  virtual void Initialize();
  virtual void Finalize();
  virtual DWORD FindSession(WeaselSessionId ipc_id);
  virtual DWORD AddSession(LPWSTR buffer, EatLine eat = 0);
  virtual DWORD RemoveSession(WeaselSessionId ipc_id);
  virtual void DropDetachedSessions(const std::vector<DWORD>& session_ids);
  virtual BOOL ProcessKeyEvent(weasel::KeyEvent keyEvent,
                               WeaselSessionId ipc_id,
                               EatLine eat);
  virtual void CommitComposition(WeaselSessionId ipc_id);
  virtual void ClearComposition(WeaselSessionId ipc_id);
  virtual void SelectCandidateOnCurrentPage(size_t index,
                                            WeaselSessionId ipc_id);
  virtual bool HighlightCandidateOnCurrentPage(size_t index,
                                               WeaselSessionId ipc_id,
                                               EatLine eat);
  virtual bool ChangePage(bool backward, WeaselSessionId ipc_id, EatLine eat);
  virtual void FocusIn(DWORD param, WeaselSessionId ipc_id);
  virtual void FocusOut(DWORD param, WeaselSessionId ipc_id);
  virtual void UpdateInputPosition(RECT const& rc, WeaselSessionId ipc_id);
  virtual void StartMaintenance();
  virtual void EndMaintenance();
  virtual void SetOption(WeaselSessionId ipc_id,
                         const std::string& opt,
                         bool val);
  virtual void UpdateColorTheme(BOOL darkMode);

  void OnUpdateUI(std::function<void()> const& cb);

 private:
  void _Setup();
  bool _IsDeployerRunning();
  void _UpdateUI(WeaselSessionId ipc_id);
  void _LoadSchemaSpecificSettings(WeaselSessionId ipc_id,
                                   const std::string& schema_id);
  void _LoadAppInlinePreeditSet(WeaselSessionId ipc_id,
                                bool ignore_app_name = false);
  bool _ShowMessage(weasel::Context& ctx, weasel::Status& status);
  bool _Respond(WeaselSessionId ipc_id, EatLine eat);
  void _ReadClientInfo(WeaselSessionId ipc_id, LPWSTR buffer);
  void _GetCandidateInfo(weasel::CandidateInfo& cinfo, RimeContext& ctx);
  void _GetStatus(weasel::Status& stat,
                  WeaselSessionId ipc_id,
                  weasel::Context& ctx);
  void _GetContext(weasel::Context& ctx, RimeSessionId session_id);
  void _UpdateShowNotifications(RimeConfig* config, bool initialize = false);

  void _UpdateInlinePreeditStatus(WeaselSessionId ipc_id);

  RimeSessionId to_session_id(WeaselSessionId ipc_id) {
    // 以前这里是 m_session_status_map[ipc_id]：键不存在时 operator[] 会**插入**一份
    // 默认 SessionStatus（session_id=0）且不打任何日志。真机上的症状是 [sess] total
    // 只涨不落、其中一部分增长没有对应的 [sess] add（幽灵条目），并且会一直占着
    // _GenerateNewWeaselSessionId 用来算新 id 的那张表。现在只查不插，查不到就返回
    // 0（= "没有这个会话"），并把这次查询记进日志。
    auto it = m_session_status_map.find(ipc_id);
    if (it == m_session_status_map.end()) {
      _LogUnknownSession("to_session_id", ipc_id);
      return 0;
    }
    return it->second.session_id;
  }
  SessionStatus& get_session_status(WeaselSessionId ipc_id) {
    auto it = m_session_status_map.find(ipc_id);
    if (it != m_session_status_map.end())
      return it->second;
    _LogUnknownSession("get_session_status", ipc_id);
    // 同样不再往会话表里插。给调用方一份"按 id 复用"的替身：同一个未注册 id 反复取
    // 到的是同一个对象（沿用旧行为，_UpdateUI/_GetStatus 内部前后两次取到的一致），
    // 但它不进会话表：不计入 total、不影响新 id 分配、也不会永久泄漏。
    if (m_orphan_session_valid && m_orphan_session_id == ipc_id)
      return m_orphan_session_status;
    m_orphan_session_status = SessionStatus();
    m_orphan_session_valid = true;
    m_orphan_session_id = ipc_id;
    return m_orphan_session_status;
  }
  SessionStatus& new_session_status(WeaselSessionId ipc_id) {
    return m_session_status_map[ipc_id] = SessionStatus();
  }

  AppOptionsByAppName m_app_options;
  weasel::UI* m_ui;  // reference
  DWORD m_active_session;
  bool m_disabled;
  std::string m_last_schema_id;
  std::string m_last_app_name;
  weasel::UIStyle m_base_style;
  std::map<std::string, bool> m_show_notifications;
  std::map<std::string, bool> m_show_notifications_base;
  std::function<void()> _UpdateUICallback;

  static void OnNotify(void* context_object,
                       uintptr_t session_id,
                       const char* message_type,
                       const char* message_value);
  static std::string m_message_type;
  static std::string m_message_value;
  static std::string m_message_label;
  static std::string m_option_name;
  static std::mutex m_notifier_mutex;
  SessionStatusMap m_session_status_map;
  // get_session_status() 遇到未注册 ipc_id 时的替身（见上）。只保留最近一个 id，
  // 所以常驻开销是常量而不是"每个见过的 id 一份"。
  SessionStatus m_orphan_session_status;
  bool m_orphan_session_valid = false;
  WeaselSessionId m_orphan_session_id = 0;
  // 未注册会话查询的日志上限：这种查询可能每个按键都有（客户端拿着过期 ipc_id 在
  // 问），不设上限会把日志刷爆；到顶后只再写一条 suppressed。
  static constexpr unsigned kMaxUnknownSessionLogs = 200;
  unsigned m_unknown_session_logs = 0;
  void _LogUnknownSession(const char* where, WeaselSessionId ipc_id);
  // 管道断开是"客户端可能没了"的信号，不是"它一定没了"：客户端在 IPC 超时时
  // 会主动丢弃本地管道连接（见 A1 系列），下一次按键再重连 —— 那种情况下它的
  // 会话还得留着（ascii 模式、合成状态都在里面）。所以断链时先登记，宽限期内
  // 该会话又被用到就撤销登记；超过宽限期仍在、且还是同一个 librime 会话，才真正
  // 回收（客户端进程崩溃/被杀的那条路径）。
  struct DetachedSession {
    WeaselSessionId ipc_id;
    RimeSessionId rime_id;  // 用来识别 ipc_id 复用：只有同一个 librime 会话才算旧账
    ULONGLONG detached_at;
  };
  std::vector<DetachedSession> m_detached_sessions;
  void _ReapDetachedSessions();
  void _CancelDetachedSession(WeaselSessionId ipc_id);
  static constexpr ULONGLONG kDetachedGraceMs = 60 * 1000;
  bool m_current_dark_mode;
  bool m_global_ascii_mode;
  int m_show_notifications_time;
  DWORD m_pid;
};
