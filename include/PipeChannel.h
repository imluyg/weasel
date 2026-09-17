#pragma once
#include <string>
#include <memory>
#include <windows.h>
#include <boost/interprocess/streams/bufferstream.hpp>
#include <boost/thread.hpp>
#include <boost/thread/tss.hpp>

namespace weasel {

class PipeChannelBase {
 public:
  using Stream = boost::interprocess::wbufferstream;

  struct ChannelContext {
    std::unique_ptr<char[]> buffer;
    std::unique_ptr<Stream> write_stream;
    bool has_body;

    ChannelContext(size_t bs)
        : buffer(std::make_unique<char[]>(bs)), has_body(false) {}
  };

  // TLS 里存这个盒子而不是裸 HANDLE。boost::thread_specific_ptr 的线程退出清理
  // 只会 delete 盒子，不会关句柄；不在这里自己关的话，任何"用过管道但没显式
  // Disconnect 就退出"的线程都会把一条已连接的管道实例留到进程结束，服务端那条
  // 阻塞在 ReadFile 上的连接线程（线程栈 + 64KB ChannelContext）也就永不结束。
  // 显式 _FinalizePipe 过的句柄会被置为 INVALID_HANDLE_VALUE，析构时自然跳过。
  struct PipeHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~PipeHandle() {
      if (h != INVALID_HANDLE_VALUE) {
        ::DisconnectNamedPipe(h);
        ::CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
      }
    }
  };

  PipeChannelBase(std::wstring&& pn_cmd, size_t bs, SECURITY_ATTRIBUTES* s);
  ~PipeChannelBase();

 protected:
  /* To ensure connection before operation */
  bool _Ensure();
  /* Connect pipe as client */
  HANDLE _Connect(const wchar_t* name);
  /* To reconnect message pipe */
  void _Reconnect();
  /* Try to connect for one time */
  HANDLE _TryConnect();
  size_t _WritePipe(HANDLE p, size_t s, char* b);
  void _FinalizePipe(HANDLE& p);
  void _Receive(HANDLE pipe, LPVOID msg, size_t rec_len);
  /* Try to get a connection from client */
  HANDLE _ConnectServerPipe(std::wstring& pn);
  inline bool _Invalid(HANDLE p) const { return p == INVALID_HANDLE_VALUE; }

  HANDLE* _GetPipeHandle() const {
    if (!hpipe_ptr.get()) {
      hpipe_ptr.reset(new PipeHandle());
    }
    return &hpipe_ptr.get()->h;
  }

  ChannelContext* _GetContext() const {
    if (!context.get()) {
      context.reset(new ChannelContext(buff_size));
    }
    return context.get();
  }

 protected:
  std::wstring pname;
  // Thread-local pipe handle for isolation (PipeHandle closes it on thread exit)
  mutable boost::thread_specific_ptr<PipeHandle> hpipe_ptr;
  const size_t buff_size;
  // Thread-local context for buffer and state
  mutable boost::thread_specific_ptr<ChannelContext> context;

 private:
  /* Security attributes */
  SECURITY_ATTRIBUTES* sa;
};

/* Pipe based IPC channel */
template <typename _TyMsg,
          typename _TyRes = DWORD,
          size_t _MsgSize = sizeof(_TyMsg),
          size_t _ResSize = sizeof(_TyRes)>
class PipeChannel : public PipeChannelBase {
 public:
  /* Type definitions */

  using Ptr = std::shared_ptr<PipeChannel>;
  using UPtr = std::unique_ptr<PipeChannel>;
  using Msg = _TyMsg;
  using Res = _TyRes;

  enum class ChannalCommand { NEW_MSG_PIPE, REFRESH };

 public:
  PipeChannel(std::wstring&& pn_cmd,
              SECURITY_ATTRIBUTES* s = NULL,
              size_t bs = 64 * 1024)
      : PipeChannelBase(std::move(pn_cmd), bs, s) {}

 public:
  /* Common pipe operations */

  bool Connect() { return _Ensure(); }
  bool Connected() const {
    HANDLE* phandle = _GetPipeHandle();
    return !_Invalid(*phandle);
  }
  void Disconnect() {
    HANDLE* phandle = _GetPipeHandle();
    _FinalizePipe(*phandle);
  }

  /* Write data to buffer */

  template <typename _TyWrite>
  void Write(_TyWrite cnt) {
    _GetContext()->has_body = true;
    _BufferWriteStream() << cnt;
  }

  /* Write data to buffer */
  template <typename _TyWrite>
  PipeChannel& operator<<(_TyWrite cnt) {
    Write(cnt);
    return *this;
  }

  _TyRes Transact(Msg& msg) {
    // 连接不可用时不再往下走：_Send 只会在无效句柄上失败，并可能再触发一次
    // 连接等待，白白让 TSF 线程多停一个 kConnectTimeoutMs。这里直接按既有约定
    // 抛 DWORD 放行按键，同时照 _Send 收尾的做法清掉待发 body，避免残留内容
    // 污染下一次请求。
    if (!_Ensure()) {
      ClearBufferStream();
      throw (DWORD)ERROR_TIMEOUT;
    }
    HANDLE* phandle = _GetPipeHandle();
    _Send(*phandle, msg);
    return _ReceiveResponse();
  }

  void ClearBufferStream() {
    auto ctx = _GetContext();
    ctx->has_body = false;
    if (ctx->write_stream != nullptr) {
      ctx->write_stream.reset(nullptr);
    }
  }

  char* SendBuffer() const { return _GetContext()->buffer.get() + _MsgSize; }

  char* ReceiveBuffer() const { return _GetContext()->buffer.get() + _ResSize; }

  template <typename _TyHandler>
  bool HandleResponseData(_TyHandler const& handler) {
    if (!handler) {
      return false;
    }

    // Use whole buffer to receive data in client
    return handler((LPWSTR)_GetContext()->buffer.get(),
                   (UINT)(buff_size * sizeof(char) / sizeof(wchar_t)));
  }

 protected:
  void _Send(HANDLE pipe, Msg& msg) {
    auto ctx = _GetContext();
    char* pbuff = ctx->buffer.get();
    DWORD lwritten = 0;

    *reinterpret_cast<Msg*>(pbuff) = msg;
    size_t body_bytes = 0;
    if (ctx->has_body && ctx->write_stream) {
      std::streampos pos = ctx->write_stream->tellp();
      if (pos != std::streampos(-1)) {
        body_bytes = static_cast<size_t>(pos) * sizeof(wchar_t);
      }
    }
    size_t data_sz = ctx->has_body ? (_MsgSize + body_bytes) : _MsgSize;
    if (data_sz > buff_size)
      data_sz = buff_size;

    try {
      _WritePipe(pipe, data_sz, pbuff);
    } catch (...) {
      _Reconnect();
      _WritePipe(pipe, data_sz, pbuff);
    }
    ClearBufferStream();
  }

  _TyRes _ReceiveResponse() {
    HANDLE* phandle = _GetPipeHandle();
    _TyRes result{};
    // 注意：管道句柄是同步句柄（_TryConnect 的 CreateFile 未带
    // FILE_FLAG_OVERLAPPED），对它传 OVERLAPPED 会被 ReadFile 忽略。
    // 唯一安全的有界等待是 PeekNamedPipe 轮询。
    constexpr DWORD kResRecvTimeoutMs = 2000;
    constexpr DWORD kPeekIntervalMs = 10;
    // 先在极短窗口内自旋，再退回 Sleep：服务端通常几十微秒到几毫秒就回包，而
    // ::Sleep(10) 会被系统计时器粒度（默认 15.6ms）向上取整成一整个 tick —— 实测
    // 每次回包平添 15.9ms（p95），每键 4 次 IPC 就是约 60ms/键。自旋窗口只有
    // 3ms 且用 SwitchToThread 让出时间片，长时间等待仍走 Sleep，不烧 CPU，
    // 2s 上限不变。
    constexpr ULONGLONG kSpinWindowMs = 3;
    // 用真实时钟做截止时间，而不是「循环次数 × 10ms」：::Sleep(10) 会被系统
    // 计时器粒度（默认 15.6ms）向上取整，按次数计时的实际上限会漂到约 3.2s。
    const ULONGLONG deadline = ::GetTickCount64() + kResRecvTimeoutMs;
    const ULONGLONG spin_until = ::GetTickCount64() + kSpinWindowMs;
    for (;;) {
      DWORD avail = 0;
      if (!::PeekNamedPipe(*phandle, NULL, 0, NULL, &avail, NULL)) {
        const DWORD err = ::GetLastError();  // _FinalizePipe 会覆盖 GetLastError
        _FinalizePipe(*phandle);  // 管道已断：丢弃本地连接，不在读路径上等待重连
        throw err;                // 走既有 catch(DWORD) 路径
      }
      if (avail > 0) {
        _Receive(*phandle, &result, sizeof(result));  // 数据已就位，不会阻塞
        return result;
      }
      if (::GetTickCount64() >= deadline) {
        // 超时：只丢弃本地连接（防止残留字节错位），**不**就地重连——重连要等
        // 新连接可用，服务端占着实例又不新增时会长时间阻塞（_Connect 现在有上限，
        // 但没必要在这里多等一轮）。下一次 Transact 会经 _Ensure() 重新连接。
        _FinalizePipe(*phandle);
        throw (DWORD)ERROR_TIMEOUT;
      }
      if (::GetTickCount64() < spin_until)
        ::SwitchToThread();  // 快路径：不让计时器粒度决定回包延迟
      else
        ::Sleep(kPeekIntervalMs);  // 慢路径：长时间无回包时低 CPU 等待
    }
  }

  Stream& _BufferWriteStream() {
    auto ctx = _GetContext();
    if (ctx->write_stream == nullptr) {
      char* pbuff = (char*)ctx->buffer.get() + _MsgSize;
      memset(pbuff, 0, buff_size - _MsgSize);
      ctx->write_stream =
          std::make_unique<Stream>((wchar_t*)pbuff, _SendBufferSizeW());
    }
    return *ctx->write_stream;
  }

 private:
  inline size_t _SendBufferSizeW() const {
    return (buff_size - _MsgSize) * sizeof(char) / sizeof(wchar_t);
  }

  inline size_t _ReceiveBufferSizeW() const {
    return (buff_size - _ResSize) * sizeof(char) / sizeof(wchar_t);
  }
};
};  // namespace weasel
