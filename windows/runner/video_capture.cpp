#include "video_capture.h"

#include <dwmapi.h>
#include <dxgi.h>
#include <roapi.h>
#include <windows.foundation.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>
#include <Windows.Graphics.Capture.Interop.h>

#include <algorithm>
#include <cstring>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;

namespace wgc = ABI::Windows::Graphics::Capture;
namespace wgd = ABI::Windows::Graphics::DirectX;
namespace wf = ABI::Windows::Foundation;

namespace {

using FrameArrivedHandler =
    wf::ITypedEventHandler<wgc::Direct3D11CaptureFramePool*, IInspectable*>;
// A free-threaded frame pool calls the handler from arbitrary threads
// and rejects non-agile delegates (RO_E_MUST_BE_AGILE), so mix in the
// free-threaded marshaler.
using AgileFrameArrivedHandler = Microsoft::WRL::Implements<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    FrameArrivedHandler, Microsoft::WRL::FtmBase>;

std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                  static_cast<int>(w.size()), nullptr, 0,
                                  nullptr, nullptr);
  std::string s(len, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), len, nullptr, nullptr);
  return s;
}

template <typename T>
void CloseWinRt(const ComPtr<T>& obj) {
  if (!obj) return;
  ComPtr<wf::IClosable> closable;
  if (SUCCEEDED(obj.As(&closable))) closable->Close();
}

struct EnumState {
  const std::vector<DWORD>* pids;
  std::vector<VideoCapture::WindowInfo>* out;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lparam) {
  auto* state = reinterpret_cast<EnumState*>(lparam);
  if (!::IsWindowVisible(hwnd) || ::IsIconic(hwnd)) return TRUE;
  if (::GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
  DWORD pid = 0;
  ::GetWindowThreadProcessId(hwnd, &pid);
  if (pid == ::GetCurrentProcessId()) return TRUE;
  if (!state->pids->empty() &&
      std::find(state->pids->begin(), state->pids->end(), pid) ==
          state->pids->end()) {
    return TRUE;
  }
  // UWP/suspended windows and windows on other virtual desktops are
  // "cloaked": visible per Win32 but never painted.
  DWORD cloaked = 0;
  if (SUCCEEDED(::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked))) &&
      cloaked) {
    return TRUE;
  }
  RECT rc;
  if (!::GetWindowRect(hwnd, &rc)) return TRUE;
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w < 200 || h < 150) return TRUE;
  const int len = ::GetWindowTextLengthW(hwnd);
  if (len <= 0) return TRUE;
  std::wstring title(static_cast<size_t>(len) + 1, L'\0');
  const int got = ::GetWindowTextW(hwnd, title.data(), len + 1);
  title.resize(static_cast<size_t>(std::max(got, 0)));
  if (title.empty()) return TRUE;
  state->out->push_back({reinterpret_cast<int64_t>(hwnd), pid,
                         WideToUtf8(title), w, h});
  return TRUE;
}

}  // namespace

bool VideoCapture::IsSupported() {
  ComPtr<wgc::IGraphicsCaptureSessionStatics> statics;
  HRESULT hr = ::RoGetActivationFactory(
      HStringReference(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession)
          .Get(),
      IID_PPV_ARGS(&statics));
  if (FAILED(hr)) return false;
  boolean supported = false;
  if (FAILED(statics->IsSupported(&supported))) return false;
  // CreateFreeThreaded arrived in 1903, alongside the Statics2 interface.
  ComPtr<wgc::IDirect3D11CaptureFramePoolStatics2> pool_statics;
  hr = ::RoGetActivationFactory(
      HStringReference(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool)
          .Get(),
      IID_PPV_ARGS(&pool_statics));
  return supported && SUCCEEDED(hr);
}

std::vector<VideoCapture::WindowInfo> VideoCapture::FindWindows(
    const std::vector<DWORD>& pids) {
  std::vector<WindowInfo> out;
  EnumState state{&pids, &out};
  ::EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&state));
  return out;
}

VideoCapture::~VideoCapture() { Shutdown(); }

HRESULT VideoCapture::Init() {
  HRESULT hr = ::D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
      nullptr, 0, D3D11_SDK_VERSION, &device_, nullptr, &context_);
  if (FAILED(hr)) return hr;
  hr = device_.As(&video_device_);
  if (FAILED(hr)) return hr;
  hr = context_.As(&video_context_);
  if (FAILED(hr)) return hr;

  ComPtr<IDXGIDevice> dxgi;
  hr = device_.As(&dxgi);
  if (FAILED(hr)) return hr;
  ComPtr<IInspectable> inspectable;
  hr = ::CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), &inspectable);
  if (FAILED(hr)) return hr;
  hr = inspectable.As(&winrt_device_);
  if (FAILED(hr)) return hr;

  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = kWidth;
  desc.Height = kHeight;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  hr = device_->CreateTexture2D(&desc, nullptr, &nv12_);
  if (FAILED(hr)) return hr;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  hr = device_->CreateTexture2D(&desc, nullptr, &staging_);
  if (FAILED(hr)) return hr;

  // The output view needs an enumerator; any input size works for that.
  hr = EnsureProcessor(kWidth, kHeight);
  if (FAILED(hr)) return hr;
  ClearTarget();
  return S_OK;
}

HRESULT VideoCapture::EnsureProcessor(UINT in_w, UINT in_h) {
  if (vp_ && in_w == vp_in_w_ && in_h == vp_in_h_) return S_OK;
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
  cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  cd.InputWidth = in_w;
  cd.InputHeight = in_h;
  cd.OutputWidth = kWidth;
  cd.OutputHeight = kHeight;
  cd.InputFrameRate = {kFps, 1};
  cd.OutputFrameRate = {kFps, 1};
  cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  ComPtr<ID3D11VideoProcessorEnumerator> en;
  HRESULT hr = video_device_->CreateVideoProcessorEnumerator(&cd, &en);
  if (FAILED(hr)) return hr;
  ComPtr<ID3D11VideoProcessor> vp;
  hr = video_device_->CreateVideoProcessor(en.Get(), 0, &vp);
  if (FAILED(hr)) return hr;
  if (!output_view_) {
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    hr = video_device_->CreateVideoProcessorOutputView(nv12_.Get(), en.Get(),
                                                       &ovd, &output_view_);
    if (FAILED(hr)) return hr;
  }
  // Letterbox colour: video-range black.
  D3D11_VIDEO_COLOR black = {};
  black.YCbCr = {0.0625f, 0.5f, 0.5f, 1.0f};
  video_context_->VideoProcessorSetOutputBackgroundColor(vp.Get(), TRUE,
                                                         &black);
  vp_enum_ = en;
  vp_ = vp;
  vp_in_w_ = in_w;
  vp_in_h_ = in_h;
  return S_OK;
}

void VideoCapture::ClearTarget() {
  // Video-range black: Y=16, Cb=Cr=128, cleared plane by plane.
  std::lock_guard<std::mutex> lock(ctx_mu_);
  if (!nv12_) return;
  D3D11_RENDER_TARGET_VIEW_DESC rd = {};
  rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  ComPtr<ID3D11RenderTargetView> luma;
  rd.Format = DXGI_FORMAT_R8_UNORM;
  if (SUCCEEDED(device_->CreateRenderTargetView(nv12_.Get(), &rd, &luma))) {
    const float y[4] = {16.0f / 255.0f, 0, 0, 0};
    context_->ClearRenderTargetView(luma.Get(), y);
  }
  ComPtr<ID3D11RenderTargetView> chroma;
  rd.Format = DXGI_FORMAT_R8G8_UNORM;
  if (SUCCEEDED(device_->CreateRenderTargetView(nv12_.Get(), &rd, &chroma))) {
    const float uv[4] = {128.0f / 255.0f, 128.0f / 255.0f, 0, 0};
    context_->ClearRenderTargetView(chroma.Get(), uv);
  }
}

HRESULT VideoCapture::SetTarget(HWND hwnd) {
  ComPtr<wgc::IGraphicsCaptureItem> item;
  ComPtr<wgc::IDirect3D11CaptureFramePool> pool;
  ComPtr<wgc::IGraphicsCaptureSession> session;
  EventRegistrationToken token{};
  ABI::Windows::Graphics::SizeInt32 size{};

  if (hwnd) {
    ComPtr<IGraphicsCaptureItemInterop> interop;
    HRESULT hr = ::RoGetActivationFactory(
        HStringReference(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem)
            .Get(),
        IID_PPV_ARGS(&interop));
    if (FAILED(hr)) return hr;
    hr = interop->CreateForWindow(hwnd, IID_PPV_ARGS(&item));
    if (FAILED(hr)) return hr;
    hr = item->get_Size(&size);
    if (FAILED(hr)) return hr;

    ComPtr<wgc::IDirect3D11CaptureFramePoolStatics2> statics;
    hr = ::RoGetActivationFactory(
        HStringReference(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool)
            .Get(),
        IID_PPV_ARGS(&statics));
    if (FAILED(hr)) return hr;
    // Free-threaded: FrameArrived fires on a system thread, so the
    // recorder needs no dispatcher queue.
    hr = statics->CreateFreeThreaded(
        winrt_device_.Get(),
        wgd::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, size, &pool);
    if (FAILED(hr)) return hr;
    hr = pool->add_FrameArrived(
        Callback<AgileFrameArrivedHandler>(
            [this](wgc::IDirect3D11CaptureFramePool*, IInspectable*)
                -> HRESULT {
              OnFrameArrived();
              return S_OK;
            })
            .Get(),
        &token);
    if (FAILED(hr)) return hr;
    hr = pool->CreateCaptureSession(item.Get(), &session);
    if (FAILED(hr)) return hr;

    // Optional niceties; older builds lack these interfaces.
    ComPtr<wgc::IGraphicsCaptureSession2> session2;
    if (SUCCEEDED(session.As(&session2))) {
      session2->put_IsCursorCaptureEnabled(false);
    }
    ComPtr<wgc::IGraphicsCaptureSession3> session3;
    if (SUCCEEDED(session.As(&session3))) {
      session3->put_IsBorderRequired(false);
    }
  }

  // Swap under the lock, tear the old session down outside it: closing
  // a pool can wait for an in-flight FrameArrived, which takes the lock.
  ComPtr<wgc::IDirect3D11CaptureFramePool> old_pool;
  ComPtr<wgc::IGraphicsCaptureSession> old_session;
  EventRegistrationToken old_token{};
  {
    std::lock_guard<std::mutex> lock(session_mu_);
    old_pool = pool_;
    old_session = session_;
    old_token = frame_token_;
    item_ = item;
    pool_ = pool;
    session_ = session;
    frame_token_ = token;
    pool_size_ = size;
  }
  if (old_pool) old_pool->remove_FrameArrived(old_token);
  CloseWinRt(old_session);
  CloseWinRt(old_pool);

  if (!hwnd) {
    ClearTarget();
    return S_OK;
  }
  return session->StartCapture();
}

HRESULT VideoCapture::OnFrameArrived() {
  ComPtr<wgc::IDirect3D11CaptureFramePool> pool;
  ABI::Windows::Graphics::SizeInt32 pool_size{};
  {
    std::lock_guard<std::mutex> lock(session_mu_);
    pool = pool_;
    pool_size = pool_size_;
  }
  if (!pool) return S_OK;

  ComPtr<wgc::IDirect3D11CaptureFrame> frame;
  HRESULT hr = pool->TryGetNextFrame(&frame);
  if (FAILED(hr) || !frame) return hr;

  ABI::Windows::Graphics::SizeInt32 content{};
  frame->get_ContentSize(&content);
  if (content.Width <= 0 || content.Height <= 0) {
    CloseWinRt(frame);
    return S_OK;
  }

  ComPtr<wgd::Direct3D11::IDirect3DSurface> surface;
  hr = frame->get_Surface(&surface);
  ComPtr<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>
      access;
  if (SUCCEEDED(hr)) hr = surface.As(&access);
  ComPtr<ID3D11Texture2D> tex;
  if (SUCCEEDED(hr)) hr = access->GetInterface(IID_PPV_ARGS(&tex));

  if (SUCCEEDED(hr)) {
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    // The pool surface stays at the size it was created with; only the
    // top-left ContentSize region holds the window.
    const UINT cw = std::min<UINT>(static_cast<UINT>(content.Width), td.Width);
    const UINT ch =
        std::min<UINT>(static_cast<UINT>(content.Height), td.Height);

    std::lock_guard<std::mutex> lock(ctx_mu_);
    hr = EnsureProcessor(td.Width, td.Height);
    ComPtr<ID3D11VideoProcessorInputView> in_view;
    if (SUCCEEDED(hr)) {
      D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
      ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
      hr = video_device_->CreateVideoProcessorInputView(
          tex.Get(), vp_enum_.Get(), &ivd, &in_view);
    }
    if (SUCCEEDED(hr)) {
      RECT src = {0, 0, static_cast<LONG>(cw), static_cast<LONG>(ch)};
      // Aspect-fit into the output frame; the rest is background.
      const double scale = std::min(static_cast<double>(kWidth) / cw,
                                    static_cast<double>(kHeight) / ch);
      const LONG dw = static_cast<LONG>(cw * scale) & ~1L;
      const LONG dh = static_cast<LONG>(ch * scale) & ~1L;
      const LONG dx = ((static_cast<LONG>(kWidth) - dw) / 2) & ~1L;
      const LONG dy = ((static_cast<LONG>(kHeight) - dh) / 2) & ~1L;
      RECT dst = {dx, dy, dx + dw, dy + dh};
      RECT full = {0, 0, static_cast<LONG>(kWidth),
                   static_cast<LONG>(kHeight)};
      video_context_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE,
                                                        &src);
      video_context_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE,
                                                      &dst);
      video_context_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE,
                                                        &full);
      video_context_->VideoProcessorSetStreamFrameFormat(
          vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
      D3D11_VIDEO_PROCESSOR_STREAM stream = {};
      stream.Enable = TRUE;
      stream.pInputSurface = in_view.Get();
      hr = video_context_->VideoProcessorBlt(vp_.Get(), output_view_.Get(), 0,
                                             1, &stream);
    }
  }
  CloseWinRt(frame);

  // Window resized: rebuild the pool's buffers at the new size so the
  // next frames aren't cropped.
  if (content.Width != pool_size.Width || content.Height != pool_size.Height) {
    pool->Recreate(winrt_device_.Get(),
                   wgd::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, content);
    std::lock_guard<std::mutex> lock(session_mu_);
    if (pool_.Get() == pool.Get()) pool_size_ = content;
  }
  return hr;
}

HRESULT VideoCapture::ReadLatestFrame(BYTE* dst) {
  std::lock_guard<std::mutex> lock(ctx_mu_);
  if (!nv12_ || !staging_) return E_NOT_VALID_STATE;
  context_->CopyResource(staging_.Get(), nv12_.Get());
  D3D11_MAPPED_SUBRESOURCE map;
  HRESULT hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map);
  if (FAILED(hr)) return hr;
  // NV12 in one subresource: kHeight luma rows, then kHeight/2 rows of
  // interleaved chroma, both at `RowPitch`.
  const BYTE* src = static_cast<const BYTE*>(map.pData);
  for (UINT row = 0; row < kHeight + kHeight / 2; ++row) {
    std::memcpy(dst + static_cast<size_t>(row) * kWidth,
                src + static_cast<size_t>(row) * map.RowPitch, kWidth);
  }
  context_->Unmap(staging_.Get(), 0);
  return S_OK;
}

void VideoCapture::StopSession() { SetTarget(nullptr); }

void VideoCapture::Shutdown() {
  StopSession();
  std::lock_guard<std::mutex> lock(ctx_mu_);
  output_view_.Reset();
  vp_.Reset();
  vp_enum_.Reset();
  staging_.Reset();
  nv12_.Reset();
  winrt_device_.Reset();
  video_context_.Reset();
  video_device_.Reset();
  context_.Reset();
  device_.Reset();
}
