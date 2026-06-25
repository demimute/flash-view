#include "viewer/render/d3d_renderer.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11_1.h>
#include <d3d9types.h>
#include <d3dumddi.h>
#include <dwrite.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "viewer/render/d3d_error_policy.h"
#include "viewer/render/render_math.h"

namespace viewer::render {
namespace {

using Microsoft::WRL::ComPtr;

const D2D1_COLOR_F clear_color = D2D1::ColorF(0x15171A, 1.0F);

core::Result<bool> platform_failure(const wchar_t* message) {
  return core::Result<bool>::failure(
      {core::ErrorCode::platform_error, message});
}

core::Result<bool> render_target_lost(const wchar_t* message) {
  return core::Result<bool>::failure(
      {core::ErrorCode::render_target_lost, message});
}

GraphicsOutcome classify_hresult(HRESULT result) noexcept {
  if (SUCCEEDED(result)) {
    return GraphicsOutcome::success;
  }
  if (result == D2DERR_RECREATE_TARGET) {
    return GraphicsOutcome::recreate_target;
  }
  if (result == DXGI_ERROR_DEVICE_REMOVED ||
      result == D3DDDIERR_DEVICEREMOVED) {
    return GraphicsOutcome::device_removed;
  }
  if (result == DXGI_ERROR_DEVICE_RESET) {
    return GraphicsOutcome::device_reset;
  }
  if (result == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
    return GraphicsOutcome::driver_internal_error;
  }
  return GraphicsOutcome::other_failure;
}

GraphicsOutcome classify_present_hresult(HRESULT result) noexcept {
  if (result == DXGI_STATUS_OCCLUDED) {
    return GraphicsOutcome::occluded;
  }
  return classify_hresult(result);
}

bool is_device_lost(HRESULT result) noexcept {
  const GraphicsOutcome outcome = classify_hresult(result);
  return outcome == GraphicsOutcome::device_removed ||
         outcome == GraphicsOutcome::device_reset ||
         outcome == GraphicsOutcome::driver_internal_error;
}

bool is_render_target_lost(HRESULT result) noexcept {
  return classify_draw_outcome(classify_hresult(result)) ==
         DrawAction::render_target_lost;
}

float window_dpi(HWND window) noexcept {
  using GetDpiForWindowFunction = UINT(WINAPI*)(HWND);
  const HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 == nullptr) {
    return 96.0F;
  }

  const auto get_dpi_for_window = reinterpret_cast<GetDpiForWindowFunction>(
      GetProcAddress(user32, "GetDpiForWindow"));
  if (get_dpi_for_window == nullptr) {
    return 96.0F;
  }

  const UINT dpi = get_dpi_for_window(window);
  return dpi == 0 ? 96.0F : static_cast<float>(dpi);
}

bool frame_is_valid(const core::ImageFrame& frame) noexcept {
  constexpr std::uint64_t bytes_per_pixel = 4;
  if (frame.width == 0 || frame.height == 0) {
    return false;
  }

  const std::uint64_t minimum_stride =
      static_cast<std::uint64_t>(frame.width) * bytes_per_pixel;
  if (frame.stride < minimum_stride) {
    return false;
  }

  const std::uint64_t required_bytes =
      static_cast<std::uint64_t>(frame.stride) * frame.height;
  return required_bytes <= (std::numeric_limits<std::size_t>::max)() &&
         frame.pixels.size() == static_cast<std::size_t>(required_bytes);
}

HRESULT create_d3d_device(D3D_DRIVER_TYPE driver_type,
                          UINT flags,
                          ID3D11Device** device,
                          ID3D11DeviceContext** context) {
  constexpr std::array feature_levels{
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };
  D3D_FEATURE_LEVEL selected_level{};

  HRESULT result = D3D11CreateDevice(
      nullptr, driver_type, nullptr, flags, feature_levels.data(),
      static_cast<UINT>(feature_levels.size()), D3D11_SDK_VERSION, device,
      &selected_level, context);
  if (result != E_INVALIDARG) {
    return result;
  }

  return D3D11CreateDevice(
      nullptr, driver_type, nullptr, flags,
      feature_levels.data() + 1,
      static_cast<UINT>(feature_levels.size() - 1), D3D11_SDK_VERSION, device,
      &selected_level, context);
}

}  // namespace

struct D3dRenderer::Impl {
  HWND window = nullptr;
  bool lost = false;
  ComPtr<ID3D11Device> d3d_device;
  ComPtr<ID3D11DeviceContext> d3d_context;
  ComPtr<IDXGISwapChain1> swap_chain;
  ComPtr<ID2D1Factory1> d2d_factory;
  ComPtr<ID2D1Device> d2d_device;
  ComPtr<ID2D1DeviceContext> d2d_context;
  ComPtr<ID2D1Bitmap1> target;
  ComPtr<ID2D1Bitmap1> image;
  ComPtr<IDWriteFactory> dwrite_factory;
  ComPtr<IDWriteTextFormat> status_text_format;
  ComPtr<ID2D1SolidColorBrush> status_text_brush;
  std::wstring status_text;

  void mark_lost() noexcept {
    lost = true;
    if (d2d_context) {
      d2d_context->SetTarget(nullptr);
    }
    image.Reset();
    target.Reset();
    swap_chain.Reset();
    status_text_brush.Reset();
    status_text_format.Reset();
    dwrite_factory.Reset();
    d2d_context.Reset();
    d2d_device.Reset();
    d2d_factory.Reset();
    d3d_context.Reset();
    d3d_device.Reset();
    window = nullptr;
  }

  HRESULT create_target() {
    ComPtr<IDXGISurface> surface;
    HRESULT result =
        swap_chain->GetBuffer(0, IID_PPV_ARGS(surface.ReleaseAndGetAddressOf()));
    if (FAILED(result)) {
      return result;
    }

    const float dpi = window_dpi(window);
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_IGNORE),
        dpi, dpi);
    result = d2d_context->CreateBitmapFromDxgiSurface(
        surface.Get(), &properties, target.ReleaseAndGetAddressOf());
    if (SUCCEEDED(result)) {
      d2d_context->SetDpi(dpi, dpi);
      d2d_context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
      d2d_context->SetTarget(target.Get());
    }
    return result;
  }
};

D3dRenderer::D3dRenderer() : impl_(std::make_unique<Impl>()) {}
D3dRenderer::~D3dRenderer() = default;
D3dRenderer::D3dRenderer(D3dRenderer&&) noexcept = default;
D3dRenderer& D3dRenderer::operator=(D3dRenderer&&) noexcept = default;

core::Result<bool> D3dRenderer::initialize(HWND window) {
  if (window == nullptr) {
    return platform_failure(L"Renderer initialization requires a window.");
  }

  const std::wstring existing_status_text =
      impl_ ? impl_->status_text : std::wstring{};
  impl_ = std::make_unique<Impl>();
  impl_->window = window;
  impl_->status_text = existing_status_text;

  UINT device_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
  device_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  HRESULT result = create_d3d_device(
      D3D_DRIVER_TYPE_HARDWARE, device_flags,
      impl_->d3d_device.ReleaseAndGetAddressOf(),
      impl_->d3d_context.ReleaseAndGetAddressOf());
#if defined(_DEBUG)
  if (FAILED(result)) {
    impl_->d3d_device.Reset();
    impl_->d3d_context.Reset();
    device_flags &= ~D3D11_CREATE_DEVICE_DEBUG;
    result = create_d3d_device(
        D3D_DRIVER_TYPE_HARDWARE, device_flags,
        impl_->d3d_device.ReleaseAndGetAddressOf(),
        impl_->d3d_context.ReleaseAndGetAddressOf());
  }
#endif
  if (FAILED(result)) {
    switch (classify_device_creation_failure(
        classify_hresult(result), RendererDriver::hardware)) {
      case DeviceCreationAction::try_warp:
        impl_->d3d_device.Reset();
        impl_->d3d_context.Reset();
        result = create_d3d_device(
            D3D_DRIVER_TYPE_WARP, device_flags,
            impl_->d3d_device.ReleaseAndGetAddressOf(),
            impl_->d3d_context.ReleaseAndGetAddressOf());
        break;

      case DeviceCreationAction::render_target_lost:
        impl_->mark_lost();
        return render_target_lost(
            L"Direct3D device was lost during renderer initialization.");

      case DeviceCreationAction::platform_error:
      case DeviceCreationAction::use_created_device:
        return platform_failure(L"Direct3D device creation failed.");
    }
  }
  if (FAILED(result)) {
    switch (classify_device_creation_failure(
        classify_hresult(result), RendererDriver::warp)) {
      case DeviceCreationAction::render_target_lost:
        impl_->mark_lost();
        return render_target_lost(
            L"Direct3D WARP device was lost during renderer initialization.");

      case DeviceCreationAction::platform_error:
      case DeviceCreationAction::try_warp:
      case DeviceCreationAction::use_created_device:
        return platform_failure(L"Direct3D device creation failed.");
    }
  }

  ComPtr<IDXGIDevice> dxgi_device;
  result = impl_->d3d_device.As(&dxgi_device);
  if (FAILED(result)) {
    return platform_failure(L"DXGI device query failed.");
  }

  D2D1_FACTORY_OPTIONS factory_options{};
  result = D2D1CreateFactory(
      D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
      &factory_options,
      reinterpret_cast<void**>(
          impl_->d2d_factory.ReleaseAndGetAddressOf()));
  if (FAILED(result)) {
    return platform_failure(L"Direct2D factory creation failed.");
  }

  result = impl_->d2d_factory->CreateDevice(
      dxgi_device.Get(), impl_->d2d_device.ReleaseAndGetAddressOf());
  if (FAILED(result)) {
    return platform_failure(L"Direct2D device creation failed.");
  }

  result = impl_->d2d_device->CreateDeviceContext(
      D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
      impl_->d2d_context.ReleaseAndGetAddressOf());
  if (FAILED(result)) {
    return platform_failure(L"Direct2D context creation failed.");
  }

  result = DWriteCreateFactory(
      DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
      reinterpret_cast<IUnknown**>(
          impl_->dwrite_factory.ReleaseAndGetAddressOf()));
  if (FAILED(result)) {
    return platform_failure(L"DirectWrite factory creation failed.");
  }

  result = impl_->dwrite_factory->CreateTextFormat(
      L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24.0F, L"",
      impl_->status_text_format.ReleaseAndGetAddressOf());
  if (FAILED(result)) {
    return platform_failure(L"DirectWrite text format creation failed.");
  }
  static_cast<void>(impl_->status_text_format->SetTextAlignment(
      DWRITE_TEXT_ALIGNMENT_CENTER));
  static_cast<void>(impl_->status_text_format->SetParagraphAlignment(
      DWRITE_PARAGRAPH_ALIGNMENT_CENTER));

  result = impl_->d2d_context->CreateSolidColorBrush(
      D2D1::ColorF(0xD8DEE9, 1.0F),
      impl_->status_text_brush.ReleaseAndGetAddressOf());
  if (FAILED(result)) {
    return platform_failure(L"Status text brush creation failed.");
  }

  ComPtr<IDXGIAdapter> adapter;
  result = dxgi_device->GetAdapter(adapter.ReleaseAndGetAddressOf());
  if (FAILED(result)) {
    return platform_failure(L"DXGI adapter query failed.");
  }

  ComPtr<IDXGIFactory2> factory;
  result = adapter->GetParent(
      IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()));
  if (FAILED(result)) {
    return platform_failure(L"DXGI factory query failed.");
  }

  DXGI_SWAP_CHAIN_DESC1 description{};
  description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  description.SampleDesc.Count = 1;
  description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  description.BufferCount = 2;
  description.Scaling = DXGI_SCALING_STRETCH;
  description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

  result = factory->CreateSwapChainForHwnd(
      impl_->d3d_device.Get(), window, &description, nullptr, nullptr,
      impl_->swap_chain.ReleaseAndGetAddressOf());
  if (FAILED(result)) {
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    result = factory->CreateSwapChainForHwnd(
        impl_->d3d_device.Get(), window, &description, nullptr, nullptr,
        impl_->swap_chain.ReleaseAndGetAddressOf());
  }
  if (FAILED(result)) {
    if (is_device_lost(result)) {
      impl_->mark_lost();
      return render_target_lost(
          L"Direct3D device was lost while creating the swap chain.");
    }
    return platform_failure(L"Swap chain creation failed.");
  }

  static_cast<void>(
      factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER));

  result = impl_->create_target();
  if (FAILED(result)) {
    if (is_render_target_lost(result)) {
      impl_->mark_lost();
      return render_target_lost(
          L"Render target was lost during renderer initialization.");
    }
    return platform_failure(L"Render target creation failed.");
  }

  impl_->lost = false;
  return core::Result<bool>::success(true);
}

core::Result<bool> D3dRenderer::resize(unsigned width, unsigned height) {
  if (impl_ && impl_->lost) {
    return render_target_lost(L"Renderer render target is lost.");
  }
  if (!impl_ || !impl_->swap_chain || !impl_->d2d_context) {
    return platform_failure(L"Renderer is not initialized.");
  }
  if (width == 0 || height == 0) {
    return core::Result<bool>::success(true);
  }

  impl_->d2d_context->SetTarget(nullptr);
  impl_->target.Reset();

  const HRESULT resize_result = impl_->swap_chain->ResizeBuffers(
      0, width, height, DXGI_FORMAT_UNKNOWN, 0);
  if (FAILED(resize_result)) {
    if (is_render_target_lost(resize_result)) {
      impl_->mark_lost();
      return render_target_lost(
          L"Direct3D device was lost while resizing the swap chain.");
    }

    const HRESULT recovery_result = impl_->create_target();
    if (FAILED(recovery_result)) {
      impl_->mark_lost();
      return render_target_lost(
          L"Swap chain resize failed and its render target could not be "
          L"restored.");
    }
    return platform_failure(L"Swap chain resize failed.");
  }

  const HRESULT target_result = impl_->create_target();
  if (FAILED(target_result)) {
    impl_->mark_lost();
    return render_target_lost(L"Render target recreation failed.");
  }
  return core::Result<bool>::success(true);
}

core::Result<bool> D3dRenderer::set_image(const core::ImageFrame& frame) {
  if (impl_ && impl_->lost) {
    return render_target_lost(L"Renderer render target is lost.");
  }
  if (!impl_ || !impl_->d2d_context) {
    return platform_failure(L"Renderer is not initialized.");
  }
  if (!frame_is_valid(frame)) {
    return core::Result<bool>::failure(
        {core::ErrorCode::invalid_format,
         L"BGRA image dimensions, stride, and pixel storage are inconsistent."});
  }

  const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_NONE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED),
      96.0F, 96.0F);

  ComPtr<ID2D1Bitmap1> uploaded_image;
  const HRESULT result = impl_->d2d_context->CreateBitmap(
      D2D1::SizeU(frame.width, frame.height), frame.pixels.data(),
      frame.stride, &properties, uploaded_image.ReleaseAndGetAddressOf());
  if (FAILED(result)) {
    if (is_render_target_lost(result)) {
      impl_->mark_lost();
      return render_target_lost(
          L"Render target was lost while uploading the image bitmap.");
    }
    return platform_failure(L"Image bitmap upload failed.");
  }

  impl_->image = std::move(uploaded_image);
  return core::Result<bool>::success(true);
}

void D3dRenderer::set_status_text(std::wstring text) {
  if (impl_) {
    impl_->status_text = std::move(text);
  }
}

core::Result<bool> D3dRenderer::draw(
    const core::ViewTransform& transform) {
  if (impl_ && impl_->lost) {
    return render_target_lost(L"Renderer render target is lost.");
  }
  if (!impl_ || !impl_->d2d_context || !impl_->target ||
      !impl_->swap_chain) {
    return platform_failure(L"Renderer is not initialized.");
  }

  impl_->d2d_context->BeginDraw();
  impl_->d2d_context->SetTransform(D2D1::Matrix3x2F::Identity());
  impl_->d2d_context->Clear(clear_color);

  if (impl_->image) {
    const D2D1_SIZE_U image_size = impl_->image->GetPixelSize();
    const D2D1_SIZE_U viewport_size = impl_->target->GetPixelSize();
    const Affine2D transform_matrix = make_image_transform(
        {image_size.width, image_size.height},
        {viewport_size.width, viewport_size.height}, transform.scale(),
        transform.rotation(), {transform.offset_x(), transform.offset_y()});
    impl_->d2d_context->SetTransform(D2D1::Matrix3x2F(
        transform_matrix.m11, transform_matrix.m12, transform_matrix.m21,
        transform_matrix.m22, transform_matrix.dx, transform_matrix.dy));
    impl_->d2d_context->DrawBitmap(
        impl_->image.Get(), nullptr, 1.0F,
        D2D1_INTERPOLATION_MODE_LINEAR, nullptr);
    impl_->d2d_context->SetTransform(D2D1::Matrix3x2F::Identity());
  } else if (!impl_->image && !impl_->status_text.empty()) {
    const D2D1_SIZE_U viewport_size = impl_->target->GetPixelSize();
    const D2D1_RECT_F layout = D2D1::RectF(
        32.0F, 0.0F, static_cast<float>(viewport_size.width) - 32.0F,
        static_cast<float>(viewport_size.height));
    impl_->d2d_context->DrawText(
        impl_->status_text.c_str(),
        static_cast<UINT32>(impl_->status_text.size()),
        impl_->status_text_format.Get(), layout,
        impl_->status_text_brush.Get());
  }

  const HRESULT draw_result = impl_->d2d_context->EndDraw();
  if (is_render_target_lost(draw_result)) {
    impl_->mark_lost();
    return render_target_lost(
        L"Direct2D render target or Direct3D device was lost.");
  }
  if (FAILED(draw_result)) {
    return platform_failure(L"Direct2D drawing failed.");
  }

  const HRESULT present_result = impl_->swap_chain->Present(1, 0);
  switch (classify_present_outcome(classify_present_hresult(present_result))) {
    case PresentAction::presented:
      return core::Result<bool>::success(true);

    case PresentAction::skipped:
      return core::Result<bool>::success(false);

    case PresentAction::render_target_lost:
      impl_->mark_lost();
      return render_target_lost(
          L"Direct3D device was removed, reset, or encountered an internal "
          L"driver error.");

    case PresentAction::platform_error:
      return platform_failure(L"Swap chain presentation failed.");
  }
  return platform_failure(L"Swap chain presentation failed.");
}

void D3dRenderer::clear_image() {
  if (impl_) {
    impl_->image.Reset();
  }
}

}  // namespace viewer::render
