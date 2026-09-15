#include "stdafx.h"
#include "WeaselPanel.h"

#include <utility>
#include <ShellScalingApi.h>
#include <VersionHelpers.hpp>
#include <WeaselIPCData.h>
#include <WeaselPerfLog.h>
#include <algorithm>

#include "VerticalLayout.h"
#include "HorizontalLayout.h"
#include "FullScreenLayout.h"
#include "VHorizontalLayout.h"

// for IDI_ZH, IDI_EN
#include <resource.h>
#define COLORTRANSPARENT(color) ((color & 0xff000000) == 0)
#define COLORNOTTRANSPARENT(color) ((color & 0xff000000) != 0)
#define TRANS_COLOR 0x00000000
#define GDPCOLOR_FROM_COLORREF(color)                                \
  Gdiplus::Color::MakeARGB(((color >> 24) & 0xff), GetRValue(color), \
                           GetGValue(color), GetBValue(color))
#define HALF_ALPHA_COLOR(color) \
  ((((color & 0xff000000) >> 25) & 0xff) << 24) | (color & 0x00ffffff)

#pragma comment(lib, "Shcore.lib")

template <class t0, class t1, class t2>
inline void LoadIconNecessary(t0& a, t1& b, t2& c, int d) {
  if (a == b)
    return;
  a = b;
  if (b.empty())
    c.LoadIconW(d, STATUS_ICON_SIZE, STATUS_ICON_SIZE, LR_DEFAULTCOLOR);
  else
    c = (HICON)LoadImage(NULL, b.c_str(), IMAGE_ICON, STATUS_ICON_SIZE,
                         STATUS_ICON_SIZE, LR_LOADFROMFILE);
}

static inline void ReconfigRoundInfo(IsToRoundStruct& rd,
                                     const int& i,
                                     const int& m_candidateCount) {
  if (i == 0 && m_candidateCount > 1) {
    std::swap(rd.IsTopLeftNeedToRound, rd.IsBottomLeftNeedToRound);
    std::swap(rd.IsTopRightNeedToRound, rd.IsBottomRightNeedToRound);
  }
  if (i == m_candidateCount - 1) {
    std::swap(rd.IsTopLeftNeedToRound, rd.IsBottomLeftNeedToRound);
    std::swap(rd.IsTopRightNeedToRound, rd.IsBottomRightNeedToRound);
  }
}

WeaselPanel::WeaselPanel(weasel::UI& ui)
    : m_layout(NULL),
      m_ctx(ui.ctx()),
      m_octx(ui.octx()),
      m_status(ui.status()),
      m_in_server(ui.InServer()),
      m_style(ui.style()),
      m_ostyle(ui.ostyle()),
      m_candidateCount(0),
      m_lastCandidateCount(0),
      m_current_zhung_icon(),
      m_inputPos(CRect()),
      m_sticky(false),
      dpi(96),
      hide_candidates(false),
      pDWR(ui.pdwr()),
      _UICallback(ui.uiCallback()),
      _m_gdiplusToken(0) {
  m_iconDisabled.LoadIconW(IDI_RELOAD, STATUS_ICON_SIZE, STATUS_ICON_SIZE,
                           LR_DEFAULTCOLOR);
  m_iconEnabled.LoadIconW(IDI_ZH, STATUS_ICON_SIZE, STATUS_ICON_SIZE,
                          LR_DEFAULTCOLOR);
  m_iconAlpha.LoadIconW(IDI_EN, STATUS_ICON_SIZE, STATUS_ICON_SIZE,
                        LR_DEFAULTCOLOR);
  m_iconFull.LoadIconW(IDI_FULL_SHAPE, STATUS_ICON_SIZE, STATUS_ICON_SIZE,
                       LR_DEFAULTCOLOR);
  m_iconHalf.LoadIconW(IDI_HALF_SHAPE, STATUS_ICON_SIZE, STATUS_ICON_SIZE,
                       LR_DEFAULTCOLOR);
  // for gdi+ drawings, initialization
  GdiplusStartup(&_m_gdiplusToken, &_m_gdiplusStartupInput, NULL);

  HMONITOR hMonitor = MonitorFromRect(m_inputPos, MONITOR_DEFAULTTONEAREST);
  UINT dpiX = 96, dpiY = 96;
  if (hMonitor) {
    GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    m_hMonitor = hMonitor;
  }
  dpi = dpiX;
  _InitFontRes();
  m_ostyle = m_style;
}

WeaselPanel::~WeaselPanel() {
  // 必须在 GdiplusShutdown 之前释放 GDI+ 对象（成员 unique_ptr 的析构发生在
  // 析构函数体之后，会把 Bitmap 释放推到 GdiplusShutdown 之后，属未定义行为）
  m_shadowCache.reset();
  _ReleaseMemDC();
  Gdiplus::GdiplusShutdown(_m_gdiplusToken);
  delete m_layout;
  m_layout = NULL;
  // pDWR.reset();
}

void WeaselPanel::_ResizeWindow() {
  CDCHandle dc = GetDC();
  CSize m_size = m_layout->GetContentSize();
  SetWindowPos(NULL, 0, 0, m_size.cx, m_size.cy,
               SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOREDRAW);
  ReleaseDC(dc);
}

// 让候选窗"别横跨整屏"。真机数据（横向布局 + 万象长句候选）：一行 8 个候选能到
// 1934/1991px，而屏宽只有 1928px —— 于是 _RepositionWindow 的贴边夹取把它推到
// x=115（光标在 x=1074），整块盖在下面两行文字上。
//
// 只把窗口压到"工作区以内"是不够的：1805px 的面板照样被夹到光标左边 960px。
// 所以这里的目标是工作区的 2/3，手段按"信息损失从小到大"排序：
//   ① 字号若是上一帧缩过的，内容变窄了就恢复配置字号；
//   ② 截断候选**显示**文本（`style.candidate_abbreviate_length` 同一套语义：
//      只影响面板显示与 ITfCandidateListUIElement::GetString，上屏内容由服务端
//      commit 决定，不受影响），保留至少 6 字 + "..."；
//   ③ 候选已经到截断下限还超预算，才缩字号（下限 85%）。
// 同帧内只朝一个方向走（缩小），绝不放大 → 不会出现"窗口 1805 而字模 1934"
// 那种尺寸/字模不一致（那会把最右边的候选裁掉）。
// 返回 true = 已改动，调用方需要重新 DoLayout。
bool WeaselPanel::_FitToWorkAreaWidth() {
  if (!pDWR || !m_layout || m_ctx.empty())
    return false;
  CRect workArea;
  HMONITOR hMonitor = MonitorFromRect(m_inputPos, MONITOR_DEFAULTTONEAREST);
  if (hMonitor) {
    MONITORINFO info;
    info.cbSize = sizeof(MONITORINFO);
    if (GetMonitorInfo(hMonitor, &info))
      workArea = info.rcWork;
  }
  if (workArea.IsRectEmpty())
    return false;
  const int avail = workArea.Width() - 8;  // 圆角/阴影余量
  const int budget = avail * 2 / 3;        // 目标：工作区的 2/3
  const int cx = m_layout->GetContentSize().cx;
  if (cx <= 0 || budget <= 0)
    return false;

  // ① 字号恢复：缩过字号、而按估算复原后仍能装进预算，就回到配置字号。
  //    估算偏乐观时下一帧会再缩回去（幅度很小，不会来回抖）。
  if (m_fitFontPercent < 100) {
    const long long fullEst =
        (long long)cx * 100 * 103 / (m_fitFontPercent * 100);
    if (fullEst <= budget) {
      if (_ApplyFitFontPercent(100)) {
        _FitLog(cx, budget, 0, 100);
        return true;
      }
      return false;
    }
    return false;  // 还得保持缩过的字号：本帧不动
  }
  if (cx <= budget)
    return false;  // 收敛

  // ② 截断候选显示文本
  size_t maxLen = 0;
  for (const auto& c : m_ctx.cinfo.candies)
    if (c.str.size() > maxLen)
      maxLen = c.str.size();
  const int kMinChars = 6;
  if (maxLen > (size_t)kMinChars) {
    // 用"上一点"做两点线性内插（宽度 ≈ 固定开销 + k×字数），通常一两轮就到位；
    // 没有上一点时退化成按比例估。
    int limit;
    if (m_fitTryLimit > 0 && m_fitTryCx > cx && m_fitTryLimit > (int)maxLen) {
      const double slope = (double)(m_fitTryCx - cx) /
                           (double)(m_fitTryLimit - (int)maxLen);
      const double fixed = cx - slope * (int)maxLen;
      limit = slope > 0.0 ? (int)((budget - fixed) / slope) : (int)maxLen;
    } else {
      limit = (int)((long long)maxLen * budget / cx);
    }
    if (limit < kMinChars)
      limit = kMinChars;
    if (limit < (int)maxLen) {
      m_fitTryLimit = limit;
      m_fitTryCx = cx;
      for (auto& c : m_ctx.cinfo.candies) {
        if (c.str.size() > (size_t)limit)
          c.str = c.str.substr(0, limit - 1) + L"...";
      }
      _FitLog(cx, budget, limit, 100);
      return true;
    }
  }

  // ③ 候选已经很短还是超预算：缩字号（下限 85%，不再往下缩）
  const int kMinPercent = 85;
  int next = (int)((long long)100 * budget / cx);
  if (next < kMinPercent)
    next = kMinPercent;
  if (next >= 100)
    return false;
  if (_ApplyFitFontPercent(next)) {
    _FitLog(cx, budget, 0, next);
    return true;
  }
  return false;
}

bool WeaselPanel::_ApplyFitFontPercent(int percent) {
  const int kMinPoint = 8;  // 绝对下限：再小也没法看
  const int labelPoint = m_style.label_font_point * percent / 100;
  int point = m_style.font_point * percent / 100;
  const int commentPoint = m_style.comment_font_point * percent / 100;
  if (point < kMinPoint)
    point = kMinPoint;
  try {
    pDWR->InitResources(
        m_style.label_font_face,
        labelPoint > 0 ? labelPoint : m_style.label_font_point, m_style.font_face,
        point, m_style.comment_font_face,
        commentPoint > 0 ? commentPoint : m_style.comment_font_point,
        m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT);
  } catch (...) {
    // InitResources 里的 HR() 会抛 ComException。Refresh() 不在绘制路径的异常
    // 边界内，这里必须就地收住：本帧不改字号（绘制路径按 pDWR 判空降级），
    // 与 B1b「DirectWrite 初始化失败不崩宿主」的口径一致。
    weasel::perf::PosLog& log = weasel::perf::PosLog::Instance();
    if (log.enabled())
      log.Writef("[ui] fit aborted: InitResources threw");
    return false;
  }
  m_fitFontPercent = percent;
  return true;
}

void WeaselPanel::_FitLog(int cx, int budget, int trimChars, int percent) {
  weasel::perf::PosLog& log = weasel::perf::PosLog::Instance();
  if (log.enabled())
    log.Writef("[ui] fit cx=%d budget=%d trim=%d pct=%d", cx, budget, trimChars,
               percent);
}

void WeaselPanel::_CreateLayout() {
  if (m_layout != NULL)
    delete m_layout;

  Layout* layout = NULL;
  if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT) {
    layout = new VHorizontalLayout(m_style, m_ctx, m_status, pDWR);
  } else {
    if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL ||
        m_style.layout_type == UIStyle::LAYOUT_VERTICAL_FULLSCREEN) {
      layout = new VerticalLayout(m_style, m_ctx, m_status, pDWR);
    } else if (m_style.layout_type == UIStyle::LAYOUT_HORIZONTAL ||
               m_style.layout_type == UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN) {
      layout = new HorizontalLayout(m_style, m_ctx, m_status, pDWR);
    }

    if (IS_FULLSCREENLAYOUT(m_style)) {
      layout = new FullScreenLayout(m_style, m_ctx, m_status, m_inputPos,
                                    layout, pDWR);
    }
  }
  m_layout = layout;
}

// 更新界面
void WeaselPanel::Refresh() {
  bool should_show_icon =
      (m_status.ascii_mode || !m_status.composing || !m_ctx.aux.empty());
  m_candidateCount = min(m_ctx.cinfo.candies.size(), MAX_CANDIDATES_COUNT);
  // When the candidate window changes from having content to having no content,
  // reset the sticky state
  if (m_lastCandidateCount > 0 && m_candidateCount == 0) {
    m_sticky = false;
  }
  m_lastCandidateCount = m_candidateCount;
  // check if to hide candidates window
  // show tips status, two kind of situation: 1) only aux strings, don't care
  // icon status; 2)only icon(ascii mode switching)
  bool show_tips =
      m_in_server &&
      ((!m_ctx.aux.empty() && m_ctx.cinfo.empty() && m_ctx.preedit.empty()) ||
       (m_ctx.empty() && should_show_icon));
  // show schema menu status: schema_id == L".default"
  bool show_schema_menu = m_status.schema_id == L".default";
  bool margin_negative =
      (DPI_SCALE(m_style.margin_x) < 0 || DPI_SCALE(m_style.margin_y) < 0);
  // when to hide_cadidates?
  // 1. margin_negative, and not in show tips mode( ascii switching / half-full
  // switching / simp-trad switching / error tips), and not in schema menu
  // 2. inline preedit without candidates
  bool inline_no_candidates =
      (m_style.inline_preedit && m_candidateCount == 0) && !show_tips;
  hide_candidates = inline_no_candidates ||
                    (margin_negative && !show_tips && !show_schema_menu);

  // only RedrawWindow if no need to hide candidates window, or
  // inline_no_candidates
  if (!hide_candidates || inline_no_candidates) {
    const bool content_changed = (m_ctx != m_octx);
    const bool style_changed = (m_ostyle != m_style);  // 必须在 _InitFontRes() 之前取
    const bool rebuild = (!m_layout || content_changed || style_changed);
    weasel::perf::PosLog& log = weasel::perf::PosLog::Instance();
    if (log.enabled())
      log.Writef("[ui] refresh changed=%d style=%d rebuild=%d hide=%d cand=%d "
                 "pre=%u sticky=%d ip=%ld,%ld,%ld,%ld srv=%d",
                 content_changed ? 1 : 0, style_changed ? 1 : 0, rebuild ? 1 : 0,
                 hide_candidates ? 1 : 0, (int)m_candidateCount,
                 (unsigned)m_ctx.preedit.str.size(), m_sticky ? 1 : 0,
                 m_inputPos.left, m_inputPos.top, m_inputPos.right,
                 m_inputPos.bottom, m_in_server ? 1 : 0);
    // 内容与样式都没变时不必重建 Layout、不必重算布局。
    // 这段跑在 IPC 管道线程上（持 g_api_mutex），省下的时间属于全部客户端。
    if (rebuild) {
      _InitFontRes();
      if (!pDWR)
        return;  // DirectWrite 资源不可用：本帧不建布局、不绘制
      _CreateLayout();

      CDCHandle dc = GetDC();
      // 候选行比预算宽时截断显示文本 / 缩字号重排，直到收敛或到下限。真机上
      // （横向布局 + 万象长句候选）见过 1934/1991px 的窗口 vs 1928px 屏宽：窗口
      // 随后被 _RepositionWindow 的贴边夹取推到光标左边近千像素，盖住整行文字。
      m_fitTryLimit = -1;
      m_fitTryCx = 0;
      for (int guard = 0; guard < 4; guard++) {
        m_layout->DoLayout(dc, pDWR);
        if (!_FitToWorkAreaWidth())
          break;
      }
      ReleaseDC(dc);
      _ResizeWindow();
    }
    _RepositionWindow();  // 保持与原代码一致：每次都执行（只查显示器+SetWindowPos，便宜且安全）
    if (content_changed) {
      m_octx = m_ctx;
      RedrawWindow();
    }
  }
}

void WeaselPanel::_InitFontRes(bool forced) {
  HMONITOR hMonitor = MonitorFromRect(m_inputPos, MONITOR_DEFAULTTONEAREST);
  UINT dpiX = 96, dpiY = 96;
  if (hMonitor)
    GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
  // prepare d2d1 resources
  // if style changed, or dpi changed, or pDWR NULL, re-initialize directwrite
  // resources
  if (forced || (pDWR == NULL) || (m_ostyle != m_style) || (dpiX != dpi)) {
    pDWR.reset();
    m_memBound = false;  // 新的 render target 需要重新 BindDC（复用 DC 时不会自动重绑）
    m_fitFontPercent = 100;  // 新资源是按皮肤配置的字号建的
    try {
      pDWR = std::make_shared<DirectWriteResources>(m_style, dpiX);
      pDWR->pRenderTarget->SetTextAntialiasMode(
          (D2D1_TEXT_ANTIALIAS_MODE)m_style.antialias_mode);
    } catch (...) {
      // DirectWrite/D2D 初始化失败的异常必须在这里就地收住：调用方
      // （Refresh / DoPaint / _DrawCandidates / _DrawPreedit / _DrawPreeditBack）
      // 靠 pDWR 判空降级为「本帧不绘制」。若让异常逃进 WM_CREATE / WM_PAINT，
      // 宿主进程会直接崩溃（#1906），而这里只是没有 DirectWrite 资源。
      pDWR.reset();
    }
  }
  m_ostyle = m_style;
  dpi = dpiX;
  dpiScaleLayout = (float)dpi / 96.0f;
}

static HBITMAP CopyDCToBitmap(HDC hDC, LPRECT lpRect) {
  if (!hDC || !lpRect || IsRectEmpty(lpRect))
    return NULL;
  HDC hMemDC = NULL;
  HBITMAP hBitmap = NULL, hOldBitmap = NULL;
  int nX, nY, nX2, nY2;
  int nWidth, nHeight;

  nX = lpRect->left;
  nY = lpRect->top;
  nX2 = lpRect->right;
  nY2 = lpRect->bottom;
  nWidth = nX2 - nX;
  nHeight = nY2 - nY;

  hMemDC = CreateCompatibleDC(hDC);
  if (!hMemDC)
    return NULL;

  hBitmap = CreateCompatibleBitmap(hDC, nWidth, nHeight);
  if (!hBitmap) {
    DeleteDC(hMemDC);
    return NULL;
  }

  hOldBitmap = (HBITMAP)SelectObject(hMemDC, hBitmap);
  if (!hOldBitmap) {
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    return NULL;
  }

  if (!BitBlt(hMemDC, 0, 0, nWidth, nHeight, hDC, nX, nY, SRCCOPY)) {
    // restore and cleanup
    SelectObject(hMemDC, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    return NULL;
  }

  SelectObject(hMemDC, hOldBitmap);
  DeleteDC(hMemDC);
  return hBitmap;
}

void WeaselPanel::_CaptureRect(CRect& rect) {
  HDC ScreenDC = ::GetDC(NULL);
  CRect rc;
  GetWindowRect(&rc);
  POINT WindowPosAtScreen = {rc.left, rc.top};
  CRect captureRect = rect;
  captureRect.OffsetRect(WindowPosAtScreen);
  // create bitmap first (avoid holding clipboard while capturing)
  HBITMAP bmp = CopyDCToBitmap(ScreenDC, LPRECT(captureRect));
  if (!bmp) {
    ::ReleaseDC(NULL, ScreenDC);
    return;
  }

  // capture input window to clipboard
  if (!OpenClipboard()) {
    DEBUG << "_CaptureRect: OpenClipord ailed";
    DeleteObject(bmp);
    ::ReleaseDC(NULL, ScreenDC);
    return;
  }
  EmptyClipboard();
  if (!SetClipboardData(CF_BITMAP, bmp)) {
    DEBUG << "_CaptureRect: SetClipboardData failed";
    DeleteObject(bmp);
  }
  CloseClipboard();
  ::ReleaseDC(NULL, ScreenDC);
}

LRESULT WeaselPanel::OnMouseActivate(UINT uMsg,
                                     WPARAM wParam,
                                     LPARAM lParam,
                                     BOOL& bHandled) {
  bHandled = true;
  return MA_NOACTIVATE;
}

LRESULT WeaselPanel::OnMouseWheel(UINT uMsg,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  BOOL& bHandled) {
  int delta = GET_WHEEL_DELTA_WPARAM(wParam);
  if (_UICallback && delta != 0) {
    bool nextpage = delta < 0;
    _UICallback(NULL, NULL, NULL, &nextpage);
  }
  bHandled = true;
  return 0;
}

LRESULT WeaselPanel::OnLeftClickedUp(UINT uMsg,
                                     WPARAM wParam,
                                     LPARAM lParam,
                                     BOOL& bHandled) {
  if (hide_candidates) {
    bHandled = true;
    return 0;
  }
  CPoint point;
  point.x = GET_X_LPARAM(lParam);
  point.y = GET_Y_LPARAM(lParam);

  ::KillTimer(m_hWnd, AUTOREV_TIMER);
  bar_scale_ = 1.0;
  ptimer = 0;
  {
    // select by click
    CRect rect = m_layout->GetCandidateRect((int)m_ctx.cinfo.highlighted);
    if (m_istorepos)
      rect.OffsetRect(0, m_offsetys[m_ctx.cinfo.highlighted]);
    rect.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                     DPI_SCALE(m_style.hilite_padding_y));
    if (rect.PtInRect(point)) {
      size_t i = m_ctx.cinfo.highlighted;
      if (_UICallback) {
        m_mouse_entry = false;
        _UICallback(&i, NULL, NULL, NULL);
        if (!m_status.composing)
          DestroyWindow();
      }
    } else {
      RedrawWindow();
    }
  }
  bHandled = true;
  return 0;
}

LRESULT WeaselPanel::OnLeftClickedDown(UINT uMsg,
                                       WPARAM wParam,
                                       LPARAM lParam,
                                       BOOL& bHandled) {
  if (hide_candidates) {
    bHandled = true;
    return 0;
  }
  CPoint point;
  point.x = GET_X_LPARAM(lParam);
  point.y = GET_Y_LPARAM(lParam);

  // capture
  if (m_style.click_to_capture) {
    CRect recth = m_layout->GetCandidateRect((int)m_ctx.cinfo.highlighted);
    if (m_istorepos)
      recth.OffsetRect(0, m_offsetys[m_ctx.cinfo.highlighted]);
    recth.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                      DPI_SCALE(m_style.hilite_padding_y));
    // capture widow
    if (recth.PtInRect(point))
      _CaptureRect(recth);
    else {
      // if shadow_color transparent, decrease the capture rectangle size
      if (COLORTRANSPARENT(m_style.shadow_color) &&
          DPI_SCALE(m_style.shadow_radius) != 0) {
        CRect crc(rcw);
        int shadow_gap = (DPI_SCALE(m_style.shadow_offset_x) == 0 &&
                          DPI_SCALE(m_style.shadow_offset_y) == 0)
                             ? 2 * DPI_SCALE(m_style.shadow_radius)
                             : DPI_SCALE(m_style.shadow_radius) +
                                   DPI_SCALE(m_style.shadow_radius) / 2;
        int ofx = DPI_SCALE(m_style.hilite_padding_x) +
                              abs(DPI_SCALE(m_style.shadow_offset_x)) +
                              shadow_gap >
                          abs(DPI_SCALE(m_style.margin_x))
                      ? DPI_SCALE(m_style.hilite_padding_x) +
                            abs(DPI_SCALE(m_style.shadow_offset_x)) +
                            shadow_gap - abs(DPI_SCALE(m_style.margin_x))
                      : 0;
        int ofy = DPI_SCALE(m_style.hilite_padding_y) +
                              abs(DPI_SCALE(m_style.shadow_offset_y)) +
                              shadow_gap >
                          abs(DPI_SCALE(m_style.margin_y))
                      ? DPI_SCALE(m_style.hilite_padding_y) +
                            abs(DPI_SCALE(m_style.shadow_offset_y)) +
                            shadow_gap - abs(DPI_SCALE(m_style.margin_y))
                      : 0;
        crc.DeflateRect(m_layout->offsetX - ofx, m_layout->offsetY - ofy);
        _CaptureRect(crc);
      } else {
        _CaptureRect(rcw);
      }
    }
  }
  {
    if (!m_style.inline_preedit && m_candidateCount != 0 &&
        COLORNOTTRANSPARENT(m_style.prevpage_color) &&
        COLORNOTTRANSPARENT(m_style.nextpage_color)) {
      // click prepage
      if (m_ctx.cinfo.currentPage != 0) {
        CRect prc = m_layout->GetPrepageRect();
        if (m_istorepos)
          prc.OffsetRect(0, m_offsety_preedit);
        if (prc.PtInRect(point)) {
          bool nextPage = false;
          if (_UICallback)
            _UICallback(NULL, NULL, &nextPage, NULL);
          bHandled = true;
          return 0;
        }
      }
      // click nextpage
      if (!m_ctx.cinfo.is_last_page) {
        CRect prc = m_layout->GetNextpageRect();
        if (m_istorepos)
          prc.OffsetRect(0, m_offsety_preedit);
        if (prc.PtInRect(point)) {
          bool nextPage = true;
          if (_UICallback)
            _UICallback(NULL, NULL, &nextPage, NULL);
          bHandled = true;
          return 0;
        }
      }
    }
    // select by click relative actions
    for (size_t i = 0; i < m_candidateCount && i < MAX_CANDIDATES_COUNT; ++i) {
      CRect rect = m_layout->GetCandidateRect((int)i);
      if (m_istorepos)
        rect.OffsetRect(0, m_offsetys[i]);
      rect.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                       DPI_SCALE(m_style.hilite_padding_y));
      if (rect.PtInRect(point)) {
        bar_scale_ = 0.8f;
        // modify highlighted
        if (i != m_ctx.cinfo.highlighted) {
          if (_UICallback)
            _UICallback(NULL, &i, NULL, NULL);
        } else {
          RedrawWindow();
        }
        ptimer = UINT_PTR(this);
        ::SetTimer(m_hWnd, AUTOREV_TIMER, 1000, &WeaselPanel::OnTimer);
        bHandled = true;
        return 0;
      }
    }
  }
  bHandled = true;
  return 0;
}

UINT_PTR WeaselPanel::ptimer = 0;
VOID CALLBACK WeaselPanel::OnTimer(_In_ HWND hwnd,
                                   _In_ UINT uMsg,
                                   _In_ UINT_PTR idEvent,
                                   _In_ DWORD dwTime) {
  ::KillTimer(hwnd, idEvent);
  WeaselPanel* self = (WeaselPanel*)ptimer;
  ptimer = 0;
  if (self) {
    self->bar_scale_ = 1.0;
    self->RedrawWindow();
  }
}

LRESULT WeaselPanel::OnMouseMove(UINT uMsg,
                                 WPARAM wParam,
                                 LPARAM lParam,
                                 BOOL& bHandled) {
  if (m_style.hover_type == UIStyle::NONE)
    return 0;
  if (m_mouse_entry == false) {
    TRACKMOUSEEVENT tme;
    tme.cbSize = sizeof(TRACKMOUSEEVENT);
    tme.dwFlags = TME_LEAVE;
    tme.dwHoverTime = 20;  // unit: ms
    tme.hwndTrack = m_hWnd;
    TrackMouseEvent(&tme);
  }
  bHandled = true;
  m_mouse_entry = true;
  CPoint point;
  point.x = GET_X_LPARAM(lParam);
  point.y = GET_Y_LPARAM(lParam);

  // Ignore if mouse screen position not changed
  CPoint ptScreen = point;
  ClientToScreen(&ptScreen);
  if (ptScreen == m_lastMousePos || m_lastMousePos.x == -1) {
    if (m_lastMousePos.x == -1)
      m_lastMousePos = ptScreen;
    return 0;
  }
  m_lastMousePos = ptScreen;

  for (size_t i = 0; i < m_candidateCount && i < MAX_CANDIDATES_COUNT; ++i) {
    CRect rect = m_layout->GetCandidateRect((int)i);
    if (m_istorepos)
      rect.OffsetRect(0, m_offsetys[i]);
    rect.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                     DPI_SCALE(m_style.hilite_padding_y));
    if (rect.PtInRect(point)) {
      if (i != m_ctx.cinfo.highlighted) {
        if (m_style.hover_type == UIStyle::HoverType::HILITE) {
          if (_UICallback)
            _UICallback(NULL, &i, NULL, NULL);
        } else if (m_hoverIndex != i) {
          m_hoverIndex = static_cast<int>(i);
          InvalidateRect(&rcw, true);
        }
      } else if (m_style.hover_type == UIStyle::HoverType::SEMI_HILITE &&
                 m_hoverIndex != -1) {
        m_hoverIndex = -1;
        InvalidateRect(&rcw, true);
      }
    }
  }
  return 0;
}

LRESULT WeaselPanel::OnMouseLeave(UINT uMsg,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  BOOL& bHandled) {
  m_hoverIndex = -1;
  InvalidateRect(&rcw, true);
  m_mouse_entry = false;
  return 0;
}

void WeaselPanel::_HighlightText(CDCHandle& dc,
                                 const CRect& rc,
                                 const COLORREF& color,
                                 const COLORREF& shadowColor,
                                 const int& radius,
                                 const BackType& type = BackType::TEXT,
                                 const IsToRoundStruct& rd = IsToRoundStruct(),
                                 const COLORREF& bordercolor = TRANS_COLOR) {
  // Graphics obj with SmoothingMode
  Gdiplus::Graphics g_back(dc);
  g_back.SetSmoothingMode(Gdiplus::SmoothingMode::SmoothingModeHighQuality);

  // blur buffer
  int blurMarginX = m_layout->offsetX;
  int blurMarginY = m_layout->offsetY;

  GraphicsRoundRectPath* hiliteBackPath;
  if (rd.Hemispherical && type != BackType::BACKGROUND &&
      NOT_FULLSCREENLAYOUT(m_style))
    hiliteBackPath = new GraphicsRoundRectPath(
        rc,
        DPI_SCALE(m_style.round_corner_ex) -
            (DPI_SCALE(m_style.border) % 2 ? DPI_SCALE(m_style.border) / 2 : 0),
        rd.IsTopLeftNeedToRound, rd.IsTopRightNeedToRound,
        rd.IsBottomRightNeedToRound, rd.IsBottomLeftNeedToRound);
  else  // background or current candidate background not out of window
        // background
    hiliteBackPath = new GraphicsRoundRectPath(rc, radius);

  // 必须shadow_color都是非完全透明色才做绘制, 全屏状态不绘制阴影保证响应速度
  if (DPI_SCALE(m_style.shadow_radius) && COLORNOTTRANSPARENT(shadowColor) &&
      NOT_FULLSCREENLAYOUT(m_style)) {
    BYTE r = GetRValue(shadowColor);
    BYTE g = GetGValue(shadowColor);
    BYTE b = GetBValue(shadowColor);
    BYTE alpha = (BYTE)((shadowColor >> 24) & 255);
    const int sw = (INT)rc.Width() + blurMarginX * 2;
    const int sh = (INT)rc.Height() + blurMarginY * 2;
    const int sr = DPI_SCALE(m_style.shadow_radius);
    const int sox = DPI_SCALE(m_style.shadow_offset_x);
    const int soy = DPI_SCALE(m_style.shadow_offset_y);
    if (!m_shadowCache || m_shadowCacheW != sw || m_shadowCacheH != sh ||
        m_shadowCacheRadius != sr || m_shadowCacheCorner != radius ||
        m_shadowCacheColor != shadowColor || m_shadowCacheOffX != sox ||
        m_shadowCacheOffY != soy) {
      m_shadowCache.reset(new Gdiplus::Bitmap(sw, sh, PixelFormat32bppPARGB));
      m_shadowCacheW = sw;
      m_shadowCacheH = sh;
      m_shadowCacheRadius = sr;
      m_shadowCacheCorner = radius;
      m_shadowCacheColor = shadowColor;
      m_shadowCacheOffX = sox;
      m_shadowCacheOffY = soy;

      CRect rect(blurMarginX + sox, blurMarginY + soy,
                 rc.Width() + blurMarginX + sox,
                 rc.Height() + blurMarginY + soy);
      Gdiplus::Color shadow_color = Gdiplus::Color::MakeARGB(alpha, r, g, b);
      Gdiplus::Graphics g_shadow(m_shadowCache.get());
      g_shadow.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
      if (sox != 0 || soy != 0) {
        GraphicsRoundRectPath shadow_path(rect, radius);
        Gdiplus::SolidBrush shadow_brush(shadow_color);
        g_shadow.FillPath(&shadow_brush, &shadow_path);
      } else {
        int step = alpha / sr / 2;
        Gdiplus::Pen pen_shadow(shadow_color, (Gdiplus::REAL)1);
        for (int i = 0; i < sr; i++) {
          GraphicsRoundRectPath round_path(rect, radius + 1 + i);
          g_shadow.DrawPath(&pen_shadow, &round_path);
          shadow_color = Gdiplus::Color::MakeARGB(alpha - i * step, r, g, b);
          pen_shadow.SetColor(shadow_color);
          rect.InflateRect(1, 1);
        }
      }
      DoGaussianBlur(m_shadowCache.get(), (float)sr, (float)sr);
    }
    g_back.DrawImage(m_shadowCache.get(), rc.left - blurMarginX,
                     rc.top - blurMarginY);
  }

  // 必须back_color非完全透明才绘制
  if (COLORNOTTRANSPARENT(color)) {
    Gdiplus::Color back_color = GDPCOLOR_FROM_COLORREF(color);
    Gdiplus::SolidBrush back_brush(back_color);
    g_back.FillPath(&back_brush, hiliteBackPath);
  }
  // draw border, for bordercolor not transparent and border valid
  if (COLORNOTTRANSPARENT(bordercolor) && DPI_SCALE(m_style.border) > 0) {
    Gdiplus::Color border_color = GDPCOLOR_FROM_COLORREF(bordercolor);
    Gdiplus::Pen gPenBorder(border_color,
                            (Gdiplus::REAL)DPI_SCALE(m_style.border));
    // candidate window border
    if (type == BackType::BACKGROUND) {
      GraphicsRoundRectPath bgPath(rc, DPI_SCALE(m_style.round_corner_ex));
      g_back.DrawPath(&gPenBorder, &bgPath);
    } else if (type !=
               BackType::TEXT)  // hilited_candidate_border / candidate_border
      g_back.DrawPath(&gPenBorder, hiliteBackPath);
  }
  // free memory
  delete hiliteBackPath;
  hiliteBackPath = NULL;
}

// draw preedit text, text only
bool WeaselPanel::_DrawPreedit(const Text& text,
                               CDCHandle dc,
                               const CRect& rc) {
  bool drawn = false;
  if (!pDWR)
    return false;
  std::wstring const& t = text.str;
  IDWriteTextFormat1* txtFormat = pDWR->pPreeditTextFormat.Get();

  if (!t.empty()) {
    weasel::TextRange range = m_layout->GetPreeditRange();

    if (range.start < range.end) {
      std::wstring before_str = t.substr(0, range.start);
      std::wstring hilited_str = t.substr(range.start, range.end);
      std::wstring after_str = t.substr(range.end);
      CSize beforeSz = m_layout->GetBeforeSize();
      CSize hilitedSz = m_layout->GetHilitedSize();
      CSize afterSz = m_layout->GetAfterSize();

      int x = rc.left;
      int y = rc.top;

      if (range.start > 0) {
        // zzz
        std::wstring str_before(t.substr(0, range.start));
        CRect rc_before;
        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT)
          rc_before = CRect(rc.left, y, rc.right, y + beforeSz.cy);
        else
          rc_before = CRect(x, rc.top, rc.left + beforeSz.cx, rc.bottom);
        _TextOut(rc_before, str_before.c_str(), str_before.length(),
                 m_style.text_color, txtFormat);
        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT)
          y += beforeSz.cy + DPI_SCALE(m_style.hilite_spacing);
        else
          x += beforeSz.cx + DPI_SCALE(m_style.hilite_spacing);
      }
      {
        // zzz[yyy]
        std::wstring str_highlight(
            t.substr(range.start, (size_t)range.end - range.start));
        CRect rc_hi;

        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT)
          rc_hi = CRect(rc.left, y, rc.right, y + hilitedSz.cy);
        else
          rc_hi = CRect(x, rc.top, x + hilitedSz.cx, rc.bottom);
        _TextOut(rc_hi, str_highlight.c_str(), str_highlight.length(),
                 m_style.hilited_text_color, txtFormat);
        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT)
          y += rc_hi.Height() + DPI_SCALE(m_style.hilite_spacing);
        else
          x += rc_hi.Width() + DPI_SCALE(m_style.hilite_spacing);
      }
      if (range.end < static_cast<int>(t.length())) {
        // zzz[yyy]xxx
        std::wstring str_after(t.substr(range.end));
        CRect rc_after;
        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT)
          rc_after = CRect(rc.left, y, rc.right, y + afterSz.cy);
        else
          rc_after = CRect(x, rc.top, x + afterSz.cx, rc.bottom);
        _TextOut(rc_after, str_after.c_str(), str_after.length(),
                 m_style.text_color, txtFormat);
      }
    } else {
      CRect rcText(rc.left, rc.top, rc.right, rc.bottom);
      _TextOut(rcText, t.c_str(), t.length(), m_style.text_color, txtFormat);
    }
    // draw pager mark if not inline_preedit if necessary
    if (m_candidateCount && !m_style.inline_preedit &&
        COLORNOTTRANSPARENT(m_style.prevpage_color) &&
        COLORNOTTRANSPARENT(m_style.nextpage_color)) {
      const std::wstring pre = L"<";
      const std::wstring next = L">";
      CRect prc = m_layout->GetPrepageRect();
      // clickable color / disabled color
      int color =
          m_ctx.cinfo.currentPage ? m_style.prevpage_color : m_style.text_color;
      if (m_istorepos)
        prc.OffsetRect(0, m_offsety_preedit);
      _TextOut(prc, pre.c_str(), pre.length(), color, txtFormat);

      CRect nrc = m_layout->GetNextpageRect();
      // clickable color / disabled color
      color = m_ctx.cinfo.is_last_page ? m_style.text_color
                                       : m_style.nextpage_color;
      if (m_istorepos)
        nrc.OffsetRect(0, m_offsety_preedit);
      _TextOut(nrc, next.c_str(), next.length(), color, txtFormat);
    }
    drawn = true;
  }
  return drawn;
}

// draw hilited back color, back only
bool WeaselPanel::_DrawPreeditBack(const Text& text,
                                   CDCHandle dc,
                                   const CRect& rc) {
  bool drawn = false;
  if (!pDWR)
    return false;
  std::wstring const& t = text.str;
  IDWriteTextFormat1* txtFormat = pDWR->pPreeditTextFormat.Get();

  if (!t.empty()) {
    weasel::TextRange range = m_layout->GetPreeditRange();

    if (range.start < range.end) {
      CSize beforeSz = m_layout->GetBeforeSize();
      CSize hilitedSz = m_layout->GetHilitedSize();

      int x = rc.left;
      int y = rc.top;

      if (range.start > 0) {
        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT)
          y += beforeSz.cy + DPI_SCALE(m_style.hilite_spacing);
        else
          x += beforeSz.cx + DPI_SCALE(m_style.hilite_spacing);
      }
      {
        CRect rc_hi;
        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT)
          rc_hi = CRect(rc.left, y, rc.right, y + hilitedSz.cy);
        else
          rc_hi = CRect(x, rc.top, x + hilitedSz.cx, rc.bottom);
        // if preedit rect size smaller than icon, fill the gap to
        // STATUS_ICON_SIZE
        if (m_layout->ShouldDisplayStatusIcon()) {
          if ((m_style.layout_type == UIStyle::LAYOUT_HORIZONTAL ||
               m_style.layout_type == UIStyle::LAYOUT_VERTICAL) &&
              hilitedSz.cy < STATUS_ICON_SIZE)
            rc_hi.InflateRect(0, (STATUS_ICON_SIZE - hilitedSz.cy) / 2);
          if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT &&
              hilitedSz.cx < STATUS_ICON_SIZE)
            rc_hi.InflateRect((STATUS_ICON_SIZE - hilitedSz.cx) / 2, 0);
        }

        rc_hi.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                          DPI_SCALE(m_style.hilite_padding_y));
        IsToRoundStruct rd = m_layout->GetTextRoundInfo();
        if (m_istorepos) {
          std::swap(rd.IsTopLeftNeedToRound, rd.IsBottomLeftNeedToRound);
          std::swap(rd.IsTopRightNeedToRound, rd.IsBottomRightNeedToRound);
        }
        _HighlightText(dc, rc_hi, m_style.hilited_back_color,
                       m_style.hilited_shadow_color,
                       DPI_SCALE(m_style.round_corner), BackType::TEXT, rd);
      }
    }
    drawn = true;
  }
  return drawn;
}

bool WeaselPanel::_DrawCandidates(CDCHandle& dc, bool back) {
  bool drawn = false;
  const std::vector<Text>& candidates(m_ctx.cinfo.candies);
  const std::vector<Text>& comments(m_ctx.cinfo.comments);
  const std::vector<Text>& labels(m_ctx.cinfo.labels);
  // prevent all text format nullptr
  if (!pDWR) {          // 上次 _InitFontRes 创建失败（抛异常）时 pDWR 为空
    _InitFontRes(true);
    if (!pDWR)          // 仍失败：本帧放弃绘制，绝不空指针解引用
      return false;
  }
  if (pDWR->pTextFormat.Get() == nullptr &&
      pDWR->pLabelTextFormat.Get() == nullptr &&
      pDWR->pCommentTextFormat.Get() == nullptr) {
    _InitFontRes(true);
    if (!pDWR)
      return false;
  }
  ComPtr<IDWriteTextFormat1> txtFormat = pDWR->pTextFormat;
  ComPtr<IDWriteTextFormat1> labeltxtFormat = pDWR->pLabelTextFormat;
  ComPtr<IDWriteTextFormat1> commenttxtFormat = pDWR->pCommentTextFormat;
  BackType bkType = BackType::CAND;

  CRect rect;
  // draw back color and shadow color, with gdi+
  if (back) {
    // if candidate_shadow_color not transparent, draw candidate shadow first
    if (COLORNOTTRANSPARENT(m_style.candidate_shadow_color)) {
      for (auto i = 0; i < m_candidateCount && i < MAX_CANDIDATES_COUNT; ++i) {
        if (i == m_ctx.cinfo.highlighted || i == m_hoverIndex)
          continue;  // draw non hilited candidates only
        rect = m_layout->GetCandidateRect((int)i);
        IsToRoundStruct rd = m_layout->GetRoundInfo(i);
        if (m_istorepos) {
          rect.OffsetRect(0, m_offsetys[i]);
          ReconfigRoundInfo(rd, i, m_candidateCount);
        }
        rect.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                         DPI_SCALE(m_style.hilite_padding_y));
        _HighlightText(dc, rect, 0x00000000, m_style.candidate_shadow_color,
                       DPI_SCALE(m_style.round_corner), bkType, rd);
        drawn = true;
      }
    }
    // draw non highlighted candidates, without shadow
    if ((COLORNOTTRANSPARENT(m_style.candidate_back_color) ||
         COLORNOTTRANSPARENT(m_style.candidate_border_color))) {
      for (auto i = 0; i < m_candidateCount && i < MAX_CANDIDATES_COUNT; ++i) {
        if (i == m_ctx.cinfo.highlighted || i == m_hoverIndex)
          continue;
        rect = m_layout->GetCandidateRect((int)i);
        IsToRoundStruct rd = m_layout->GetRoundInfo(i);
        if (m_istorepos) {
          rect.OffsetRect(0, m_offsetys[i]);
          ReconfigRoundInfo(rd, i, m_candidateCount);
        }
        rect.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                         DPI_SCALE(m_style.hilite_padding_y));
        _HighlightText(dc, rect, m_style.candidate_back_color, 0x00000000,
                       DPI_SCALE(m_style.round_corner), bkType, rd,
                       m_style.candidate_border_color);
        drawn = true;
      }
    }
    // draw semi-hilite background and shadow
    if (m_hoverIndex >= 0) {
      rect = m_layout->GetCandidateRect(m_hoverIndex);
      IsToRoundStruct rd = m_layout->GetRoundInfo(m_hoverIndex);
      if (m_istorepos) {
        rect.OffsetRect(0, m_offsetys[m_hoverIndex]);
        ReconfigRoundInfo(rd, m_hoverIndex, m_candidateCount);
      }
      rect.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                       DPI_SCALE(m_style.hilite_padding_y));
      _HighlightText(dc, rect,
                     HALF_ALPHA_COLOR(m_style.hilited_candidate_back_color),
                     HALF_ALPHA_COLOR(m_style.hilited_candidate_shadow_color),
                     DPI_SCALE(m_style.round_corner), bkType, rd,
                     HALF_ALPHA_COLOR(m_style.hilited_candidate_border_color));
    }
    // draw highlighted background and shadow
    {
      rect = m_layout->GetHighlightRect();
      bool markSt = bar_scale_ == 1.0 || (!m_style.mark_text.empty());
      IsToRoundStruct rd = m_layout->GetRoundInfo(m_ctx.cinfo.highlighted);
      if (m_istorepos) {
        rect.OffsetRect(0, m_offsetys[m_ctx.cinfo.highlighted]);
        ReconfigRoundInfo(rd, m_ctx.cinfo.highlighted, m_candidateCount);
      }
      rect.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                       DPI_SCALE(m_style.hilite_padding_y));
      _HighlightText(dc, rect, m_style.hilited_candidate_back_color,
                     markSt ? m_style.hilited_candidate_shadow_color : 0,
                     DPI_SCALE(m_style.round_corner), bkType, rd,
                     m_style.hilited_candidate_border_color);
      if (m_style.mark_text.empty() &&
          COLORNOTTRANSPARENT(m_style.hilited_mark_color)) {
        int height =
            min(rect.Height() - DPI_SCALE(m_style.hilite_padding_y) * 2,
                rect.Height() - DPI_SCALE(m_style.round_corner) * 2);
        int width = min(rect.Width() - DPI_SCALE(m_style.hilite_padding_x) * 2,
                        rect.Width() - DPI_SCALE(m_style.round_corner) * 2);
        width = min(width, static_cast<int>(rect.Width() * 0.618));
        height = min(height, static_cast<int>(rect.Height() * 0.618));
        if (bar_scale_ != 1.0f) {
          width = static_cast<int>(width * bar_scale_);
          height = static_cast<int>(height * bar_scale_);
        }
        Gdiplus::Graphics g_back(dc);
        g_back.SetSmoothingMode(
            Gdiplus::SmoothingMode::SmoothingModeHighQuality);
        Gdiplus::Color mark_color =
            GDPCOLOR_FROM_COLORREF(m_style.hilited_mark_color);
        Gdiplus::SolidBrush mk_brush(mark_color);
        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT) {
          int x = rect.left + (rect.Width() - width) / 2;
          CRect mkrc{x, rect.top, x + width, rect.top + m_layout->mark_height};
          GraphicsRoundRectPath mk_path(mkrc, mkrc.Height() / 2);
          g_back.FillPath(&mk_brush, &mk_path);
        } else {
          int y = rect.top + (rect.Height() - height) / 2;
          CRect mkrc{rect.left, y, rect.left + m_layout->mark_width,
                     y + height};
          GraphicsRoundRectPath mk_path(mkrc, mkrc.Width() / 2);
          g_back.FillPath(&mk_brush, &mk_path);
        }
      }
      drawn = true;
    }
  }
  // draw text with direct write
  else {
    // begin draw candidate texts
    int label_text_color, candidate_text_color, comment_text_color;
    for (auto i = 0; i < m_candidateCount && i < MAX_CANDIDATES_COUNT; ++i) {
      if (i == m_ctx.cinfo.highlighted || i == m_hoverIndex) {
        label_text_color = m_style.hilited_label_text_color;
        candidate_text_color = m_style.hilited_candidate_text_color;
        comment_text_color = m_style.hilited_comment_text_color;
      } else {
        label_text_color = m_style.label_text_color;
        candidate_text_color = m_style.candidate_text_color;
        comment_text_color = m_style.comment_text_color;
      }
      // Draw label
      std::wstring label = m_layout->GetLabelText(
          labels, (int)i, m_style.label_text_format.c_str());
      if (!label.empty()) {
        rect = m_layout->GetCandidateLabelRect((int)i);
        if (m_istorepos)
          rect.OffsetRect(0, m_offsetys[i]);
        _TextOut(rect, label.c_str(), label.length(), label_text_color,
                 labeltxtFormat.Get());
      }
      // Draw text
      std::wstring text = candidates.at(i).str;
      if (!text.empty()) {
        rect = m_layout->GetCandidateTextRect((int)i);
        if (m_istorepos)
          rect.OffsetRect(0, m_offsetys[i]);
        _TextOut(rect, text.c_str(), text.length(), candidate_text_color,
                 txtFormat.Get());
      }
      // Draw comment
      std::wstring comment = comments.at(i).str;
      if (!comment.empty() && COLORNOTTRANSPARENT(comment_text_color)) {
        rect = m_layout->GetCandidateCommentRect((int)i);
        if (m_istorepos)
          rect.OffsetRect(0, m_offsetys[i]);
        _TextOut(rect, comment.c_str(), comment.length(), comment_text_color,
                 commenttxtFormat.Get());
      }
      drawn = true;
    }
    // draw highlight mark
    {
      if (!m_style.mark_text.empty() &&
          COLORNOTTRANSPARENT(m_style.hilited_mark_color)) {
        CRect rc = m_layout->GetHighlightRect();
        if (m_istorepos)
          rc.OffsetRect(0, m_offsetys[m_ctx.cinfo.highlighted]);
        rc.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                       DPI_SCALE(m_style.hilite_padding_y));
        int vgap = m_layout->mark_height
                       ? (rc.Height() - m_layout->mark_height) / 2
                       : 0;
        int hgap =
            m_layout->mark_width ? (rc.Width() - m_layout->mark_width) / 2 : 0;
        CRect hlRc;
        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT)
          hlRc = CRect(rc.left + hgap,
                       rc.top + DPI_SCALE(m_style.hilite_padding_y),
                       rc.left + hgap + m_layout->mark_width,
                       rc.top + DPI_SCALE(m_style.hilite_padding_y) +
                           m_layout->mark_height);
        else
          hlRc = CRect(rc.left + DPI_SCALE(m_style.hilite_padding_x),
                       rc.top + vgap,
                       rc.left + DPI_SCALE(m_style.hilite_padding_x) +
                           m_layout->mark_width,
                       rc.bottom - vgap);
        _TextOut(hlRc, m_style.mark_text.c_str(), m_style.mark_text.length(),
                 m_style.hilited_mark_color, pDWR->pTextFormat.Get());
      }
    }
  }
  return drawn;
}

// draw client area
void WeaselPanel::DoPaint(CDCHandle dc) {
  // DirectWrite 资源或布局不可用时放弃本帧绘制，绝不空指针解引用。
  // 绘制路径上没有异常出口（WM_PAINT 里抛异常会直接崩宿主），只能在这里降级。
  if (!pDWR || !m_layout)
    return;
  // 绘制路径上还有一类会抛异常的调用：HR()（include/WeaselUtility.h:315-321）
  // 只要 HRESULT 非 S_OK 就抛 ComException，_TextOut / _HighlightText / Layout 的
  // 度量路径都在用。它们抛出来会越过 WM_PAINT（RedrawWindow 直接调本函数），
  // 直接崩宿主 —— 这正是 #1906「d2d1.dll 崩溃」的同族路径。这里整帧收住降级：
  // 本帧不绘制（可能留下上一帧内容），下一帧重新尝试。
  weasel::perf::PerfLog& log = weasel::perf::PerfLog::Instance();
  const bool profiling = log.enabled();
  const ULONGLONG t0 = profiling ? weasel::perf::PerfLog::Now() : 0;
  const char* fail = "";
  bool reentered = false;
  if (profiling) {
    static thread_local bool in_paint = false;
    if (in_paint)
      reentered = true;
    in_paint = true;
  }
  try {
    _DoPaintImpl(dc);
  } catch (const std::exception&) {
    fail = "cpp";
    _ReleaseMemDC();
  } catch (...) {
    fail = "unknown";
    _ReleaseMemDC();
  }
  if (profiling) {
    static thread_local bool in_paint = false;
    in_paint = false;
    char line[192];
    sprintf_s(line, sizeof(line),
              "dopaint reenter=%d w=%d h=%d cands=%d hide=%d sw=%d exc=%s "
              "total=%.3f",
              reentered ? 1 : 0, rcw.Width(), rcw.Height(),
              (int)m_candidateCount, hide_candidates ? 1 : 0,
              (pDWR && pDWR->use_software_rt_) ? 1 : 0,
              fail[0] ? fail : "-",
              weasel::perf::PerfLog::Ms(t0, weasel::perf::PerfLog::Now()));
    log.Write(line);
  }
}

void WeaselPanel::_DoPaintImpl(CDCHandle dc) {
  // turn off WS_EX_TRANSPARENT, for better resp performance
  ModifyStyleEx(WS_EX_TRANSPARENT, WS_EX_LAYERED);
  GetClientRect(&rcw);
  // prepare memDC: 复用同一块离屏位图（尺寸变化时才重建），并把 BindDC 一起省掉
  if (m_memDC == NULL || m_memBitmap == NULL || m_memW != rcw.Width() ||
      m_memH != rcw.Height()) {
    _ReleaseMemDC();
    CDCHandle hdc = ::GetDC(m_hWnd);
    m_memDC = ::CreateCompatibleDC(hdc);
    m_memBitmap = ::CreateCompatibleBitmap(hdc, rcw.Width(), rcw.Height());
    m_memOldBitmap = ::SelectObject(m_memDC, m_memBitmap);
    ReleaseDC(hdc);
    m_memW = rcw.Width();
    m_memH = rcw.Height();
    m_memBound = false;
  }
  CDCHandle memDC = m_memDC;
  // 复用位图时必须先清成透明黑，否则上一帧的内容会与半透明背景叠加
  ::PatBlt(memDC, 0, 0, rcw.Width(), rcw.Height(), BLACKNESS);
  bool drawn = false;
  if (!hide_candidates) {
    CRect auxrc = m_layout->GetAuxiliaryRect();
    CRect preeditrc = m_layout->GetPreeditRect();
    if (m_istorepos) {
      // 用固定容量栈数组替代 new[]/delete[]：候选人数量本就被
      // MAX_CANDIDATES_COUNT 限制，避免每帧两次堆分配；原写法在两次 new
      // 之间抛出异常时还会泄漏（绘制路径上无异常出口，但成本为零的加固没必要省）。
      CRect rects[MAX_CANDIDATES_COUNT];
      int btmys[MAX_CANDIDATES_COUNT];
      for (auto i = 0; i < m_candidateCount && i < MAX_CANDIDATES_COUNT; ++i) {
        rects[i] = m_layout->GetCandidateRect(i);
        btmys[i] = rects[i].bottom;
      }
      if (m_candidateCount) {
        if (!m_layout->IsInlinePreedit() && !m_ctx.preedit.str.empty())
          m_offsety_preedit =
              rects[m_candidateCount - 1].bottom - preeditrc.bottom;
        if (!m_ctx.aux.str.empty())
          m_offsety_aux = rects[m_candidateCount - 1].bottom - auxrc.bottom;
      } else {
        m_offsety_preedit = 0;
        m_offsety_aux = 0;
      }
      int base_gap = 0;
      if (!m_ctx.aux.str.empty())
        base_gap = auxrc.Height() + m_style.spacing;
      else if (!m_layout->IsInlinePreedit() && !m_ctx.preedit.str.empty())
        base_gap = preeditrc.Height() + m_style.spacing;

      for (auto i = 0; i < m_candidateCount && i < MAX_CANDIDATES_COUNT; ++i) {
        if (i == 0)
          m_offsetys[i] =
              btmys[m_candidateCount - i - 1] - base_gap - rects[i].bottom;
        else
          m_offsetys[i] = (rects[i - 1].top + m_offsetys[i - 1] -
                           DPI_SCALE(m_style.candidate_spacing)) -
                          rects[i].bottom;
      }
    }
    // background and candidates back, hilite back drawing start
    if ((!m_ctx.empty() && !m_style.inline_preedit) ||
        (m_style.inline_preedit && (m_candidateCount || !m_ctx.aux.empty()))) {
      CRect backrc = m_layout->GetContentRect();
      _HighlightText(memDC, backrc, m_style.back_color, m_style.shadow_color,
                     DPI_SCALE(m_style.round_corner_ex), BackType::BACKGROUND,
                     IsToRoundStruct(), m_style.border_color);
    }
    if (!m_ctx.aux.str.empty()) {
      if (m_istorepos)
        auxrc.OffsetRect(0, m_offsety_aux);
      drawn |= _DrawPreeditBack(m_ctx.aux, memDC, auxrc);
    }
    if (!m_layout->IsInlinePreedit() && !m_ctx.preedit.str.empty()) {
      if (m_istorepos)
        preeditrc.OffsetRect(0, m_offsety_preedit);
      drawn |= _DrawPreeditBack(m_ctx.preedit, memDC, preeditrc);
    }
    if (m_candidateCount)
      drawn |= _DrawCandidates(memDC, true);
    // background and candidates back, hilite back drawing end

    // begin  texts drawing, if pRenderTarget failed, force to reinit
    // directwrite resources
    if (!m_memBound) {
      // BindDC 只在 DC 新建或 pDWR 重建后做一次。注意 BindDC 本身的实测成本只有
      // 约 0.07~0.08ms（与目标尺寸无关）；之前注释里的「约 0.70ms」是把 BindDC 与
      // 它强制触发的下一次表面重获权混算了。真正省下的是「每帧重绑」的 0.37~0.53ms。
      if (FAILED(pDWR->pRenderTarget->BindDC(memDC, &rcw))) {
        _InitFontRes(true);
        if (!pDWR)
          return;  // 重建失败：pDWR 已被置空，下面的解引用会崩宿主
        pDWR->pRenderTarget->BindDC(memDC, &rcw);
      }
      // 只有绑定成功才置位：原来无条件置 true，若第二次 BindDC 仍失败，
      // 之后每帧都会跳过重绑并让 EndDraw 失败 → 反复重建 DirectWrite 资源。
      m_memBound = (pDWR->pRenderTarget != NULL);
    }
    pDWR->pRenderTarget->BeginDraw();
    // draw auxiliary string
    if (!m_ctx.aux.str.empty())
      drawn |= _DrawPreedit(m_ctx.aux, memDC, auxrc);
    // draw preedit string
    if (!m_layout->IsInlinePreedit() && !m_ctx.preedit.str.empty())
      drawn |= _DrawPreedit(m_ctx.preedit, memDC, preeditrc);
    // draw candidates string
    if (m_candidateCount)
      drawn |= _DrawCandidates(memDC);
    if (FAILED(pDWR->pRenderTarget->EndDraw())) {
      _InitFontRes(true);
      Refresh();
    }
    // end texts drawing

    // status icon (I guess Metro IME stole my idea :)
    if (m_layout->ShouldDisplayStatusIcon()) {
      // decide if custom schema zhung icon to show
      LoadIconNecessary(m_current_zhung_icon, m_style.current_zhung_icon,
                        m_iconEnabled, IDI_ZH);
      LoadIconNecessary(m_current_ascii_icon, m_style.current_ascii_icon,
                        m_iconAlpha, IDI_EN);
      LoadIconNecessary(m_current_half_icon, m_style.current_half_icon,
                        m_iconHalf, IDI_HALF_SHAPE);
      LoadIconNecessary(m_current_full_icon, m_style.current_full_icon,
                        m_iconFull, IDI_FULL_SHAPE);
      CRect iconRect(m_layout->GetStatusIconRect());
      if (m_istorepos && !m_ctx.aux.str.empty())
        iconRect.OffsetRect(0, m_offsety_aux);
      else if (m_istorepos && !m_layout->IsInlinePreedit() &&
               !m_ctx.preedit.str.empty())
        iconRect.OffsetRect(0, m_offsety_preedit);

      CIcon& icon(
          m_status.disabled ? m_iconDisabled
          : m_status.ascii_mode
              ? m_iconAlpha
              : (m_status.type == SCHEMA
                     ? m_iconEnabled
                     : (m_status.full_shape ? m_iconFull : m_iconHalf)));
      memDC.DrawIconEx(iconRect.left, iconRect.top, icon, 0, 0);
      drawn = true;
    }
    /* Nothing drawn, hide candidate window */
    if (!drawn)
      ShowWindow(SW_HIDE);
  }
  _LayerUpdate(rcw, memDC);

  // 离屏 DC/位图是复用成员，不再在这里销毁（见 _ReleaseMemDC）
}

void WeaselPanel::_ReleaseMemDC() {
  if (m_memDC != NULL) {
    if (m_memOldBitmap != NULL)
      ::SelectObject(m_memDC, m_memOldBitmap);
    ::DeleteDC(m_memDC);
    m_memDC = NULL;
  }
  if (m_memBitmap != NULL) {
    ::DeleteObject(m_memBitmap);
    m_memBitmap = NULL;
  }
  m_memOldBitmap = NULL;
  m_memW = m_memH = 0;
  m_memBound = false;
}

// 由于某些软件并不依赖 WM_PAINT 消息来重绘，在消息循环中直接忽略掉了 WM_PAINT
// 消息， 导致 DoPaint() 永远不会被调用，这里手动调用 DoPaint() 强制重绘
void WeaselPanel::RedrawWindow() {
  HDC hdc = GetDC();
  DoPaint(hdc);
  ReleaseDC(hdc);
}

void WeaselPanel::_LayerUpdate(const CRect& rc, CDCHandle dc) {
  HDC ScreenDC = ::GetDC(NULL);
  CRect rect;
  GetWindowRect(&rect);
  POINT WindowPosAtScreen = {rect.left, rect.top};
  POINT PointOriginal = {0, 0};
  SIZE sz = {rc.Width(), rc.Height()};

  BLENDFUNCTION bf = {AC_SRC_OVER, 0, 0XFF, AC_SRC_ALPHA};
  UpdateLayeredWindow(m_hWnd, ScreenDC, &WindowPosAtScreen, &sz, dc,
                      &PointOriginal, RGB(0, 0, 0), &bf, ULW_ALPHA);
  // 必须写成 ::ReleaseDC(NULL, ...)：这个 DC 来自上面的 ::GetDC(NULL)（屏幕 DC），
  // 而 CWindow::ReleaseDC(HDC) 会转成 ::ReleaseDC(m_hWnd, ScreenDC)，两者不配对 →
  // 每帧泄漏一个屏幕 DC（约一万次重绘后耗尽 GDI 句柄）。_CaptureRect 里的写法是对的。
  ::ReleaseDC(NULL, ScreenDC);
}

LRESULT WeaselPanel::OnCreate(UINT uMsg,
                              WPARAM wParam,
                              LPARAM lParam,
                              BOOL& bHandled) {
  m_mouse_entry = false;
  m_hoverIndex = -1;
  {
    weasel::perf::PosLog& log = weasel::perf::PosLog::Instance();
    if (log.enabled())
      log.Writef("[ui] create ip=%ld,%ld,%ld,%ld sticky=%d srv=%d",
                 m_inputPos.left, m_inputPos.top, m_inputPos.right,
                 m_inputPos.bottom, m_sticky ? 1 : 0, m_in_server ? 1 : 0);
  }
  Refresh();
  return TRUE;
}

LRESULT WeaselPanel::OnDestroy(UINT uMsg,
                               WPARAM wParam,
                               LPARAM lParam,
                               BOOL& bHandled) {
  {
    weasel::perf::PosLog& log = weasel::perf::PosLog::Instance();
    if (log.enabled())
      log.Writef("[ui] destroy ip=%ld,%ld,%ld,%ld sticky=%d",
                 m_inputPos.left, m_inputPos.top, m_inputPos.right,
                 m_inputPos.bottom, m_sticky ? 1 : 0);
  }
  m_hoverIndex = -1;
  m_lastMousePos = {-1, -1};
  m_sticky = false;
  _ReleaseMemDC();  // 窗口销毁后复用位图不再可用
  delete m_layout;
  m_layout = NULL;
  return 0;
}

LRESULT WeaselPanel::OnDpiChanged(UINT uMsg,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  BOOL& bHandled) {
  Refresh();
  return LRESULT();
}

void WeaselPanel::MoveTo(RECT const& rc) {
  weasel::perf::PosLog& log = weasel::perf::PosLog::Instance();
  const bool logging = log.enabled();
  if (!m_layout) {
    if (logging)
      log.Writef("[ui] move rc=%ld,%ld,%ld,%ld ip=%ld,%ld,%ld,%ld sticky=%d "
                 "cand=%d pre=%u act=noLayout",
                 rc.left, rc.top, rc.right, rc.bottom, m_inputPos.left,
                 m_inputPos.top, m_inputPos.right, m_inputPos.bottom,
                 m_sticky ? 1 : 0, (int)m_candidateCount,
                 (unsigned)m_ctx.preedit.str.size());
    return;  // avoid handling nullptr in _RepositionWindow
  }
  const LONG ip_left = m_inputPos.left;
  const LONG ip_bottom = m_inputPos.bottom;
  const bool ip_empty = (m_inputPos.left == 0 && m_inputPos.top == 0 &&
                         m_inputPos.right == 0 && m_inputPos.bottom == 0);
  m_redraw_by_monitor_change = false;
  // The conditions for resetting the sticky state:
  // 1. When the input session ends (ctx.empty() is true)
  // 2. When the input position changes significantly (the position change
  // exceeds the threshold)
  // 3. When the content of the candidate window is empty
  bool should_reset_sticky =
      (m_ctx.empty() || (abs(rc.left - m_inputPos.left) > 50) ||
       (abs(rc.bottom - m_inputPos.bottom) > 50));
  if (should_reset_sticky && m_sticky) {
    if (logging)
      log.Writef("[ui] move rc=%ld,%ld,%ld,%ld ip=%ld,%ld,%ld,%ld sticky=1 "
                 "reset=1 ipEmpty=%d cand=%d pre=%u act=resetSticky",
                 rc.left, rc.top, rc.right, rc.bottom, ip_left, m_inputPos.top,
                 m_inputPos.right, ip_bottom, ip_empty ? 1 : 0,
                 (int)m_candidateCount, (unsigned)m_ctx.preedit.str.size());
    m_sticky = false;
    // Force reposition the window
    m_inputPos = rc;
    m_inputPos.OffsetRect(0, 6);
    _RepositionWindow(true);
    RedrawWindow();
    return;
  }
  // if ascii_tip_follow_cursor set, move tip icon to mouse cursor
  if (m_style.ascii_tip_follow_cursor && m_ctx.empty() &&
      (!m_status.composing) && m_layout->ShouldDisplayStatusIcon()) {
    if (logging)
      log.Writef("[ui] move rc=%ld,%ld,%ld,%ld ip=%ld,%ld,%ld,%ld sticky=%d "
                 "reset=%d act=tip",
                 rc.left, rc.top, rc.right, rc.bottom, ip_left, m_inputPos.top,
                 m_inputPos.right, ip_bottom, m_sticky ? 1 : 0,
                 should_reset_sticky ? 1 : 0);
    // ascii icon follow cursor
    POINT p;
    ::GetCursorPos(&p);
    RECT irc{p.x - STATUS_ICON_SIZE, p.y - STATUS_ICON_SIZE, p.x, p.y};
    m_inputPos = irc;
    _RepositionWindow(true);
    RedrawWindow();
  } else if (!(rc.left == m_inputPos.left && rc.bottom != m_inputPos.bottom &&
               abs(rc.bottom - m_inputPos.bottom) < 6) ||
             m_layout->ShouldDisplayStatusIcon()) {
    if (logging)
      log.Writef("[ui] move rc=%ld,%ld,%ld,%ld ip=%ld,%ld,%ld,%ld sticky=%d "
                 "reset=%d ipEmpty=%d cand=%d pre=%u act=move",
                 rc.left, rc.top, rc.right, rc.bottom, ip_left, m_inputPos.top,
                 m_inputPos.right, ip_bottom, m_sticky ? 1 : 0,
                 should_reset_sticky ? 1 : 0, ip_empty ? 1 : 0,
                 (int)m_candidateCount, (unsigned)m_ctx.preedit.str.size());
    // in some apps like word 2021, with inline_preedit set,
    // bottom of rc would flicker 1 px or 2, make the candidate flickering
    m_inputPos = rc;
    m_inputPos.OffsetRect(0, 6);
    // buffer current m_istorepos status
    bool m_istorepos_buf = m_istorepos;
    // with parameter to avoid vertical flicker
    _RepositionWindow(true);
    // m_istorepos status changed by _RepositionWindow, or tips to show,
    // redrawing is required
    if (m_istorepos != m_istorepos_buf || !m_ctx.aux.empty() ||
        m_layout->ShouldDisplayStatusIcon() || m_redraw_by_monitor_change)
      RedrawWindow();
  } else if (logging) {
    log.Writef("[ui] move rc=%ld,%ld,%ld,%ld ip=%ld,%ld,%ld,%ld sticky=%d "
               "reset=%d cand=%d pre=%u act=noop",
               rc.left, rc.top, rc.right, rc.bottom, ip_left, m_inputPos.top,
               m_inputPos.right, ip_bottom, m_sticky ? 1 : 0,
               should_reset_sticky ? 1 : 0, (int)m_candidateCount,
               (unsigned)m_ctx.preedit.str.size());
  }
}

void WeaselPanel::_RepositionWindow(const bool& adj) {
  weasel::perf::PosLog& log = weasel::perf::PosLog::Instance();
  const bool logging = log.enabled();
  const LONG raw_bottom = m_inputPos.bottom;
  RECT rcWorkArea;
  memset(&rcWorkArea, 0, sizeof(rcWorkArea));
  HMONITOR hMonitor = MonitorFromRect(m_inputPos, MONITOR_DEFAULTTONEAREST);
  if (hMonitor) {
    MONITORINFO info;
    info.cbSize = sizeof(MONITORINFO);
    if (GetMonitorInfo(hMonitor, &info)) {
      rcWorkArea = info.rcWork;
    }
    if (hMonitor != m_hMonitor) {
      m_hMonitor = hMonitor;
      m_redraw_by_monitor_change = true;
    }
  }
  RECT rcWindow;
  GetWindowRect(&rcWindow);
  int width = (rcWindow.right - rcWindow.left);
  int height = (rcWindow.bottom - rcWindow.top);
  // keep panel visible
  rcWorkArea.right -= width;
  rcWorkArea.bottom -= height;
  int x = m_inputPos.left;
  int y = m_inputPos.bottom;
  bool flipped = false;
  if (DPI_SCALE(m_style.shadow_radius)) {
    x -= (DPI_SCALE(m_style.shadow_offset_x) >= 0 ||
          COLORTRANSPARENT(m_style.shadow_color))
             ? m_layout->offsetX
             : (m_layout->offsetX / 2);
    if (adj)
      y -= (DPI_SCALE(m_style.shadow_offset_y) > 0 ||
            COLORTRANSPARENT(m_style.shadow_color))
               ? m_layout->offsetY
               : (m_layout->offsetY / 2);
  }
  // for vertical text layout, flow right to left, make window left side
  if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT &&
      !m_style.vertical_text_left_to_right) {
    x += m_layout->offsetX - width;
    if (DPI_SCALE(m_style.shadow_offset_x) < 0)
      x += m_layout->offsetX;
  }
  if (adj)
    m_istorepos = false;
  if (x > rcWorkArea.right)
    x = rcWorkArea.right;  // over workarea right
  if (x < rcWorkArea.left)
    x = rcWorkArea.left;  // over workarea left
  // show panel above the input focus if we're around the bottom
  if (y > rcWorkArea.bottom || m_sticky) {
    flipped = true;
    if (!m_sticky)
      m_sticky = true;
    y = m_inputPos.top - height - 6;  // over workarea bottom
    if (DPI_SCALE(m_style.shadow_radius) &&
        DPI_SCALE(m_style.shadow_offset_y) > 0)
      y -= DPI_SCALE(m_style.shadow_offset_y);
    m_istorepos = (m_style.vertical_auto_reverse &&
                   m_style.layout_type == UIStyle::LAYOUT_VERTICAL);
    if (DPI_SCALE(m_style.shadow_radius) > 0)
      y += (DPI_SCALE(m_style.shadow_offset_y) < 0 ||
            COLORTRANSPARENT(m_style.shadow_color))
               ? m_layout->offsetY
               : (m_layout->offsetY / 2);
  }
  if (y < rcWorkArea.top)
    y = rcWorkArea.top;  // over workarea top
  // memorize adjusted position (to avoid window bouncing on height change)
  m_inputPos.bottom = y;
  SetWindowPos(HWND_TOPMOST, x, y, 0, 0,
               SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
  if (logging)
    log.Writef("[ui] repo adj=%d ip=%ld,%ld,%ld,%ld rawBottom=%ld win=%dx%d "
               "work=%ld,%ld,%ld,%ld -> x=%d y=%d flip=%d sticky=%d "
               "istorepos=%d srv=%d",
               adj ? 1 : 0, m_inputPos.left, m_inputPos.top, m_inputPos.right,
               m_inputPos.bottom, raw_bottom, width, height, rcWorkArea.left,
               rcWorkArea.top, rcWorkArea.right, rcWorkArea.bottom, x, y,
               flipped ? 1 : 0, m_sticky ? 1 : 0, m_istorepos ? 1 : 0,
               m_in_server ? 1 : 0);
}

void WeaselPanel::_TextOut(const CRect& rc,
                           const std::wstring& psz,
                           const size_t& cch,
                           const int& inColor,
                           IDWriteTextFormat1* const pTextFormat) {
  // pDWR 可能在 _InitFontRes() 里因 DirectWrite/D2D 初始化失败被置空（见
  // _InitFontRes 的 catch 分支）。_DrawPreeditBack 只在自己的入口判空，随后仍会
  // 调到这里；_DrawCandidates 判空后也可能因样式变化让 pDWR 在调用途中失效。
  // 缺这一句就是空指针解引用 → 宿主进程崩溃（#1906 同族）。
  if (pDWR == NULL || pTextFormat == NULL)
    return;
  float r = (float)(GetRValue(inColor)) / 255.0f;
  float g = (float)(GetGValue(inColor)) / 255.0f;
  float b = (float)(GetBValue(inColor)) / 255.0f;
  float alpha = (float)((inColor >> 24) & 255) / 255.0f;
  HRESULT hr = S_OK;
  if (pDWR->pBrush == NULL) {
    HR(pDWR->CreateBrush(D2D1::ColorF(r, g, b, alpha)));
  } else
    pDWR->SetBrushColor(D2D1::ColorF(r, g, b, alpha));

  HR(pDWR->CreateTextLayout(psz.c_str(), (int)cch, pTextFormat,
                            (float)rc.Width(), (float)rc.Height()));
  if (pDWR->pTextLayout == NULL)
    return;  // 布局创建失败：不取度量也不绘制，否则下面的
             // GetLayoutOverhangMetrics 会解引用空指针（pTextLayout 为空）
  if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT) {
    DWRITE_FLOW_DIRECTION flow = m_style.vertical_text_left_to_right
                                     ? DWRITE_FLOW_DIRECTION_LEFT_TO_RIGHT
                                     : DWRITE_FLOW_DIRECTION_RIGHT_TO_LEFT;
    HR(pDWR->SetLayoutReadingDirection(DWRITE_READING_DIRECTION_TOP_TO_BOTTOM));
    HR(pDWR->SetLayoutFlowDirection(flow));
  }

  // offsetx for font glyph over left
  float offsetx = (float)rc.left;
  float offsety = (float)rc.top;
  // prepare for space when first character overhanged
  DWRITE_OVERHANG_METRICS omt;
  HR(pDWR->GetLayoutOverhangMetrics(&omt));
  if (m_style.layout_type != UIStyle::LAYOUT_VERTICAL_TEXT && omt.left > 0)
    offsetx += omt.left;
  if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT && omt.top > 0)
    offsety += omt.top;

  if (pDWR->pTextLayout != NULL) {
    pDWR->DrawTextLayoutAt({offsetx, offsety});
#if 0
    D2D1_RECT_F rectf =  D2D1::RectF(offsetx, offsety, offsetx + rc.Width(), offsety + rc.Height());
    pDWR->DrawRect(&rectf);
#endif
  }
}
