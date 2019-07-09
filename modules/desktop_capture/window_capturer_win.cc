/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <assert.h>

#include <memory>

#include "modules/desktop_capture/cropped_desktop_frame.h"
#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_frame_win.h"
#include "modules/desktop_capture/win/screen_capture_utils.h"
#include "modules/desktop_capture/win/window_capture_utils.h"
#include "modules/desktop_capture/win/windows_graphics_capturer.h"
#include "modules/desktop_capture/window_finder_win.h"
#include "rtc_base/checks.h"
#include "rtc_base/constructor_magic.h"
#include "rtc_base/logging.h"
#include "rtc_base/string_utils.h"
#include "rtc_base/trace_event.h"
#include "rtc_base/win32.h"

namespace webrtc {

namespace {

struct SourceListContext {
  DesktopCapturer::SourceList list;
  const bool allow_magnification_api_for_window_capture;
};

BOOL CALLBACK WindowsEnumerationHandler(HWND hwnd, LPARAM param) {
  SourceListContext* context = reinterpret_cast<SourceListContext*>(param);
  DesktopCapturer::SourceList* list = &context->list;

  // Skip windows that are invisible, minimized, have no title, or are owned,
  // unless they have the app window style set.
  int len = GetWindowTextLength(hwnd);
  HWND owner = GetWindow(hwnd, GW_OWNER);
  LONG exstyle = GetWindowLong(hwnd, GWL_EXSTYLE);
  if (len == 0 || IsIconic(hwnd) || !IsWindowVisible(hwnd) ||
      (owner && !(exstyle & WS_EX_APPWINDOW))) {
    return TRUE;
  }
  // Skip unresponsive windows. Set timout with 50ms, in case system is under
  // heavy load, the check can wait longer but wont' be too long to delay the
  // the enumeration.
  const UINT uTimeout = 50;  // ms
  if (!SendMessageTimeout(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, uTimeout,
                          nullptr)) {
    return TRUE;
  }

  // Skip the Program Manager window and the Start button.
  const size_t kClassLength = 256;
  WCHAR class_name[kClassLength];
  const int class_name_length = GetClassNameW(hwnd, class_name, kClassLength);
  RTC_DCHECK(class_name_length)
      << "Error retrieving the application's class name";

  // Skip Program Manager window and the Start button. This is the same logic
  // that's used in Win32WindowPicker in libjingle. Consider filtering other
  // windows as well (e.g. toolbars).
  if (wcscmp(class_name, L"Progman") == 0 || wcscmp(class_name, L"Button") == 0)
    return TRUE;

  // Windows 8 introduced a "Modern App" identified by their class name being
  // either ApplicationFrameWindow or windows.UI.Core.coreWindow. The
  // associated windows cannot be captured, so we skip them.
  // http://crbug.com/526883.
  if (rtc::IsWindows8OrLater()) {
    if (wcscmp(class_name, L"ApplicationFrameWindow") == 0 && 
        !(context->allow_magnification_api_for_window_capture &&
          ChildWindowsContains(hwnd, L"Windows.UI.Core.CoreWindow"))) {
        return TRUE;
    } else if (wcscmp(class_name, L"Windows.UI.Core.CoreWindow") == 0) {
      return TRUE;
    }
  }

  DesktopCapturer::Source window;
  window.id = reinterpret_cast<WindowId>(hwnd);

  const size_t kTitleLength = 500;
  WCHAR window_title[kTitleLength];
  // Truncate the title if it's longer than kTitleLength.
  GetWindowTextW(hwnd, window_title, kTitleLength);
  window.title = rtc::ToUtf8(window_title);

  // Skip windows when we failed to convert the title or it is empty.
  if (window.title.empty())
    return TRUE;

  list->push_back(window);

  return TRUE;
}

class WindowCapturerWin : public DesktopCapturer {
 public:
  WindowCapturerWin(const bool allow_magnification_api_for_window_capture, 
    const bool allow_windows_graphics_capturer);
  ~WindowCapturerWin() override;

  // DesktopCapturer interface.
  void Start(Callback* callback) override;
  void CaptureFrame() override;
  bool GetSourceList(SourceList* sources) override;
  bool SelectSource(SourceId id) override;
  bool FocusOnSelectedSource() override;
  bool IsOccluded(const DesktopVector& pos) override;

 private:
  Callback* callback_ = nullptr;

  // HWND and HDC for the currently selected window or nullptr if window is not
  // selected.
  HWND window_ = nullptr;

  DesktopSize previous_size_;

  WindowCaptureHelperWin window_capture_helper_;

  // This map is used to avoid flickering for the case when SelectWindow() calls
  // are interleaved with Capture() calls.
  std::map<HWND, DesktopSize> window_size_map_;

  WindowFinderWin window_finder_;

  const bool allow_magnification_api_for_window_capture_;
  const bool allow_windows_graphics_capturer_;

  int frame_counter_;

  std::unique_ptr< WindowsGraphicsCapturer> windows_graphics_capturer_;

  RTC_DISALLOW_COPY_AND_ASSIGN(WindowCapturerWin);
};

WindowCapturerWin::WindowCapturerWin(
    const bool allow_magnification_api_for_window_capture,
    const bool allow_windows_graphics_capturer)
    : allow_magnification_api_for_window_capture_(
          allow_magnification_api_for_window_capture), 
      allow_windows_graphics_capturer_(allow_windows_graphics_capturer), 
      frame_counter_(0) {}
WindowCapturerWin::~WindowCapturerWin() {}

bool WindowCapturerWin::GetSourceList(SourceList* sources) {
  SourceListContext context = {DesktopCapturer::SourceList(),
                              allow_magnification_api_for_window_capture_};
  LPARAM param = reinterpret_cast<LPARAM>(&context);
  // EnumWindows only enumerates root windows.
  if (!EnumWindows(&WindowsEnumerationHandler, param))
    return false;

  DesktopCapturer::SourceList& result = context.list;
  for (auto it = result.begin(); it != result.end();) {
    if (!window_capture_helper_.IsWindowOnCurrentDesktop(
            reinterpret_cast<HWND>(it->id))) {
      it = result.erase(it);
    } else {
      ++it;
    }
  }
  sources->swap(result);

  std::map<HWND, DesktopSize> new_map;
  for (const auto& item : *sources) {
    HWND hwnd = reinterpret_cast<HWND>(item.id);
    new_map[hwnd] = window_size_map_[hwnd];
  }
  window_size_map_.swap(new_map);

  return true;
}

bool WindowCapturerWin::SelectSource(SourceId id) {
  HWND window = reinterpret_cast<HWND>(id);
  if (!IsWindow(window) || !IsWindowVisible(window) || IsIconic(window))
    return false;
  window_ = window;
  // When a window is not in the map, window_size_map_[window] will create an
  // item with DesktopSize (0, 0).
  previous_size_ = window_size_map_[window];
  frame_counter_ = 0;
  return true;
}

bool WindowCapturerWin::FocusOnSelectedSource() {
  if (!window_)
    return false;

  if (!IsWindow(window_) || !IsWindowVisible(window_) || IsIconic(window_))
    return false;

  return BringWindowToTop(window_) != FALSE &&
         SetForegroundWindow(window_) != FALSE;
}

bool WindowCapturerWin::IsOccluded(const DesktopVector& pos) {
  DesktopVector sys_pos = pos.add(GetFullscreenRect().top_left());
  return reinterpret_cast<HWND>(window_finder_.GetWindowUnderPoint(sys_pos)) !=
         window_;
}

void WindowCapturerWin::Start(Callback* callback) {
  assert(!callback_);
  assert(callback);

  callback_ = callback;
}

void WindowCapturerWin::CaptureFrame() {
  TRACE_EVENT0("webrtc", "WindowCapturerWin::CaptureFrame");

  if (!window_) {
    RTC_LOG(LS_ERROR) << "Window hasn't been selected: " << GetLastError();
    callback_->OnCaptureResult(Result::ERROR_PERMANENT, nullptr);
    return;
  }

  // Stop capturing if the window has been closed.
  if (!IsWindow(window_)) {
    RTC_LOG(LS_ERROR) << "target window has been closed";
    callback_->OnCaptureResult(Result::ERROR_PERMANENT, nullptr);
    return;
  }

  DesktopRect cropped_rect;
  DesktopRect original_rect;
  if (!GetCroppedWindowRect(window_, &cropped_rect, &original_rect)) {
    RTC_LOG(LS_WARNING) << "Failed to get drawable window area: "
                        << GetLastError();
    callback_->OnCaptureResult(Result::ERROR_TEMPORARY, nullptr);
    return;
  }

  // Return a 1x1 black frame if the window is minimized or invisible on current
  // desktop, to match behavior on mace. Window can be temporarily invisible
  // during the transition of full screen mode on/off.
  if (original_rect.is_empty() ||
      !window_capture_helper_.IsWindowVisibleOnCurrentDesktop(window_)) {
    std::unique_ptr<DesktopFrame> frame(
        new BasicDesktopFrame(DesktopSize(1, 1)));

    previous_size_ = frame->size();
    window_size_map_[window_] = previous_size_;
    callback_->OnCaptureResult(Result::SUCCESS, std::move(frame));
    return;
  }
  if (frame_counter_ < 2) {
    frame_counter_++;
  } else if (allow_windows_graphics_capturer_ && WindowsGraphicsCapturer::IsSupported()) {
    if (!windows_graphics_capturer_) {
      windows_graphics_capturer_ = std::make_unique<WindowsGraphicsCapturer>();
      if (windows_graphics_capturer_->SelectSource(reinterpret_cast<SourceId>(window_))) {
        windows_graphics_capturer_->Start(callback_);
      } else {
        windows_graphics_capturer_.reset();
      }
    }
    if (windows_graphics_capturer_) {
      windows_graphics_capturer_->CaptureFrame();
      return;
    }
  }

  HDC window_dc = GetWindowDC(window_);
  if (!window_dc) {
    RTC_LOG(LS_WARNING) << "Failed to get window DC: " << GetLastError();
    callback_->OnCaptureResult(Result::ERROR_TEMPORARY, nullptr);
    return;
  }

  DesktopSize window_dc_size;
  if (GetDcSize(window_dc, &window_dc_size)) {
    // The |window_dc_size| is used to detect the scaling of the original
    // window. If the application does not support high-DPI settings, it will
    // be scaled by Windows according to the scaling setting.
    // https://www.google.com/search?q=windows+scaling+settings&ie=UTF-8
    // So the size of the |window_dc|, i.e. the bitmap we can retrieve from
    // PrintWindow() or BitBlt() function, will be smaller than
    // |original_rect| and |cropped_rect|. Part of the captured desktop frame
    // will be black. See
    // bug https://bugs.chromium.org/p/webrtc/issues/detail?id=8112 for
    // details.

    // If |window_dc_size| is smaller than |window_rect|, let's resize both
    // |original_rect| and |cropped_rect| according to the scaling factor.
    const double vertical_scale =
        static_cast<double>(window_dc_size.width()) / original_rect.width();
    const double horizontal_scale =
        static_cast<double>(window_dc_size.height()) / original_rect.height();
    original_rect.Scale(vertical_scale, horizontal_scale);
    cropped_rect.Scale(vertical_scale, horizontal_scale);
  }

  std::unique_ptr<DesktopFrameWin> frame(
      DesktopFrameWin::Create(original_rect.size(), nullptr, window_dc));
  if (!frame.get()) {
    RTC_LOG(LS_WARNING) << "Failed to create frame.";
    ReleaseDC(window_, window_dc);
    callback_->OnCaptureResult(Result::ERROR_TEMPORARY, nullptr);
    return;
  }

  HDC mem_dc = CreateCompatibleDC(window_dc);
  HGDIOBJ previous_object = SelectObject(mem_dc, frame->bitmap());
  BOOL result = FALSE;

  // When desktop composition (Aero) is enabled each window is rendered to a
  // private buffer allowing BitBlt() to get the window content even if the
  // window is occluded. PrintWindow() is slower but lets rendering the window
  // contents to an off-screen device context when Aero is not available.
  // PrintWindow() is not supported by some applications.
  //
  // If Aero is enabled, we prefer BitBlt() because it's faster and avoids
  // window flickering. Otherwise, we prefer PrintWindow() because BitBlt() may
  // render occluding windows on top of the desired window.
  //
  // When composition is enabled the DC returned by GetWindowDC() doesn't always
  // have window frame rendered correctly. Windows renders it only once and then
  // caches the result between captures. We hack it around by calling
  // PrintWindow() whenever window size changes, including the first time of
  // capturing - it somehow affects what we get from BitBlt() on the subsequent
  // captures.
  //
  // For Windows 8 and later, we want to awlays use PrintWindow when the
  // cropping screen capturer falls back to the window capturer. I.e.
  // on Windows 8 and later, PrintWindow is only used when the window is
  // occluded. When the window is not occluded, it is much faster to capture
  // the screen and to crop it to the window position and size.
  if (rtc::IsWindows8OrLater()) {
    // Special flag that makes PrintWindow to work on Windows 8 and later.
    // otherwise it either fails or it renders black frames.
    const UINT flags = PW_RENDERFULLCONTENT;
    result = PrintWindow(window_, mem_dc, flags);
  } else if (!window_capture_helper_.IsAeroEnabled() ||
             !previous_size_.equals(frame->size())) {
    result = PrintWindow(window_, mem_dc, 0);
  }

  // Data cropped rect is relative to the first pixel of the frame.
  DesktopRect data_cropped_rect = cropped_rect;
  data_cropped_rect.Translate(-original_rect.left(), -original_rect.top());

  // Aero is enabled or PrintWindow() failed, use BitBlt. Do not use BitBlt
  // on Windows 8 or later as it renders black content and this is why we use
  // the cropping screen capturer.
  if (!result) {
    result = BitBlt(mem_dc, 0, 0, frame->size().width(), frame->size().height(),
                    window_dc, data_cropped_rect.left(),
                    data_cropped_rect.top(), SRCCOPY);
  }

  SelectObject(mem_dc, previous_object);
  DeleteDC(mem_dc);
  ReleaseDC(window_, window_dc);

  previous_size_ = frame->size();
  window_size_map_[window_] = previous_size_;

  frame->mutable_updated_region()->SetRect(
      DesktopRect::MakeSize(frame->size()));
  frame->set_top_left(
      original_rect.top_left().subtract(GetFullscreenRect().top_left()));

  if (!result) {
    RTC_LOG(LS_ERROR) << "Both PrintWindow() and BitBlt() failed.";
    callback_->OnCaptureResult(Result::ERROR_TEMPORARY, nullptr);
  }

  std::unique_ptr<DesktopFrame> cropped_frame =
      CreateCroppedDesktopFrame(std::move(frame), data_cropped_rect);
  RTC_DCHECK(cropped_frame);

  callback_->OnCaptureResult(Result::SUCCESS, std::move(cropped_frame));
}

}  // namespace

// static
std::unique_ptr<DesktopCapturer> DesktopCapturer::CreateRawWindowCapturer(
    const DesktopCaptureOptions& options) {
  return std::unique_ptr<DesktopCapturer>(new WindowCapturerWin(
      options.allow_magnification_api_for_window_capture(),
      options.allow_windows_graphics_capturer()));
}

}  // namespace webrtc
