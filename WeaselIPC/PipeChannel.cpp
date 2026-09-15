#include "stdafx.h"

#include <PipeChannel.h>

using namespace weasel;
using namespace std;
using namespace boost;

#define _ThrowLastError throw ::GetLastError()
#define _ThrowCode(__c) throw __c
#define _ThrowIfNot(__c)                 \
  {                                      \
    DWORD err;                           \
    if ((err = ::GetLastError()) != __c) \
      throw err;                         \
  }

PipeChannelBase::PipeChannelBase(std::wstring&& pn_cmd,
                                 size_t bs = 4 * 1024,
                                 SECURITY_ATTRIBUTES* s = NULL)
    : pname(pn_cmd), buff_size(bs), sa(s) {};

PipeChannelBase::~PipeChannelBase() {
  // Thread-specific pointers are cleaned up automatically
}

bool PipeChannelBase::_Ensure() {
  try {
    HANDLE* phandle = _GetPipeHandle();
    if (_Invalid(*phandle)) {
      *phandle = _Connect(pname.c_str());
      return !_Invalid(*phandle);
    }
  } catch (...) {
    return false;
  }

  return true;
}

HANDLE PipeChannelBase::_Connect(const wchar_t* name) {
  // 管道存在但实例一直被占用时（服务端卡死、占着实例又不新增），这里原本是
  // 无上限的 while (WaitNamedPipe(name, 500))：调用它的是 TSF 回调线程，
  // 一旦停住就是这么也回不来的宿主冻结。给等待加一个上限，超时按既有约定
  // 抛 DWORD —— 上层 _Ensure() 会返回 false，客户端 _SendMessage 捕获后返回 0，
  // 按键被放行，宿主继续可用。
  constexpr DWORD kConnectTimeoutMs = 2000;
  constexpr DWORD kConnectWaitSliceMs = 500;
  const ULONGLONG deadline = ::GetTickCount64() + kConnectTimeoutMs;
  HANDLE pipe = INVALID_HANDLE_VALUE;
  while (_Invalid(pipe = _TryConnect())) {
    if (::GetTickCount64() >= deadline) {
      _ThrowCode(ERROR_TIMEOUT);
    }
    ::WaitNamedPipe(name, kConnectWaitSliceMs);
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(pipe, &mode, NULL, NULL)) {
    _ThrowLastError;
  }
  return pipe;
}

void PipeChannelBase::_Reconnect() {
  HANDLE* phandle = _GetPipeHandle();
  _FinalizePipe(*phandle);
  _Ensure();
}

HANDLE PipeChannelBase::_TryConnect() {
  auto pipe = ::CreateFile(pname.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
  if (!_Invalid(pipe)) {
    // connected to the pipe
    return pipe;
  }
  // being busy is not really an error since we just need to wait.
  _ThrowIfNot(ERROR_PIPE_BUSY);
  // All pipe instances are busy
  return INVALID_HANDLE_VALUE;
}

size_t PipeChannelBase::_WritePipe(HANDLE pipe, size_t s, char* b) {
  DWORD lwritten;
  if (!::WriteFile(pipe, b, s, &lwritten, NULL) || lwritten <= 0) {
    _ThrowLastError;
  }
  ::FlushFileBuffers(pipe);
  return lwritten;
}

void PipeChannelBase::_FinalizePipe(HANDLE& p) {
  if (!_Invalid(p)) {
    DisconnectNamedPipe(p);
    CloseHandle(p);
  }
  p = INVALID_HANDLE_VALUE;
}

void PipeChannelBase::_Receive(HANDLE pipe, LPVOID msg, size_t rec_len) {
  DWORD lread;
  BOOL success = ::ReadFile(pipe, msg, rec_len, &lread, NULL);
  if (!success) {
    _ThrowIfNot(ERROR_MORE_DATA);

    auto ctx = _GetContext();
    memset(ctx->buffer.get(), 0, buff_size);
    success = ::ReadFile(pipe, ctx->buffer.get(), buff_size, &lread, NULL);
    if (!success) {
      _ThrowLastError;
    }
  }
  _GetContext()->has_body = false;
}

HANDLE PipeChannelBase::_ConnectServerPipe(std::wstring& pn) {
  HANDLE pipe =
      CreateNamedPipe(pn.c_str(), PIPE_ACCESS_DUPLEX,
                      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                      PIPE_UNLIMITED_INSTANCES, buff_size, buff_size, 0, sa);
  if (pipe == INVALID_HANDLE_VALUE || !::ConnectNamedPipe(pipe, NULL)) {
    _ThrowLastError;
  }
  return pipe;
}
