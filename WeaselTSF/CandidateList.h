#pragma once
#include <WeaselUI.h>
#include "ctffunc.h"

class WeaselTSF;

class CCandidateList : public ITfIntegratableCandidateListUIElement,
                       public ITfCandidateListUIElementBehavior {
 public:
  CCandidateList(WeaselTSF* pTextService);
  ~CCandidateList();

  // _tsf 的生命周期由 WeaselTSF 通过 Attach/Detach 显式管理（见成员声明处）。
  void Attach(WeaselTSF* pTextService) { _tsf = pTextService; }
  void Detach() { _tsf = nullptr; }

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, _Outptr_ void** ppvObj);
  STDMETHODIMP_(ULONG) AddRef(void);
  STDMETHODIMP_(ULONG) Release(void);

  // ITfUIElement
  STDMETHODIMP GetDescription(BSTR* pbstr);
  STDMETHODIMP GetGUID(GUID* pguid);
  STDMETHODIMP Show(BOOL showCandidateWindow);
  STDMETHODIMP IsShown(BOOL* pIsShow);

  // ITfCandidateListUIElement
  STDMETHODIMP GetUpdatedFlags(DWORD* pdwFlags);
  STDMETHODIMP GetDocumentMgr(ITfDocumentMgr** ppdim);
  STDMETHODIMP GetCount(UINT* pCandidateCount);
  STDMETHODIMP GetSelection(UINT* pSelectedCandidateIndex);
  STDMETHODIMP GetString(UINT uIndex, BSTR* pbstr);
  STDMETHODIMP GetPageIndex(UINT* pIndex, UINT uSize, UINT* puPageCnt);
  STDMETHODIMP SetPageIndex(UINT* pIndex, UINT uPageCnt);
  STDMETHODIMP GetCurrentPage(UINT* puPage);

  // ITfCandidateListUIElementBehavior methods
  STDMETHODIMP SetSelection(UINT nIndex);
  STDMETHODIMP Finalize(void);
  STDMETHODIMP Abort(void);

  // ITfIntegratableCandidateListUIElement methods
  STDMETHODIMP SetIntegrationStyle(GUID guidIntegrationStyle);
  STDMETHODIMP GetSelectionStyle(
      _Out_ TfIntegratableCandidateListSelectionStyle* ptfSelectionStyle);
  STDMETHODIMP OnKeyDown(_In_ WPARAM wParam,
                         _In_ LPARAM lParam,
                         _Out_ BOOL* pIsEaten);
  STDMETHODIMP ShowCandidateNumbers(_Out_ BOOL* pIsShow);
  STDMETHODIMP FinalizeExactCompositionString();

  /* Update */
  void UpdateUI(const weasel::Context& ctx, const weasel::Status& status);
  void UpdateStyle(const weasel::UIStyle& sty);
  void UpdateInputPosition(RECT const& rc);
  void Destroy();
  void DestroyAll();
  void StartUI();
  void EndUI();

  com_ptr<ITfContext> GetContextDocument();
  bool GetIsReposition() {
    if (_ui)
      return _ui->GetIsReposition();
    else
      return false;
  }

  weasel::UIStyle& style();

 private:
  // void _UpdateOwner();
  HWND _GetActiveWnd();
  HRESULT _UpdateUIElement();

  // for CCandidateList::EndUI(), after ending composition ||
  // WeaselTSF::_EndUI()
  void _DisposeUIWindow();
  // for CCandidateList::Destroy(), when inputing app exit
  void _DisposeUIWindowAll();
  void _MakeUIWindow();

  std::unique_ptr<weasel::UI> _ui;
  DWORD _cRef;
  // 必须是不增加引用计数的裸指针。WeaselTSF 用 com_ptr<CCandidateList> 持有本
  // 对象；若这里再持一个 com_ptr<WeaselTSF>，两者引用计数都降不到 0，
  // WeaselTSF 的析构函数永不执行 —— 连带 ClientImpl 的析构（唯一的自动
  // Disconnect 路径）也不执行，于是宿主进程每次激活输入法就泄漏一整棵对象图，
  // 并在服务端留下一条永不关闭的管道连接。
  // 因此约定：Activate 期间有效（Attach），Deactivate 与 ~WeaselTSF 里清空
  // （Detach）；清空后本对象的方法一律走空指针降级分支（不再有 TSF 可调用，
  // 那属于 msctf 在失活后继续回调，本身就是不该发生的事）。
  WeaselTSF* _tsf;
  DWORD uiid;
  TfIntegratableCandidateListSelectionStyle _selectionStyle =
      STYLE_ACTIVE_SELECTION;

  BOOL _pbShow;
  bool _uiStarted = false;
  weasel::UIStyle _style;

  com_ptr<ITfContext> _pContextDocument;
  com_ptr<ITfUIElementMgr> _pUIElementMgr;  // 缓存，避免每次按键 QueryInterface
};
