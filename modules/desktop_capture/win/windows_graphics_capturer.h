/*
*  Copyright(c) 2019 The WebRTC project authors.All Rights Reserved.
*
*  Use of this source code is governed by a BSD - style license
*  that can be found in the LICENSE file in the root of the source
*  tree.An additional intellectual property rights grant can be found
*  in the file PATENTS.All contributing project authors may
*  be found in the AUTHORS file in the root of the source tree.
*/

#ifndef MODULES_DESKTOP_CAPTURE_WIN_WINDOWS_GRAPHICS_CAPTURER_H_
#define MODULES_DESKTOP_CAPTURE_WIN_WINDOWS_GRAPHICS_CAPTURER_H_

#include <memory>

#include "rtc_base/critical_section.h"
#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "modules/desktop_capture/desktop_geometry.h"
#include "modules/desktop_capture/shared_desktop_frame.h"
#include "modules/desktop_capture/win/dxgi_texture_staging.h"

#include <wrl.h>
#include <windows.graphics.capture.h>

namespace webrtc {

class WindowsGraphicsCapturer : public DesktopCapturer {
public:
  WindowsGraphicsCapturer();
  ~WindowsGraphicsCapturer() override;

  // DesktopCapturer interface.
  void Start(Callback* callback) override;
  void CaptureFrame() override;
  bool GetSourceList(SourceList* sources) override;
  bool SelectSource(SourceId id) override;
  bool FocusOnSelectedSource() override;
  bool IsOccluded(const DesktopVector& pos) override;

  static bool IsSupported();

private:
  Callback* callback_ = nullptr;

  Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool> frame_pool_;
  Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Capture::IGraphicsCaptureSession> graphics_capture_session_;
  Microsoft::WRL::ComPtr<ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice> d3d_device_;
  Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame> capture_frame_;
  ABI::Windows::Graphics::SizeInt32 last_size_;
  EventRegistrationToken frame_arrived_token_;
  std::unique_ptr<DxgiTextureStaging> dxgi_texture_staging_;
  std::unique_ptr<SharedDesktopFrame> output_;
  rtc::CriticalSection capture_frame_lock_;

  RTC_DISALLOW_COPY_AND_ASSIGN(WindowsGraphicsCapturer);
};

}  // namespace webrtc

#endif // MODULES_DESKTOP_CAPTURE_WIN_WINDOWS_GRAPHICS_CAPTURER_H_
