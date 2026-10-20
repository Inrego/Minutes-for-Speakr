#ifndef RUNNER_VIDEO_CAPTURE_H_
#define RUNNER_VIDEO_CAPTURE_H_

#include <windows.h>

#include <d3d11.h>
#include <windows.graphics.capture.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Captures one top-level window via Windows.Graphics.Capture and keeps
// the latest frame scaled/letterboxed into a fixed-size NV12 texture,
// ready for the H.264 stream of `SpeakrAudioRecorder`'s sink writer.
//
// Written against the raw WinRT ABI with WRL rather than C++/WinRT:
// the runner builds with `_HAS_EXCEPTIONS=0`, and C++/WinRT reports
// failures by throwing.
//
// Threading: frames arrive on a free-threaded frame pool's worker
// thread; `ReadLatestFrame` is called from the recorder's mux thread.
// Both touch the D3D11 immediate context, so every context call is
// serialised through `ctx_mu_`.
class VideoCapture {
 public:
  struct WindowInfo {
    int64_t hwnd;
    DWORD pid;
    std::string title;  // UTF-8
    int width;
    int height;
  };

  static constexpr UINT kWidth = 1920;
  static constexpr UINT kHeight = 1080;
  static constexpr UINT kFps = 15;
  static constexpr UINT kBitrate = 1500000;  // ~0.7 GB per hour

  // True when Windows.Graphics.Capture is available (Windows 10 1903+).
  static bool IsSupported();

  // Visible, titled, non-tool top-level windows in Z order (front-most
  // first). Empty `pids` means windows of every process except ours.
  static std::vector<WindowInfo> FindWindows(const std::vector<DWORD>& pids);

  VideoCapture() = default;
  ~VideoCapture();

  VideoCapture(const VideoCapture&) = delete;
  VideoCapture& operator=(const VideoCapture&) = delete;

  // Creates the D3D11 device and NV12 target. Must run on an MTA thread.
  HRESULT Init();

  // Start capturing `hwnd`, replacing any current target. `hwnd == 0`
  // stops capturing and the output turns black.
  HRESULT SetTarget(HWND hwnd);

  // Copy the latest frame as tightly packed NV12 (kWidth*kHeight*3/2
  // bytes) into `dst`.
  HRESULT ReadLatestFrame(BYTE* dst);

  void Shutdown();

 private:
  HRESULT OnFrameArrived();
  HRESULT EnsureProcessor(UINT in_w, UINT in_h);
  void StopSession();
  void ClearTarget();

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
  Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
  Microsoft::WRL::ComPtr<
      ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>
      winrt_device_;

  // Fixed-size NV12 output, plus its CPU-readable twin.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view_;

  // Rebuilt whenever the captured window changes size.
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vp_enum_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> vp_;
  UINT vp_in_w_ = 0;
  UINT vp_in_h_ = 0;

  Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>
      item_;
  Microsoft::WRL::ComPtr<
      ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool>
      pool_;
  Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Capture::IGraphicsCaptureSession>
      session_;
  EventRegistrationToken frame_token_{};
  ABI::Windows::Graphics::SizeInt32 pool_size_{};

  std::mutex ctx_mu_;
  // Guards item_/pool_/session_ against a target swap racing a frame.
  std::mutex session_mu_;
};

#endif  // RUNNER_VIDEO_CAPTURE_H_
