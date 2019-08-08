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

#include "modules/desktop_capture/win/screen_capture_utils.h"
#include "modules/desktop_capture/win/window_capture_utils.h"
#include "rtc_base/event.h"
#include "rtc_base/logging.h"
#include "rtc_base/thread.h"
#include "rtc_base/time_utils.h"
#include "rtc_base/trace_event.h"
#include "rtc_base/win32.h"

namespace webrtc {

namespace {

const size_t kTitleLength = 256;
const size_t kClassLength = 256;

BOOL IsAncestor(const HWND ancestor,
                const WCHAR* ancestor_title,
                const DWORD ancestor_process_id,
                const HWND hwnd,
                const bool allow_uwp_window_capture) {
  // Ignore descendant windows since we want to capture them.
  // This check does not work for tooltips and context menus. Drop down menus
  // and popup windows are fine.
  //
  // GA_ROOT returns the root window instead of the owner. I.e. for a dialog
  // window, GA_ROOT returns the dialog window itself. GA_ROOTOWNER returns the
  // application main window which opens the dialog window. Since we are sharing
  // the application main window, GA_ROOT should be used here.
  if (GetAncestor(hwnd, GA_ROOT) == ancestor) {
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
  GetWindowTextW(hwnd, window_title, kTitleLength);
  if (wcsnlen_s(window_title, kTitleLength) == 0 ||
      wcscmp(window_title, ancestor_title) == 0) {
    DWORD enumerated_window_process_id;
    GetWindowThreadProcessId(hwnd, &enumerated_window_process_id);
    if (ancestor_process_id == enumerated_window_process_id) {
      return TRUE;
    }
  }

  if (allow_uwp_window_capture) {
    // Xaml_WindowedPopupClass has "PopupHost" title, and different process id
    // hence we need to iterate using GetParent to confirm the ancestry
    HWND it = hwnd;
    while (it != NULL) {
      it = GetParent(it);
      if (it == ancestor) {
        // we don't won't to capture child window that has titlebar / WS_CAPTION 
        return !(GetWindowLongPtr(hwnd, GWL_STYLE) & WS_CAPTION);
      }
    }
  }
  return FALSE;
}
// Used to pass input/output data during the EnumWindow call for verifying if
// the selected window is on top.
struct TopWindowVerifierContext {
  TopWindowVerifierContext(HWND selected_window,
                           HWND excluded_window,
                           DesktopRect selected_window_rect,
                           WindowCaptureHelperWin* window_capture_helper,
                           const bool allow_uwp_window_capture)
      : selected_window(selected_window),
        excluded_window(excluded_window),
        selected_window_rect(selected_window_rect),
        window_capture_helper(window_capture_helper),
        allow_uwp_window_capture(allow_uwp_window_capture),
        is_top_window(false) {
    RTC_DCHECK_NE(selected_window, excluded_window);

    GetWindowTextW(selected_window, selected_window_title, kTitleLength);
    GetWindowThreadProcessId(selected_window, &selected_window_process_id);
  }

  const HWND selected_window;
  const HWND excluded_window;
  const DesktopRect selected_window_rect;
  WindowCaptureHelperWin* window_capture_helper;
  WCHAR selected_window_title[kTitleLength];
  DWORD selected_window_process_id;
  const bool allow_uwp_window_capture;
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

  if (IsAncestor(context->selected_window, context->selected_window_title,
                 context->selected_window_process_id, hwnd,
                 context->allow_uwp_window_capture)) {
    return TRUE;
  }

  if (context->allow_uwp_window_capture) {
    WCHAR class_name[kClassLength];
    const int class_name_length = GetClassNameW(hwnd, class_name, kClassLength);
    RTC_DCHECK(class_name_length) << "Error retrieving the window's class name";
    if (wcscmp(class_name, L"Windows.UI.Core.CoreWindow") == 0) {
      return TRUE;
    }
  }

  // Checks whether current window |hwnd| intersects with
  // |context|->selected_window.
  if (context->window_capture_helper->IsWindowIntersectWithSelectedWindow(
          hwnd, context->selected_window, context->selected_window_rect)) {
    // If intersection is not empty, the selected window is not on top.
    context->is_top_window = false;
    return FALSE;
  }

  // Otherwise, keep enumerating.
  return TRUE;
}

class WindowsTopOfMeWorker : rtc::Runnable {
 public:
  WindowsTopOfMeWorker(WindowCaptureHelperWin* window_capture_helper);
  void SelectWindow(HWND window);
  bool IsChanged(uint32_t in_last_ms);

  // used by ShouldUseScreenCapture
  std::vector<HWND> core_windows() { 
    if (thread_) {
      event_.Set();
      event_.Reset();
      event_.Wait(rtc::Event::kForever);
    }
    return core_windows_; 
  }

  static const uint32_t kLastMsThreshold;

 private:
  void Run(rtc::Thread* thread) override;
  int ignore_counter_;
  std::unique_ptr<rtc::Thread> thread_;
  rtc::Event event_;
  WindowCaptureHelperWin* window_capture_helper_;
  HWND selected_window_;
  std::vector<HWND> core_windows_;
  std::vector<HWND> windows_top_of_me_;
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
        should_use_screen_capturer_cache_(Empty) {}

 private:
  bool ShouldUseScreenCapturer() override;
  void CaptureFrame() override;
  bool SelectSource(SourceId id) override;
  DesktopRect GetWindowRectInVirtualScreen() override;
  void OnCaptureResult(DesktopCapturer::Result result,
                       std::unique_ptr<DesktopFrame> frame) override;

  // The region from GetWindowRgn in the desktop coordinate if the region is
  // rectangular, or the rect from GetWindowRect if the region is not set.
  DesktopRect window_region_rect_;
  DesktopVector offset_;
  enum Capturer {
    Unknown,
    Screen,
    Window,
  };
  Capturer capturer_;
  WindowCaptureHelperWin window_capture_helper_;
  std::unique_ptr<WindowsTopOfMeWorker> windows_top_of_me_worker_;
  enum BoolCache {
    Empty = -1,
    False,
    True,
  };
  BoolCache should_use_screen_capturer_cache_;
};

bool CroppingWindowCapturerWin::ShouldUseScreenCapturer() {
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
  TopWindowVerifierContext context(selected,
                                   reinterpret_cast<HWND>(excluded_window()),
                                   content_rect, &window_capture_helper_,
                                   options_.allow_uwp_window_capture() &&
                                   rtc::IsWindows8OrLater());

  if (windows_top_of_me_worker_) {
    for (auto hwnd : windows_top_of_me_worker_->core_windows()) {
      if (context.window_capture_helper->IsWindowIntersectWithSelectedWindow(
        hwnd, context.selected_window, context.selected_window_rect)) {
        return false;
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

struct WindowsTopOfMeContext {
  WindowsTopOfMeContext(HWND selected_window,
                        WindowCaptureHelperWin* window_capture_helper)
      : selected_window(selected_window),
        window_capture_helper(window_capture_helper),
        window_is_moving(false) {
    GetWindowTextW(selected_window, selected_window_title, kTitleLength);
    GetWindowThreadProcessId(selected_window, &selected_window_process_id);
    GetWindowContentRect(selected_window, &selected_window_rect);
  }

  const HWND selected_window;
  WindowCaptureHelperWin* window_capture_helper;
  DesktopRect selected_window_rect;
  WCHAR selected_window_title[kTitleLength];
  DWORD selected_window_process_id;
  bool window_is_moving;
  std::vector<HWND> windows_top_of_me;
};

BOOL CALLBACK WindowsTopOfMe(HWND hwnd, LPARAM param) {
  WindowsTopOfMeContext* context =
      reinterpret_cast<WindowsTopOfMeContext*>(param);

  if (!context->window_is_moving) {
    GUITHREADINFO gui;
    gui.cbSize = sizeof(GUITHREADINFO);
    if (GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, NULL), &gui)) {
      if (gui.flags & GUI_INMOVESIZE) {
        context->window_is_moving = true;
      }
    }
  }

  if (hwnd == context->selected_window) {
    return FALSE;
  }

  // Ignore invisible window on current desktop.
  if (!context->window_capture_helper->IsWindowVisibleOnCurrentDesktop(hwnd)) {
    return TRUE;
  }

  if (IsAncestor(context->selected_window, context->selected_window_title,
                 context->selected_window_process_id, hwnd, true)) {
    return TRUE;
  }

  // If intersection empty, ignore
  if (!context->window_capture_helper->IsWindowIntersectWithSelectedWindow(
    hwnd, context->selected_window, context->selected_window_rect)) {
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

void WindowsTopOfMeWorker::SelectWindow(HWND window) {
  selected_window_ = window;
  windows_top_of_me_.clear();
  last_changed_ = 0;
  ignore_counter_ = kIgnoreCounter;
}

void WindowsTopOfMeWorker::Run(rtc::Thread* thread) {
  do {
    std::vector<HWND> windows;
    if (rtc::IsWindows8OrLater()) {
      // these window classes if not cloacked, add it to windows vector
      const WCHAR* string_class[] = {L"Windows.UI.Core.CoreWindow",
                                     L"Shell_InputSwitchTopLevelWindow"};
      for (auto s : string_class) {
        HWND hWnd = FindWindowExW(NULL, NULL, s, NULL);
        while (hWnd != NULL) {
          if (!window_capture_helper_->IsWindowCloaked(hWnd)) {
            windows.push_back(hWnd);
          }
          hWnd = FindWindowExW(NULL, hWnd, s, NULL);
        }
      }
    }
    WindowsTopOfMeContext context(selected_window_, window_capture_helper_);
    HWND hWnd = FindWindowExW(NULL, NULL, L"Shell_TrayWnd", NULL);
    // if "start" menu is visible
    if (hWnd != NULL &&
        window_capture_helper_->IsWindowVisibleOnCurrentDesktop(hWnd)) {
      windows.push_back(hWnd);
      // these classes sometimes are not enumerated by "EnumWindows"
      // these window classes can only exist in the "root" if start menu is visible
      const WCHAR* string_class[] = {L"TaskListThumbnailWnd", L"#32768",
                                     L"tooltips_class32",
                                     L"Xaml_WindowedPopupClass", L"SysShadow"};
      for (auto s : string_class) {
        hWnd = FindWindowExW(NULL, NULL, s, NULL);
        while (hWnd != NULL) {
          if (!IsAncestor(selected_window_, context.selected_window_title,
                          context.selected_window_process_id, hWnd, true) &&
              window_capture_helper_->IsWindowVisibleOnCurrentDesktop(hWnd)) {
            windows.push_back(hWnd);
          }
          hWnd = FindWindowExW(NULL, hWnd, s, NULL);
        }
      }
    }
    core_windows_ = windows;

    EnumWindows(&WindowsTopOfMe, reinterpret_cast<LPARAM>(&context));

    // remove hwnd from windows vector if it is already in the
    // context.windows_top_of_me
    for (auto hWnd : context.windows_top_of_me) {
      auto it = std::find(windows.begin(), windows.end(), hWnd);
      if (it != windows.end()) {
        windows.erase(it);
      }
    }

    // add hwnd in windows vector to context.windows_top_of_me vector
    // if hwnd is intersecting with selected window
    for (auto hWnd : windows) {
      DesktopRect content_rect;
      if (GetWindowContentRect(hWnd, &content_rect)) {
        content_rect.IntersectWith(context.selected_window_rect);
        if (!content_rect.is_empty()) {
          context.windows_top_of_me.push_back(hWnd);
        }
      }
    }

    // the main purpose of this function, calculate if windows_top_of_me has changed
    if (context.window_is_moving ||
        windows_top_of_me_ != context.windows_top_of_me) {
      windows_top_of_me_ = context.windows_top_of_me;
      last_changed_ = rtc::Time32();
    }

    if (thread && !thread->IsQuitting()) {
      event_.Set();
      event_.Reset();
      event_.Wait(1000 / kFps);
    }
  } while (thread && !thread->IsQuitting());
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

bool CroppingWindowCapturerWin::SelectSource(SourceId id) {
  capturer_ = Unknown;
  HWND hwnd = reinterpret_cast<HWND>(id);
  if (windows_top_of_me_worker_) {
    windows_top_of_me_worker_->SelectWindow(hwnd);
  }
  return CroppingWindowCapturer::SelectSource(id);
}

void CroppingWindowCapturerWin::CaptureFrame() {
  if (!GetWindowRect(reinterpret_cast<HWND>(selected_window()),
                     &window_region_rect_)) {
    window_region_rect_ = DesktopRect();
  }

  if (options_.allow_uwp_window_capture() &&
      !windows_top_of_me_worker_) {
    windows_top_of_me_worker_ =
        std::make_unique<WindowsTopOfMeWorker>(&window_capture_helper_);
    windows_top_of_me_worker_->SelectWindow(
        reinterpret_cast<HWND>(selected_window()));
  }

  if (windows_top_of_me_worker_ &&
      windows_top_of_me_worker_->IsChanged(
          WindowsTopOfMeWorker::kLastMsThreshold)) {
    RTC_DLOG(LS_INFO) << "Windows order was changed, during the past "
                      << WindowsTopOfMeWorker::kLastMsThreshold << " ms";

    // hack so CroppingWindowCapturer::OnCaptureResult doesn't fallback to
    // window capturer
    should_use_screen_capturer_cache_ = True;
    CroppingWindowCapturer::OnCaptureResult(Result::ERROR_TEMPORARY, nullptr);
    should_use_screen_capturer_cache_ = Empty;
    return;
  }

  RTC_DCHECK(should_use_screen_capturer_cache_ == Empty)
      << "should_use_screen_capturer_cache_ should be Empty";
  should_use_screen_capturer_cache_ = ShouldUseScreenCapturer() ? True : False;


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
  RTC_DLOG(LS_INFO) << "Captured using " << capturer_;
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

  CroppingWindowCapturer::OnCaptureResult(result, std::move(screen_frame));
  should_use_screen_capturer_cache_ = Empty;
}

DesktopRect CroppingWindowCapturerWin::GetWindowRectInVirtualScreen() {
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
