#pragma once
#include <string>
#include <vector>

#include <WeaselIPCData.h>
#include <WeaselUI.h>
#include "StandardLayout.h"
#include "Layout.h"
#include "GdiplusBlur.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using namespace weasel;

typedef CWinTraits<WS_POPUP | WS_CLIPSIBLINGS | WS_DISABLED,
                   WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                       WS_EX_LAYERED>
    CWeaselPanelTraits;

enum class BackType {
  TEXT = 0,
  CAND = 1,
  BACKGROUND = 2  // background
};

class WeaselPanel
    : public CWindowImpl<WeaselPanel, CWindow, CWeaselPanelTraits>,
      CDoubleBufferImpl<WeaselPanel> {
 public:
  BEGIN_MSG_MAP(WeaselPanel)
  MESSAGE_HANDLER(WM_CREATE, OnCreate)
  MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
  MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
  MESSAGE_HANDLER(WM_MOUSEACTIVATE, OnMouseActivate)
  MESSAGE_HANDLER(WM_LBUTTONUP, OnLeftClickedUp)
  MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLeftClickedDown)
  MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
  MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
  MESSAGE_HANDLER(WM_MOUSELEAVE, OnMouseLeave)
  CHAIN_MSG_MAP(CDoubleBufferImpl<WeaselPanel>)
  END_MSG_MAP()

  LRESULT OnCreate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
  LRESULT OnDestroy(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
  LRESULT OnDpiChanged(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
  LRESULT OnMouseActivate(UINT uMsg,
                          WPARAM wParam,
                          LPARAM lParam,
                          BOOL& bHandled);
  LRESULT OnLeftClickedUp(UINT uMsg,
                          WPARAM wParam,
                          LPARAM lParam,
                          BOOL& bHandled);
  LRESULT OnLeftClickedDown(UINT uMsg,
                            WPARAM wParam,
                            LPARAM lParam,
                            BOOL& bHandled);
  LRESULT OnMouseWheel(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
  LRESULT OnMouseMove(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
  LRESULT OnMouseLeave(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);

  WeaselPanel(weasel::UI& ui);
  ~WeaselPanel();

  void MoveTo(RECT const& rc);
  void Refresh();
  // 隐藏期间被推迟的 SetWindowPos 在"真正要显示"之前补应用（UI::Show /
  // ShowWithTimeout 会先调它，再 ShowWindow）—— 见 _RepositionWindow 里的 defer。
  void EnsurePositionApplied();
  void DoPaint(CDCHandle dc);
  bool GetIsReposition() { return m_istorepos; }
  void RedrawWindow();

  static VOID CALLBACK OnTimer(_In_ HWND hwnd,
                               _In_ UINT uMsg,
                               _In_ UINT_PTR idEvent,
                               _In_ DWORD dwTime);
  static const int AUTOREV_TIMER = 20240315;
  static UINT_PTR ptimer;

 private:
  template <typename T>
  int DPI_SCALE(T t) {
    return (int)(t * dpiScaleLayout);
  }
  void _InitFontRes(bool forced = false);
  void _DoPaintImpl(CDCHandle dc);  // DoPaint 的实际实现，由 DoPaint 包异常边界
  void _CaptureRect(CRect& rect);
  bool m_mouse_entry = false;
  CPoint m_lastMousePos = {-1, -1};
  void _CreateLayout();
  void _ResizeWindow();
  void _RepositionWindow(const bool& adj = false);
  // 候选行比预算（工作区 2/3）宽时：先截断显示用候选文本，再考虑缩字号
  // （true = 已改动，需要重新 DoLayout）
  bool _FitToWorkAreaWidth();
  bool _ApplyFitFontPercent(int percent);
  // 按预算把候选的**显示**文本截到 limit 个字符（含结尾的 "..."）
  bool _ApplyFitTrim(size_t limit);
  void _FitLog(int cx, int budget, int trimChars, int percent);
  // 只有画出来有意义时才重绘（隐藏的服务端面板跳过）
  void _RedrawIfUseful();
  // SetWindowPos 的实际执行点：立即用，或由 EnsurePositionApplied 补用
  void _ApplyPosition(int x, int y);
  bool _DrawPreedit(const Text& text, CDCHandle dc, const CRect& rc);
  bool _DrawPreeditBack(const Text& text, CDCHandle dc, const CRect& rc);
  bool _DrawCandidates(CDCHandle& dc, bool back = false);
  void _HighlightText(CDCHandle& dc,
                      const CRect& rc,
                      const COLORREF& color,
                      const COLORREF& shadowColor,
                      const int& radius,
                      const BackType& type,
                      const IsToRoundStruct& rd,
                      const COLORREF& bordercolor);
  void _TextOut(const CRect& rc,
                const std::wstring& psz,
                const size_t& cch,
                const int& inColor,
                IDWriteTextFormat1* const pTextFormat = NULL);

  void _LayerUpdate(const CRect& rc, CDCHandle dc);

  weasel::Layout* m_layout;
  weasel::Context& m_ctx;
  weasel::Context& m_octx;
  weasel::Status& m_status;
  weasel::UIStyle& m_style;
  weasel::UIStyle& m_ostyle;
  const bool& m_in_server;

  CRect m_inputPos;
  int m_offsetys[MAX_CANDIDATES_COUNT];  // offset y for candidates when
                                         // vertical layout over bottom
  int m_offsety_preedit;
  int m_offsety_aux;
  bool m_istorepos;

  CIcon m_iconDisabled;
  CIcon m_iconEnabled;
  CIcon m_iconAlpha;
  CIcon m_iconFull;
  CIcon m_iconHalf;
  std::wstring m_current_zhung_icon;
  std::wstring m_current_ascii_icon;
  std::wstring m_current_half_icon;
  std::wstring m_current_full_icon;
  // for gdiplus drawings
  Gdiplus::GdiplusStartupInput _m_gdiplusStartupInput;
  ULONG_PTR _m_gdiplusToken;

  UINT dpi;

  CRect rcw;
  BYTE m_candidateCount;
  BYTE m_lastCandidateCount;

  bool hide_candidates;
  bool m_sticky;
  // 当前生效的字号百分比（100 = 皮肤里配的字号）；_InitFontRes 重建资源时复位
  int m_fitFontPercent = 100;
  // 本帧 fit 用的**截断前**候选显示文本（快照）。每轮都从这份快照重新截断：
  // 旧实现直接在上一轮截断过的串上再截，而 substr(0, limit-1)+L"..." 得到的长度是
  // limit+2，于是"按比例算出的下一轮 limit"正好等于上一轮的值 —— 宽度卡在一个高于
  // 预算的不动点上，4 次迭代全烧在那里，永远走不到第 ③ 步缩字号。
  // 真机 pos.log.3248：#275 帧 cx 连续三轮都是 1425 而 trim 一直是 12。
  std::vector<std::wstring> m_fitOrigCandies;
  size_t m_fitOrigMaxLen = 0;  // 快照里的最大字符数
  int m_fitCxFull = 0;         // 未截断时的行宽（两点内插的第一个点）
  bool m_fitOrigValid = false;
  // 上一轮 fit 结束时量到的行宽：本轮的 cx 没有变小就说明截断已经帮不上忙，
  // 直接转去缩字号，不必把迭代次数烧光
  int m_fitLastCx = 0;
  // 候选行超预算时优先"换行"而不是截断显示文本（HorizontalLayout 会按
  // style.max_width 折行，一个字都不丢）。两个值都是**逻辑像素**，Layout 构造时
  // 再按 DPI 放大：
  //   m_fitWrapWidth    本轮用的换行宽度；0 = 用皮肤自己的配置，-1 = 本轮放弃换行
  //   m_layoutWrapWidth 当前 m_layout 是按哪个宽度建出来的（不同就必须重建）
  int m_fitWrapWidth = 0;
  int m_layoutWrapWidth = 0;
  // 本帧是否已经缩过字号：① 的字号恢复用的是乐观估算（cx×103/pct），而实际宽度
  // 受折行位置影响不是线性的，不拦住就会在同一帧里"恢复→又超→再缩"来回拉锯，
  // 每次都重建 DirectWrite 字体资源（真机 pos.log.7368：一帧内 pct 在 98/100
  // 之间跳 3~4 次，8 次迭代全烧完，最后还停在 98% 且超预算）。
  bool m_fitFontShrunkThisFrame = false;
  // 隐藏期间被推迟的窗口位置（服务端面板）：SetWindowPos 对 topmost + layered
  // 窗口不免费，而每次位置上报都会走到这里。显示前用 EnsurePositionApplied 补上。
  int m_pendingX = 0;
  int m_pendingY = 0;
  bool m_posDirty = false;
  // 本帧内"两点线性内插"用的试探点：上次截到的字数 → 当前量到的宽度就是那个点的宽度
  int m_fitTryLimit = -1;
  // for multi font_face & font_point
  PDWR pDWR;
  std::function<void(size_t* const, size_t* const, bool* const, bool* const)>&
      _UICallback;
  float bar_scale_ = 1.0;
  float dpiScaleLayout = 1.0f;
  int m_hoverIndex = -1;
  HMONITOR m_hMonitor = NULL;
  bool m_redraw_by_monitor_change = false;
  // 阴影位图缓存：避免每个候选每帧重新做一次高斯模糊
  std::unique_ptr<Gdiplus::Bitmap> m_shadowCache;
  int m_shadowCacheW = 0;
  int m_shadowCacheH = 0;
  int m_shadowCacheRadius = -1;
  int m_shadowCacheCorner = -1;
  int m_shadowCacheOffX = 0;
  int m_shadowCacheOffY = 0;
  COLORREF m_shadowCacheColor = 0x00000000;
  // 复用的离屏 DC/位图。原来每帧都 CreateCompatibleBitmap + BindDC，实测这两项
  // 合计约 0.95ms/帧（BindDC 占约 0.70ms），而窗口尺寸在按键过程中基本不变。
  HDC m_memDC = NULL;
  HBITMAP m_memBitmap = NULL;
  HGDIOBJ m_memOldBitmap = NULL;
  int m_memW = 0;
  int m_memH = 0;
  bool m_memBound = false;  // 当前 DC 是否已 BindDC 到 pDWR 的 render target
  void _ReleaseMemDC();
  // 绘制帧内文本路径的分段累计（只在 WEASEL_PAINT_LOG 打开时被写；每帧开始时清零）。
  // 用来回答"_TextOut 那 8ms 到底是建布局、画字形还是 EndDraw"这个问题。
  double m_profLayoutMs = 0;  // 本帧 CreateTextLayout 累计
  double m_profDrawMs = 0;    // 本帧 DrawTextLayoutAt 累计
  double m_profEndDrawMs = 0; // 本帧 EndDraw 累计
  int m_profTextOut = 0;      // 本帧 _TextOut 调用次数
};
