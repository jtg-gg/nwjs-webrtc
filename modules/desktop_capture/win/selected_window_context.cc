/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/desktop_capture/win/selected_window_context.h"

namespace webrtc {

SelectedWindowContext::SelectedWindowContext(
    HWND selected_window,
    DesktopRect selected_window_rect,
    WindowCaptureHelperWin* window_capture_helper)
    : selected_window_(selected_window),
      selected_window_rect_(selected_window_rect),
      window_capture_helper_(window_capture_helper) {
  selected_window_thread_id_ =
      GetWindowThreadProcessId(selected_window, &selected_window_process_id_);
}

bool SelectedWindowContext::IsSelectedWindowValid() const {
  return selected_window_thread_id_ != 0;
}

bool SelectedWindowContext::IsWindowSelected(HWND hwnd) const {
  return hwnd == selected_window_;
}

bool SelectedWindowContext::IsWindowOwned(HWND hwnd) const {
  // This check works for drop-down menus & dialog pop-up windows. It doesn't
  // work for context menus or tooltips, which are handled differently below.
  if (GetAncestor(hwnd, GA_ROOTOWNER) == selected_window_) {
    return true;
  }

  // Some pop-up windows aren't owned (e.g. context menus, tooltips); treat
  // windows that belong to the same thread as owned.
  DWORD enumerated_window_process_id = 0;
  DWORD enumerated_window_thread_id =
      GetWindowThreadProcessId(hwnd, &enumerated_window_process_id);
  return enumerated_window_thread_id != 0 &&
         enumerated_window_process_id == selected_window_process_id_ &&
         enumerated_window_thread_id == selected_window_thread_id_;
}

bool SelectedWindowContext::IsUWPAncestor(HWND hwnd) const {
  // Xaml_WindowedPopupClass has "PopupHost" title, and different process id
  // hence we need to iterate using GetParent to confirm the ancestry
  HWND it = hwnd;
  while (it != NULL) {
    it = GetParent(it);
    if (it == selected_window_) {
      // we don't won't to capture child window that has titlebar / WS_CAPTION 
      return !(GetWindowLongPtr(hwnd, GWL_STYLE) & WS_CAPTION);
    }
  }
  return FALSE;
}

bool SelectedWindowContext::IsWindowOverlapping(HWND hwnd) const {
  return window_capture_helper_->IsWindowIntersectWithSelectedWindow(
      hwnd, selected_window_, selected_window_rect_);
}

WindowCaptureHelperWin* SelectedWindowContext::window_capture_helper() const {
  return window_capture_helper_;
}

}  // namespace webrtc
