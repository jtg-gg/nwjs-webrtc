/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "windows_graphics_capturer.h"
#include "modules/desktop_capture/win/d3d_device.h"
#include "modules/desktop_capture/win/window_capture_utils.h"
#include "rtc_base/logging.h"

#include "base/strings/string_piece_forward.h"
#include "base/strings/string_piece.h"
#include "base/win/core_winrt_util.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace ABI::Windows::Graphics;
using namespace ABI::Windows::Graphics::DirectX;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Graphics::Capture;

namespace webrtc {

namespace {

FARPROC LoadD3D11Function(const char* function_name) {
  static HMODULE const handle = ::LoadLibrary(L"d3d11.dll");
  return handle ? ::GetProcAddress(handle, function_name) : nullptr;
}

decltype(&::CreateDirect3D11DeviceFromDXGIDevice)
  GetCreateDirect3D11DeviceFromDXGIDeviceFunction() {
  static decltype(&::CreateDirect3D11DeviceFromDXGIDevice) const function =
    reinterpret_cast<decltype(&::CreateDirect3D11DeviceFromDXGIDevice)>(
      LoadD3D11Function("CreateDirect3D11DeviceFromDXGIDevice"));
  return function;
}

HRESULT WrapperCreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice* in,
  IInspectable** out) {
  *out = nullptr;
  auto func = GetCreateDirect3D11DeviceFromDXGIDeviceFunction();
  if (!func)
    return E_FAIL;
  return func(in, out);
}

}  // namespace

bool WindowsGraphicsCapturer::IsSupported() {
  static HRESULT hr = -1;
  static rtc::CriticalSection* lock = nullptr;
  if (!lock) {
    rtc::CriticalSection* temp_lock = new rtc::CriticalSection();
    if (InterlockedCompareExchangePointer(
      reinterpret_cast<PVOID*>(&lock), temp_lock, NULL)) {
      delete temp_lock;
    }
  }

  rtc::CritScope lockScoped(lock);
  if (hr != -1) {
    return SUCCEEDED(hr);
  }

  base::win::ScopedHString graphics_capture_item_guid = base::win::ScopedHString::Create(
    RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem);

  ComPtr<IGraphicsCaptureItemInterop> interop_factory;
  hr = base::win::RoGetActivationFactory(
    graphics_capture_item_guid.get(), IID_PPV_ARGS(&interop_factory));

  return SUCCEEDED(hr);
}

WindowsGraphicsCapturer::WindowsGraphicsCapturer() {}

WindowsGraphicsCapturer::~WindowsGraphicsCapturer() {
  HRESULT hr = frame_pool_->remove_FrameArrived(frame_arrived_token_);
  if (FAILED(hr)) { RTC_LOG(LS_WARNING) << "frame_pool_->remove_FrameArrived fail " << hr; }

  ComPtr<IClosable> closable;
 
  hr = frame_pool_.As(&closable);
  if (FAILED(hr)) { RTC_LOG(LS_WARNING) << "frame_pool_.As(&closable) fail " << hr; }

  hr = closable->Close();
  if (FAILED(hr)) { RTC_LOG(LS_WARNING) << "frame_pool_->Close() fail " << hr; }

  hr = graphics_capture_session_.As(&closable);
  if (FAILED(hr)) { RTC_LOG(LS_WARNING) << "graphics_capture_session_.As(&closable) fail " << hr; }

  hr = closable->Close();
  if (FAILED(hr)) { RTC_LOG(LS_WARNING) << "graphics_capture_session_->Close() fail " << hr; }
}

void WindowsGraphicsCapturer::Start(Callback* callback) {
  callback_ = callback;
  HRESULT hr = graphics_capture_session_->StartCapture();
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "graphics_capture_session_->StartCapture() fail " << hr;
  }
}

void WindowsGraphicsCapturer::CaptureFrame() {
  HRESULT hr;
  ComPtr<Direct3D11::IDirect3DSurface> surface;
  {
    rtc::CritScope lock(&capture_frame_lock_);
    if (!capture_frame_.Get()) {
      callback_->OnCaptureResult(Result::ERROR_TEMPORARY, nullptr);
      return;
    }
    hr = capture_frame_->get_Surface(&surface);
    if (FAILED(hr)) { callback_->OnCaptureResult(Result::ERROR_TEMPORARY, nullptr); return; }
  }

  ComPtr<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> dxgiInterface;
  hr = surface.As(&dxgiInterface);
  if (FAILED(hr)) { callback_->OnCaptureResult(Result::ERROR_TEMPORARY, nullptr); return; }
  
  ComPtr<IDXGIResource> resource;
  hr = dxgiInterface->GetInterface(IID_PPV_ARGS(&resource));
  if (FAILED(hr)) { callback_->OnCaptureResult(Result::ERROR_TEMPORARY, nullptr); return; }

  DXGI_OUTDUPL_FRAME_INFO frame_info;
  frame_info.AccumulatedFrames = 1;
  bool res = dxgi_texture_staging_->CopyFrom(frame_info, resource.Get());
  if (!res) { callback_->OnCaptureResult(Result::ERROR_TEMPORARY, nullptr); return; }

  const DesktopFrame& source = dxgi_texture_staging_->AsDesktopFrame();
  if (!source.size().equals(output_->size())) {
    output_.reset(SharedDesktopFrame::Wrap(new BasicDesktopFrame(source.size())));
  }
  output_->CopyPixelsFrom(source, source.top_left(), source.rect());

  res = dxgi_texture_staging_->Release();
  if (!res) { RTC_LOG(LS_ERROR) << "dxgi_texture_staging_->Release() fail"; }

  // cheat so the desktop_and_cursor_composer does not draw cursor
  output_->set_top_left(DesktopVector(output_->size().height(), output_->size().width()));
  callback_->OnCaptureResult(Result::SUCCESS, output_->Share());
}

bool WindowsGraphicsCapturer::GetSourceList(SourceList* sources) {
  return false;
}

bool WindowsGraphicsCapturer::SelectSource(SourceId id) {
  HRESULT hr;
  HWND window = reinterpret_cast<HWND>(id);

  base::win::ScopedHString graphics_capture_item_guid = base::win::ScopedHString::Create(
    RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem);

  ComPtr<IGraphicsCaptureItemInterop> interop_factory;
  hr = base::win::RoGetActivationFactory(
    graphics_capture_item_guid.get(), IID_PPV_ARGS(&interop_factory));
  if (FAILED(hr)) return false;

  ComPtr<IGraphicsCaptureItem> graphics_capture_item;
  hr = interop_factory->CreateForWindow(window, IID_IGraphicsCaptureItem, &graphics_capture_item);
  if (FAILED(hr)) return false;

  hr = graphics_capture_item->get_Size(&last_size_);
  if (FAILED(hr)) return false;

  output_.reset(SharedDesktopFrame::Wrap(new BasicDesktopFrame(DesktopSize(last_size_.Width, last_size_.Height))));

  std::vector<D3dDevice> rtcD3dDevices = D3dDevice::EnumDevices(D3D11_CREATE_DEVICE_BGRA_SUPPORT);
  ComPtr<ID3D11Device> d3d11Device(rtcD3dDevices[0].d3d_device());

  ComPtr<IDXGIDevice> dxgiDevice;
  hr = d3d11Device.As(&dxgiDevice);
  if (FAILED(hr)) return false;

  ComPtr<IInspectable> inspectable;
  hr = WrapperCreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), &inspectable);
  if (FAILED(hr)) return false;

  hr = inspectable.As(&d3d_device_);
  if (FAILED(hr)) return false;

  dxgi_texture_staging_ = std::make_unique<DxgiTextureStaging>(rtcD3dDevices[0]);

  base::win::ScopedHString capture_frame_pool_guid = base::win::ScopedHString::Create(
    RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool);

  ComPtr<IDirect3D11CaptureFramePoolStatics2> frame_pool_factory;
  hr = base::win::RoGetActivationFactory(capture_frame_pool_guid.get(), IID_PPV_ARGS(&frame_pool_factory));
  if (FAILED(hr)) return false;

  hr = frame_pool_factory->CreateFreeThreaded(d3d_device_.Get(), DirectXPixelFormat::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, last_size_, &frame_pool_);
  if (FAILED(hr)) return false;

  hr = frame_pool_->CreateCaptureSession(graphics_capture_item.Get(), &graphics_capture_session_);
  if (FAILED(hr)) return false;

  typedef __FITypedEventHandler_2_Windows__CGraphics__CCapture__CDirect3D11CaptureFramePool_IInspectable FrameArrivedHandler;
  auto frameArrivedHandler = Microsoft::WRL::Callback<Implements<RuntimeClassFlags<ClassicCom>, FrameArrivedHandler, FtmBase>>(
    [this](IDirect3D11CaptureFramePool* frame_pool, IInspectable*)
  {
    rtc::CritScope lock(&this->capture_frame_lock_);
    HRESULT hr = frame_pool->TryGetNextFrame(&this->capture_frame_);
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "frame_pool->TryGetNextFrame fail " << hr;
      return hr;
    }

    SizeInt32 size;
    hr = capture_frame_->get_ContentSize(&size);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "capture_frame_->get_ContentSize(&size) fail " << hr;
      return hr;
    }
    
    if (size.Width != last_size_.Width || size.Height != last_size_.Height) {
      hr = frame_pool_->Recreate(d3d_device_.Get(), DirectXPixelFormat::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, size);
      if (FAILED(hr)) {
        RTC_LOG(LS_WARNING) << "frame_pool_->Recreate() fail " << hr;
      }
      else {
        last_size_ = size;
      }
    }
    return hr;
  });
  
  hr = frame_pool_->add_FrameArrived(frameArrivedHandler.Get(), &frame_arrived_token_);
  if (FAILED(hr)) return false;

  return SUCCEEDED(hr);
}

bool WindowsGraphicsCapturer::FocusOnSelectedSource() {
  return false;
}

bool WindowsGraphicsCapturer::IsOccluded(const DesktopVector& pos) {
  return false;
}

}  // namespace webrtc
