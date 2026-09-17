#include "stdafx.h"
#include "WeaselServerImpl.h"
#include <atomic>
#include <mutex>
#include <set>
#include <Windows.h>
#include <resource.h>
#include <WeaselUtility.h>
#include <WeaselPerfLog.h>

namespace weasel {

// 活跃连接数。长驻进程里"连接数只增不减"才是内存单调增长的根因，而 [sess] 只
// 看得到会话表、看不到连接本身（EndSession 会清会话但连接可能还留着）。连接的
// 生与死各记一条，配合 WEASEL_POS_LOG 采集即可判断是否单调增长。
static std::atomic<long> g_live_connections{0};

// 串行化所有进入 librime 的调用。管道路径与消息线程（OnColorChange）都必须持有
// 它：两者会同时读写 m_session_status_map 并调 librime 的非线程安全 API。
static std::mutex g_api_mutex;

class PipeServer : public PipeChannel<DWORD, PipeMessage> {
 public:
  using ServerRunner = std::function<void()>;
  using Respond = std::function<void(Msg)>;
  using ServerHandler = std::function<void(PipeMessage, Respond)>;
  // 这条连接断开（客户端进程消失）时回调，参数是它用过的会话 id。正常退出会先
  // 发 END_SESSION，所以这里覆盖的是崩溃/被杀/未走 Deactivate 的路径。
  using ClientGone = std::function<void(const std::vector<DWORD>&)>;

  PipeServer(std::wstring&& pn_cmd, SECURITY_ATTRIBUTES* s);

 public:
  void Listen(ServerHandler const& handler, ClientGone const& on_client_gone);
  /* Get a server runner */
  ServerRunner GetServerRunner(ServerHandler const& handler,
                               ClientGone const& on_client_gone);

 private:
  void _ProcessPipeThread(HANDLE pipe,
                          ServerHandler const& handler,
                          ClientGone const& on_client_gone);
};
}  // namespace weasel

using namespace weasel;

extern CAppModule _Module;

ServerImpl::ServerImpl()
    : m_pRequestHandler(NULL),
      m_darkMode(IsUserDarkMode()),
      channel(std::make_unique<PipeServer>(GetPipeName(), sa.get_attr())) {
  m_hUser32Module = GetModuleHandle(_T("user32.dll"));
}

ServerImpl::~ServerImpl() {
  _Finailize();
}

void ServerImpl::_Finailize() {
  if (pipeThread != nullptr) {
    pipeThread->interrupt();
    pipeThread = nullptr;
  } else {
    // avoid finalize again
    return;
  }

  if (IsWindow()) {
    DestroyWindow();
  }
}

LRESULT ServerImpl::OnColorChange(UINT uMsg,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  BOOL& bHandled) {
  if (IsUserDarkMode() != m_darkMode) {
    m_darkMode = IsUserDarkMode();
    // 本回调跑在消息线程上，而 UpdateColorTheme 会遍历 m_session_status_map
    // （逐个 get_status、_LoadSchemaSpecificSettings 并写回 status）以及改写
    // m_base_style / m_ui->style()；管道线程是在 g_api_mutex 内对同一容器
    // erase/insert、对同一份 UI 样式赋值的。不共用这把锁，切系统深色模式时正在
    // 打字就是 map 迭代器失效 + 并发调 librime 的非线程安全 API。
    if (m_pRequestHandler) {
      std::lock_guard guard(g_api_mutex);
      m_pRequestHandler->UpdateColorTheme(m_darkMode);
    }
  }
  return 0;
}

LRESULT ServerImpl::OnCreate(UINT uMsg,
                             WPARAM wParam,
                             LPARAM lParam,
                             BOOL& bHandled) {
  // not neccessary...
  ::SetWindowText(m_hWnd, WEASEL_IPC_WINDOW);
  return 0;
}

LRESULT ServerImpl::OnClose(UINT uMsg,
                            WPARAM wParam,
                            LPARAM lParam,
                            BOOL& bHandled) {
  Stop();
  return 0;
}

LRESULT ServerImpl::OnDestroy(UINT uMsg,
                              WPARAM wParam,
                              LPARAM lParam,
                              BOOL& bHandled) {
  bHandled = FALSE;
  return 1;
}

LRESULT ServerImpl::OnQueryEndSystemSession(UINT uMsg,
                                            WPARAM wParam,
                                            LPARAM lParam,
                                            BOOL& bHandled) {
  return TRUE;
}

LRESULT ServerImpl::OnEndSystemSession(UINT uMsg,
                                       WPARAM wParam,
                                       LPARAM lParam,
                                       BOOL& bHandled) {
  if (m_pRequestHandler) {
    m_pRequestHandler->Finalize();
    m_pRequestHandler = nullptr;
  }
  return 0;
}

LRESULT ServerImpl::OnCommand(UINT uMsg,
                              WPARAM wParam,
                              LPARAM lParam,
                              BOOL& bHandled) {
  UINT uID = LOWORD(wParam);
  switch (uID) {
    case ID_WEASELTRAY_ENABLE_ASCII:
      m_pRequestHandler->SetOption(lParam, "ascii_mode", true);
      return 0;
    case ID_WEASELTRAY_DISABLE_ASCII:
      m_pRequestHandler->SetOption(lParam, "ascii_mode", false);
      return 0;
    default:;
  }

  std::map<UINT, CommandHandler>::iterator it = m_MenuHandlers.find(uID);
  if (it == m_MenuHandlers.end()) {
    bHandled = FALSE;
    return 0;
  }
  it->second();  // execute command
  return 0;
}

LRESULT ServerImpl::OnServiceNotifyMessage(UINT uMsg,
                                           WPARAM wParam,
                                           LPARAM lParam,
                                           BOOL& bHandled) {
  // Runs on the server message thread, NOT on a pipe worker thread and
  // without holding g_api_mutex, so that Shell_NotifyIcon inside the tray
  // update can never deadlock against the taskbar UI thread.
  if (m_trayRefreshCallback) {
    m_trayRefreshCallback();
  }
  return 0;
}

DWORD ServerImpl::OnCommand(WEASEL_IPC_COMMAND uMsg,
                            DWORD wParam,
                            DWORD lParam) {
  BOOL handled = TRUE;
  OnCommand(uMsg, wParam, lParam, handled);
  return handled;
}

HWND ServerImpl::Start() {
  std::wstring instanceName = L"(WEASEL)Furandōru-Sukāretto-";
  instanceName += getUsername();
  HANDLE hMutexOneInstance = ::CreateMutex(NULL, FALSE, instanceName.c_str());
  bool areYouOK = (::GetLastError() == ERROR_ALREADY_EXISTS ||
                   ::GetLastError() == ERROR_ACCESS_DENIED);

  if (areYouOK) {
    return 0;  // assure single instance
  }

  HWND hwnd = Create(NULL);

  return hwnd;
}

int ServerImpl::Stop() {
  // DO NOT exit process or finalize here
  // Let WeaselServer handle this
  PostMessage(WM_QUIT);
  return 0;
}

int ServerImpl::Run() {
  // This workaround causes a VC internal error:
  // void PipeServer::Listen(ServerHandler handler);
  //
  // auto handler = boost::bind(&ServerImpl::HandlePipeMessage, this);
  // auto listener = boost::bind(&PipeServer::Listen, channel.get(), handler);
  //
  auto listener = [this](PipeMessage msg, PipeServer::Respond resp) -> void {
    std::lock_guard guard(g_api_mutex);
    HandlePipeMessage(msg, resp);
  };
  // 管道断开 = 客户端进程消失，按它用过的会话回收（与消息处理共用 g_api_mutex，
  // 因为要进 librime；这里不持有管道锁，不会和收包互相阻塞）。
  auto on_client_gone = [this](const std::vector<DWORD>& sessions) -> void {
    std::lock_guard guard(g_api_mutex);
    if (m_pRequestHandler)
      m_pRequestHandler->DropDetachedSessions(sessions);
  };
  pipeThread = std::make_unique<boost::thread>([this, &listener,
                                                &on_client_gone]() {
    channel->Listen(listener, on_client_gone);
  });

  CMessageLoop theLoop;
  _Module.AddMessageLoop(&theLoop);
  int nRet = theLoop.Run();
  _Module.RemoveMessageLoop();
  return nRet;
}

DWORD ServerImpl::OnEcho(WEASEL_IPC_COMMAND uMsg, DWORD wParam, DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  return m_pRequestHandler->FindSession(lParam);
}

DWORD ServerImpl::OnStartSession(WEASEL_IPC_COMMAND uMsg,
                                 DWORD wParam,
                                 DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  return m_pRequestHandler->AddSession(
      reinterpret_cast<LPWSTR>(channel->ReceiveBuffer()),
      [this](std::wstring& msg) -> bool {
        *channel << msg;
        return true;
      });
}

DWORD ServerImpl::OnEndSession(WEASEL_IPC_COMMAND uMsg,
                               DWORD wParam,
                               DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  return m_pRequestHandler->RemoveSession(lParam);
}

DWORD ServerImpl::OnKeyEvent(WEASEL_IPC_COMMAND uMsg,
                             DWORD wParam,
                             DWORD lParam) {
  if (!m_pRequestHandler /* || !m_pSharedMemory*/)
    return 0;

  auto eat = [this](std::wstring& msg) -> bool {
    *channel << msg;
    return true;
  };
  return m_pRequestHandler->ProcessKeyEvent(KeyEvent(wParam), lParam, eat);
}

DWORD ServerImpl::OnShutdownServer(WEASEL_IPC_COMMAND uMsg,
                                   DWORD wParam,
                                   DWORD lParam) {
  Stop();
  return 0;
}

DWORD ServerImpl::OnFocusIn(WEASEL_IPC_COMMAND uMsg,
                            DWORD wParam,
                            DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  m_pRequestHandler->FocusIn(wParam, lParam);
  return 0;
}

DWORD ServerImpl::OnFocusOut(WEASEL_IPC_COMMAND uMsg,
                             DWORD wParam,
                             DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  m_pRequestHandler->FocusOut(wParam, lParam);
  return 0;
}

DWORD ServerImpl::OnUpdateInputPosition(WEASEL_IPC_COMMAND uMsg,
                                        DWORD wParam,
                                        DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  /*
   * 移位标志 = 1bit == 0
   * height: 0~127 = 7bit
   * top:-2048~2047 = 12bit（有符号）
   * left:-2048~2047 = 12bit（有符号）
   *
   * 高解析度下：
   * 移位标志 = 1bit == 1
   * height: 0~254 = 7bit（舍弃低1位）
   * top: -4096~4094 = 12bit（有符号，舍弃低1位）
   * left: -4096~4094 = 12bit（有符号，舍弃低1位）
   */
  RECT rc;
  int hi_res = (wParam >> 31) & 0x01;
  rc.left = ((wParam & 0x7ff) - (wParam & 0x800)) << hi_res;
  rc.top = (((wParam >> 12) & 0x7ff) - ((wParam >> 12) & 0x800)) << hi_res;
  const int width = 6;
  int height = ((wParam >> 24) & 0x7f) << hi_res;
  rc.right = rc.left + width;
  rc.bottom = rc.top + height;

  {
    using PPTLPFPMDPI = BOOL(WINAPI*)(HWND, LPPOINT);
    // 函数指针只查一次：GetProcAddress 每次输入都做是纯粹的浪费；
    // 函数不存在时必须跳过，不能调用空指针。
    static PPTLPFPMDPI PhysicalToLogicalPointForPerMonitorDPI =
        (PPTLPFPMDPI)::GetProcAddress(m_hUser32Module,
                                      "PhysicalToLogicalPointForPerMonitorDPI");
    if (PhysicalToLogicalPointForPerMonitorDPI) {
      POINT lt = {rc.left, rc.top};
      POINT rb = {rc.right, rc.bottom};
      PhysicalToLogicalPointForPerMonitorDPI(NULL, &lt);
      PhysicalToLogicalPointForPerMonitorDPI(NULL, &rb);
      rc = {lt.x, lt.y, rb.x, rb.y};
    }
  }

  m_pRequestHandler->UpdateInputPosition(rc, lParam);
  return 0;
}

DWORD ServerImpl::OnStartMaintenance(WEASEL_IPC_COMMAND uMsg,
                                     DWORD wParam,
                                     DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->StartMaintenance();
  return 0;
}

DWORD ServerImpl::OnEndMaintenance(WEASEL_IPC_COMMAND uMsg,
                                   DWORD wParam,
                                   DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->EndMaintenance();
  return 0;
}

DWORD ServerImpl::OnCommitComposition(WEASEL_IPC_COMMAND uMsg,
                                      DWORD wParam,
                                      DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->CommitComposition(lParam);
  return 0;
}

DWORD ServerImpl::OnClearComposition(WEASEL_IPC_COMMAND uMsg,
                                     DWORD wParam,
                                     DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->ClearComposition(lParam);
  return 0;
}

DWORD ServerImpl::OnSelectCandidateOnCurrentPage(WEASEL_IPC_COMMAND uMsg,
                                                 DWORD wParam,
                                                 DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->SelectCandidateOnCurrentPage(wParam, lParam);
  return 0;
}

DWORD ServerImpl::OnHighlightCandidateOnCurrentPage(WEASEL_IPC_COMMAND uMsg,
                                                    DWORD wParam,
                                                    DWORD lParam) {
  if (m_pRequestHandler) {
    auto eat = [this](std::wstring& msg) -> bool {
      *channel << msg;
      return true;
    };
    m_pRequestHandler->HighlightCandidateOnCurrentPage(wParam, lParam, eat);
  }
  return 0;
}

DWORD ServerImpl::OnChangePage(WEASEL_IPC_COMMAND uMsg,
                               DWORD wParam,
                               DWORD lParam) {
  if (m_pRequestHandler) {
    auto eat = [this](std::wstring& msg) -> bool {
      *channel << msg;
      return true;
    };
    m_pRequestHandler->ChangePage(wParam, lParam, eat);
  }
  return 0;
}

#define MAP_PIPE_MSG_HANDLE(__msg, __wParam, __lParam) \
  {                                                    \
    auto lParam = __lParam;                            \
    auto wParam = __wParam;                            \
    LRESULT _result = 0;                               \
    switch (__msg) {
#define PIPE_MSG_HANDLE(__msg, __func)       \
  case __msg:                                \
    _result = __func(__msg, wParam, lParam); \
    break;

#define END_MAP_PIPE_MSG_HANDLE(__result) \
  }                                       \
  __result = _result;                     \
  }

template <typename _Resp>
void ServerImpl::HandlePipeMessage(PipeMessage pipe_msg, _Resp resp) {
  DWORD result = 0;
  // D1：焦点切换路径上的每一次往返都在这里量一遍。按命令分别统计才好判断
  // "OnSetThreadFocus 那个空按键"值不值得改（它会走一遍完整的按键处理）。
  weasel::perf::PosLog& log = weasel::perf::PosLog::Instance();
  const bool logging = log.enabled();
  const ULONGLONG t0 = logging ? weasel::perf::PosLog::Now() : 0;
  try {
    MAP_PIPE_MSG_HANDLE(pipe_msg.Msg, pipe_msg.wParam, pipe_msg.lParam)
    PIPE_MSG_HANDLE(WEASEL_IPC_ECHO, OnEcho)
    PIPE_MSG_HANDLE(WEASEL_IPC_START_SESSION, OnStartSession)
    PIPE_MSG_HANDLE(WEASEL_IPC_END_SESSION, OnEndSession)
    PIPE_MSG_HANDLE(WEASEL_IPC_PROCESS_KEY_EVENT, OnKeyEvent)
    PIPE_MSG_HANDLE(WEASEL_IPC_SHUTDOWN_SERVER, OnShutdownServer)
    PIPE_MSG_HANDLE(WEASEL_IPC_FOCUS_IN, OnFocusIn)
    PIPE_MSG_HANDLE(WEASEL_IPC_FOCUS_OUT, OnFocusOut)
    PIPE_MSG_HANDLE(WEASEL_IPC_UPDATE_INPUT_POS, OnUpdateInputPosition)
    PIPE_MSG_HANDLE(WEASEL_IPC_START_MAINTENANCE, OnStartMaintenance)
    PIPE_MSG_HANDLE(WEASEL_IPC_END_MAINTENANCE, OnEndMaintenance)
    PIPE_MSG_HANDLE(WEASEL_IPC_COMMIT_COMPOSITION, OnCommitComposition)
    PIPE_MSG_HANDLE(WEASEL_IPC_CLEAR_COMPOSITION, OnClearComposition);
    PIPE_MSG_HANDLE(WEASEL_IPC_SELECT_CANDIDATE_ON_CURRENT_PAGE,
                    OnSelectCandidateOnCurrentPage);
    PIPE_MSG_HANDLE(WEASEL_IPC_HIGHLIGHT_CANDIDATE_ON_CURRENT_PAGE,
                    OnHighlightCandidateOnCurrentPage);
    PIPE_MSG_HANDLE(WEASEL_IPC_CHANGE_PAGE, OnChangePage);
    PIPE_MSG_HANDLE(WEASEL_IPC_TRAY_COMMAND, OnCommand);
    END_MAP_PIPE_MSG_HANDLE(result);
  } catch (DWORD /* ex */) {
    result = 0;
  } catch (...) {
    // UI / DirectWriteResources 路径抛出的异常（含 HR() 的 ComException）必须
    // 在回包之后再丢弃：客户端在 _ReceiveResponse 上等这次发送，异常跳过
    // resp() 会让客户端收不到任何字节而永久阻塞（见任务 A1）。
    result = 0;
  }
  if (logging) {
    const double ms = weasel::perf::PerfLog::Ms(t0, weasel::perf::PosLog::Now());
    const bool is_key = (pipe_msg.Msg == WEASEL_IPC_PROCESS_KEY_EVENT);
    // 按键与 FOCUS_IN 每次都记（要对比"空按键"和普通按键）；其它命令只记慢的。
    if (is_key) {
      weasel::KeyEvent ke(pipe_msg.wParam);
      log.Writef("[ipc] key=%u mask=0x%x ms=%.2f", (unsigned)ke.keycode,
                 (unsigned)ke.mask, ms);
    } else if (pipe_msg.Msg == WEASEL_IPC_FOCUS_IN || ms >= 1.0) {
      log.Writef("[ipc] cmd=0x%x ms=%.2f", (unsigned)pipe_msg.Msg, ms);
    }
  }
  resp(result);
}

PipeServer::PipeServer(std::wstring&& pn_cmd, SECURITY_ATTRIBUTES* s)
    : PipeChannel(std::move(pn_cmd), s) {}

void PipeServer::Listen(ServerHandler const& handler,
                        ClientGone const& on_client_gone) {
  for (;;) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    try {
      boost::this_thread::interruption_point();
      pipe = _ConnectServerPipe(pname);
      boost::thread th([&handler, &on_client_gone, pipe, this] {
        _ProcessPipeThread(pipe, handler, on_client_gone);
      });
    } catch (DWORD ex) {
      _FinalizePipe(pipe);
    }
    boost::this_thread::interruption_point();
  }
}

PipeServer::ServerRunner PipeServer::GetServerRunner(
    ServerHandler const& handler,
    ClientGone const& on_client_gone) {
  return [&handler, &on_client_gone, this]() {
    Listen(handler, on_client_gone);
  };
}

void PipeServer::_ProcessPipeThread(HANDLE pipe,
                                    ServerHandler const& handler,
                                    ClientGone const& on_client_gone) {
  // 记下这条连接用过的会话：客户端进程崩溃/被杀时不会发 END_SESSION，断开时
  // 按这个集合回收，否则服务器的 session 表与 librime 的 session 都会留在
  // 服务端（长驻进程内存单调增长）。
  std::set<DWORD> sessions;
  weasel::perf::PosLog& log = weasel::perf::PosLog::Instance();
  const bool logging = log.enabled();
  const long live = ++g_live_connections;
  if (logging)
    log.Writef("[conn] enter live=%ld", live);
  try {
    for (;;) {
      Res msg;
      _Receive(pipe, &msg, sizeof(msg));
      // 客户端发来的消息都把会话 id 放在 lParam；非会话消息（0）忽略。
      // 万一收到不是会话 id 的值也无害：清理时找不到就跳过。
      if (msg.lParam != 0)
        sessions.insert(msg.lParam);
      handler(msg, [this, pipe](Msg resp) { _Send(pipe, resp); });
    }
  } catch (...) {
    _FinalizePipe(pipe);
  }
  const long remaining = --g_live_connections;
  if (logging)
    log.Writef("[conn] exit live=%ld sessions=%u", remaining,
               (unsigned)sessions.size());
  if (!sessions.empty() && on_client_gone) {
    try {
      on_client_gone(std::vector<DWORD>(sessions.begin(), sessions.end()));
    } catch (...) {
      // 清理失败不能影响管道线程收尾
    }
  }
}

// weasel::Server

Server::Server() : m_pImpl(new ServerImpl) {}

Server::~Server() {
  if (m_pImpl)
    delete m_pImpl;
}

HWND Server::Start() {
  return m_pImpl->Start();
}

int Server::Stop() {
  return m_pImpl->Stop();
}

int Server::Run() {
  return m_pImpl->Run();
}

void Server::SetRequestHandler(RequestHandler* pHandler) {
  m_pImpl->SetRequestHandler(pHandler);
}

void Server::AddMenuHandler(UINT uID, CommandHandler handler) {
  m_pImpl->AddMenuHandler(uID, handler);
}

void Server::SetTrayRefreshCallback(std::function<void()> callback) {
  m_pImpl->SetTrayRefreshCallback(callback);
}

HWND Server::GetHWnd() {
  return m_pImpl->m_hWnd;
}
