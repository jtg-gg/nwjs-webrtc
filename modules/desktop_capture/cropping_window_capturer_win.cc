/*
 *  Copyright (c) 2014 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/desktop_capture/cropping_window_capturer.h"

#include <dwmapi.h>
#include "modules/desktop_capture/win/screen_capture_utils.h"
#include "modules/desktop_capture/win/screen_capturer_win_magnifier.h"
#include "modules/desktop_capture/win/window_capture_utils.h"
#include "rtc_base/event.h"
#include "rtc_base/logging.h"
#include "rtc_base/task_queue.h"
#include "rtc_base/thread.h"
#include "rtc_base/timeutils.h"
#include "rtc_base/trace_event.h"
#include "rtc_base/win32.h"

namespace webrtc {

namespace {

const size_t kTitleLength = 256;

// Used to pass input/output data during the EnumWindow call for verifying if
// the selected window is on top.
struct TopWindowVerifierContext {
  TopWindowVerifierContext(
      HWND selected_window,
      HWND excluded_window,
      DesktopRect selected_window_rect,
      WindowCaptureHelperWin* window_capture_helper,
      const bool allow_magnification_api_for_window_capture)
      : selected_window(selected_window),
        excluded_window(excluded_window),
        selected_window_rect(selected_window_rect),
        window_capture_helper(window_capture_helper),
        allow_magnification_api_for_window_capture(
            allow_magnification_api_for_window_capture),
        is_top_window(false) {
    RTC_DCHECK_NE(selected_window, excluded_window);

    GetWindowText(selected_window, selected_window_title, kTitleLength);
    GetWindowThreadProcessId(selected_window, &selected_window_process_id);
  }

  const HWND selected_window;
  const HWND excluded_window;
  const DesktopRect selected_window_rect;
  WindowCaptureHelperWin* window_capture_helper;
  WCHAR selected_window_title[kTitleLength];
  DWORD selected_window_process_id;
  const bool allow_magnification_api_for_window_capture;
  bool is_top_window;
};

// The function is called during EnumWindow for every window enumerated and is
// responsible for verifying if the selected window is on top.
// Return TRUE to continue enumerating if the current window belongs to the
// selected window or is to be ignored.
// Return FALSE to stop enumerating if the selected window is found or decided
// if it's on top most.
BOOL CALLBACK TopWindowVerifier(HWND hwnd, LPARAM param) {
  TopWindowVerifierContext* context =
      reinterpret_cast<TopWindowVerifierContext*>(param);

  if (hwnd == context->selected_window) {
    context->is_top_window = true;
    return FALSE;
  }

  // Ignore the excluded window.
  if (hwnd == context->excluded_window) {
    return TRUE;
  }

  // Ignore invisible window on current desktop.
  if (!context->window_capture_helper->IsWindowVisibleOnCurrentDesktop(hwnd)) {
    return TRUE;
  }

  // Ignore Chrome notification windows, especially the notification for the
  // ongoing window sharing.
  // Notes:
  // - This only works with notifications from Chrome, not other Apps.
  // - All notifications from Chrome will be ignored.
  // - This may cause part or whole of notification window being cropped into
  // the capturing of the target window if there is overlapping.
  if (context->window_capture_helper->IsWindowChromeNotification(hwnd)) {
    return TRUE;
  }

  // Ignore descendant windows since we want to capture them.
  // This check does not work for tooltips and context menus. Drop down menus
  // and popup windows are fine.
  //
  // GA_ROOT returns the root window instead of the owner. I.e. for a dialog
  // window, GA_ROOT returns the dialog window itself. GA_ROOTOWNER returns the
  // application main window which opens the dialog window. Since we are sharing
  // the application main window, GA_ROOT should be used here.
  if (GetAncestor(hwnd, GA_ROOT) == context->selected_window) {
    return TRUE;
  }

  // If |hwnd| has no title or has same title as the selected window (i.e.
  // Window Media Player consisting of several sibling windows) and belongs to
  // the same process, assume it's a tooltip or context menu or sibling window
  // from the selected window and ignore it.
  // TODO(zijiehe): This check cannot cover the case where tooltip or context
  // menu of the child-window is covering the main window. See
  // https://bugs.chromium.org/p/webrtc/issues/detail?id=8062 for details.
  WCHAR window_title[kTitleLength];
  GetWindowText(hwnd, window_title, kTitleLength);
  if (wcsnlen_s(window_title, kTitleLength) == 0 ||
      wcscmp(window_title, context->selected_window_title) == 0) {
    DWORD enumerated_window_process_id;
    GetWindowThreadProcessId(hwnd, &enumerated_window_process_id);
    if (context->selected_window_process_id == enumerated_window_process_id) {
      return TRUE;
    }
  }

  if (context->allow_magnification_api_for_window_capture) {
    const size_t kClassLength = 256;
    WCHAR class_name[kClassLength];
    const int class_name_length = GetClassName(hwnd, class_name, kClassLength);
    RTC_DCHECK(class_name_length) << "Error retrieving the window's class name";
    if (wcscmp(class_name, L"Windows.UI.Core.CoreWindow") == 0) {
      return TRUE;
    }
  }

  // Checks whether current window |hwnd| intersects with
  // |context|->selected_window.
  // |content_rect| is preferred because,
  // 1. WindowCapturerWin is using GDI capturer, which cannot capture DX output.
  //    So ScreenCapturer should be used as much as possible to avoid
  //    uncapturable cases. Note: lots of new applications are using DX output
  //    (hardware acceleration) to improve the performance which cannot be
  //    captured by WindowCapturerWin. See bug http://crbug.com/741770.
  // 2. WindowCapturerWin is still useful because we do not want to expose the
  //    content on other windows if the target window is covered by them.
  // 3. Shadow and borders should not be considered as "content" on other
  //    windows because they do not expose any useful information.
  //
  // So we can bear the false-negative cases (target window is covered by the
  // borders or shadow of other windows, but we have not detected it) in favor
  // of using ScreenCapturer, rather than let the false-positive cases (target
  // windows is only covered by borders or shadow of other windows, but we treat
  // it as overlapping) impact the user experience.
  DesktopRect content_rect;
  if (!GetWindowContentRect(hwnd, &content_rect)) {
    // Bail out if failed to get the window area.
    context->is_top_window = false;
    return FALSE;
  }

  content_rect.IntersectWith(context->selected_window_rect);

  // If intersection is not empty, the selected window is not on top.
  if (!content_rect.is_empty()) {
    context->is_top_window = false;
    return FALSE;
  }
  // Otherwise, keep enumerating.
  return TRUE;
}

class ScreenCapturerWinMagnifierWorker : DesktopCapturer::Callback {
 public:
  static rtc::RefCountedObject<ScreenCapturerWinMagnifierWorker>* Get();
  bool CaptureFrame(DesktopCapturer::Callback* callback,
                    const std::vector<WindowId>& windows);
  DesktopCapturer::SourceId source_id() { return source_id_; }

 protected:
  ~ScreenCapturerWinMagnifierWorker();
  ScreenCapturerWinMagnifierWorker(ScreenId main_screen);
  void OnCaptureResult(DesktopCapturer::Result result,
                       std::unique_ptr<DesktopFrame> screen_frame);

 private:
  const DesktopCapturer::SourceId source_id_;
  std::unique_ptr<ScreenCapturerWinMagnifier> screen_magnifier_capturer_;
  DesktopCapturer::Result result_;
  std::unique_ptr<DesktopFrame> screen_frame_;
  rtc::CriticalSection capture_lock_;
  std::unique_ptr<rtc::TaskQueue> task_queue_;

  static rtc::CriticalSection* singleton_lock_;
  static rtc::RefCountedObject<ScreenCapturerWinMagnifierWorker>* singleton_;
};

rtc::RefCountedObject<ScreenCapturerWinMagnifierWorker>*
    ScreenCapturerWinMagnifierWorker::singleton_ = nullptr;

rtc::CriticalSection* ScreenCapturerWinMagnifierWorker::singleton_lock_ =
    nullptr;

rtc::RefCountedObject<ScreenCapturerWinMagnifierWorker>*
ScreenCapturerWinMagnifierWorker::Get() {
  if (!singleton_lock_) {
    rtc::CriticalSection* singleton_lock = new rtc::CriticalSection();
    if (InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID*>(&singleton_lock_), singleton_lock, NULL)) {
      delete singleton_lock;
    }
  }
  rtc::CritScope lock(singleton_lock_);
  if (singleton_ == nullptr) {
    ScreenId main_screen = kInvalidScreenId;
    // DesktopRect desktop_rect_screen_0;
    DesktopRect desktop_rect;
    std::wstring device_key;

    DesktopCapturer::SourceList screens;
    GetScreenList(&screens);
    for (unsigned int i = 0; i < screens.size(); i++) {
      const ScreenId screen_id = screens[i].id;
      if (IsScreenValid(screen_id, &device_key)) {
        desktop_rect = GetScreenRect(screen_id, device_key);
        // if (screen_id == 0) {
        //  desktop_rect_screen_0 = desktop_rect;
        //}
        if (desktop_rect.top_left().is_zero()) {
          main_screen = screen_id;
          break;
        }
      }
    }

    // Magic configuration for magnification api window capturer
    // basically there are some fail cases if the screen is configured
    // vertically --> NOT USED ANYMORE
    /*bool allow_magnification_api_for_window_capture = true;
    switch (main_screen) {
      case 0: {
        if (screens.size() == 2) {
          if (IsScreenValid(1, &device_key)) {
            desktop_rect = GetScreenRect(1, device_key);
            allow_magnification_api_for_window_capture =
                desktop_rect_screen_0.bottom() < desktop_rect.top() ||
                desktop_rect_screen_0.top() >= desktop_rect.top() ||
                desktop_rect_screen_0.bottom() >= desktop_rect.bottom() ||
                desktop_rect_screen_0.top() >= desktop_rect.bottom();
          }
        } else if (screens.size() >= 3) {
          allow_magnification_api_for_window_capture = false;
        }
      } break;
      case 1:
        allow_magnification_api_for_window_capture =
            desktop_rect_screen_0.top() < desktop_rect.bottom() ||
            desktop_rect_screen_0.bottom() <= desktop_rect.bottom() ||
            desktop_rect_screen_0.top() <= desktop_rect.top() ||
            desktop_rect_screen_0.bottom() <= desktop_rect.top();
        break;
      default:
        allow_magnification_api_for_window_capture = false;
    }*/

    if (main_screen != kInvalidScreenId) {
      singleton_ = new rtc::RefCountedObject<ScreenCapturerWinMagnifierWorker>(
          main_screen);
    }
  }
  return singleton_;
}

ScreenCapturerWinMagnifierWorker::ScreenCapturerWinMagnifierWorker(
    ScreenId main_screen)
    : source_id_(main_screen) {
  task_queue_ =
      std::make_unique<rtc::TaskQueue>("ScreenCapturerWinMagnifierWorker");
  screen_magnifier_capturer_ = std::make_unique<ScreenCapturerWinMagnifier>();
  // Magnifier Capturer only works on main monitor
  screen_magnifier_capturer_->SelectSource(source_id_);
  rtc::Event event;
  task_queue_->PostTask([this, &event]() {
    screen_magnifier_capturer_->Start(this);
    event.Set();
  });
  event.Wait(rtc::Event::kForever);
}

ScreenCapturerWinMagnifierWorker::~ScreenCapturerWinMagnifierWorker() {
  rtc::CritScope lock(singleton_lock_);
  singleton_ = nullptr;
}

bool ScreenCapturerWinMagnifierWorker::CaptureFrame(
    DesktopCapturer::Callback* callback,
    const std::vector<WindowId>& windows) {
  capture_lock_.Enter();
  rtc::Event event;
  bool result = false;
  task_queue_->PostTask([this, &event, &result, &windows]() {
    if (screen_magnifier_capturer_->SetExcludedWindows(windows)) {
      screen_magnifier_capturer_->CaptureFrame();
      result = true;
    } else {
      bool res = screen_magnifier_capturer_->SetExcludedWindows(
          std::vector<WindowId>());
      RTC_DCHECK(res);
    }
    event.Set();
  });
  event.Wait(rtc::Event::kForever);
  std::unique_ptr<DesktopFrame> screen_frame = std::move(screen_frame_);
  DesktopCapturer::Result capture_result = result_;
  capture_lock_.Leave();

  if (result == false) {
    return false;
  }

  callback->OnCaptureResult(capture_result, std::move(screen_frame));
  return true;
}

void ScreenCapturerWinMagnifierWorker::OnCaptureResult(
    DesktopCapturer::Result result,
    std::unique_ptr<DesktopFrame> screen_frame) {
  DCHECK(!screen_frame_);
  result_ = result;
  screen_frame_ = std::move(screen_frame);
}

class WindowsTopOfMeWorker : rtc::Runnable {
 public:
  WindowsTopOfMeWorker(WindowCaptureHelperWin* window_capture_helper);
  void SelectWindow(HWND window, const bool is_using_magnifier);
  bool IsChanged(uint32_t in_last_ms);
  
  // used by ShouldUseScreenCapture
  std::vector<HWND> core_windows() {
    return core_windows_;
  }
  
  // used by Magnifier Capturer
  std::vector<WindowId> exclusion_window_list() {
    return reinterpret_cast<std::vector<WindowId>&>(exclusion_window_list_);
  }
  
  static const uint32_t kLastMsThreshold;

 private:
  void Run(rtc::Thread* thread) override;
  int ignore_counter_;
  std::unique_ptr<rtc::Thread> thread_;
  WindowCaptureHelperWin* window_capture_helper_;
  HWND selected_window_;
  std::vector<HWND> core_windows_;
  std::vector<HWND> exclusion_window_list_;
  uint32_t last_changed_;
  // kFps is how fast is this worker should run
  static const int kFps;
  // kIgnoreCounter is currently 2, the number of IsChanged function called
  // during CaptureFrame until OnCaptureResult
  static const int kIgnoreCounter;
};

class CroppingWindowCapturerWin : public CroppingWindowCapturer {
 public:
  CroppingWindowCapturerWin(const DesktopCaptureOptions& options)
      : CroppingWindowCapturer(options),
        capturer_(Unknown),
        cant_get_screen_magnifier_capturer_worker_(false),
        selected_window_should_use_magnifier_(false),
        should_use_screen_capturer_cache_(Empty) {}

 private:
  bool ShouldUseScreenCapturer() override;
  bool ShouldUseMagnifier();
  void CaptureFrame() override;
  bool SelectSource(SourceId id) override;
  DesktopRect GetWindowRectInVirtualScreen() override;
  DesktopRect GetWindowRectInVirtualScreen(const bool magnifier);
  void OnCaptureResult(DesktopCapturer::Result result,
                       std::unique_ptr<DesktopFrame> frame) override;
  bool CaptureFrameUsingMagnifierApi();

  // The region from GetWindowRgn in the desktop coordinate if the region is
  // rectangular, or the rect from GetWindowRect if the region is not set.
  DesktopRect window_region_rect_;
  DesktopVector offset_;
  enum Capturer {
    Unknown,
    Screen,
    Magnifier,
    Window,
  };
  Capturer capturer_;
  bool cant_get_screen_magnifier_capturer_worker_;
  bool selected_window_should_use_magnifier_;
  WindowCaptureHelperWin window_capture_helper_;
  std::unique_ptr<WindowsTopOfMeWorker> windows_top_of_me_worker_;
  enum BoolCache {
    Empty = -1,
    False,
    True,
  };
  BoolCache should_use_screen_capturer_cache_;
  rtc::scoped_refptr<rtc::RefCountedObject<ScreenCapturerWinMagnifierWorker>>
      screen_magnifier_capturer_worker_;
};

bool CroppingWindowCapturerWin::ShouldUseScreenCapturer() {
  if (should_use_screen_capturer_cache_ != Empty) {
    return should_use_screen_capturer_cache_;
  }

  // RTC_DLOG(INFO) << "ShouldUseScreenCapturer()";

  if (!rtc::IsWindows8OrLater() && window_capture_helper_.IsAeroEnabled()) {
    return false;
  }

  const HWND selected = reinterpret_cast<HWND>(selected_window());
  // Check if the window is visible on current desktop.
  if (!window_capture_helper_.IsWindowVisibleOnCurrentDesktop(selected)) {
    return false;
  }

  // Check if the window is a translucent layered window.
  const LONG window_ex_style = GetWindowLong(selected, GWL_EXSTYLE);
  if (window_ex_style & WS_EX_LAYERED) {
    COLORREF color_ref_key = 0;
    BYTE alpha = 0;
    DWORD flags = 0;

    // GetLayeredWindowAttributes fails if the window was setup with
    // UpdateLayeredWindow. We have no way to know the opacity of the window in
    // that case. This happens for Stiky Note (crbug/412726).
    if (!GetLayeredWindowAttributes(selected, &color_ref_key, &alpha, &flags))
      return false;

    // UpdateLayeredWindow is the only way to set per-pixel alpha and will cause
    // the previous GetLayeredWindowAttributes to fail. So we only need to check
    // the window wide color key or alpha.
    if ((flags & LWA_COLORKEY) || ((flags & LWA_ALPHA) && (alpha < 255))) {
      return false;
    }
  }

  if (window_region_rect_.equals(DesktopRect())) {
    return false;
  }

  DesktopRect content_rect;
  if (!GetWindowContentRect(selected, &content_rect)) {
    return false;
  }

  DesktopRect region_rect;
  // Get the window region and check if it is rectangular.
  const int region_type =
      GetWindowRegionTypeWithBoundary(selected, &region_rect);

  // Do not use the screen capturer if the region is empty or not rectangular.
  if (region_type == COMPLEXREGION || region_type == NULLREGION) {
    return false;
  }

  if (region_type == SIMPLEREGION) {
    // The |region_rect| returned from GetRgnBox() is always in window
    // coordinate.
    region_rect.Translate(window_region_rect_.left(),
                          window_region_rect_.top());
    // MSDN: The window region determines the area *within* the window where the
    // system permits drawing.
    // https://msdn.microsoft.com/en-us/library/windows/desktop/dd144950(v=vs.85).aspx.
    //
    // |region_rect| should always be inside of |window_region_rect_|. So after
    // the intersection, |window_region_rect_| == |region_rect|. If so, what's
    // the point of the intersecting operations? Why cannot we directly retrieve
    // |window_region_rect_| from GetWindowRegionTypeWithBoundary() function?
    // TODO(zijiehe): Figure out the purpose of these intersections.
    window_region_rect_.IntersectWith(region_rect);
    content_rect.IntersectWith(region_rect);
  }

  // Check if the client area is out of the screen area. When the window is
  // maximized, only its client area is visible in the screen, the border will
  // be hidden. So we are using |content_rect| here.
  if (!GetFullscreenRect().ContainsRect(content_rect)) {
    return false;
  }

  // Check if the window is occluded by any other window, excluding the child
  // windows, context menus, and |excluded_window_|.
  // |content_rect| is preferred, see the comments in TopWindowVerifier()
  // function.
  TopWindowVerifierContext context(
      selected, reinterpret_cast<HWND>(excluded_window()), content_rect,
      &window_capture_helper_,
      options_.allow_magnification_api_for_window_capture() &&
          rtc::IsWindows8OrLater());

  if (windows_top_of_me_worker_ && rtc::IsWindows8OrLater()) {
    for (auto hwnd : windows_top_of_me_worker_->core_windows()) {
      DesktopRect content_rect;
      if (GetWindowContentRect(hwnd, &content_rect)) {
        content_rect.IntersectWith(context.selected_window_rect);
        if (!content_rect.is_empty()) {
          return false;
        }
      }
    }
  }

  const LPARAM enum_param = reinterpret_cast<LPARAM>(&context);
  EnumWindows(&TopWindowVerifier, enum_param);
  if (!context.is_top_window) {
    return false;
  }

  // If |selected| is not covered by other windows, check whether it is
  // covered by its own child windows. Note: EnumChildWindows() enumerates child
  // windows in all generations, but does not include any controls like buttons
  // or textboxes.
  EnumChildWindows(selected, &TopWindowVerifier, enum_param);
  return context.is_top_window;
}

bool CroppingWindowCapturerWin::ShouldUseMagnifier() {
  if (!options_.allow_magnification_api_for_window_capture())
    return false;

  bool result = selected_window_should_use_magnifier_;
  if (result && screen_magnifier_capturer_worker_) {
    DesktopRect rect = GetWindowRectInVirtualScreen(true);
    if (rect.is_empty()) {
      result = false;
    }
  }
  return result;
}

struct WindowsTopOfMeContext {
  WindowsTopOfMeContext(HWND selected_window,
                        WindowCaptureHelperWin* window_capture_helper)
      : selected_window(selected_window),
        window_capture_helper(window_capture_helper) {
    GetWindowText(selected_window, selected_window_title, kTitleLength);
    GetWindowThreadProcessId(selected_window, &selected_window_process_id);
    GetWindowContentRect(selected_window, &selected_window_rect);
  }

  const HWND selected_window;
  WindowCaptureHelperWin* window_capture_helper;
  DesktopRect selected_window_rect;
  WCHAR selected_window_title[kTitleLength];
  DWORD selected_window_process_id;
  std::vector<HWND> windows_top_of_me;
};

BOOL CALLBACK WindowsTopOfMe(HWND hwnd, LPARAM param) {
  WindowsTopOfMeContext* context =
      reinterpret_cast<WindowsTopOfMeContext*>(param);

  if (hwnd == context->selected_window) {
    return FALSE;
  }

  // Ignore invisible window on current desktop.
  if (!context->window_capture_helper->IsWindowVisibleOnCurrentDesktop(hwnd)) {
    return TRUE;
  }

  // Ignore descendant windows since we want to capture them.
  // This check does not work for tooltips and context menus. Drop down menus
  // and popup windows are fine.
  //
  // GA_ROOT returns the root window instead of the owner. I.e. for a dialog
  // window, GA_ROOT returns the dialog window itself. GA_ROOTOWNER returns the
  // application main window which opens the dialog window. Since we are sharing
  // the application main window, GA_ROOT should be used here.
  if (GetAncestor(hwnd, GA_ROOT) == context->selected_window) {
    return TRUE;
  }

  WCHAR window_title[kTitleLength];
  GetWindowText(hwnd, window_title, kTitleLength);
  if (wcsnlen_s(window_title, kTitleLength) == 0 ||
      wcscmp(window_title, context->selected_window_title) == 0) {
    DWORD enumerated_window_process_id;
    GetWindowThreadProcessId(hwnd, &enumerated_window_process_id);
    if (context->selected_window_process_id == enumerated_window_process_id) {
      return TRUE;
    }
  }

  DesktopRect content_rect;
  if (!GetWindowContentRect(hwnd, &content_rect)) {
    // Bail out if failed to get the window area.
    return TRUE;
  }

  content_rect.IntersectWith(context->selected_window_rect);

  // If intersection empty, ignore
  if (content_rect.is_empty()) {
    return TRUE;
  }

  context->windows_top_of_me.push_back(hwnd);
  return TRUE;
}

const int WindowsTopOfMeWorker::kFps = 30;
const int WindowsTopOfMeWorker::kIgnoreCounter = 2;
const uint32_t WindowsTopOfMeWorker::kLastMsThreshold = 500;

WindowsTopOfMeWorker::WindowsTopOfMeWorker(
    WindowCaptureHelperWin* window_capture_helper)
    : ignore_counter_(kIgnoreCounter),
      window_capture_helper_(window_capture_helper),
      last_changed_(0) {}

void WindowsTopOfMeWorker::SelectWindow(HWND window,
                                     const bool is_using_magnifier) {
  selected_window_ = window;
  if (is_using_magnifier) {
    Run(NULL);
  } else {
    exclusion_window_list_.clear();
    last_changed_ = 0;
  }
  ignore_counter_ = kIgnoreCounter;
}

void WindowsTopOfMeWorker::Run(rtc::Thread* thread) {
  HRESULT hr = S_FALSE;
  if (thread) {
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  }
  do {
    std::vector<HWND> windows;
    if (rtc::IsWindows8OrLater()) {
      const WCHAR* string_class[] = {L"Windows.UI.Core.CoreWindow",
                                     L"Shell_InputSwitchTopLevelWindow"};
      for (auto s : string_class) {
        HWND hWnd =
            FindWindowEx(NULL, NULL, s, NULL);
        while (hWnd != NULL) {
          int CloakedVal;
          HRESULT hRes = DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &CloakedVal,
                                               sizeof(CloakedVal));
          if (hRes == S_OK && CloakedVal == 0) {
            windows.push_back(hWnd);
          }
          hWnd = FindWindowEx(NULL, hWnd, s, NULL);
        }
      }
      core_windows_ = windows;
    }

    WindowsTopOfMeContext context(selected_window_, window_capture_helper_);
    EnumWindows(&WindowsTopOfMe, reinterpret_cast<LPARAM>(&context));

    // remove hwnd from windows vector if it is already in the context.windows_top_of_me
    for (auto hWnd : context.windows_top_of_me) {
      auto it = std::find(windows.begin(), windows.end(), hWnd);
      if (it != windows.end()) {
        windows.erase(it);
      }
    }

    HWND hWnd = FindWindowEx(NULL, NULL, L"Shell_TrayWnd", NULL);
    if (hWnd != NULL &&
        window_capture_helper_->IsWindowVisibleOnCurrentDesktop(hWnd)) {
      const std::vector<HWND>& vec = context.windows_top_of_me;
      if (std::find(vec.begin(), vec.end(), hWnd) == vec.end()) {
        windows.push_back(hWnd);
      }

      const WCHAR* string_class[] = {L"TaskListThumbnailWnd", L"#32768",
                                     L"tooltips_class32"};

      for (auto s : string_class) {
        hWnd = FindWindowEx(NULL, NULL, s, NULL);
        while (hWnd != NULL) {
          if (std::find(vec.begin(), vec.end(), hWnd) == vec.end() &&
              window_capture_helper_->IsWindowVisibleOnCurrentDesktop(hWnd)) {
            windows.push_back(hWnd);
          }
          hWnd = FindWindowEx(NULL, hWnd, s, NULL);
        }
      }
    }

    for (auto hWnd : windows) {
      DesktopRect content_rect;
      if (GetWindowContentRect(hWnd, &content_rect)) {
        content_rect.IntersectWith(context.selected_window_rect);
        if (!content_rect.is_empty()) {
          context.windows_top_of_me.push_back(hWnd);
        }
      }
    }

    if (exclusion_window_list_ != context.windows_top_of_me) {
      exclusion_window_list_ = context.windows_top_of_me;
      last_changed_ = rtc::Time32();
    }

    if (thread) {
      Sleep(1000 / kFps);
    }
  } while (thread && !thread->IsQuitting());
  if (hr == S_OK) {
    CoUninitialize();
  }
}

bool WindowsTopOfMeWorker::IsChanged(uint32_t in_last_ms) {
  if (ignore_counter_ > 0) {
    ignore_counter_--;
    return false;
  }
  if (!thread_) {
    thread_ = rtc::Thread::Create();
    if (thread_->Start(this)) {
      RTC_LOG(LS_INFO) << "WindowsTopOfMeWorker Started succesfully";
    } else {
      RTC_LOG(LS_ERROR) << "WindowsTopOfMeWorker Start fail";
    }
  }
  return (rtc::Time32() - last_changed_) < in_last_ms;
}

bool CroppingWindowCapturerWin::CaptureFrameUsingMagnifierApi() {
  if (!screen_magnifier_capturer_worker_) {
    return false;
  }

  std::vector<WindowId> exclusion_windows =
      windows_top_of_me_worker_->exclusion_window_list();
  capturer_ = Magnifier;
  return screen_magnifier_capturer_worker_->CaptureFrame(this,
                                                         exclusion_windows);
}

bool CroppingWindowCapturerWin::SelectSource(SourceId id) {
  capturer_ = Unknown;
  HWND hwnd = reinterpret_cast<HWND>(id);
  const size_t kClassLength = 256;
  WCHAR class_name[kClassLength];
  const int class_name_length = GetClassName(hwnd, class_name, kClassLength);
  RTC_DCHECK(class_name_length)
      << "Error retrieving the application's class name";
  selected_window_should_use_magnifier_ = false;
  if (rtc::IsWindows8OrLater() &&
      wcscmp(class_name, L"ApplicationFrameWindow") == 0) {
    selected_window_should_use_magnifier_ = true;
  }

  if (!selected_window_should_use_magnifier_ &&
      wcscmp(class_name, L"screenClass") == 0) {
    selected_window_should_use_magnifier_ = true;
  }

  if (!selected_window_should_use_magnifier_ &&
      ChildWindowsContains(hwnd, L"Intermediate D3D Window")) {
    selected_window_should_use_magnifier_ = true;
  }

  if (windows_top_of_me_worker_) {
    windows_top_of_me_worker_->SelectWindow(hwnd,
                                         selected_window_should_use_magnifier_);
  }

  return CroppingWindowCapturer::SelectSource(id);
}

void CroppingWindowCapturerWin::CaptureFrame() {
  if (!GetWindowRect(reinterpret_cast<HWND>(selected_window()),
                     &window_region_rect_)) {
    window_region_rect_ = DesktopRect();
  }

  if (options_.allow_magnification_api_for_window_capture() &&
      !windows_top_of_me_worker_) {
    windows_top_of_me_worker_ =
        std::make_unique<WindowsTopOfMeWorker>(&window_capture_helper_);
    windows_top_of_me_worker_->SelectWindow(
        reinterpret_cast<HWND>(selected_window()),
        selected_window_should_use_magnifier_);
  }

  if (windows_top_of_me_worker_ &&
      windows_top_of_me_worker_->IsChanged(
          WindowsTopOfMeWorker::kLastMsThreshold)) {
    RTC_DLOG(LS_INFO) << "Windows order was changed, during the past"
                      << WindowsTopOfMeWorker::kLastMsThreshold << " ms";

    // hack so CroppingWindowCapturer::OnCaptureResult doesn't fallback to
    // window capturer
    should_use_screen_capturer_cache_ = True;
    CroppingWindowCapturer::OnCaptureResult(Result::ERROR_TEMPORARY, nullptr);
    should_use_screen_capturer_cache_ = Empty;
    return;
  }

  RTC_DLOG(LS_INFO) << "Captured using " << capturer_;
  RTC_DCHECK(should_use_screen_capturer_cache_ == Empty);
  should_use_screen_capturer_cache_ = ShouldUseScreenCapturer() ? True : False;

  if (ShouldUseMagnifier() && !cant_get_screen_magnifier_capturer_worker_) {
    if (should_use_screen_capturer_cache_ == False) {
      if (!screen_magnifier_capturer_worker_.get()) {
        screen_magnifier_capturer_worker_ =
            ScreenCapturerWinMagnifierWorker::Get();

        cant_get_screen_magnifier_capturer_worker_ =
            !screen_magnifier_capturer_worker_;
      }
      if (CaptureFrameUsingMagnifierApi()) {
        return;
      }
    }
  }

  if (capturer_ != Unknown && capturer_ != Screen &&
      should_use_screen_capturer_cache_ == True) {
    static const DWORD kFullScreenTransitionTime = 34;
    // transtion to screen capturer
    RTC_DLOG(LS_INFO) << "transtion to screen capturer Sleep for "
                      << kFullScreenTransitionTime << " ms";
    Sleep(kFullScreenTransitionTime);
    capturer_ = Screen;
    CroppingWindowCapturer::OnCaptureResult(Result::ERROR_TEMPORARY, nullptr);
    should_use_screen_capturer_cache_ = Empty;
    return;
  }

  switch (should_use_screen_capturer_cache_) {
    case True:
      capturer_ = Screen;
      break;
    case False:
      capturer_ = Window;
      break;
    default:
      capturer_ = Unknown;
  }
  CroppingWindowCapturer::CaptureFrame();
  should_use_screen_capturer_cache_ = Empty;
}

void CroppingWindowCapturerWin::OnCaptureResult(
    DesktopCapturer::Result result,
    std::unique_ptr<DesktopFrame> screen_frame) {
  // hack so CroppingWindowCapturer::OnCaptureResult doesn't fallback to
  // window capturer
  should_use_screen_capturer_cache_ = True;
  if (windows_top_of_me_worker_ &&
      windows_top_of_me_worker_->IsChanged(
          WindowsTopOfMeWorker::kLastMsThreshold)) {
    RTC_DLOG(LS_INFO) << "Windows order has changed, during capture";
    CroppingWindowCapturer::OnCaptureResult(Result::ERROR_TEMPORARY, nullptr);
    // clear cache for next capture frame
    should_use_screen_capturer_cache_ = Empty;
    return;
  }

  if (capturer_ == Magnifier) {
    offset_ = screen_frame->top_left();
    screen_frame->set_top_left(DesktopVector());
  }

  CroppingWindowCapturer::OnCaptureResult(result, std::move(screen_frame));
  should_use_screen_capturer_cache_ = Empty;
}

DesktopRect CroppingWindowCapturerWin::GetWindowRectInVirtualScreen() {
  return GetWindowRectInVirtualScreen(capturer_ == Magnifier);
}

DesktopRect CroppingWindowCapturerWin::GetWindowRectInVirtualScreen(
    const bool magnifier) {
  TRACE_EVENT0("webrtc",
               "CroppingWindowCapturerWin::GetWindowRectInVirtualScreen");
  DesktopRect window_rect;
  HWND hwnd = reinterpret_cast<HWND>(selected_window());
  if (!GetCroppedWindowRect(hwnd, &window_rect, /* original_rect */ nullptr)) {
    RTC_LOG(LS_WARNING) << "Failed to get window info: " << GetLastError();
    return window_rect;
  }
  window_rect.IntersectWith(window_region_rect_);

  // Convert |window_rect| to be relative to the top-left of the virtual screen.
  DesktopRect screen_rect(GetFullscreenRect());
  if (magnifier && screen_magnifier_capturer_worker_) {
    std::wstring device_key;
    DesktopCapturer::SourceId mainId =
        screen_magnifier_capturer_worker_->source_id();
    if (IsScreenValid(mainId, &device_key)) {
      screen_rect = GetScreenRect(mainId, device_key);
    }
    window_rect.Translate(offset_);
  }

  window_rect.IntersectWith(screen_rect);
  window_rect.Translate(-screen_rect.left(), -screen_rect.top());
  return window_rect;
}

}  // namespace

// static
std::unique_ptr<DesktopCapturer> CroppingWindowCapturer::CreateCapturer(
    const DesktopCaptureOptions& options) {
  return std::unique_ptr<DesktopCapturer>(
      new CroppingWindowCapturerWin(options));
}

}  // namespace webrtc
